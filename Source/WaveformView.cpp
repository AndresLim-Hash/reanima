#include "WaveformView.h"
#include "Look.h"

namespace
{
constexpr float handleRadius = 9.0f, markerGrab = 6.0f, magnetPixels = 5.0f;
constexpr float maxZoom = 400.0f;
constexpr int headerHeight = 26;
}

WaveformView::WaveformView (ReanimaProcessor& p) : proc (p)
{
    setRepaintsOnMouseActivity (false);
    setBufferedToImage (true);

    zoomOut.setTooltip (P::u8 ("Alejar. También con la rueda del ratón sobre la onda."));
    zoomIn.setTooltip (P::u8 ("Acercar alrededor del inicio. Con la rueda, alrededor del ratón; con Mayús y la rueda, o arrastrando el fondo, te desplazas."));
    zoomAll.setTooltip (P::u8 ("Ver el sample entero."));
    zoomOut.onClick = [this] { const auto g = geometry (false); zoomAround (g.first + g.shown * 0.5, zoom / 2.0f); };
    zoomIn.onClick = [this] { zoomAround (geometry (false).startSeconds, zoom * 2.0f); };
    zoomAll.onClick = [this] { zoomAround (0.0, 1.0f); };
    for (auto* b : { &zoomOut, &zoomIn, &zoomAll })
        addAndMakeVisible (b);
    updateZoomButtons();
}

void WaveformView::resized()
{
    overview.clear();
    int x = 8;
    for (auto* b : { &zoomOut, &zoomIn, &zoomAll })
    {
        const int w = b == &zoomAll ? 44 : 24;
        b->setBounds (x, 4, w, 18);
        x += w + 4;
    }
}

void WaveformView::updateZoomButtons()
{
    const bool has = sample != nullptr;
    zoomOut.setEnabled (has && zoom > 1.0f);
    zoomIn.setEnabled (has && zoom < maxZoom);
    zoomAll.setEnabled (has && zoom > 1.0f);
}

void WaveformView::zoomAround (double seconds, float newZoom)
{
    const auto before = geometry (false);
    zoom = juce::jlimit (1.0f, maxZoom, newZoom);
    const double shown = before.viewSeconds / zoom;
    viewFirst = seconds - (seconds - before.first) * shown / before.shown;
    updateZoomButtons();
    repaint();
}

WaveformView::Playheads::Playheads (WaveformView& view) : owner (view)
{
    setInterceptsMouseClicks (false, false);
}

void WaveformView::Playheads::update()
{
    if (owner.sample == nullptr)
        return;

    const auto g = owner.geometry (owner.dragging >= 0);
    std::array<float, SynthEngine::physicalVoices> now {};
    std::array<int, SynthEngine::physicalVoices> nowSlice {};
    int count = 0;
    const auto& engine = owner.proc.engine;
    for (size_t v = 0; v < engine.playheads.size(); ++v)
    {
        const float pos = engine.playheads[v].load (std::memory_order_relaxed);
        if (pos >= 0.0f)
        {
            const float x = g.xFor (pos / owner.sample->sampleRate);
            if (g.visible (x))
            {
                nowSlice[(size_t) count] = engine.playSlices[v].load (std::memory_order_relaxed);
                now[(size_t) count++] = x;
            }
        }
    }

    // The slices sounding now: lit while they play, each in its own colour (several keys: several lit).
    if (litSlices.capacity() < 128)
        litSlices.reserve (128);
    bool litChanged = false;
    {
        std::array<int, SynthEngine::physicalVoices> sorted {};
        int n = 0;
        for (int i = 0; i < count; ++i)
            if (nowSlice[(size_t) i] >= 0)
                sorted[(size_t) n++] = nowSlice[(size_t) i];
        std::sort (sorted.begin(), sorted.begin() + n);
        n = (int) (std::unique (sorted.begin(), sorted.begin() + n) - sorted.begin());
        litChanged = n != (int) litSlices.size() || ! std::equal (sorted.begin(), sorted.begin() + n, litSlices.begin());
        if (litChanged)
            litSlices.assign (sorted.begin(), sorted.begin() + n);
    }

    if (count == 0 && drawnCount == 0 && ! litChanged)
        return;
    drawnSlice = nowSlice;
    if (litChanged)
    {
        drawn = now;
        drawnCount = count;
        repaint();
        return;
    }

    // Only the columns that change are redrawn.
    float left = 1.0e9f, right = -1.0e9f;
    for (int i = 0; i < drawnCount; ++i) { left = std::min (left, drawn[(size_t) i]); right = std::max (right, drawn[(size_t) i]); }
    for (int i = 0; i < count; ++i)      { left = std::min (left, now[(size_t) i]);   right = std::max (right, now[(size_t) i]); }

    drawn = now;
    drawnCount = count;
    repaint ((int) left - 2, 0, (int) (right - left) + 5, getHeight());
}

juce::Colour WaveformView::Playheads::colourFor (int slice) const
{
    static const juce::Colour colours[] = { Theme::accent, Theme::accent2, Theme::good, Theme::warning,
                                            juce::Colour (0xffd38bff), juce::Colour (0xffff6f91) };
    return slice < 0 ? juce::Colours::white : colours[(size_t) slice % std::size (colours)];
}

