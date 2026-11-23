#include "EditPage.h"
#include "HitEdits.h"

namespace
{
constexpr float grab = 6.0f, handleSize = 10.0f;
constexpr double minShown = 0.0005;

const char* names[] = { "INICIO", "FIN", "BUCLE", "BUCLE", "", "" };
}

EditView::EditView (ReanimaProcessor& p)
    : ParamView (p, { P::StartPos, P::EndPos, P::FadeIn, P::FadeOut, P::LoopOn, P::LoopStart, P::LoopEnd, P::LoopXfade })
{
    setRepaintsOnMouseActivity (false);
    setWantsKeyboardFocus (true);
}

// An edit that keeps the length keeps the view and the selection; a new length or a new
// sample starts from the whole file.
void EditView::sampleChanged()
{
    const auto previous = sample;
    sample = proc.getSample();
    overview.clear();
    peak = sample != nullptr ? sample->peakLevel : 1.0f;
    if (sample == nullptr || previous == nullptr || previous->length != sample->length)
    {
        first = 0.0;
        shown = 1.0;
        selectionStart = selectionEnd = 0.0;
        insertAt = -1.0;
    }
    repaint();
}

double EditView::zeroCrossingNear (double fraction) const
{
    if (sample == nullptr || ! snapsToZero)
        return fraction;
    const int n = sample->length;
    const int at = juce::jlimit (0, n, (int) std::round (fraction * n));
    const int radius = (int) (0.005 * sample->sampleRate);
    const float* l = sample->audio.getReadPointer (0);
    const float* r = sample->audio.getReadPointer (1);
    auto mono = [&] (int i) { return l[i] + r[i]; };
    for (int d = 0; d <= radius; ++d)
        for (const int i : { at - d, at + d })
            if (i > 0 && i < n && (mono (i - 1) <= 0.0f) != (mono (i) <= 0.0f))
                return (double) i / n;
    return fraction;
}

bool EditView::snapsToGrid() const
{
    return (bool) proc.apvts.state.getProperty ("editSnap", false);
}

void EditView::setSnapsToGrid (bool on)
{
    proc.apvts.state.setProperty ("editSnap", on, nullptr);
    repaint();
}

int EditView::gridChoice() const
{
    return juce::jlimit (0, (int) gridChoices().size(), (int) proc.apvts.state.getProperty ("editGrid", 0));
}

void EditView::setGridChoice (int choice)
{
    proc.apvts.state.setProperty ("editGrid", juce::jlimit (0, (int) gridChoices().size(), choice), nullptr);
    repaint();
}

const std::vector<EditView::GridChoice>& EditView::gridChoices()
{
    static const std::vector<GridChoice> choices {
        { "4 compases", 16.0 }, { "2 compases", 8.0 }, { "1 compás", 4.0 }, { "1/2", 2.0 }, { "1/4", 1.0 }, { "1/8", 0.5 },
        { "1/16", 0.25 }, { "1/32", 0.125 }, { "1/64", 0.0625 }, { "1/4T", 2.0 / 3.0 }, { "1/8T", 1.0 / 3.0 }, { "1/16T", 1.0 / 6.0 } };
    return choices;
}

// A loop's own beats (LOOP, or a sample the analysis is sure is a loop), else the host's tempo, else none.
double EditView::beatSamples() const
{
    if (sample == nullptr)
        return 0.0;
    const bool loopish = (int) value (P::PlayMode) == P::PlayLoop || sample->loopInfo.likely;
    const double beats = loopish ? proc.loopBeatsNow() : 0.0;
    const int length = sample->loopInfo.length > 0 ? std::min (sample->loopInfo.length, sample->length) : sample->length;
    if (beats > 0.0 && length > 0)
        return length / beats;
    const double bpm = proc.getHostBpm();
    return bpm > 0.0 ? sample->sampleRate * 60.0 / bpm : 0.0;
}

EditView::Grid EditView::grid() const
{
    Grid gr;
    if (sample == nullptr)
        return gr;
    const double len = std::max (1, sample->length);
    const double pixelsPerSample = area().getWidth() / (shown * len);
    constexpr double minPixels = 40.0;          // Auto: as fine as leaves this much room between lines (14 px was a line every 1/64 of a 2 s loop)
    gr.beat = beatSamples();
    if (gr.beat > 0.0)
    {
        gr.musical = true;
        const int choice = gridChoice();
        if (choice > 0)
        {
            const auto& c = gridChoices()[(size_t) choice - 1];
            gr.step = c.beats * gr.beat;
            gr.name = c.name;
        }
        else
        {
            gr.step = 16.0 * gr.beat;
            gr.name = "4 compases";
            for (const auto& c : gridChoices())
                if (c.beats >= 0.0625 && std::abs (std::log2 (c.beats) - std::round (std::log2 (c.beats))) < 1.0e-9    // the straight ones
                    && c.beats * gr.beat * pixelsPerSample >= minPixels && c.beats * gr.beat < gr.step)
                {
                    gr.step = c.beats * gr.beat;
                    gr.name = c.name;
                }
            gr.name = "Auto (" + gr.name + ")";
        }
        return gr;
    }
    // Without a tempo: time, as fine as the zoom leaves room for (down to 1 ms; a sample at the finest zoom).
    const double steps[] = { 60, 30, 10, 5, 1, 0.5, 0.1, 0.05, 0.01, 0.005, 0.001 };
    gr.step = 60.0 * sample->sampleRate;
    gr.name = "60 s";
    for (const double st : steps)
        if (st * sample->sampleRate * pixelsPerSample >= minPixels)
        {
            gr.step = st * sample->sampleRate;
            gr.name = st >= 1.0 ? juce::String ((int) st) + " s" : juce::String (juce::roundToInt (st * 1000.0)) + " ms";
        }
    return gr;
}

