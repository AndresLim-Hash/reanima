#pragma once

#include "Look.h"
#include <functional>

// Guided tours over the real window: the quick guide of the whole plugin (header "?") and one guide per
// tab (the "?" beside each tab's name, see TabGuides). Each step dims everything except the control it
// talks about, outlines it with a slow pulse and explains it in a bubble placed beside it. The control
// stays usable: clicks inside the outline reach it, so one learns by doing while the guide goes on. The
// guide never changes a parameter itself. Esc closes it, the arrows move through it.
class GuideOverlay : public juce::Component, private juce::Timer
{
public:
    struct Step
    {
        juce::String title, text;
        std::function<juce::Rectangle<int>()> target;     // in the overlay's coordinates; empty: the whole window
        int tab = -1;                                       // tab to show first, or -1
        int level = 0;                                      // 1 fundamentos, 2 aplicación práctica, 3 avanzado; 0: none
        juce::String action;                                // "Pruébalo": what to do with the control now
        juce::String tip;                                   // "Para empezar": a good first setting
        // Within the panel lit up, the controls the step talks about, outlined one by one (optional).
        std::function<std::vector<juce::Rectangle<int>>()> focus;
    };

    // The colour of each level (0: the quick guide): Fundamentos green, Práctica blue, Avanzado orange,
    // the plugin's own three accents.
    static juce::Colour levelColour (int level) noexcept;

    explicit GuideOverlay (std::function<void (int)> showTab);

    // Opens a tour (replacing the one shown, if any) from its first step.
    void start (std::vector<Step> steps, const juce::String& name, bool offerDontShow = false);
    // Called once when the tour started last ends (closed, finished or Esc), with whether "no volver a
    // mostrar" was ticked. Set it after start().
    std::function<void (bool dontShowAgain)> onFinished;
    juce::ToggleButton& dontShowButton() noexcept { return dontShow; }
    void stop();
    bool isRunning() const noexcept { return isVisible(); }
    const juce::String& tourName() const noexcept { return name; }

    // For the checks of the interface (tests/UiSnapshot).
    int stepCount() const noexcept { return (int) steps.size(); }
    int currentStep() const noexcept { return current; }
    int levelOf (int index) const noexcept { return index >= 0 && index < (int) steps.size() ? steps[(size_t) index].level : 0; }
    void showStep (int index) { show (index); }
    juce::Rectangle<float> spotArea() const noexcept { return spot; }
    juce::Rectangle<float> bubbleArea() const noexcept { return bubble; }
    juce::Rectangle<int> targetArea() const noexcept { return lastTarget; }
    const std::vector<juce::Rectangle<float>>& focusAreas() const noexcept { return focusRects; }

    void paint (juce::Graphics& g) override;
    void resized() override { layout(); }
    bool keyPressed (const juce::KeyPress& key) override;
    bool hitTest (int x, int y) override;                   // the outlined control takes its own clicks
    void mouseDown (const juce::MouseEvent&) override {}    // elsewhere the window below stays untouched

private:
    void show (int index);
    void layout();
    void timerCallback() override;
    juce::TextLayout textFor (float width) const;
    int firstOfLevel (int level) const noexcept;

    std::vector<Step> steps;
    std::function<void (int)> showTab;
    juce::String name;
    int current = 0;
    float pulse = 0.0f;
    juce::Rectangle<int> lastTarget;
    juce::Rectangle<float> spot, bubble, levelRow;
    std::vector<juce::Rectangle<float>> focusRects;
    juce::Colour tint() const noexcept { return levelColour (steps.empty() ? 0 : steps[(size_t) current].level); }
    juce::TextButton back { "Anterior" }, next { "Siguiente" }, close { "Cerrar" };
    std::array<juce::TextButton, 3> levels;
    juce::ToggleButton dontShow { "No volver a mostrar este tour (en ninguna instancia)" };

    static constexpr float bubbleWidth = 392.0f;
};

// The small "?" beside the name of each tab: opens the guide of that tab.
class GuideDot : public juce::Button
{
public:
    GuideDot() : juce::Button ("?") { setSize (18, 18); }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        // Quiet: a fine ring in grey, the accent only under the mouse (the click area stays 18 px).
        const auto r = getLocalBounds().toFloat().withSizeKeepingCentre (13.0f, 13.0f);
        if (over || down)
        {
            g.setColour (Theme::accent.withAlpha (down ? 0.45f : 0.22f));
            g.fillEllipse (r);
        }
        g.setColour (over || down ? Theme::accent : Theme::dim.withAlpha (0.8f));
        g.drawEllipse (r.reduced (0.5f), 1.0f);
        g.setFont (Theme::font (9.5f, true));
        g.drawText ("?", r.translated (0.0f, -0.5f), juce::Justification::centred);
    }
};