void WaveformView::Playheads::paint (juce::Graphics& gfx)
{
    const auto g = owner.geometry (owner.dragging >= 0);
    const auto area = g.area;
    if (owner.sample != nullptr && ! litSlices.empty())
    {
        // Each slice playing: its region lit, from its marker to the next.
        const auto marks = owner.proc.getSlices();
        const auto& s = *owner.sample;
        const int length = s.loopInfo.length > 0 ? std::min (s.loopInfo.length, s.length) : s.length;
        for (const int k : litSlices)
            if (k < (int) marks.size())
            {
                const float x0 = g.xFor (marks[(size_t) k] / s.sampleRate);
                const float x1 = g.xFor ((k + 1 < (int) marks.size() ? marks[(size_t) k + 1] : length) / s.sampleRate);
                const auto c = colourFor (k);
                gfx.setColour (c.withAlpha (0.16f));
                gfx.fillRect (juce::Rectangle<float> (x0, area.getY(), x1 - x0, area.getHeight()).getIntersection (area));
                gfx.setColour (c.withAlpha (0.9f));
                gfx.fillRect (juce::Rectangle<float> (x0, area.getY(), x1 - x0, 3.0f).getIntersection (area));
            }
    }
    for (int i = 0; i < drawnCount; ++i)
    {
        gfx.setColour (colourFor (drawnSlice[(size_t) i]).withAlpha (drawnSlice[(size_t) i] >= 0 ? 0.95f : 0.75f));
        gfx.fillRect ((float) (int) drawn[(size_t) i], area.getY(), drawnSlice[(size_t) i] >= 0 ? 2.0f : 1.0f, area.getHeight());
    }
}

void WaveformView::sampleChanged()
{
    sample = proc.getSample();
    overview.clear();
    zoom = 1.0f;
    viewFirst = 0.0;
    samplePeak = sample != nullptr ? std::max (1.0e-6f, std::max (sample->audio.getMagnitude (0, 0, sample->length),
                                                                   sample->audio.getMagnitude (1, 0, sample->length)))
                                   : 1.0f;
    updateZoomButtons();
    repaint();
}