double EditView::snapped (double fraction) const
{
    if (sample == nullptr)
        return fraction;
    const auto gr = grid();
    if (gr.step <= 0.0)
        return fraction;
    const double len = std::max (1, sample->length);
    const double at = fraction * len;
    const double line = gr.origin + std::round ((at - gr.origin) / gr.step) * gr.step;
    return juce::jlimit (0.0, 1.0, std::round (line) / len);      // on a sample: a cut is a whole sample
}

double EditView::place (double fraction, const juce::MouseEvent& e) const
{
    return snapsToGrid() && ! e.mods.isAltDown() ? snapped (fraction) : fraction;
}

void EditView::selectAll()
{
    selectionStart = 0.0;
    selectionEnd = 1.0;
    repaint();
}

void EditView::copy()
{
    if (hasSelection())
        proc.copyAudio ((int) std::round (selectionStart * sample->length), (int) std::round (selectionEnd * sample->length));
}

void EditView::runEdit (ReanimaProcessor::Edit what, float amount)
{
    if (sample == nullptr)
        return;
    const int n = sample->length;
    int from = (int) std::round (selectionStart * n), to = (int) std::round (selectionEnd * n);
    if (what == ReanimaProcessor::Edit::Paste && ! hasSelection())
        from = to = (int) std::round (juce::jlimit (0.0, 1.0, insertAt) * n);
    proc.edit (what, from, to, amount);
}

bool EditView::keyPressed (const juce::KeyPress& key)
{
    using E = ReanimaProcessor::Edit;
    const bool command = key.getModifiers().isCommandDown();
    const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter() != 0 ? key.getTextCharacter() : (juce::juce_wchar) key.getKeyCode());

    if (command && c == 'a') { selectAll(); return true; }
    if (command && c == 'c') { copy(); return true; }
    if (command && c == 'x') { runEdit (E::Cut); return true; }
    if (command && c == 'v') { runEdit (E::Paste); return true; }
    if (command && (c == 'y' || (c == 'z' && key.getModifiers().isShiftDown()))) { proc.redo(); return true; }
    if (command && c == 'z') { proc.undo(); return true; }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) { runEdit (E::Delete); return true; }
    if (key == juce::KeyPress::escapeKey) { selectionStart = selectionEnd = 0.0; repaint(); return true; }
    if (! command && c == 'g') { setSnapsToGrid (! snapsToGrid()); if (onMagnetChanged) onMagnetChanged(); return true; }
    return false;
}

juce::Rectangle<float> EditView::area() const
{
    return getLocalBounds().toFloat().withTrimmedTop (22.0f).withTrimmedBottom (18.0f).reduced (10.0f, 4.0f);
}

float EditView::xFor (double fraction) const
{
    const auto a = area();
    return a.getX() + (float) ((fraction - first) / shown) * a.getWidth();
}

double EditView::fractionFor (float x) const
{
    const auto a = area();
    return first + (x - a.getX()) / a.getWidth() * shown;
}

double EditView::markerFraction (int m) const
{
    if (sample == nullptr)
        return 0.0;
    const double seconds = sample->sampleRate;
    const double len = std::max (1, sample->length);
    switch (m)
    {
        case Start:        return sample->startPosition (value (P::StartPos)) / len;
        case End:          return value (P::EndPos);
        case LoopStart:    return std::min (value (P::LoopStart), value (P::LoopEnd));
        case LoopEnd:      return std::max (value (P::LoopStart), value (P::LoopEnd));
        case FadeInEnd:    return markerFraction (Start) + value (P::FadeIn) * 0.001 * seconds / len;
        case FadeOutStart: return markerFraction (End) - value (P::FadeOut) * 0.001 * seconds / len;
        default:           return 0.0;
    }
}

juce::Rectangle<float> EditView::hitStrip() const
{
    const auto a = area();
    return a.withTop (a.getBottom() - 22.0f);
}

int EditView::hitAt (juce::Point<float> p, bool& isCut) const
{
    isCut = false;
    if (sample == nullptr || ! proc.hitsEditable() || ! hitStrip().contains (p))
        return -1;
    const double len = std::max (1, sample->length);
    int best = -1;
    float nearest = grab + 1.0f;
    for (const int c : proc.getLoopCuts())
        if (const float d = std::abs (xFor (c / len) - p.x); c > 0 && d < nearest)
        {
            nearest = d;
            best = c;
            isCut = true;
        }
    const int length = sample->loopInfo.length > 0 ? std::min (sample->loopInfo.length, sample->length) : sample->length;
    for (const int c : proc.getHitEdits().off)
        if (const float d = std::abs (xFor (c / len) - p.x); c < length && d < nearest)
        {
            nearest = d;
            best = c;
            isCut = false;
        }
    return best;
}

