#include "Guide.h"

namespace
{
const char* levelNames[] { "1 · Fundamentos", "2 · Práctica", "3 · Avanzado" };
}

GuideOverlay::GuideOverlay (std::function<void (int)> tabShower) : showTab (std::move (tabShower))
{
    setVisible (false);
    setWantsKeyboardFocus (true);
    for (auto* b : { &back, &next, &close })
    {
        b->setColour (juce::TextButton::textColourOffId, Theme::text);
        addAndMakeVisible (b);
    }
    next.setColour (juce::TextButton::buttonColourId, Theme::accent.withAlpha (0.35f));
    back.setTooltip ("Paso anterior (flecha izquierda).");
    next.setTooltip ("Paso siguiente (flecha derecha).");
    close.setTooltip (juce::String::fromUTF8 ("Cierra la guía (Esc). No cambia nada del proyecto."));
    back.onClick = [this] { show (current - 1); };
    next.onClick = [this] { if (current + 1 < (int) steps.size()) show (current + 1); else stop(); };
    close.onClick = [this] { stop(); };
    dontShow.setColour (juce::ToggleButton::textColourId, Theme::dim);
    dontShow.setColour (juce::ToggleButton::tickColourId, Theme::accent);
    dontShow.setTooltip (P::u8 ("Marcado, el tour no volverá a abrirse solo en ninguna instancia de Reanima en este ordenador. "
                                "Siempre puedes verlo desde el botón ? de arriba."));
    addChildComponent (dontShow);

    for (int l = 0; l < 3; ++l)
    {
        auto& b = levels[(size_t) l];
        b.setButtonText (juce::String::fromUTF8 (levelNames[l]));
        b.setClickingTogglesState (false);
        const auto colour = levelColour (l + 1);
        b.setColour (juce::TextButton::textColourOffId, colour.withAlpha (0.75f));
        b.setColour (juce::TextButton::textColourOnId, Theme::bg);
        b.setColour (juce::TextButton::buttonColourId, colour.withAlpha (0.08f));
        b.setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.85f));
        b.setTooltip (juce::String::fromUTF8 ("Salta a este nivel. No hace falta completar los anteriores."));
        b.onClick = [this, l] { const int i = firstOfLevel (l + 1); if (i >= 0) show (i); };
        addChildComponent (b);
    }
}

juce::Colour GuideOverlay::levelColour (int level) noexcept
{
    switch (level)
    {
        case 1: return Theme::good;
        case 3: return Theme::accent2;
        default: return Theme::accent;
    }
}

void GuideOverlay::start (std::vector<Step> s, const juce::String& tour, bool offerDontShow)
{
    onFinished = nullptr;
    dontShow.setVisible (offerDontShow);
    dontShow.setToggleState (false, juce::dontSendNotification);
    steps = std::move (s);
    name = tour;
    if (steps.empty())
        return;
    bool leveled = false;
    for (const auto& st : steps)
        leveled = leveled || st.level > 0;
    for (int l = 0; l < 3; ++l)
        levels[(size_t) l].setVisible (leveled && firstOfLevel (l + 1) >= 0);
    setVisible (true);
    toFront (true);
    show (0);
    startTimerHz (30);
}

void GuideOverlay::stop()
{
    stopTimer();
    setVisible (false);
    if (auto done = std::move (onFinished))
    {
        onFinished = nullptr;
        done (dontShow.isVisible() && dontShow.getToggleState());
    }
}

int GuideOverlay::firstOfLevel (int level) const noexcept
{
    for (int i = 0; i < (int) steps.size(); ++i)
        if (steps[(size_t) i].level == level)
            return i;
    return -1;
}

void GuideOverlay::show (int index)
{
    if (steps.empty())
        return;
    current = juce::jlimit (0, (int) steps.size() - 1, index);
    if (steps[(size_t) current].tab >= 0 && showTab)
        showTab (steps[(size_t) current].tab);
    back.setEnabled (current > 0);
    next.setColour (juce::TextButton::buttonColourId, tint().withAlpha (0.35f));
    next.setButtonText (current + 1 < (int) steps.size() ? "Siguiente" : "Terminar");
    for (int l = 0; l < 3; ++l)
        levels[(size_t) l].setToggleState (steps[(size_t) current].level == l + 1, juce::dontSendNotification);
    lastTarget = {};
    layout();
    repaint();
    grabKeyboardFocus();
}