void WaveformView::setParam (int index, float value)
{
    if (auto* p = proc.apvts.getParameter (P::defs()[(size_t) index].id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

WaveformView::Geometry WaveformView::geometry (bool useFrozenView) const
{
    Geometry g;
    g.area = getLocalBounds().toFloat().withTrimmedTop ((float) headerHeight).withTrimmedBottom (18.0f).reduced (10.0f, 4.0f);
    g.a = param (P::AmpA) * 0.001f;
    g.d = param (P::AmpD) * 0.001f;
    g.s = param (P::AmpS);
    g.fade = param (P::AmpFade);
    g.r = param (P::AmpR) * 0.001f;
    g.curve = param (P::AmpCurve);

    const double sampleSeconds = sample != nullptr ? sample->length / sample->sampleRate : 1.0;
    g.startSeconds = sample != nullptr ? sample->startPosition (param (P::StartPos)) / sample->sampleRate : 0.0;

    if (useFrozenView && frozenView > 0.0)
    {
        g.viewSeconds = frozenView;
        g.holdSeconds = frozenHold;
    }
    else if ((int) param (P::PlayMode) == P::PlayLoop)
    {
        // LOOP: the sample, end to end, whatever Envolvente says (a long Release of a preset made room for an
        // envelope LOOP does not draw, and the loop shrank to part of the width).
        g.holdSeconds = 0.15;
        g.viewSeconds = sampleSeconds;
    }
    else
    {
        g.holdSeconds = juce::jlimit (0.15, 4.0, (sampleSeconds - g.startSeconds - g.a - g.d) * 0.6);
        const double total = g.startSeconds + g.a + g.d + g.holdSeconds + g.r;
        g.viewSeconds = std::max (sampleSeconds, total) * 1.04;
    }

    g.shown = g.viewSeconds / zoom;
    g.first = juce::jlimit (0.0, g.viewSeconds - g.shown, viewFirst);
    return g;
}

// The tab at the top of the start line: the obvious place to grab it.
juce::Rectangle<float> WaveformView::startTab (const Geometry& g) const
{
    return { g.xFor (g.startSeconds) - 1.0f, g.area.getY(), 58.0f, 16.0f };
}

juce::Point<float> WaveformView::handlePos (const Geometry& g, int handle) const
{
    const AdsrShape user { g.a, g.curve, g.d, g.s, g.fade, g.r };

    switch (handle)
    {
        case HAttack:  return { g.xFor (g.startSeconds + g.a), g.yFor (1.0f) };
        case HDecay:   return { g.xFor (g.startSeconds + g.a + g.d), g.yFor (user.heldLevel (g.a + g.d)) };
        case HFade:    return { g.xFor (g.startSeconds + g.a + g.d + g.holdSeconds), g.yFor (user.heldLevel (g.a + g.d + g.holdSeconds)) };
        case HRelease: return { g.xFor (g.startSeconds + g.a + g.d + g.holdSeconds + g.r), g.yFor (0.0f) };
        default:       return {};
    }
}

bool WaveformView::slicing() const
{
    return sample != nullptr && (int) proc.paramValue (P::PlayMode) == P::PlayLoop && (int) proc.paramValue (P::LoopKind) == P::LoopSlice;
}

int WaveformView::sliceAt (juce::Point<float> p) const
{
    const auto g = geometry (false);
    if (! g.area.contains (p))
        return -1;
    const auto marks = proc.getSlices();
    int best = -1;
    float nearest = (float) markerGrab + 1.0f;
    for (size_t i = 1; i < marks.size(); ++i)
    {
        const float d = std::abs (g.xFor (marks[i] / sample->sampleRate) - p.x);
        if (d < nearest)
        {
            nearest = d;
            best = (int) i;
        }
    }
    return best;
}

// A marker near a detected hit (within a few pixels at this zoom) goes to where the hit begins.
int WaveformView::snapToHit (int at, const Geometry& g) const
{
    int best = at;
    float nearest = magnetPixels * 2.0f;
    for (const auto& h : sample->loopInfo.hits)
    {
        const float d = std::abs (g.xFor (h.first / sample->sampleRate) - g.xFor (at / sample->sampleRate));
        if (d < nearest)
        {
            nearest = d;
            best = h.first;
        }
    }
    return best;
}

int WaveformView::handleAt (juce::Point<float> p) const
{
    if (slicing())
    {
        const_cast<WaveformView*> (this)->hoverSlice = sliceAt (p);
        return hoverSlice >= 0 ? HSlice : -1;
    }
    if (sample == nullptr || (int) proc.paramValue (P::PlayMode) == P::PlayLoop)     // LOOP: no ADSR handles
        return -1;

    const auto g = geometry (false);
    for (int h = NumHandles - 1; h >= 0; --h)
    {
        const auto pos = handlePos (g, h);
        if (g.visible (pos.x) && pos.getDistanceFrom (p) < handleRadius)
            return h;
    }

    const float x = g.xFor (g.startSeconds);
    if (g.visible (x) && (startTab (g).contains (p) || (std::abs (p.x - x) < markerGrab && p.y >= g.area.getY() && p.y <= g.area.getBottom())))
        return HStart;
    if (zoom > 1.0f && g.area.contains (p))
        return HPan;
    return -1;
}

void WaveformView::setHover (int h)
{
    if (h != hover)
    {
        hover = h;
        setMouseCursor (h == HStart ? juce::MouseCursor::LeftRightResizeCursor
                      : h == HPan   ? juce::MouseCursor::DraggingHandCursor
                      : h >= 0      ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

juce::String WaveformView::getTooltip()
{
    switch (hover)
    {
        case HAttack:  return P::u8 ("Ataque: arrastra en horizontal.");
        case HDecay:   return P::u8 ("Decay en horizontal, Sustain en vertical.");
        case HFade:    return P::u8 ("Caída del sustain: arrastra en vertical. No se aplica en modo Sostenido.");
        case HRelease: return P::u8 ("Release: arrastra en horizontal.");
        case HStart:   return P::u8 ("Inicio: arrástralo para elegir dónde empieza la nota; se pega al inicio detectado. Con Mayús, ajuste fino. "
                                     "Doble clic: vuelve al inicio detectado.");
        case HPan:     return P::u8 ("Arrastra para desplazarte. Rueda: zoom. Doble clic: pone aquí el inicio de la nota.");
        case HSlice:   return P::u8 ("Corte de Slice: arrástralo para moverlo (se pega a los golpes; con Mayús, libre); doble clic lo quita. "
                                     "Cada trozo suena en la tecla que lleva escrita.");
        default:       return sample == nullptr ? juce::String()
                            : slicing() ? P::u8 ("Slice: doble clic añade un corte aquí. Rueda: zoom.")
                                        : P::u8 ("Rueda: zoom. Doble clic: pone aquí el inicio de la nota.");
    }
}

void WaveformView::setGesture (int handle, bool starting)
{
    auto notify = [starting, this] (int id)
    {
        if (auto* p = proc.apvts.getParameter (P::defs()[(size_t) id].id))
        {
            if (starting)
                p->beginChangeGesture();
            else
                p->endChangeGesture();
        }
    };

    switch (handle)
    {
        case HAttack:  notify (P::AmpA); break;
        case HDecay:   notify (P::AmpD); notify (P::AmpS); break;
        case HFade:    notify (P::AmpFade); break;
        case HRelease: notify (P::AmpR); break;
        case HStart:   notify (P::StartPos); break;
        default:       break;
    }
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    setHover (handleAt (e.position));
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    dragging = handleAt (e.position);
    if (dragging == HSlice)
    {
        dragSlice = hoverSlice;
        dragMarks = proc.getSlices();
        frozenView = geometry (false).viewSeconds;
        return;
    }
    if (dragging < 0)
        return;

    const auto g = geometry (false);
    frozenView = g.viewSeconds;
    frozenHold = g.holdSeconds;
    dragStartValue = param (P::StartPos);
    panFrom = g.first;
    setGesture (dragging, true);
}

void WaveformView::setStartFromMouse (const juce::MouseEvent& e, const Geometry& g)
{
    const double fileSeconds = sample->length / sample->sampleRate;
    const float pixelsPerFile = (float) (fileSeconds / g.shown) * g.area.getWidth();

    if (e.mods.isShiftDown())
    {
        setParam (P::StartPos, dragStartValue + 0.1f * (float) e.getDistanceFromDragStartX() / pixelsPerFile);
        return;
    }

    const double wanted = g.secondsFor (e.position.x);
    const double onset = sample->onset / sample->sampleRate;
    const bool nearOnset = std::abs (g.xFor (wanted) - g.xFor (onset)) < magnetPixels;
    setParam (P::StartPos, (float) ((nearOnset ? onset : wanted) / fileSeconds));
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0)
        return;

    const auto g = geometry (true);
    if (dragging == HSlice)
    {
        if (dragSlice <= 0 || dragSlice >= (int) dragMarks.size())
            return;
        // Between its neighbours, at least 10 ms from each.
        const int gap = (int) (0.01 * sample->sampleRate);
        const int length = sample->loopInfo.length > 0 ? std::min (sample->loopInfo.length, sample->length) : sample->length;
        const int lo = dragMarks[(size_t) dragSlice - 1] + gap;
        const int hi = (dragSlice + 1 < (int) dragMarks.size() ? dragMarks[(size_t) dragSlice + 1] : length) - gap;
        if (hi <= lo)
            return;
        int at = (int) std::lround (g.secondsFor (e.position.x) * sample->sampleRate);
        if (! e.mods.isShiftDown())
            at = snapToHit (at, g);
        dragMarks[(size_t) dragSlice] = juce::jlimit (lo, hi, at);
        proc.setSliceMarks (dragMarks);
        repaint();
        return;
    }
    const double t = g.secondsFor (e.position.x) - g.startSeconds;
    const float level = g.levelFor (e.position.y);

    switch (dragging)
    {
        case HAttack:
            setParam (P::AmpA, (float) juce::jlimit (0.0001, 20.0, t) * 1000.0f);
            break;

        case HDecay:
            setParam (P::AmpD, (float) juce::jlimit (0.001, 30.0, t - g.a) * 1000.0f);
            setParam (P::AmpS, level);
            break;

        case HFade:
        {
            const float fade = g.s > 0.0f ? -20.0f * std::log10 (std::max (level, 1.0e-4f) / g.s) / (float) g.holdSeconds : 0.0f;
            setParam (P::AmpFade, juce::jlimit (0.0f, 120.0f, fade));
            break;
        }

        case HRelease:
            setParam (P::AmpR, (float) juce::jlimit (0.001, 30.0, t - g.a - g.d - g.holdSeconds) * 1000.0f);
            break;

        case HStart:
            setStartFromMouse (e, g);
            break;

        case HPan:
            viewFirst = panFrom - e.getDistanceFromDragStartX() / g.area.getWidth() * g.shown;
            break;

        default: break;
    }

    repaint();
}

void WaveformView::mouseUp (const juce::MouseEvent&)
{
    if (dragging == HSlice)
    {
        dragging = dragSlice = -1;
        frozenView = -1.0;
        repaint();
        return;
    }
    setGesture (dragging, false);
    viewFirst = geometry (true).first;
    dragging = -1;
    frozenView = -1.0;
    overview.clear();
    repaint();
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (slicing())
    {
        // On a marker: it goes. Elsewhere on the loop: a new one. Either way the cuts are the user's now.
        auto marks = proc.getSlices();
        const int under = sliceAt (e.position);
        const auto g = geometry (false);
        const int length = sample->loopInfo.length > 0 ? std::min (sample->loopInfo.length, sample->length) : sample->length;
        if (under > 0)
            marks.erase (marks.begin() + under);
        else if (g.area.contains (e.position))
        {
            int at = (int) std::lround (g.secondsFor (e.position.x) * sample->sampleRate);
            if (! e.mods.isShiftDown())
                at = snapToHit (at, g);
            if (at > 0 && at < length && (int) marks.size() < P::maxSlices)
                marks.push_back (at);
        }
        proc.setSliceMarks (marks);
        hoverSlice = -1;
        repaint();
        return;
    }
    const int under = handleAt (e.position);
    if (sample == nullptr || (under >= 0 && under != HStart && under != HPan))
        return;

    const auto g = geometry (false);
    const double fileSeconds = sample->length / sample->sampleRate;
    const double chosen = under == HStart ? sample->onset / sample->sampleRate
                                          : juce::jlimit (0.0, fileSeconds, g.secondsFor (e.position.x));
    setGesture (HStart, true);
    setParam (P::StartPos, (float) (chosen / fileSeconds));
    setGesture (HStart, false);
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (sample == nullptr)
        return;

    const auto g = geometry (false);
    const bool sideways = e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY);
    if (sideways)
    {
        const float delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
        viewFirst = g.first - delta * g.shown * 0.5;
        repaint();
        return;
    }

    zoomAround (g.secondsFor (e.position.x), zoom * std::exp (wheel.deltaY * 2.5f));
}

void WaveformView::buildOverview (const SampleData& s, const Geometry& g)
{
    const int columns = std::max (1, (int) g.area.getWidth());
    overview.assign ((size_t) columns, { 0.0f, 0.0f });
    overviewFirst = g.first;
    overviewShown = g.shown;

    const float* l = s.audio.getReadPointer (0);
    const float* r = s.audio.getReadPointer (1);

    for (int c = 0; c < columns; ++c)
    {
        const int i0 = std::max (0, (int) ((g.first + (double) c / columns * g.shown) * s.sampleRate));
        const int i1 = std::min (s.length, (int) ((g.first + (double) (c + 1) / columns * g.shown) * s.sampleRate));
        float mn = 0.0f, mx = 0.0f;
        for (int i = i0; i < i1; ++i)
        {
            const float v = 0.5f * (l[i] + r[i]);
            mn = std::min (mn, v);
            mx = std::max (mx, v);
        }

        // Zoomed in further than one sample per column: the waveform itself, interpolated.
        if (i1 <= i0 && i0 < s.length)
        {
            const double at = (g.first + ((double) c + 0.5) / columns * g.shown) * s.sampleRate;
            const int i = std::min (s.length - 2, (int) at);
            const float frac = (float) (at - i);
            const float v = 0.5f * ((l[i] + frac * (l[i + 1] - l[i])) + (r[i] + frac * (r[i + 1] - r[i])));
            mn = std::min (0.0f, v);
            mx = std::max (0.0f, v);
        }
        overview[(size_t) c] = { mn, mx };
    }
}

void WaveformView::paintStart (juce::Graphics& gfx, const SampleData& s, const Geometry& g)
{
    const auto& area = g.area;
    const double sr = s.sampleRate;
    const float x = g.xFor (g.startSeconds);
    const bool active = hover == HStart || dragging == HStart;

    // What the note skips: darkened and hatched.
    if (x > area.getX())
    {
        const auto skipped = area.withRight (std::min (x, area.getRight()));
        gfx.setColour (Theme::well.withAlpha (0.7f));
        gfx.fillRect (skipped);
        juce::Graphics::ScopedSaveState state (gfx);
        gfx.reduceClipRegion (skipped.toNearestInt());
        gfx.setColour (Theme::accent.withAlpha (0.08f));
        for (float h = skipped.getX() - area.getHeight(); h < skipped.getRight(); h += 9.0f)
            gfx.drawLine (h, area.getBottom(), h + area.getHeight(), area.getY(), 1.0f);
    }

    // Where the sound itself starts, if the note begins elsewhere.
    const double onset = s.onset / sr;
    const float xOnset = g.xFor (onset);
    if (std::abs (xOnset - x) > 3.0f && g.visible (xOnset))
    {
        juce::Path line;
        line.startNewSubPath (xOnset, area.getY());
        line.lineTo (xOnset, area.getBottom());
        juce::Path dashed;
        const float dashes[] = { 3.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, line, dashes, 2);
        gfx.setColour (Theme::accent.withAlpha (0.5f));
        gfx.fillPath (dashed);
        gfx.setFont (Theme::font (10.0f));
        gfx.drawText ("detectado", (int) xOnset + 4, (int) area.getBottom() - 14, 70, 12, juce::Justification::centredLeft);
    }

    if (! g.visible (x))
        return;

    // The line, with a glow while it is being touched.
    if (active)
    {
        gfx.setColour (Theme::accent.withAlpha (0.18f));
        gfx.fillRect (x - 4.0f, area.getY(), 8.0f, area.getHeight());
    }
    gfx.setColour (Theme::accent);
    gfx.fillRect (x - 1.0f, area.getY(), 2.0f, area.getHeight());

    // Tab at the top, and the exact place at the bottom.
    const auto tab = startTab (g);
    gfx.setColour (active ? Theme::accent.brighter (0.25f) : Theme::accent);
    gfx.fillRoundedRectangle (tab, 3.0f);
    gfx.setColour (Theme::well);
    gfx.setFont (Theme::caps (10.0f));
    gfx.drawText ("INICIO", tab, juce::Justification::centred);

    juce::Path foot;
    foot.addTriangle (x - 5.0f, area.getBottom(), x + 5.0f, area.getBottom(), x, area.getBottom() - 6.0f);
    gfx.setColour (Theme::accent);
    gfx.fillPath (foot);

    // How far into the sound the note starts.
    const double ms = (g.startSeconds - onset) * 1000.0;
    if (std::abs (ms) >= 0.05 || active)
    {
        const juce::String text = std::abs (ms) < 0.05 ? juce::String ("en el inicio detectado")
                                                        : (ms > 0.0 ? "+" : "") + juce::String (ms, ms < 100.0 ? 1 : 0) + " ms";
        gfx.setFont (Theme::font (11.0f, true));
        const float w = (float) juce::GlyphArrangement::getStringWidthInt (Theme::font (11.0f, true), text) + 12.0f;
        const auto bubble = juce::Rectangle<float> (x + 6.0f, tab.getBottom() + 4.0f, w, 16.0f);
        gfx.setColour (Theme::panelTop.withAlpha (0.9f));
        gfx.fillRoundedRectangle (bubble, 3.0f);
        gfx.setColour (Theme::accent);
        gfx.drawText (text, bubble, juce::Justification::centred);
    }
}

void WaveformView::paint (juce::Graphics& gfx)
{
    const auto bounds = getLocalBounds().toFloat();
    gfx.setColour (Theme::well);
    gfx.fillRoundedRectangle (bounds, 6.0f);
    gfx.setColour (dropHighlight ? Theme::accent : Theme::panelEdge);
    gfx.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, dropHighlight ? 2.0f : 1.0f);

    if (sample == nullptr)
    {
        gfx.setColour (Theme::dim);
        gfx.setFont (Theme::font (16.0f));
        gfx.drawText (P::u8 ("Arrastra aquí un sample (WAV, AIFF, FLAC, MP3, OGG) o pulsa \"Cargar\""),
                      getLocalBounds().withTrimmedBottom (24), juce::Justification::centred);
        gfx.setFont (Theme::font (13.0f));
        gfx.drawText (P::u8 ("¿Primera vez? Pulsa ? (junto a Deshacer) para la guía rápida."),
                      getLocalBounds().withTrimmedTop (24), juce::Justification::centred);
        return;
    }

    const auto& s = *sample;
    const auto g = geometry (dragging >= 0);
    const auto& area = g.area;
    const double sr = s.sampleRate;

    {
        // Everything on the timeline stays inside it, whatever the zoom.
        juce::Graphics::ScopedSaveState state (gfx);
        gfx.reduceClipRegion (area.withBottom (area.getBottom() + 18.0f).expanded (1.0f, 0.0f).toNearestInt());

        // Time grid.
        {
            const double steps[] = { 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 30, 60 };
            double step = 60;
            for (const double st : steps)
                if (g.shown / st <= 12.0) { step = st; break; }

            gfx.setFont (Theme::font (10.0f));
            for (double t = std::ceil (g.first / step) * step; t <= g.first + g.shown; t += step)
            {
                const float x = g.xFor (t);
                gfx.setColour (Theme::panelEdge.withAlpha (0.6f));
                gfx.drawVerticalLine ((int) x, area.getY(), area.getBottom());
                gfx.setColour (Theme::dim);
                const juce::String label = step < 0.01 ? juce::String (t * 1000.0, 1) + " ms"
                                         : step < 1.0  ? juce::String (juce::roundToInt (t * 1000.0)) + " ms"
                                                       : juce::String (t, 1) + " s";
                gfx.drawText (label, (int) x + 2, (int) area.getBottom() + 2, 60, 14, juce::Justification::centredLeft);
            }
        }

        // Analysis zones.
        const int engineMode = (int) param (P::Engine);
        const int sustainMode = (int) param (P::SustainMode);
        const bool looping = (int) param (P::PlayMode) == P::PlayLoop;
        const auto& loop = s.loopInfo;
        const int chosenBeats = P::loopBeatChoices[(size_t) juce::jlimit (0, 12, (int) param (P::LoopBeats))];
        const int loopBeats = chosenBeats > 0 ? chosenBeats : loop.beats;
        const double loopSeconds = (loop.length > 0 ? loop.length : s.length) / sr;

        if (looping)
        {
            // The beats (bars brighter) over the loop, the cuts of Beats along the bottom.
            gfx.setColour (Theme::good.withAlpha (0.06f));
            gfx.fillRect (g.xFor (0.0), area.getY(), std::max (1.0f, g.xFor (loopSeconds) - g.xFor (0.0)), area.getHeight());
            for (int b = 0; b <= loopBeats && loopBeats > 0; ++b)
            {
                gfx.setColour (Theme::good.withAlpha (b % 4 == 0 ? 0.55f : 0.22f));
                gfx.drawVerticalLine ((int) g.xFor (b * loopSeconds / loopBeats), area.getY(), area.getBottom());
            }
            if (slicing())
            {
                // SLICE: each slice shaded by turns, its marker and the key that plays it.
                const auto marks = proc.getSlices();
                const auto order = proc.getSliceOrder();
                std::vector<int> keyOf (marks.size(), 0);
                for (size_t k = 0; k < order.size() && k < marks.size(); ++k)
                    if (order[k] >= 0 && order[k] < (int) marks.size())
                        keyOf[(size_t) order[k]] = (int) k;
                const int first = juce::roundToInt (param (P::SliceStart));
                static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
                gfx.setFont (Theme::font (10.0f, true));
                for (size_t i = 0; i < marks.size(); ++i)
                {
                    const double from = marks[i] / sr, to = i + 1 < marks.size() ? marks[i + 1] / sr : loopSeconds;
                    const float x0 = g.xFor (from), x1 = g.xFor (to);
                    if (x1 < area.getX() || x0 > area.getRight())
                        continue;
                    if (i % 2 == 1)
                    {
                        gfx.setColour (Theme::accent2.withAlpha (0.07f));
                        gfx.fillRect (x0, area.getY(), x1 - x0, area.getHeight());
                    }
                    const bool hot = (int) i == hoverSlice || (int) i == dragSlice;
                    gfx.setColour (Theme::accent2.withAlpha (hot ? 1.0f : 0.75f));
                    gfx.fillRect (x0 - (hot ? 1.0f : 0.5f), area.getY(), hot ? 2.0f : 1.0f, area.getHeight());
                    const int key = first + (i < keyOf.size() ? keyOf[i] : (int) i);
                    if (x1 - x0 > 18.0f && key <= 127)
                        gfx.drawText (juce::String (names[key % 12]) + juce::String (key / 12 - 2), (int) x0 + 3, (int) area.getY() + 3,
                                      (int) std::min (60.0f, x1 - x0 - 4.0f), 12, juce::Justification::centredLeft);
                }
            }
            else
            {
                // The cuts: Beats's, or with Por golpe the hits the envelopes start again on (taller).
                const bool hits = param (P::LoopHitEnv) > 0.5f;
                gfx.setColour (Theme::accent2.withAlpha (hits ? 0.95f : 0.8f));
                for (const int cut : hits ? proc.getSlices() : loop.slices)
                    gfx.fillRect (g.xFor (cut / sr) - 0.5f, area.getBottom() - (hits ? 14.0f : 6.0f), 1.0f, hits ? 14.0f : 6.0f);
            }
            gfx.setColour (Theme::good);
            gfx.setFont (Theme::font (10.0f, true));
            juce::String label ("LOOP");
            if (loopBeats > 0)
                label << P::u8 (" · ") << juce::String (loopBeats) << " TIEMPOS · " << juce::String (loopBeats * 60.0 / loopSeconds, 1) << " BPM";
            gfx.drawText (label, (int) g.xFor (0.0) + 3, (int) area.getBottom() - 20, 260, 12, juce::Justification::centredLeft);
        }
        else if (loop.beats > 0 && loop.confidence >= 0.4f)
        {
            gfx.setColour (Theme::good.withAlpha (0.85f));
            gfx.setFont (Theme::font (10.0f, true));
            gfx.drawText (P::u8 ("¿LOOP? ") + juce::String (loop.beats) + P::u8 (" tiempos a ") + juce::String (loop.bpm, 1)
                              + P::u8 (" BPM · pulsa ONE SHOT en MOTOR"),
                          (int) area.getX() + 6, (int) area.getY() + 4, 340, 12, juce::Justification::centredLeft);
        }

        if (looping)
        {
        }
        else if (engineMode == P::EngineHybrid)
        {
            const float x0 = g.xFor (g.startSeconds), x1 = g.xFor (g.startSeconds + param (P::TransientMs) * 0.001);
            gfx.setColour (Theme::accent2.withAlpha (0.12f));
            gfx.fillRect (x0, area.getY(), std::max (1.0f, x1 - x0), area.getHeight());
            gfx.setColour (Theme::accent2);
            gfx.setFont (Theme::font (10.0f, true));
            gfx.drawText ("TRANS", (int) x0 + 2, (int) area.getBottom() - 14, 50, 12, juce::Justification::centredLeft);
        }

        if (! looping && engineMode == P::EngineDrone)
        {
            // Where the drone reads, and how much of the sound each of its frames hears.
            const double centre = s.soundingPosition (param (P::GrainPos)) / sr;
            const double frame = std::ldexp (1.0, Drone::orderFor ((int) param (P::DroneSize), 48000.0)) / 48000.0;
            const float x0 = g.xFor (centre - frame * 0.5), x1 = g.xFor (centre + frame * 0.5);
            gfx.setColour (Theme::good.withAlpha (0.14f));
            gfx.fillRect (x0, area.getY(), std::max (2.0f, x1 - x0), area.getHeight());
            gfx.setColour (Theme::good);
            gfx.fillRect (g.xFor (centre) - 1.0f, area.getY(), 2.0f, area.getHeight());
            gfx.setFont (Theme::font (10.0f, true));
            gfx.drawText (param (P::DroneFreeze) > 0.5f ? juce::String ("DRONE · CONGELADO")
                                                        : "DRONE · x" + juce::String (juce::roundToInt (param (P::DroneStretch))),
                          (int) x0 + 3, (int) area.getBottom() - 14, 160, 12, juce::Justification::centredLeft);
        }
        else if (looping)
        {
        }
        else if (engineMode == P::EngineGranular)
        {
            const double centre = s.soundingPosition (param (P::GrainPos)) / sr, spray = param (P::Spray) * 0.25;
            const float x0 = g.xFor (centre - spray), x1 = g.xFor (centre + spray + param (P::GrainMs) * 0.001);
            gfx.setColour (Theme::good.withAlpha (0.14f));
            gfx.fillRect (x0, area.getY(), std::max (2.0f, x1 - x0), area.getHeight());
            gfx.setColour (Theme::good);
            gfx.fillRect (g.xFor (centre) - 1.0f, area.getY(), 2.0f, area.getHeight());
            gfx.setFont (Theme::font (10.0f, true));
            gfx.drawText ("GRANULAR", (int) x0 + 3, (int) area.getBottom() - 14, 100, 12, juce::Justification::centredLeft);
        }
        else
        {
            // The region that keeps the note alive: at the sustain point (Sostenido) or
            // at the end of the sample (Natural). Engine C holds at the sustain point.
            const bool hold = (int) param (P::SustainType) == P::SustainHold;
            const auto& src = hold ? s.early : s.late;
            const char* names[] = { "LOOP", "GRANULAR", "FREEZE" };
            const bool rebuild = engineMode == P::EngineRebuild;
            const float x0 = g.xFor ((rebuild ? s.bedStart : src.start) / sr), x1 = g.xFor ((rebuild ? s.bedEnd : src.end) / sr);
            gfx.setColour (Theme::good.withAlpha (0.10f));
            gfx.fillRect (x0, area.getY(), std::max (1.0f, x1 - x0), area.getHeight());
            gfx.setColour (Theme::good.withAlpha (0.7f));
            gfx.drawVerticalLine ((int) x0, area.getY(), area.getBottom());
            gfx.drawVerticalLine ((int) x1, area.getY(), area.getBottom());
            gfx.setFont (Theme::font (10.0f, true));
            gfx.drawText ((rebuild ? P::u8 ("RECONSTRUCCIÓN") : juce::String (names[juce::jlimit (0, 2, sustainMode)]))
                              + (hold ? P::u8 (" · SOSTENIDO") : P::u8 (" · NATURAL")),
                          (int) x0 + 3, (int) area.getBottom() - 14, 160, 12, juce::Justification::centredLeft);
        }

        // Waveform, scaled to the loudest moment of the whole sample at any zoom.
        if (overview.size() != (size_t) std::max (1, (int) area.getWidth()) || overviewFirst != g.first || overviewShown != g.shown)
            buildOverview (s, g);

        {
            const float mid = area.getCentreY();
            const float scale = area.getHeight() * 0.48f / samplePeak;
            gfx.setColour (Theme::accent.withAlpha (0.35f));
            for (size_t c = 0; c < overview.size(); ++c)
            {
                const float x = area.getX() + (float) c;
                if (g.secondsFor (x) * sr > s.length)
                    break;
                gfx.drawVerticalLine ((int) x, mid - overview[c].second * scale, mid - overview[c].first * scale + 1.0f);
            }
        }

        // What the note will look like with the ADSR, in engines A to D: Remodelar keeps the ups and
        // downs of the sample under the curve; Sintetizador flattens them and only the ADSR shapes it.
        if (! looping && engineMode <= P::EngineHybrid)
        {
            const bool synthMode = (int) param (P::EnvMode) == P::EnvSynth;
            const AdsrShape user { g.a, g.curve, g.d, g.s, g.fade, g.r };
            const double holdEnd = g.a + g.d + g.holdSeconds;
            const float mid = area.getCentreY(), scale = area.getHeight() * 0.48f / samplePeak;
            std::vector<juce::Point<float>> edge;

            for (size_t c = 0; c < overview.size(); ++c)
            {
                const float x = area.getX() + (float) c;
                const double seconds = g.secondsFor (x), t = seconds - g.startSeconds;
                if (t < 0.0)
                    continue;
                if (t > holdEnd + g.r)
                    break;
                const float level = t <= holdEnd ? user.heldLevel (t) : user.heldLevel (holdEnd) * std::exp (-6.9f * (float) ((t - holdEnd) / g.r));
                const double pos = seconds * sr;
                const float peak = pos < s.length ? std::max (std::abs (overview[c].first), std::abs (overview[c].second)) : 0.0f;
                float amp;
                const float bedAmp = 0.7f * samplePeak * level;
                if (pos >= s.usableEnd)
                    amp = engineMode == P::EngineRebuild || synthMode ? bedAmp : 0.0f;     // the reconstruction goes on
                else if (synthMode)
                    amp = peak / std::max (0.05f, s.envAt (pos)) * level;
                else
                {
                    const float model = std::max (1.0e-3f, s.detected.heldLevel (seconds - s.onset / sr));
                    const float gain = std::min (4.0f, level / model);
                    amp = peak * gain;
                    if (engineMode == P::EngineRebuild)
                    {
                        // As engine A does it: the reconstruction takes over where the sample falls
                        // short of the curve, and before the sample runs out, so the shape goes on
                        // without a step.
                        const double endingLength = std::max (1.0, std::min (0.15 * sr, (s.usableEnd - s.onset) / 3.0));
                        const float ending = juce::jlimit (0.0f, 1.0f, (float) ((pos - (s.usableEnd - endingLength)) / endingLength));
                        const float shortfall = std::log2 (std::max (level, 1.0e-6f) / std::max (1.0e-6f, s.envAt (pos) * gain));
                        const float excess = std::log2 (std::max (level, 1.0e-6f) / model);
                        const float takeover = std::max ({ ending, juce::jlimit (0.0f, 1.0f, (excess - 0.3f) / 1.3f),
                                                           juce::jlimit (0.0f, 1.0f, (shortfall - 0.15f) / 1.0f) });
                        const float smooth = takeover * takeover * (3.0f - 2.0f * takeover);
                        const float wOriginal = std::cos (smooth * 1.5708f), wBed = std::sin (smooth * 1.5708f);
                        amp = std::sqrt (amp * amp * wOriginal * wOriginal + bedAmp * bedAmp * wBed * wBed);
                    }
                }
                edge.push_back ({ x, std::min (amp, 1.2f * samplePeak) * scale });
            }

            if (edge.size() > 1)
            {
                juce::Path shape;
                shape.startNewSubPath (edge.front().x, mid - edge.front().y);
                for (const auto& e : edge)
                    shape.lineTo (e.x, mid - e.y);
                for (auto it = edge.rbegin(); it != edge.rend(); ++it)
                    shape.lineTo (it->x, mid + it->y);
                shape.closeSubPath();
                gfx.setColour (Theme::accent2.withAlpha (0.16f));
                gfx.fillPath (shape);
                gfx.setColour (Theme::accent2.withAlpha (0.45f));
                gfx.strokePath (shape, juce::PathStrokeType (0.8f));
            }

            gfx.setColour (Theme::accent2.withAlpha (0.8f));
            gfx.setFont (Theme::font (10.0f, true));
            gfx.drawText (synthMode ? P::u8 ("RESULTADO · SINTETIZADOR: el sample queda plano y sólo manda tu ADSR")
                                    : P::u8 ("RESULTADO · REMODELAR: tu ADSR sobre la forma propia del sample"),
                          (int) area.getX() + 4, (int) area.getY() + 2, (int) area.getWidth() - 8, 12, juce::Justification::centredRight);
        }

        // Measured envelope.
        {
            juce::Path env;
            for (int c = 0; c < (int) area.getWidth(); c += 2)
            {
                const double pos = g.secondsFor (area.getX() + (float) c) * sr;
                if (pos > s.length)
                    break;
                const auto pt = juce::Point<float> (area.getX() + (float) c, g.yFor (s.envAt (pos)));
                if (c == 0) env.startNewSubPath (pt); else env.lineTo (pt);
            }
            gfx.setColour (Theme::text.withAlpha (0.45f));
            gfx.strokePath (env, juce::PathStrokeType (1.0f));
        }

        // Detected model (dashed).
        {
            juce::Path model;
            const double onsetSeconds = s.onset / sr;
            const double end = s.length / sr - onsetSeconds;
            for (int i = 0; i <= 400; ++i)
            {
                const double t = end * i / 400.0;
                const auto pt = juce::Point<float> (g.xFor (onsetSeconds + t), g.yFor (s.detected.heldLevel (t)));
                if (i == 0) model.startNewSubPath (pt); else model.lineTo (pt);
            }
            juce::Path dashed;
            const float dashes[] = { 4.0f, 4.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, model, dashes, 2);
            gfx.setColour (Theme::accent2.withAlpha (0.45f));
            gfx.fillPath (dashed);
        }

        // User ADSR (not in LOOP: there it is only a gate, its fades on the panel).
        if (! looping)
        {
            const AdsrShape user { g.a, g.curve, g.d, g.s, g.fade, g.r };
            const double holdEnd = g.a + g.d + g.holdSeconds;
            const float holdLevel = user.heldLevel (holdEnd);

            juce::Path path;
            path.startNewSubPath (g.xFor (g.startSeconds), g.yFor (0.0f));
            for (int i = 1; i <= 60; ++i)
            {
                const double t = g.a * i / 60.0;
                path.lineTo (g.xFor (g.startSeconds + t), g.yFor (user.heldLevel (t)));
            }
            for (int i = 1; i <= 120; ++i)
            {
                const double t = g.a + (g.d + g.holdSeconds) * i / 120.0;
                path.lineTo (g.xFor (g.startSeconds + t), g.yFor (user.heldLevel (t)));
            }
            for (int i = 1; i <= 60; ++i)
            {
                const double tr = g.r * i / 60.0;
                path.lineTo (g.xFor (g.startSeconds + holdEnd + tr), g.yFor (holdLevel * std::exp (-6.9f * (float) (tr / g.r))));
            }

            auto fill = path;
            fill.lineTo (g.xFor (g.startSeconds + holdEnd + g.r), g.yFor (0.0f));
            fill.closeSubPath();
            gfx.setColour (Theme::accent2.withAlpha (0.08f));
            gfx.fillPath (fill);
            gfx.setColour (Theme::accent2);
            gfx.strokePath (path, juce::PathStrokeType (2.0f));

            // Note-off marker.
            const float xOff = g.xFor (g.startSeconds + holdEnd);
            gfx.setColour (Theme::accent2.withAlpha (0.35f));
            gfx.drawVerticalLine ((int) xOff, area.getY(), area.getBottom());

            for (int h = 0; h < NumHandles; ++h)
            {
                const auto pos = handlePos (g, h);
                const float rad = (h == hover || h == dragging) ? 7.0f : 5.0f;
                gfx.setColour (Theme::accent2);
                gfx.fillEllipse (juce::Rectangle<float> (rad * 2, rad * 2).withCentre (pos));
                gfx.setColour (juce::Colours::white);
                gfx.drawEllipse (juce::Rectangle<float> (rad * 2, rad * 2).withCentre (pos), 1.2f);
            }
        }

        if (! slicing())        // SLICE plays from its markers, not from Inicio
            paintStart (gfx, s, g);
    }

    // Where the visible part sits in the whole sample.
    if (zoom > 1.0f)
    {
        const auto track = juce::Rectangle<float> (area.getX(), (float) getHeight() - 4.0f, area.getWidth(), 2.0f);
        gfx.setColour (Theme::panelEdge);
        gfx.fillRect (track);
        gfx.setColour (Theme::accent);
        gfx.fillRect (track.getX() + (float) (g.first / g.viewSeconds) * track.getWidth(), track.getY(),
                      std::max (4.0f, (float) (g.shown / g.viewSeconds) * track.getWidth()), track.getHeight());
    }

    // Header text.
    gfx.setFont (Theme::font (12.0f));
    int x = zoomAll.getRight() + 10;
    gfx.setColour (Theme::dim);
    if (zoom > 1.0f)
    {
        gfx.drawText (juce::String::formatted ("x%.1f", (double) zoom), x, 5, 60, 16, juce::Justification::centredLeft);
        x += 60;
    }
    gfx.drawText (juce::String ((double) s.length / sr, 2) + P::u8 (" s  ·  ") + juce::String (sr / 1000.0, 1) + " kHz"
                      + (s.reversed ? "  ·  Reverse" : ""), x, 5, 260, 16, juce::Justification::centredLeft);

    const auto& d = s.detected;
    auto ms = [] (float sec)
    {
        return sec >= 1.0f ? juce::String (sec, 2) + " s"
             : sec < 0.01f ? juce::String (sec * 1000.0f, 1) + " ms"
                           : juce::String (juce::roundToInt (sec * 1000.0f)) + " ms";
    };
    gfx.setColour (Theme::dim);
    gfx.drawText (P::u8 ("Detectado → A ") + ms (d.attack) + "  D " + ms (d.decay)
                      + "  S " + juce::String (juce::roundToInt (d.sustain * 100.0f)) + " %"
                      + P::u8 ("  Caída ") + juce::String (d.fadeDbPerSec, 1) + " dB/s  R " + ms (d.release),
                  getWidth() - 520, 5, 508, 16, juce::Justification::centredRight);
}