int EditView::markerAt (const juce::MouseEvent& e) const
{
    if (sample == nullptr)
        return None;
    const auto p = e.position;

    // LOOP: a cut (or a hit turned off) in the strip along the bottom.
    {
        bool isCut = false;
        auto* self = const_cast<EditView*> (this);
        self->hoverHit = hitAt (p, isCut);
        self->hoverHitIsCut = isCut;
        if (hoverHit >= 0)
            return Hit;
    }

    const auto a = area();
    // The fade handles sit at the top; the lines run the whole height.
    for (const int m : { FadeInEnd, FadeOutStart })
        if (juce::Point<float> (xFor (markerFraction (m)), a.getY() + handleSize * 0.5f).getDistanceFrom (p) < handleSize)
            return m;

    // The edges of the selection: they move (to the grid with the magnet).
    if (hasSelection() && a.contains (p))
        for (const int m : { SelStart, SelEnd })
            if (std::abs (p.x - xFor (m == SelStart ? selectionStart : selectionEnd)) < grab)
                return m;

    const bool loop = value (P::LoopOn) > 0.5f;
    for (const int m : { Start, End, LoopStart, LoopEnd })
    {
        if ((m == LoopStart || m == LoopEnd) && ! loop)
            continue;
        if (std::abs (p.x - xFor (markerFraction (m))) < grab && a.contains (p))
            return m;
    }
    if (! a.contains (p))
        return None;
    return (e.mods.isShiftDown() || e.mods.isMiddleButtonDown()) && shown < 1.0 ? Pan : Select;
}

void EditView::setHover (int m)
{
    if (m != hover)
    {
        hover = m;
        setMouseCursor (m == Pan ? juce::MouseCursor::DraggingHandCursor
                      : m == Hit ? juce::MouseCursor::PointingHandCursor
                      : m == Select ? juce::MouseCursor::IBeamCursor
                      : m != None ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

juce::String EditView::getTooltip()
{
    switch (hover)
    {
        case Start:        return Help::forParam (P::StartPos);
        case End:          return Help::forParam (P::EndPos);
        case LoopStart:
        case LoopEnd:      return P::u8 ("Arrastra para mover el bucle. Mantén Bucle encendido para que se repita.");
        case FadeInEnd:    return Help::forParam (P::FadeIn);
        case FadeOutStart: return Help::forParam (P::FadeOut);
        case Pan:          return P::u8 ("Arrastra para desplazarte. Rueda: zoom.");
        case Select:       return P::u8 ("Arrastra para seleccionar; un clic marca dónde se pega. Ctrl+C, Ctrl+X, Ctrl+V, Supr, Ctrl+Z. "
                                         "Rueda: zoom; Mayús + arrastrar: desplazarte. Imán (G): los bordes van a la rejilla; con Alt, libres.");
        case SelStart:
        case SelEnd:       return P::u8 ("Borde de la selección: arrástralo para ajustarla (con Imán, a la rejilla; con Alt, libre).");
        case Hit:          return hoverHitIsCut ? P::u8 ("Golpe que corta el loop: clic lo apaga (deja de cortar y de reiniciar las envolventes con "
                                                         "Por golpe). Doble clic en un hueco de esta franja añade uno. Lo mismo que en SAMPLE.")
                                                : P::u8 ("Golpe apagado: clic lo vuelve a encender.");
        default:           return sample != nullptr ? P::u8 ("Rueda: zoom. Arrastra las líneas para el inicio, el fin, los fundidos y el bucle.") : juce::String();
    }
}

void EditView::setValue (int index, float v)
{
    if (auto* param = proc.apvts.getParameter (P::defs()[(size_t) index].id))
        param->setValueNotifyingHost (param->convertTo0to1 (v));
}

void EditView::setFraction (int m, double fraction)
{
    const double len = std::max (1, sample->length), rate = sample->sampleRate;
    const float f = (float) juce::jlimit (0.0, 1.0, fraction);
    switch (m)
    {
        case Start:        setValue (P::StartPos, std::min (f, (float) markerFraction (End) - 0.001f)); break;
        case End:          setValue (P::EndPos, std::max (f, (float) markerFraction (Start) + 0.001f)); break;
        case LoopStart:    setValue (value (P::LoopStart) <= value (P::LoopEnd) ? P::LoopStart : P::LoopEnd, f); break;
        case LoopEnd:      setValue (value (P::LoopStart) <= value (P::LoopEnd) ? P::LoopEnd : P::LoopStart, f); break;
        case FadeInEnd:    setValue (P::FadeIn, (float) std::max (0.0, (fraction - markerFraction (Start)) * len / rate * 1000.0)); break;
        case FadeOutStart: setValue (P::FadeOut, (float) std::max (0.0, (markerFraction (End) - fraction) * len / rate * 1000.0)); break;
        case SelStart:     selectionStart = juce::jlimit (0.0, selectionEnd, fraction); repaint(); break;
        case SelEnd:       selectionEnd = juce::jlimit (selectionStart, 1.0, fraction); repaint(); break;
        default:           break;
    }
}

void EditView::mouseMove (const juce::MouseEvent& e)
{
    const int hitBefore = hoverHit;
    setHover (markerAt (e));
    if (hoverHit != hitBefore)
        repaint();          // from one cut to the next: the highlight moves
}

void EditView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragging = markerAt (e);
    panFrom = first;
    if (dragging == Hit)
        return;
    if (dragging == Select)
    {
        anchor = place (juce::jlimit (0.0, 1.0, fractionFor (e.position.x)), e);
        selectionStart = selectionEnd = anchor;
        repaint();
    }
    static const int ids[] = { P::StartPos, P::EndPos, P::LoopStart, P::LoopEnd, P::FadeIn, P::FadeOut };
    if (dragging >= 0 && dragging < Pan)
        if (auto* param = proc.apvts.getParameter (P::defs()[(size_t) ids[dragging]].id))
            param->beginChangeGesture();
}

void EditView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging == Pan)
    {
        first = juce::jlimit (0.0, 1.0 - shown, panFrom - e.getDistanceFromDragStartX() / area().getWidth() * shown);
        repaint();
    }
    else if (dragging == Select)
    {
        const double at = place (juce::jlimit (0.0, 1.0, fractionFor (e.position.x)), e);
        selectionStart = std::min (anchor, at);
        selectionEnd = std::max (anchor, at);
        repaint();
    }
    else if (dragging != None)
    {
        setFraction (dragging, place (fractionFor (e.position.x), e));
    }
}