juce::TextLayout GuideOverlay::textFor (float width) const
{
    const auto& step = steps[(size_t) current];
    juce::AttributedString text;
    text.append (step.text, Theme::font (13.5f), Theme::text);
    if (step.action.isNotEmpty())
    {
        text.append ("\n\n", Theme::font (6.0f), Theme::text);
        text.append (juce::String::fromUTF8 ("Pruébalo: "), Theme::caps (12.0f), tint());
        text.append (step.action, Theme::font (13.0f), Theme::text);
    }
    if (step.tip.isNotEmpty())
    {
        text.append ("\n", Theme::font (6.0f), Theme::text);
        text.append ("Para empezar: ", Theme::caps (12.0f), tint().interpolatedWith (Theme::text, 0.35f));
        text.append (step.tip, Theme::font (13.0f), Theme::dim);
    }
    text.setLineSpacing (3.0f);
    juce::TextLayout layout;
    layout.createLayout (text, width);
    return layout;
}

// The bubble goes below the control it explains, or above it, or beside it: always inside, and never
// over it (it has to stay usable).
void GuideOverlay::layout()
{
    if (steps.empty())
        return;
    const auto& step = steps[(size_t) current];
    const auto bounds = getLocalBounds().toFloat();
    lastTarget = step.target ? step.target() : juce::Rectangle<int>();
    spot = lastTarget.isEmpty() ? juce::Rectangle<float>() : lastTarget.toFloat().expanded (6.0f);
    focusRects.clear();
    if (step.focus)
        for (const auto& r : step.focus())
            if (! r.isEmpty())
                focusRects.push_back (r.toFloat().expanded (3.0f));

    const bool leveled = levels[0].isVisible() || levels[1].isVisible() || levels[2].isVisible();
    constexpr float pad = 16.0f, titleHeight = 24.0f, buttonsHeight = 30.0f, dotsHeight = 14.0f;
    const float levelHeight = leveled ? 30.0f : 0.0f;
    const float textHeight = textFor (bubbleWidth - 2 * pad).getHeight();
    const float checkHeight = dontShow.isVisible() ? 28.0f : 0.0f;
    const float height = pad + levelHeight + titleHeight + 6.0f + textHeight + 12.0f + dotsHeight + 10.0f + checkHeight + buttonsHeight + pad;
    juce::Rectangle<float> b (bubbleWidth, height);

    const auto fits = [&bounds] (juce::Rectangle<float> r) { return bounds.reduced (8.0f).contains (r); };
    if (spot.isEmpty())
        b.setCentre (bounds.getCentre());
    else
    {
        // Below, above, right, left of the control: the first place where it fits whole.
        const juce::Rectangle<float> places[] {
            b.withPosition (spot.getX(), spot.getBottom() + 12.0f),
            b.withPosition (spot.getX(), spot.getY() - 12.0f - height),
            b.withPosition (spot.getRight() + 12.0f, spot.getCentreY() - height * 0.5f),
            b.withPosition (spot.getX() - 12.0f - bubbleWidth, spot.getCentreY() - height * 0.5f) };
        bool placed = false;
        for (auto p : places)
        {
            p = p.constrainedWithin (bounds.reduced (8.0f));
            if (fits (p) && ! p.intersects (spot))
            {
                b = p;
                placed = true;
                break;
            }
        }
        if (! placed)       // a control as big as the window (a whole page): the bubble sits in a corner of it
            b = b.withPosition (bounds.getRight() - 8.0f - bubbleWidth, bounds.getBottom() - 8.0f - height);
    }
    bubble = b.constrainedWithin (bounds.reduced (8.0f));

    auto inner = bubble.reduced (pad);
    if (leveled)
    {
        levelRow = inner.removeFromTop (levelHeight - 6.0f);
        auto row = levelRow.toNearestInt();
        const int w = (row.getWidth() - 8) / 3;
        for (int l = 0; l < 3; ++l)
        {
            levels[(size_t) l].setBounds (row.removeFromLeft (w));
            row.removeFromLeft (4);
        }
    }
    auto lower = bubble.reduced (pad);
    auto buttons = lower.removeFromBottom (buttonsHeight).toNearestInt();
    if (dontShow.isVisible())
        dontShow.setBounds (lower.removeFromBottom (checkHeight).toNearestInt().withTrimmedBottom (4));
    close.setBounds (buttons.removeFromLeft (80));
    next.setBounds (buttons.removeFromRight (100));
    buttons.removeFromRight (6);
    back.setBounds (buttons.removeFromRight (90));
}

