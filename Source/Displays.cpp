#include "Displays.h"

// The colour of the module (section) a display sits in: its curve in the same colour as its knobs.
static juce::Colour moduleColour (const juce::Component& c, juce::Colour fallback)
{
    if (auto* section = c.findParentComponentOfClass<Section>())
        return section->getAccent();
    return fallback;
}
#include "Mechanism.h"

namespace
{
void drawDisplay (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (Theme::well);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (Theme::panelEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
}

void drawCurve (juce::Graphics& g, const juce::Path& line, juce::Rectangle<float> area, juce::Colour colour)
{
    juce::Path fill (line);
    fill.lineTo (line.getCurrentPosition().x, area.getBottom());
    fill.lineTo (area.getX(), area.getBottom());
    fill.closeSubPath();
    g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.28f), 0.0f, area.getY(), colour.withAlpha (0.03f), 0.0f, area.getBottom(), false));
    g.fillPath (fill);
    g.setColour (colour);
    g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}
}

//==============================================================================
void ParamView::update()
{
    bool changed = false;
    for (size_t i = 0; i < watched.size(); ++i)
    {
        const float v = proc.paramValue (watched[i]);
        changed = changed || v != last[i];
        last[i] = v;
    }

    if (changed)
    {
        refresh();
        repaint();
    }
}

//==============================================================================
OutputMeter::OutputMeter()
{
    setTooltip (P::u8 ("Nivel antes de la etapa de salida. Hasta 0 dB el sonido sale limpio; de 0 a +6 dB (zona roja) entra en una "
                       "saturación suave que añade armónicos y densidad poco a poco, sin pasar nunca de 0 dBFS a la salida. La barra "
                       "sólida es el nivel medio (VU), la clara el pico y la línea el último pico. Abajo, el pico más alto: clic para ponerlo a cero."));
    setOpaque (false);
}

juce::Rectangle<float> OutputMeter::barArea (int channel) const
{
    constexpr float barWidth = 15.0f, top = 13.0f, bottom = 20.0f;
    const float x = channel == 0 ? 0.0f : (float) getWidth() - barWidth;
    return { x, top, barWidth, (float) getHeight() - top - bottom };
}

float OutputMeter::yFor (float db) const
{
    const auto bar = barArea (0);
    return bar.getBottom() - (juce::jlimit (minDb, maxDb, db) - minDb) / (maxDb - minDb) * bar.getHeight();
}

void OutputMeter::update (const ReanimaProcessor::MeterReading& reading)
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float seconds = juce::jlimit (0.0f, 0.1f, (float) (now - lastUpdate) * 0.001f);
    lastUpdate = now;

    // A VU meter reaches 99 % of a step in 300 ms.
    constexpr float vuTime = 0.065f, fallDbPerSecond = 26.0f, holdTime = 1.5f;
    const float vuStep = 1.0f - std::exp (-seconds / vuTime);

    juce::String state;
    for (size_t ch = 0; ch < 2; ++ch)
    {
        auto& c = levels[ch];
        const float peak = juce::Decibels::gainToDecibels (reading.peak[ch], minDb);

        c.rms += (std::sqrt (reading.meanSquare[ch]) - c.rms) * vuStep;
        c.vu = juce::Decibels::gainToDecibels (c.rms, minDb);
        c.peak = std::max (peak, c.peak - fallDbPerSecond * seconds);

        if (peak >= c.hold)
        {
            c.hold = peak;
            c.holdSeconds = 0.0f;
        }
        else if ((c.holdSeconds += seconds) > holdTime)
        {
            c.hold = std::max (minDb, c.hold - fallDbPerSecond * seconds);
        }

        c.clipped = c.clipped || reading.peak[ch] >= 1.0f;
        highest = std::max (highest, peak);

        state << juce::roundToInt (yFor (c.vu)) << " " << juce::roundToInt (yFor (c.peak)) << " "
              << juce::roundToInt (yFor (c.hold)) << " " << (int) c.clipped << " ";
    }

    state << juce::roundToInt (highest * 10.0f);
    if (state != drawnState)
    {
        drawnState = state;
        repaint();
    }
}

void OutputMeter::mouseDown (const juce::MouseEvent&)
{
    highest = minDb;
    for (auto& c : levels)
        c.clipped = false;
    drawnState.clear();
    repaint();
}