void EditView::mouseUp (const juce::MouseEvent& e)
{
    if (dragging == Hit)
    {
        if (std::abs (e.getDistanceFromDragStartX()) < 4 && hoverHit > 0)
            proc.toggleHit (hoverHit, hoverHitIsCut);
        dragging = None;
        repaint();
        return;
    }
    if (dragging == Select)
    {
        // A click is not a selection: it marks where Pegar inserts. With the magnet the edges stay on the grid
        // (exactly: no move to a zero crossing); without it, Cruces por cero as before.
        const bool onGrid = snapsToGrid() && ! e.mods.isAltDown();
        if (std::abs (e.getDistanceFromDragStartX()) < 3)
        {
            selectionStart = selectionEnd = 0.0;
            insertAt = onGrid ? anchor : zeroCrossingNear (anchor);
        }
        else if (! onGrid)
        {
            selectionStart = zeroCrossingNear (selectionStart);
            selectionEnd = zeroCrossingNear (selectionEnd);
        }
        repaint();
    }
    else if ((dragging == SelStart || dragging == SelEnd) && ! (snapsToGrid() && ! e.mods.isAltDown()))
    {
        selectionStart = zeroCrossingNear (selectionStart);
        selectionEnd = zeroCrossingNear (selectionEnd);
        repaint();
    }

    static const int ids[] = { P::StartPos, P::EndPos, P::LoopStart, P::LoopEnd, P::FadeIn, P::FadeOut };
    if (dragging >= 0 && dragging < Pan)
        if (auto* param = proc.apvts.getParameter (P::defs()[(size_t) ids[dragging]].id))
            param->endChangeGesture();
    dragging = None;
}

// LOOP: a double click in the strip of cuts, away from one, adds a hit: on the grid with the magnet, else on the
// nearest hit the analysis found within a few pixels (with Mayús, where the mouse is).
void EditView::mouseDoubleClick (const juce::MouseEvent& e)
{
    bool isCut = false;
    if (sample == nullptr || ! proc.hitsEditable() || ! hitStrip().contains (e.position) || hitAt (e.position, isCut) >= 0)
        return;
    const double len = std::max (1, sample->length);
    double fraction = juce::jlimit (0.0, 1.0, fractionFor (e.position.x));
    if (snapsToGrid() && ! e.mods.isAltDown())
        fraction = snapped (fraction);
    int at = (int) std::lround (fraction * len);
    if (! snapsToGrid() && ! e.mods.isShiftDown())
    {
        float nearest = grab * 2.0f;
        for (const auto& h : sample->loopInfo.hits)
            if (const float d = std::abs (xFor (h.first / len) - e.position.x); d < nearest)
            {
                nearest = d;
                at = h.first;
            }
    }
    const int length = sample->loopInfo.length > 0 ? std::min (sample->loopInfo.length, sample->length) : sample->length;
    if (at > 0 && at < length)
        proc.addHit (at);
    repaint();
}

void EditView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (sample == nullptr)
        return;

    if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
    {
        const float delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
        first = juce::jlimit (0.0, 1.0 - shown, first - delta * shown * 0.5);
    }
    else
    {
        const double centre = fractionFor (e.position.x);
        const double newShown = juce::jlimit (minShown, 1.0, shown * std::exp (-wheel.deltaY * 2.5));
        first = juce::jlimit (0.0, 1.0 - newShown, centre - (centre - first) * newShown / shown);
        shown = newShown;
    }
    repaint();
}