bool GuideOverlay::hitTest (int x, int y)
{
    // Inside the outline the control below is used as normal; the bubble and the dimmed rest stay the guide's.
    return ! spot.contains ((float) x, (float) y) || bubble.contains ((float) x, (float) y);
}

void GuideOverlay::timerCallback()
{
    // A slow pulse on the outline, and the outline follows its control if the window changes (another
    // engine shows other controls, the user scrolls a menu...).
    pulse += 0.06f;
    if (pulse > juce::MathConstants<float>::twoPi)
        pulse -= juce::MathConstants<float>::twoPi;
    if (! steps.empty() && steps[(size_t) current].target && steps[(size_t) current].target() != lastTarget)
    {
        layout();
        repaint();
    }
    repaint (spot.expanded (6.0f).getSmallestIntegerContainer());
    repaint (bubble.getSmallestIntegerContainer());
}

void GuideOverlay::paint (juce::Graphics& g)
{
    if (steps.empty())
        return;

    // Everything dimmed except the control being explained.
    juce::Path shade;
    shade.addRectangle (getLocalBounds().toFloat());
    if (! spot.isEmpty())
        shade.addRoundedRectangle (spot, 8.0f);
    shade.setUsingNonZeroWinding (false);
    g.setColour (juce::Colours::black.withAlpha (0.72f));
    g.fillPath (shade);
    if (! spot.isEmpty())
    {
        const float wave = 0.5f + 0.5f * std::sin (pulse);
        g.setColour (tint().withAlpha (0.25f + 0.35f * wave));
        g.drawRoundedRectangle (spot.expanded (2.0f + 2.0f * wave), 10.0f, 2.0f);
        g.setColour (tint());
        g.drawRoundedRectangle (spot, 8.0f, 2.0f);
        // The controls the step is about, inside the panel.
        for (const auto& r : focusRects)
        {
            g.setColour (tint().withAlpha (0.10f + 0.10f * wave));
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (tint().withAlpha (0.9f));
            g.drawRoundedRectangle (r, 6.0f, 1.4f);
        }
    }

    g.setColour (Theme::panelTop);
    g.fillRoundedRectangle (bubble, 9.0f);
    g.setColour (tint().withAlpha (0.8f));
    g.drawRoundedRectangle (bubble.reduced (0.5f), 9.0f, 1.2f);

    // No arrow from the bubble to the control: the control is already lit up in the window.

    auto area = bubble.reduced (16.0f);
    if (levels[0].isVisible() || levels[1].isVisible() || levels[2].isVisible())
        area.removeFromTop (levelRow.getHeight() + 6.0f);
    const auto& step = steps[(size_t) current];
    g.setColour (tint());
    g.setFont (Theme::caps (13.0f));
    g.drawFittedText (step.title, area.removeFromTop (24.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    area.removeFromTop (6.0f);
    const auto text = textFor (area.getWidth());
    text.draw (g, area.removeFromTop (text.getHeight()));

    // Progress: a dot per step, the steps of the current level brighter.
    area.removeFromTop (12.0f);
    const auto dots = area.removeFromTop (14.0f);
    const int n = (int) steps.size();
    const float spacing = std::min (12.0f, (dots.getWidth() - 170.0f) / (float) std::max (1, n));
    for (int i = 0; i < n; ++i)
    {
        const bool here = i == current, sameLevel = steps[(size_t) i].level == step.level;
        const auto dotColour = levelColour (steps[(size_t) i].level);
        g.setColour (here ? dotColour : sameLevel ? dotColour.withAlpha (0.55f) : dotColour.withAlpha (0.22f));
        const float r = here ? 4.0f : 3.0f;
        g.fillEllipse (dots.getX() + spacing * (float) i + 4.0f - r, dots.getCentreY() - r, 2.0f * r, 2.0f * r);
    }
    g.setColour (Theme::dim);
    g.setFont (Theme::font (11.5f));
    g.drawText (name + P::u8 ("  ·  ") + juce::String (current + 1) + " / " + juce::String (n), dots, juce::Justification::centredRight);
}

bool GuideOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
        stop();
    else if (key == juce::KeyPress::rightKey || key == juce::KeyPress::returnKey)
        next.onClick();
    else if (key == juce::KeyPress::leftKey && current > 0)
        show (current - 1);
    else
        return false;
    return true;
}