void OutputMeter::paint (juce::Graphics& g)
{
    const auto top = barArea (0).getY(), bottom = barArea (0).getBottom();

    juce::ColourGradient colours (Theme::good, 0.0f, bottom, Theme::danger, 0.0f, top, false);
    const auto stop = [&] (float db, juce::Colour c) { colours.addColour ((double) ((yFor (db) - bottom) / (top - bottom)), c); };
    stop (-12.0f, Theme::good);
    stop (-4.0f, Theme::warning);
    stop (-0.1f, Theme::accent2);
    stop (0.0f, Theme::danger);

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto& c = levels[(size_t) ch];
        const auto bar = barArea (ch);

        g.setColour (Theme::well);
        g.fillRoundedRectangle (bar, 2.5f);

        g.setGradientFill (colours);
        g.setOpacity (0.3f);
        g.fillRect (bar.withTop (yFor (c.peak)));
        g.setOpacity (1.0f);
        g.fillRect (bar.withTop (yFor (c.vu)));

        // The segments of the bar.
        g.setColour (Theme::well);
        for (float y = bar.getBottom() - 3.0f; y > bar.getY(); y -= 4.0f)
            g.fillRect (bar.getX(), y, bar.getWidth(), 1.0f);

        if (c.hold > minDb)
        {
            g.setGradientFill (colours);
            g.fillRect (bar.getX(), yFor (c.hold) - 1.0f, bar.getWidth(), 2.0f);
        }

        g.setColour (c.clipped ? Theme::danger : Theme::well);
        g.fillRoundedRectangle (bar.getX(), 2.0f, bar.getWidth(), 7.0f, 2.0f);
    }

    g.setFont (Theme::font (10.0f));
    const float left = barArea (0).getRight(), width = barArea (1).getX() - left;
    for (const int db : { 6, 0, -6, -12, -18, -24, -36, -48, -60 })
    {
        g.setColour (db == 0 ? Theme::text : Theme::dim);
        g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<float> (left, yFor ((float) db) - 6.0f, width, 12.0f),
                    juce::Justification::centred);
    }

    g.setColour (highest >= 0.0f ? Theme::danger : Theme::text);
    g.setFont (Theme::font (12.5f, true));
    g.drawText (highest <= minDb ? P::u8 ("-∞ dB") : juce::String (highest, 1) + " dB",
                getLocalBounds().removeFromBottom (17), juce::Justification::centred);
}

//==============================================================================
FilterCurve::FilterCurve (ReanimaProcessor& p)
    : ParamView (p, { P::FltOn, P::FltType, P::FltSlope, P::FltCircuit, P::FltFreq, P::FltRes, P::FltMorph, P::FltMix })
{
    setTooltip (P::u8 ("Respuesta real del filtro con los ajustes actuales, sin modulación, de 20 Hz a 20 kHz."));
    filter.prepare (rate);
}

void FilterCurve::measure()
{
    filter.reset();
    filter.set ((int) value (P::FltType), (int) value (P::FltSlope), (int) value (P::FltCircuit), value (P::FltFreq),
                value (P::FltRes), 0.0f, value (P::FltMorph), value (P::FltMix));

    // Small enough for the circuits to behave linearly.
    constexpr float impulse = 0.01f;
    std::fill (work.begin(), work.end(), 0.0f);
    for (int i = 0; i < fftSize; ++i)
    {
        float l = i == 0 ? impulse : 0.0f, r = l;
        filter.process (l, r);
        work[(size_t) i] = l / impulse;
    }
    fft.performFrequencyOnlyForwardTransform (work.data());

    for (int i = 0; i < numPoints; ++i)
    {
        const double hz = 20.0 * std::pow (1000.0, i / (double) (numPoints - 1));
        const double bin = hz / rate * fftSize;
        const int low = (int) bin;
        const float frac = (float) (bin - low);
        const float magnitude = work[(size_t) low] + frac * (work[(size_t) low + 1] - work[(size_t) low]);
        response[(size_t) i] = juce::Decibels::gainToDecibels (magnitude, -60.0f);
    }
}