void EditView::buildOverview()
{
    const auto a = area();
    const int columns = std::max (1, (int) a.getWidth());
    overview.assign ((size_t) columns, { 0.0f, 0.0f });
    overviewFirst = first;
    overviewShown = shown;

    const float* l = sample->audio.getReadPointer (0);
    const float* r = sample->audio.getReadPointer (1);
    const double len = sample->length;
    for (int c = 0; c < columns; ++c)
    {
        const int i0 = std::max (0, (int) ((first + (double) c / columns * shown) * len));
        const int i1 = std::min (sample->length, std::max (i0 + 1, (int) ((first + (double) (c + 1) / columns * shown) * len)));
        float mn = 0.0f, mx = 0.0f;
        for (int i = i0; i < i1; ++i)
        {
            const float v = 0.5f * (l[i] + r[i]);
            mn = std::min (mn, v);
            mx = std::max (mx, v);
        }
        overview[(size_t) c] = { mn, mx };
    }
}

void EditView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::well);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (Theme::panelEdge);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    if (sample == nullptr)
    {
        g.setColour (Theme::dim);
        g.setFont (Theme::font (16.0f));
        g.drawText (P::u8 ("Carga un sample para editarlo"), getLocalBounds(), juce::Justification::centred);
        return;
    }

    const auto a = area();
    const double seconds = sample->length / sample->sampleRate;
    g.saveState();
    g.reduceClipRegion (a.withBottom (a.getBottom() + 18.0f).expanded (1.0f, 0.0f).toNearestInt());

    // Grid: with the magnet, the one the edges go to (bars brightest, then beats, then the rest, labelled bar.beat);
    // without it, time (seconds), as before.
    const auto gr = grid();
    if (snapsToGrid() && gr.step > 0.0)
    {
        const double len = std::max (1, sample->length);
        const double from = std::ceil ((first * len - gr.origin) / gr.step), to = ((first + shown) * len - gr.origin) / gr.step;
        g.setFont (Theme::font (10.0f));
        float lastLabel = -1000.0f;
        for (double k = from; k <= to && k - from < 4000.0; k += 1.0)
        {
            const double at = gr.origin + k * gr.step;
            const float x = xFor (at / len);
            const double beats = gr.musical ? at / gr.beat : 0.0;
            const bool bar = gr.musical && std::abs (beats / 4.0 - std::round (beats / 4.0)) < 1.0e-6;
            const bool beat = gr.musical && std::abs (beats - std::round (beats)) < 1.0e-6;
            g.setColour (Theme::accent.withAlpha (bar ? 0.55f : beat ? 0.32f : 0.16f));
            g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
            if ((bar || beat || ! gr.musical) && x - lastLabel > 34.0f)
            {
                g.setColour (Theme::dim);
                const juce::String label = gr.musical ? juce::String ((int) std::floor (beats / 4.0 + 1.0e-6) + 1) + "." + juce::String ((int) std::round (beats) % 4 + 1)
                                                      : (gr.step < sample->sampleRate ? juce::String (juce::roundToInt (at / sample->sampleRate * 1000.0)) + " ms"
                                                                                      : juce::String (at / sample->sampleRate, 1) + " s");
                g.drawText (label, (int) x + 2, (int) a.getBottom() + 2, 60, 14, juce::Justification::centredLeft);
                lastLabel = x;
            }
        }
    }
    else
    {
        const double steps[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 30, 60 };
        double step = 60;
        for (const double st : steps)
            if (shown * seconds / st <= 12.0) { step = st; break; }
        g.setFont (Theme::font (10.0f));
        for (double t = std::ceil (first * seconds / step) * step; t <= (first + shown) * seconds; t += step)
        {
            const float x = xFor (t / seconds);
            g.setColour (Theme::panelEdge.withAlpha (0.6f));
            g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
            g.setColour (Theme::dim);
            g.drawText (step < 1.0 ? juce::String (juce::roundToInt (t * 1000.0)) + " ms" : juce::String (t, 1) + " s",
                        (int) x + 2, (int) a.getBottom() + 2, 60, 14, juce::Justification::centredLeft);
        }
    }

    const bool loop = value (P::LoopOn) > 0.5f;
    const float xStart = xFor (markerFraction (Start)), xEnd = xFor (markerFraction (End));
    const float xLoopA = xFor (markerFraction (LoopStart)), xLoopB = xFor (markerFraction (LoopEnd));

    // Loop region, and the crossfade before its end.
    {
        g.setColour (Theme::good.withAlpha (loop ? 0.12f : 0.04f));
        g.fillRect (xLoopA, a.getY(), xLoopB - xLoopA, a.getHeight());
        if (loop)
        {
            const double fadeFraction = value (P::LoopXfade) * 0.001 * sample->sampleRate / sample->length;
            const float xFade = std::max (xLoopA, xFor (markerFraction (LoopEnd) - fadeFraction));
            g.setColour (Theme::good.withAlpha (0.18f));
            g.fillRect (xFade, a.getY(), xLoopB - xFade, a.getHeight());
        }
    }

    // Waveform.
    if (overview.size() != (size_t) std::max (1, (int) a.getWidth()) || overviewFirst != first || overviewShown != shown)
        buildOverview();
    {
        const float mid = a.getCentreY(), scale = a.getHeight() * 0.48f / peak;
        g.setColour (Theme::accent.withAlpha (0.5f));
        for (size_t c = 0; c < overview.size(); ++c)
            g.drawVerticalLine ((int) (a.getX() + (float) c), mid - overview[c].second * scale, mid - overview[c].first * scale + 1.0f);
    }

    // What is left out, darkened.
    g.setColour (Theme::well.withAlpha (0.75f));
    if (xStart > a.getX())
        g.fillRect (a.withRight (xStart));
    if (xEnd < a.getRight())
        g.fillRect (a.withLeft (xEnd));

    // Fades: ramps from silence, with a handle at the top.
    {
        g.setColour (Theme::accent2);
        const float xIn = xFor (markerFraction (FadeInEnd)), xOut = xFor (markerFraction (FadeOutStart));
        g.drawLine (xStart, a.getBottom(), xIn, a.getY(), 1.5f);
        g.drawLine (xOut, a.getY(), xEnd, a.getBottom(), 1.5f);
        for (const auto [x, m] : { std::pair<float, int> { xIn, FadeInEnd }, { xOut, FadeOutStart } })
        {
            const float size = hover == m || dragging == m ? handleSize + 3.0f : handleSize;
            g.fillEllipse (juce::Rectangle<float> (size, size).withCentre ({ x, a.getY() + handleSize * 0.5f }));
        }
    }

    // Marker lines with their tabs.
    const auto marker = [&] (int m, float x, juce::Colour colour, bool top)
    {
        const bool active = hover == m || dragging == m;
        g.setColour (colour.withAlpha (active ? 0.25f : 0.0f));
        g.fillRect (x - 4.0f, a.getY(), 8.0f, a.getHeight());
        g.setColour (colour);
        g.fillRect (x - 1.0f, a.getY(), 2.0f, a.getHeight());
        const auto tab = juce::Rectangle<float> (m == End || m == LoopEnd ? x - 50.0f : x, top ? a.getY() + 14.0f : a.getBottom() - 16.0f, 50.0f, 15.0f);
        g.fillRoundedRectangle (tab, 3.0f);
        g.setColour (Theme::well);
        g.setFont (Theme::caps (9.5f));
        g.drawText (names[m], tab, juce::Justification::centred);
    };
    if (loop)
    {
        marker (LoopStart, xLoopA, Theme::good, false);
        marker (LoopEnd, xLoopB, Theme::good, false);
    }
    marker (Start, xStart, Theme::accent, true);
    marker (End, xEnd, Theme::accent2, true);

    // LOOP: its cuts along the bottom (as in SAMPLE): the one under the mouse full height, the added ones with a
    // dot, the ones turned off grey and dashed with a cross.
    if (proc.hitsEditable())
    {
        const double len = std::max (1, sample->length);
        const auto strip = hitStrip();
        const auto edits = proc.getHitEdits();
        const int tol = HitEdits::tolerance (sample->sampleRate);
        g.setColour (Theme::accent2.withAlpha (0.06f));
        g.fillRect (strip);
        for (const int cut : proc.getLoopCuts())
        {
            const bool hot = (hover == Hit || dragging == Hit) && hoverHitIsCut && std::abs (cut - hoverHit) <= tol;
            const float x = xFor (cut / len), tall = hot ? a.getHeight() : 14.0f;
            g.setColour (Theme::accent2.withAlpha (hot ? 1.0f : 0.85f));
            g.fillRect (x - (hot ? 1.0f : 0.5f), a.getBottom() - tall, hot ? 2.0f : 1.0f, tall);
            if (HitEdits::near (edits.on, cut, tol))
                g.fillEllipse (x - 2.5f, a.getBottom() - 21.0f, 5.0f, 5.0f);
        }
        const int length = sample->loopInfo.length > 0 ? std::min (sample->loopInfo.length, sample->length) : sample->length;
        for (const int off : edits.off)
        {
            if (off >= length)
                continue;
            const bool hot = (hover == Hit || dragging == Hit) && ! hoverHitIsCut && std::abs (off - hoverHit) <= tol;
            const float x = xFor (off / len);
            const float top = hot ? a.getY() : a.getBottom() - 14.0f;
            g.setColour (Theme::text.withAlpha (hot ? 0.9f : 0.45f));
            for (float y = a.getBottom() - 3.0f; y > top; y -= 5.0f)
                g.fillRect (x - 0.5f, std::max (top, y - 3.0f), 1.0f, std::min (3.0f, y - top));
            g.drawLine (x - 3.0f, a.getBottom() - 21.0f, x + 3.0f, a.getBottom() - 15.0f, 1.2f);
            g.drawLine (x - 3.0f, a.getBottom() - 15.0f, x + 3.0f, a.getBottom() - 21.0f, 1.2f);
        }
    }

    // The selection, and the point where Pegar inserts.
    if (hasSelection())
    {
        const float x0 = xFor (selectionStart), x1 = xFor (selectionEnd);
        g.setColour (Theme::text.withAlpha (0.13f));
        g.fillRect (x0, a.getY(), std::max (1.0f, x1 - x0), a.getHeight());
        // The edges: wider when under the mouse; on the grid, in the magnet's colour.
        const bool onGrid = snapsToGrid() && gr.step > 0.0;
        for (const auto [x, m] : { std::pair<float, int> { x0, SelStart }, { x1, SelEnd } })
        {
            const bool hot = hover == m || dragging == m;
            g.setColour (onGrid ? Theme::accent : Theme::text.withAlpha (0.8f));
            g.fillRect (x - (hot ? 1.0f : 0.5f), a.getY(), hot ? 2.0f : 1.0f, a.getHeight());
            g.fillRect (x - 3.0f, a.getY(), 6.0f, 3.0f);
            g.fillRect (x - 3.0f, a.getBottom() - 3.0f, 6.0f, 3.0f);
        }
    }
    else if (insertAt >= 0.0)
    {
        g.setColour (Theme::text.withAlpha (0.8f));
        const float x = xFor (insertAt);
        const float dashes[] = { 3.0f, 3.0f };
        g.drawDashedLine ({ x, a.getY(), x, a.getBottom() }, dashes, 2, 1.0f);
    }

    g.restoreState();

    g.setFont (Theme::font (12.0f));
    g.setColour (Theme::dim);
    const double len = (markerFraction (End) - markerFraction (Start)) * seconds;
    juce::String text = juce::String (len, 3) + P::u8 (" s entre INICIO y FIN");
    if (hasSelection())
    {
        text << P::u8 ("   ·   selección ") << juce::String ((selectionEnd - selectionStart) * seconds * 1000.0, 1) << " ms";
        if (gr.musical)
            text << " (" << juce::String ((selectionEnd - selectionStart) * sample->length / gr.beat, 2) << " tiempos)";
    }
    if (snapsToGrid() && gr.step > 0.0)
        text << P::u8 ("   ·   imán: ") << gr.name << (gr.musical ? "" : P::u8 (" (sin tempo)"));
    if (shown < 1.0)
        text << juce::String::formatted ("   ·   x%.1f", 1.0 / shown);
    if (proc.isEdited())
        text << P::u8 ("   ·   editado (se guarda en el proyecto)");
    g.drawText (text, 12, 3, getWidth() - 24, 16, juce::Justification::centredLeft);
}

//==============================================================================
EditToolbar::EditToolbar (ReanimaProcessor& p, EditView& v) : proc (p), view (v)
{
    using E = ReanimaProcessor::Edit;
    trim = &add ("Recortar", "Deja sólo la parte seleccionada.", [this] { view.runEdit (E::Trim); });
    remove = &add ("Borrar", "Quita la parte seleccionada (Supr).", [this] { view.runEdit (E::Delete); });
    silence = &add ("Silencio", "Deja la parte seleccionada en silencio, sin cambiar la duración.", [this] { view.runEdit (E::Silence); });
    copyButton = &add ("Copiar", "Copia la selección (Ctrl+C).", [this] { view.copy(); refresh(); });
    cut = &add ("Cortar", "Copia la selección y la quita (Ctrl+X).", [this] { view.runEdit (E::Cut); });
    paste = &add ("Pegar", "Pega lo copiado en el punto marcado con un clic, o en lugar de la selección (Ctrl+V). Puede venir de otro sample.",
                  [this] { view.runEdit (E::Paste); });
    reverse = &add ("Invertir", "Da la vuelta a la parte seleccionada.", [this] { view.runEdit (E::Reverse); });
    fadeIn = &add (P::u8 ("Fundido ↗"), "La parte seleccionada sube desde el silencio.", [this] { view.runEdit (E::FadeIn); }, 1.1f);
    fadeOut = &add (P::u8 ("Fundido ↘"), "La parte seleccionada baja hasta el silencio.", [this] { view.runEdit (E::FadeOut); }, 1.1f);
    normalize = &add ("Normalizar", "Sube o baja la selección hasta que su pico llegue a -0,1 dB.", [this] { view.runEdit (E::Normalize); }, 1.15f);
    gain = &add (P::u8 ("Ganancia ▾"), "Sube o baja el volumen de la selección.", [this]
    {
        juce::PopupMenu menu;
        const float steps[] = { 6.0f, 3.0f, 1.0f, -1.0f, -3.0f, -6.0f, -12.0f };
        for (int i = 0; i < (int) std::size (steps); ++i)
            menu.addItem (i + 1, (steps[i] > 0.0f ? "+" : "") + juce::String ((int) steps[i]) + " dB");
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (gain), [this, steps] (int r)
        {
            if (r > 0)
                view.runEdit (ReanimaProcessor::Edit::Gain, steps[r - 1]);
        });
    }, 1.15f);
    seamless = &add ("Bucle sin costura", "Escribe en el audio el fundido cruzado del bucle (con la duración de Cruce), para que el salto no se oiga en ningún "
                     "sampler. Después Cruce pasa a 0.", [this] { view.runEdit (E::SeamlessLoop); }, 1.6f);
    zero = &add ("Cruces por cero", "Ajusta los bordes de la selección y el punto de pegado al cruce por cero más cercano, para que los cortes no hagan clic.",
                 [this] { view.snapsToZero = zero->getToggleState(); }, 1.45f);
    zero->setClickingTogglesState (true);
    zero->setToggleState (view.snapsToZero, juce::dontSendNotification);
    magnet = &add (P::u8 ("Imán"), "Ajuste a la rejilla (tecla G): los bordes de la selección y las líneas van a la rejilla al arrastrarlos, "
                   "con precisión de una muestra. Con tempo, la rejilla son los tiempos del loop (o el tempo del proyecto); sin tempo, "
                   "segundos y milisegundos. Con Alt, libres. Con el imán no se mueven al cruce por cero.",
                   [this] { view.setSnapsToGrid (magnet->getToggleState()); refresh(); }, 0.9f);
    magnet->setClickingTogglesState (true);
    magnet->setToggleState (view.snapsToGrid(), juce::dontSendNotification);
    gridMenu = &add (P::u8 ("Auto ▾"), "Rejilla del imán. La resolución de la rejilla: Auto (según el zoom) o fija, de 4 compases a 1/64 y tresillos.", [this]
    {
        juce::PopupMenu menu;
        const bool musical = view.grid().musical;
        menu.addItem (1, "Auto (según el zoom)", true, view.gridChoice() == 0);
        menu.addSeparator();
        const auto& choices = EditView::gridChoices();
        for (int i = 0; i < (int) choices.size(); ++i)
            menu.addItem (i + 2, P::u8 (choices[(size_t) i].name), musical, view.gridChoice() == i + 1);
        if (! musical)
            menu.addSectionHeader (P::u8 ("Sin tempo: rejilla de tiempo según el zoom"));
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (gridMenu), [this] (int r)
        {
            if (r > 0)
                view.setGridChoice (r - 1);
            refresh();
        });
    }, 1.0f);
    view.onMagnetChanged = [this] { refresh(); };
    refresh();
}