void FilterCurve::paint (juce::Graphics& g)
{
    if (stale)
    {
        measure();
        stale = false;
    }

    constexpr float topDb = 24.0f, bottomDb = -48.0f;
    const auto bounds = getLocalBounds().toFloat();
    drawDisplay (g, bounds);
    const auto area = bounds.reduced (1.0f, 6.0f);
    const auto xFor = [&] (double hz) { return area.getX() + (float) (std::log (hz / 20.0) / std::log (1000.0)) * area.getWidth(); };
    const auto yFor = [&] (float db) { return area.getY() + (topDb - juce::jlimit (bottomDb, topDb, db)) / (topDb - bottomDb) * area.getHeight(); };

    g.setFont (Theme::font (10.0f));
    for (const double hz : { 100.0, 1000.0, 10000.0 })
    {
        g.setColour (Theme::panelEdge.withAlpha (0.7f));
        g.fillRect (xFor (hz), area.getY(), 1.0f, area.getHeight());
        g.setColour (Theme::dim);
        g.drawText (hz >= 1000.0 ? juce::String (juce::roundToInt (hz / 1000.0)) + "k" : juce::String (juce::roundToInt (hz)),
                    (int) xFor (hz) + 4, (int) area.getBottom() - 13, 30, 12, juce::Justification::centredLeft);
    }
    g.setColour (Theme::panelEdge);
    g.fillRect (area.getX(), yFor (0.0f), area.getWidth(), 1.0f);

    juce::Path line;
    for (int i = 0; i < numPoints; ++i)
    {
        const float x = area.getX() + (float) i / (float) (numPoints - 1) * area.getWidth();
        const float y = yFor (response[(size_t) i]);
        if (i == 0)
            line.startNewSubPath (x, y);
        else
            line.lineTo (x, y);
    }

    const bool on = value (P::FltOn) > 0.5f;
    drawCurve (g, line, area, on ? moduleColour (*this, Theme::accent) : Theme::dim.withAlpha (0.6f));

    if (! on)
    {
        g.setColour (Theme::dim);
        g.setFont (Theme::font (12.0f));
        g.drawText ("Filtro apagado", getLocalBounds().reduced (10, 8), juce::Justification::topRight);
    }
}

//==============================================================================
EnvelopeShape::EnvelopeShape (ReanimaProcessor& p) : ParamView (p, { P::E2A, P::E2D, P::E2S, P::E2R })
{
    setTooltip (P::u8 ("Forma de la envolvente 2. La escala de tiempo es comprimida para que se vean a la vez tiempos cortos y largos."));
}

void EnvelopeShape::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    drawDisplay (g, bounds);
    const auto area = bounds.reduced (10.0f, 12.0f);

    // Each stage takes room by the logarithm of its time; the held part is fixed.
    const auto room = [] (float ms) { return std::log1p (ms); };
    const float attack = room (value (P::E2A)), decay = room (value (P::E2D)), release = room (value (P::E2R));
    const float hold = (attack + decay + release) * 0.25f;
    const float scale = area.getWidth() / (attack + decay + hold + release);

    const float xPeak = area.getX() + attack * scale;
    const float xSustain = xPeak + decay * scale;
    const float xRelease = xSustain + hold * scale;
    const float ySustain = area.getBottom() - value (P::E2S) * area.getHeight();

    juce::Path line;
    line.startNewSubPath (area.getX(), area.getBottom());
    line.lineTo (xPeak, area.getY());
    line.quadraticTo (xPeak + (xSustain - xPeak) * 0.2f, ySustain, xSustain, ySustain);
    line.lineTo (xRelease, ySustain);
    line.quadraticTo (xRelease + (area.getRight() - xRelease) * 0.2f, area.getBottom(), area.getRight(), area.getBottom());
    drawCurve (g, line, area, moduleColour (*this, Theme::accent2));

    g.setColour (Theme::accent2);
    for (const auto point : { juce::Point<float> (xPeak, area.getY()), { xSustain, ySustain }, { xRelease, ySustain } })
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (point));
}