juce::TextButton& EditToolbar::add (const juce::String& text, const char* tip, std::function<void()> action, float weight)
{
    auto button = std::make_unique<juce::TextButton> (text);
    button->setTooltip (P::u8 (tip));
    button->onClick = std::move (action);
    button->setColour (juce::TextButton::textColourOffId, Theme::text);
    addAndMakeVisible (*button);
    auto& ref = *button;
    buttons.push_back ({ std::move (button), weight });
    return ref;
}

void EditToolbar::resized()
{
    float total = 0.0f;
    for (const auto& b : buttons)
        total += b.second;
    constexpr int spacing = 4;
    const float room = (float) (getWidth() - spacing * ((int) buttons.size() - 1));
    float x = 0.0f;
    for (const auto& [button, weight] : buttons)
    {
        const float w = room * weight / total;
        button->setBounds (juce::roundToInt (x), 0, juce::roundToInt (w), getHeight());
        x += w + (float) spacing;
    }
}

void EditToolbar::refresh()
{
    const bool loaded = proc.getSample() != nullptr && ! proc.isLoading();
    const bool selected = loaded && view.hasSelection();
    for (auto* b : { trim, remove, silence, copyButton, cut, reverse, fadeIn, fadeOut, normalize, gain })
        b->setEnabled (selected);
    paste->setEnabled (loaded && proc.hasClipboard());
    seamless->setEnabled (loaded && proc.paramValue (P::LoopOn) > 0.5f && proc.paramValue (P::LoopXfade) > 0.0f);
    magnet->setToggleState (view.snapsToGrid(), juce::dontSendNotification);
    gridMenu->setEnabled (loaded);
    const int choice = view.gridChoice();
    gridMenu->setButtonText ((choice == 0 ? juce::String ("Auto") : P::u8 (EditView::gridChoices()[(size_t) choice - 1].name)) + P::u8 (" ▾"));
}