//==============================================================================
DroneCurveView::DroneCurveView (ReanimaProcessor& p) : ParamView (p, { P::DroneCurveOn, P::DroneHarm })
{
    setTooltip (P::u8 ("Filtro dibujado del motor F: arrastra para dibujar cuánto suena cada frecuencia, de 20 Hz a 20 kHz "
                       "(de -48 a +12 dB). Doble clic lo deja plano. Dibujar lo enciende."));
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

float DroneCurveView::xFor (float hz) const
{
    const auto r = plot();
    return r.getX() + std::log2 (hz / 20.0f) / std::log2 (1000.0f) * r.getWidth();
}

float DroneCurveView::yFor (float db) const
{
    const auto r = plot();
    return r.getY() + (Drone::curveMaxDb - db) / (Drone::curveMaxDb - Drone::curveMinDb) * r.getHeight();
}

void DroneCurveView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    drawDisplay (g, bounds);
    const auto r = plot();
    const bool on = value (P::DroneCurveOn) > 0.5f;

    g.setFont (Theme::font (10.0f));
    for (const float hz : { 100.0f, 1000.0f, 10000.0f })
    {
        g.setColour (Theme::panelEdge);
        g.drawVerticalLine ((int) xFor (hz), r.getY(), r.getBottom());
        g.setColour (Theme::dim);
        g.drawText (hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz), (int) xFor (hz) + 3, (int) r.getBottom() - 13, 30, 12,
                    juce::Justification::centredLeft);
    }
    g.setColour (Theme::panelEdge.brighter (0.2f));
    g.drawHorizontalLine ((int) yFor (0.0f), r.getX(), r.getRight());

    juce::Path line;
    for (int i = 0; i < Drone::curvePoints; ++i)
    {
        const juce::Point<float> pt (xFor (Drone::curveFrequency (i)), yFor (proc.getDroneCurve (i)));
        if (i == 0) line.startNewSubPath (pt); else line.lineTo (pt);
    }
    const auto colour = on ? moduleColour (*this, Theme::teal) : Theme::dim;
    juce::Path fill (line);
    fill.lineTo (r.getRight(), r.getBottom());
    fill.lineTo (r.getX(), r.getBottom());
    fill.closeSubPath();
    g.setColour (colour.withAlpha (on ? 0.16f : 0.06f));
    g.fillPath (fill);
    g.setColour (colour);
    g.strokePath (line, juce::PathStrokeType (on ? 2.0f : 1.2f));
    for (int i = 0; i < Drone::curvePoints; ++i)
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ xFor (Drone::curveFrequency (i)), yFor (proc.getDroneCurve (i)) }));

    g.setColour (Theme::dim);
    g.drawText (on ? P::u8 ("FILTRO DIBUJADO") : P::u8 ("FILTRO DIBUJADO · apagado"), r.reduced (6.0f, 2.0f).toNearestInt(), juce::Justification::topLeft);
}

void DroneCurveView::mouseDown (const juce::MouseEvent& e)
{
    proc.noteUserChange();
    if (auto* param = proc.apvts.getParameter (P::defs()[P::DroneCurveOn].id); param != nullptr && param->getValue() < 0.5f)
        param->setValueNotifyingHost (1.0f);
    last = e.position;
    drawTo (e.position);
}

void DroneCurveView::mouseDrag (const juce::MouseEvent& e)
{
    drawTo (e.position);
}

void DroneCurveView::mouseDoubleClick (const juce::MouseEvent&)
{
    proc.noteUserChange();
    for (int i = 0; i < Drone::curvePoints; ++i)
        proc.setDroneCurve (i, 0.0f);
    repaint();
}

// Every point the mouse passed over since the last event takes the height of the stroke there.
void DroneCurveView::drawTo (juce::Point<float> p)
{
    const auto r = plot();
    const auto dbAt = [&] (float y) { return Drone::curveMaxDb - (y - r.getY()) / r.getHeight() * (Drone::curveMaxDb - Drone::curveMinDb); };
    const float x0 = std::min (last.x, p.x), x1 = std::max (last.x, p.x);

    for (int i = 0; i < Drone::curvePoints; ++i)
    {
        const float x = xFor (Drone::curveFrequency (i));
        const bool nearest = std::abs (x - p.x) <= r.getWidth() / (float) (2 * (Drone::curvePoints - 1));
        if (! nearest && (x < x0 || x > x1))
            continue;
        const float t = x1 > x0 ? (x - last.x) / (p.x - last.x) : 1.0f;
        const float y = nearest ? p.y : last.y + juce::jlimit (0.0f, 1.0f, t) * (p.y - last.y);
        proc.setDroneCurve (i, juce::jlimit (Drone::curveMinDb, Drone::curveMaxDb, dbAt (y)));
    }
    last = p;
    repaint();
}

//==============================================================================
LfoShape::LfoShape (ReanimaProcessor& p, int shapeParam) : ParamView (p, { shapeParam }), shape (shapeParam)
{
    setTooltip (P::u8 ("Forma de onda del LFO: dos ciclos."));
}