//==============================================================================
PitchEnvelopeShape::PitchEnvelopeShape (ReanimaProcessor& p) : ParamView (p, { P::PitchEnvAmt, P::PitchEnvTime })
{
    setTooltip (P::u8 ("Tono de la nota en el tiempo: arranca desplazado y vuelve a su sitio."));
}

void PitchEnvelopeShape::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::well);
    g.fillRoundedRectangle (bounds, 5.0f);
    g.setColour (Theme::panelEdge);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 5.0f, 1.0f);

    const auto a = bounds.reduced (10.0f, 10.0f);
    const float amount = value (P::PitchEnvAmt), time = std::max (0.001f, value (P::PitchEnvTime) * 0.001f);
    const float span = time * 1.5f;
    g.setColour (Theme::panelEdge);
    g.fillRect (a.getX(), a.getCentreY(), a.getWidth(), 1.0f);

    juce::Path line;
    for (int i = 0; i <= 120; ++i)
    {
        const float t = span * (float) i / 120.0f;
        const float semis = amount * std::exp (-t * 5.0f / time);
        const auto pt = juce::Point<float> (a.getX() + a.getWidth() * (float) i / 120.0f, a.getCentreY() - semis / 48.0f * a.getHeight() * 0.5f);
        if (i == 0) line.startNewSubPath (pt); else line.lineTo (pt);
    }
    g.setColour (Theme::accent2);
    g.strokePath (line, juce::PathStrokeType (2.0f));
}