void LfoShape::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    drawDisplay (g, bounds);
    const auto area = bounds.reduced (10.0f, 12.0f);

    g.setColour (Theme::panelEdge);
    g.fillRect (area.getX(), area.getCentreY(), area.getWidth(), 1.0f);

    // Steps of the sample and hold drawing: any varied sequence shows what it does.
    static constexpr float steps[] = { 0.4f, -0.7f, 0.9f, -0.2f, 0.65f, -0.85f, 0.15f, -0.45f };
    constexpr int points = 256;
    const int type = (int) value (shape);

    juce::Path line;
    for (int i = 0; i <= points; ++i)
    {
        const float cycles = 2.0f * (float) i / (float) points;
        const float phase = cycles - std::floor (cycles);
        float v = 0.0f;
        switch (type)
        {
            case P::Sine:       v = std::sin (phase * juce::MathConstants<float>::twoPi); break;
            case P::Triangle:   v = 1.0f - 4.0f * std::abs (std::fmod (phase + 0.25f, 1.0f) - 0.5f); break;
            case P::SawUp:      v = phase * 2.0f - 1.0f; break;
            case P::SawDown:    v = 1.0f - phase * 2.0f; break;
            case P::Square:     v = phase < 0.5f ? 1.0f : -1.0f; break;
            default:            v = steps[std::min (7, (int) (cycles * 4.0f))]; break;
        }

        const float x = area.getX() + (float) i / (float) points * area.getWidth();
        const float y = area.getCentreY() - v * area.getHeight() * 0.5f;
        if (i == 0)
            line.startNewSubPath (x, y);
        else
            line.lineTo (x, y);
    }

    g.setColour (moduleColour (*this, Theme::good));
    g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
}

//==============================================================================
MatrixRow::MatrixRow (ReanimaProcessor& p, int slotIndex)
    : ParamView (p, { P::M1Src + slotIndex * P::slotStride, P::M1Src + slotIndex * P::slotStride + 1 }),
      slot (slotIndex), base (P::M1Src + slotIndex * P::slotStride),
      source (p.apvts, base, false), destination (p.apvts, base + 1, false),
      amount (p.apvts, base + 2, {}, juce::Slider::LinearHorizontal)
{
    setComponentID ("mrow:" + juce::String (slotIndex));     // the guides light up a whole row
    addAndMakeVisible (source);
    addAndMakeVisible (destination);
    addAndMakeVisible (amount);
}

void MatrixRow::resized()
{
    auto r = getLocalBounds().reduced (margin, 0);
    r.removeFromLeft (numberWidth);
    source.setBounds (r.removeFromLeft (boxWidth));
    r.removeFromLeft (arrowWidth);
    destination.setBounds (r.removeFromLeft (boxWidth));
    r.removeFromLeft (24);
    amount.setBounds (r);
}

void MatrixRow::paint (juce::Graphics& g)
{
    const bool connected = value (base) > 0.5f && value (base + 1) > 0.5f;
    const auto r = getLocalBounds().toFloat();

    g.setColour (connected ? Theme::good.withAlpha (0.07f) : Theme::well.withAlpha (0.35f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (connected ? Theme::good.withAlpha (0.35f) : Theme::panelEdge.withAlpha (0.6f));
    g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);

    g.setColour (connected ? Theme::good : Theme::dim);
    g.setFont (Theme::font (14.0f, true));
    g.drawText (juce::String (slot + 1), margin, 0, numberWidth - 8, getHeight(), juce::Justification::centred);

    const float x = (float) (margin + numberWidth + boxWidth) + (float) arrowWidth * 0.5f, y = (float) getHeight() * 0.5f;
    juce::Path arrow;
    arrow.addArrow ({ x - 11.0f, y, x + 11.0f, y }, 1.5f, 9.0f, 7.0f);
    g.fillPath (arrow);
}

void MatrixTitles::paint (juce::Graphics& g)
{
    constexpr int left = MatrixRow::margin + MatrixRow::numberWidth;
    constexpr int second = left + MatrixRow::boxWidth + MatrixRow::arrowWidth;
    constexpr int third = second + MatrixRow::boxWidth + 24;

    g.setColour (Theme::dim);
    g.setFont (Theme::caps (10.5f));
    g.drawText ("FUENTE", left + 2, 0, 200, getHeight(), juce::Justification::centredLeft);
    g.drawText ("DESTINO", second + 2, 0, 200, getHeight(), juce::Justification::centredLeft);
    g.drawText ("CANTIDAD", third, 0, 200, getHeight(), juce::Justification::centredLeft);
}

//==============================================================================
MechanismAdvice::MechanismAdvice (ReanimaProcessor& p, juce::ComboBox& typeMenu) : proc (p), menu (typeMenu)
{
    onOff.setClickingTogglesState (true);
    onOff.setTooltip (Help::forParam (P::MechOn));
    onOff.setColour (juce::TextButton::buttonOnColourId, Theme::good.withAlpha (0.8f));
    onOff.setColour (juce::TextButton::textColourOnId, Theme::bg);
    onOff.setColour (juce::TextButton::textColourOffId, Theme::warning);
    onOff.onStateChange = [this] { onOff.setButtonText (onOff.getToggleState() ? "On" : "Off"); };
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, P::defs()[P::MechOn].id, onOff);
    onOff.setButtonText (onOff.getToggleState() ? "On" : "Off");
    addAndMakeVisible (onOff);
    setTooltip (P::u8 ("Cuánto encaja cada gesto de Mecanismo con este sample y con el ataque que le das: mira el ataque, si el sonido se sostiene, si tiene golpe propio, tono o ruido, y su registro. Es una guía: cualquier gesto se puede elegir."));
}

MechanismAdvice::~MechanismAdvice() = default;

void MechanismAdvice::resized()
{
    onOff.setBounds (getLocalBounds().removeFromRight (52).reduced (0, 1));
}

void MechanismAdvice::paint (juce::Graphics& g)
{
    g.setColour (warning ? Theme::warning : Theme::dim);
    g.setFont (Theme::font (11.5f));
    g.drawFittedText (text, getLocalBounds().withTrimmedRight (58), juce::Justification::centredLeft, 1, 0.85f);
}

// The menu shows the new text of the chosen item only when the choice changes.
void MechanismAdvice::refreshMenuText()
{
    const int id = menu.getSelectedId();
    if (id == 0)
        return;
    menu.setSelectedId (0, juce::dontSendNotification);
    menu.setSelectedId (id, juce::dontSendNotification);
}

void MechanismAdvice::update()
{
    const auto sample = proc.getSample();
    const std::array<float, 6> values { proc.paramValue (P::MechType), proc.paramValue (P::RelNoise) * proc.paramValue (P::MechOn), proc.paramValue (P::AmpA),
                                        proc.paramValue (P::AmpS), proc.paramValue (P::SustainType), proc.paramValue (P::MechAdapt) };
    if (sample == lastSample && values == lastValues)
        return;
    lastSample = sample;
    lastValues = values;

    const auto names = P::choicesOf (P::MechType);
    if (sample == nullptr)
    {
        for (int i = 0; i < names.size(); ++i)
            menu.changeItemText (i + 1, names[i]);
        refreshMenuText();
        text = P::u8 ("Carga un sample para saber qué gesto le encaja");
        warning = false;
        repaint();
        return;
    }

    const auto fit = Mechanism::fit (*sample, values[2], values[3], (int) values[4] == P::SustainHold);
    const auto percent = [&fit] (int t) { return juce::String (juce::roundToInt (100.0f * fit.score[(size_t) t])) + " %"; };
    for (int i = 0; i < names.size() && i < Mechanism::NumTypes; ++i)
        menu.changeItemText (i + 1, (i == fit.best ? P::u8 ("» ") : juce::String()) + names[i] + P::u8 (" · ") + percent (i));
    refreshMenuText();

    const int chosen = juce::jlimit (0, Mechanism::NumTypes - 1, (int) values[0]);
    const auto best = names[fit.best] + " (" + (fit.byName ? P::u8 ("por el nombre: ") + sample->mechanismHintWord + ", " : juce::String())
                    + percent (fit.best) + ")";
    warning = false;
    if (values[1] <= 0.0f)
        text = P::u8 ("Recomendado: ") + best;
    else if (chosen == fit.best)
        text = names[chosen] + P::u8 (" es lo que mejor encaja (") + percent (chosen) + ")";
    else if (fit.score[(size_t) chosen] < 0.45f)
    {
        text = names[chosen] + " " + percent (chosen) + ": " + P::u8 (fit.against[(size_t) chosen]) + P::u8 (" · mejor ") + best;
        warning = true;
    }
    else
        text = names[chosen] + " " + percent (chosen) + P::u8 (" · el que mejor encaja: ") + best;
    repaint();
}
