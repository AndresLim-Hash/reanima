#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "HelpText.h"
#include "Params.h"

// One colour per job: blue for the sound, orange for envelopes, green for modulation, violet for effects.
namespace Theme
{
const juce::Colour bg        { 0xff0c0e12 };
const juce::Colour panelTop  { 0xff1c2029 };
const juce::Colour panel     { 0xff151820 };
const juce::Colour panelEdge { 0xff2b313d };
const juce::Colour well      { 0xff08090c };
const juce::Colour text      { 0xffe6eaf1 };
const juce::Colour dim       { 0xff8b94a6 };
const juce::Colour accent    { 0xff3fc8ff };
const juce::Colour accent2   { 0xffff9a3c };
const juce::Colour good      { 0xff7be08a };
const juce::Colour violet    { 0xffb79bff };
const juce::Colour teal      { 0xff4fd8c4 };      // module colours (see docs/colores_modulos.md)
const juce::Colour pink      { 0xffff86b4 };
const juce::Colour silver    { 0xffc3cddd };
const juce::Colour warning   { 0xffffd257 };
const juce::Colour danger    { 0xffff4d4d };

// The window's scale (set by the editor). Text never goes under 8 px on the screen: below about 9 px Segoe UI,
// drawn through the scale (no hinting to the pixel grid), smears; at 70 % the 10 px labels were 7 px.
inline float& uiScale() { static float s = 1.0f; return s; }
inline float onScreen (float size) noexcept { return std::max (size, 8.0f / std::max (0.1f, uiScale())); }

inline juce::Font font (float size, bool bold = false)
{
    size = onScreen (size);
    const int style = bold ? juce::Font::bold : juce::Font::plain;
   #if JUCE_WINDOWS
    return juce::FontOptions ("Segoe UI", size, style);
   #else
    return juce::FontOptions (size, style);
   #endif
}

inline juce::Font caps (float size) { return font (size, true).withExtraKerningFactor (0.08f); }
}

class ReanimaLook : public juce::LookAndFeel_V4
{
public:
    ReanimaLook()
    {
        using namespace juce;
        setColour (ResizableWindow::backgroundColourId, Theme::bg);
        setColour (Label::textColourId, Theme::text);
        setColour (Slider::textBoxTextColourId, Theme::text);
        setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
        setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
        setColour (Slider::textBoxHighlightColourId, Theme::accent.withAlpha (0.4f));
        setColour (Slider::trackColourId, Theme::accent);
        setColour (Slider::backgroundColourId, Theme::well);
        setColour (Slider::thumbColourId, Theme::text);
        setColour (ComboBox::backgroundColourId, Theme::well);
        setColour (ComboBox::outlineColourId, Theme::panelEdge);
        setColour (ComboBox::textColourId, Theme::text);
        setColour (ComboBox::arrowColourId, Theme::dim);
        setColour (PopupMenu::backgroundColourId, Theme::panel);
        setColour (PopupMenu::textColourId, Theme::text);
        setColour (PopupMenu::highlightedBackgroundColourId, Theme::accent.withAlpha (0.25f));
        setColour (PopupMenu::highlightedTextColourId, Colours::white);
        setColour (TextButton::buttonColourId, Theme::well);
        setColour (TextButton::buttonOnColourId, Theme::accent.withAlpha (0.16f));
        setColour (TextButton::textColourOffId, Theme::dim);
        setColour (TextButton::textColourOnId, Theme::accent);
        setColour (TabbedComponent::backgroundColourId, Theme::bg);
        setColour (TabbedComponent::outlineColourId, Colours::transparentBlack);
        setColour (TabbedButtonBar::tabTextColourId, Theme::dim);
        setColour (TabbedButtonBar::frontTextColourId, Theme::accent);
        setColour (TextEditor::backgroundColourId, Theme::well);
        setColour (TextEditor::textColourId, Theme::text);
        setColour (TextEditor::highlightColourId, Theme::accent.withAlpha (0.4f));
        setColour (CaretComponent::caretColourId, Theme::accent);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        using namespace juce;
        const auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
        const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = startAngle + pos * (endAngle - startAngle);
        const float track = std::max (2.5f, radius * 0.11f);
        const float arcRadius = radius - track * 0.5f;
        const auto stroke = [] (float width) { return PathStrokeType (width, PathStrokeType::curved, PathStrokeType::rounded); };

        Path background;
        background.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        g.setColour (Theme::well);
        g.strokePath (background, stroke (track));

        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        const auto colour = slider.findColour (Slider::trackColourId);
        // Modulated (a macro, an LFO, an envelope... through the matrix): the value set, an inner ring; where the
        // modulation takes it, an outer one in the macros' colour, from the value to there, with a dot at its end.
        const auto modProp = slider.getProperties()["modPos"];
        const float modPos = modProp.isVoid() ? -1.0f : (float) modProp;
        const bool modulatedNow = modPos >= 0.0f && std::abs (modPos - pos) > 0.004f;
        if (modulatedNow)
        {
            const float innerRadius = arcRadius - track * 1.35f;
            if (std::abs (angle - from) > 0.01f)
            {
                Path value;
                value.addCentredArc (centre.x, centre.y, innerRadius, innerRadius, 0.0f, std::min (from, angle), std::max (from, angle), true);
                g.setColour (colour);
                g.strokePath (value, stroke (track * 0.8f));
            }
            const float modAngle = startAngle + modPos * (endAngle - startAngle);
            Path moved;
            moved.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, std::min (angle, modAngle), std::max (angle, modAngle), true);
            g.setColour (Theme::good.withAlpha (0.18f));
            g.strokePath (moved, stroke (track * 2.4f));
            g.setColour (Theme::good);
            g.strokePath (moved, stroke (track));
            g.fillEllipse (Rectangle<float> (track * 1.6f, track * 1.6f).withCentre (centre.getPointOnCircumference (arcRadius, modAngle)));
        }
        else if (std::abs (angle - from) > 0.01f)
        {
            Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                 std::min (from, angle), std::max (from, angle), true);
            g.setColour (colour.withAlpha (0.16f));
            g.strokePath (value, stroke (track * 2.6f));
            g.setColour (colour);
            g.strokePath (value, stroke (track));
        }

        const float body = radius - track * 2.4f;
        const auto cap = Rectangle<float> (body * 2.0f, body * 2.0f).withCentre (centre);
        const bool active = slider.isMouseOverOrDragging();
        g.setGradientFill (ColourGradient (Colour (active ? 0xff3a4252 : 0xff303744), centre.x, cap.getY(),
                                           Colour (0xff171a21), centre.x, cap.getBottom(), false));
        g.fillEllipse (cap);
        g.setColour (Colours::black.withAlpha (0.55f));
        g.drawEllipse (cap, 1.0f);

        g.setColour (Theme::text);
        g.drawLine ({ centre.getPointOnCircumference (body * 0.42f, angle), centre.getPointOnCircumference (body * 0.92f, angle) },
                    std::max (1.8f, radius * 0.06f));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        using namespace juce;
        const auto colour = slider.findColour (Slider::trackColourId);
        const bool bar = style == Slider::LinearBar;
        const float height = bar ? (float) h : 6.0f;
        const auto track = Rectangle<float> ((float) x, (float) y + ((float) h - height) * 0.5f, (float) w, height);
        const float corner = bar ? 4.0f : 3.0f;

        g.setColour (Theme::well);
        g.fillRoundedRectangle (track, corner);

        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float from = bipolar ? track.getCentreX() : track.getX();
        const auto fill = track.withLeft (std::min (from, pos)).withRight (std::max (from, pos));
        g.setColour (bar ? colour.withAlpha (0.35f) : colour);
        g.fillRoundedRectangle (fill, corner);

        if (bar)
        {
            g.setColour (Theme::panelEdge);
            g.drawRoundedRectangle (track.reduced (0.5f), corner, 1.0f);
            return;
        }

        if (bipolar)
        {
            g.setColour (Theme::dim);
            g.fillRect (track.getCentreX() - 0.5f, track.getY() - 3.0f, 1.0f, height + 6.0f);
        }
        g.setColour (Theme::text);
        g.fillEllipse (Rectangle<float> (13.0f, 13.0f).withCentre ({ pos, track.getCentreY() }));
    }

    juce::Label* createSliderTextBox (juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox (slider);
        label->setFont (Theme::font (12.5f));
        return label;
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                               bool highlighted, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const bool on = b.getToggleState();
        auto c = b.findColour (on ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
        if (highlighted) c = c.brighter (0.15f);
        if (down) c = c.brighter (0.3f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (on ? b.findColour (juce::TextButton::textColourOnId).withAlpha (0.7f)
                        : (highlighted ? Theme::dim : Theme::panelEdge));
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int h) override
    {
        return Theme::font (std::min (13.0f, (float) h * 0.6f), true);
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (box.isMouseOver (true) ? Theme::dim : box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        juce::Path arrow;
        const float ax = (float) w - 12.0f, ay = (float) h * 0.5f;
        arrow.startNewSubPath (ax - 3.5f, ay - 1.5f);
        arrow.lineTo (ax, ay + 2.0f);
        arrow.lineTo (ax + 3.5f, ay - 1.5f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        g.strokePath (arrow, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Flat tabs: text only, accent underline on the active one.
    void drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool isMouseOver, bool) override
    {
        auto r = b.getActiveArea();
        const bool front = b.isFrontTab();
        g.setColour (front ? Theme::accent : (isMouseOver ? Theme::text : Theme::dim));
        g.setFont (Theme::caps (12.5f));
        auto text = r;
        if (auto* extra = b.getExtraComponent())     // the guide's "?" sits after the name
            text.removeFromRight (extra->getWidth() + 14 + 6);
        g.drawText (b.getButtonText(), text, b.getExtraComponent() != nullptr ? juce::Justification::centredRight : juce::Justification::centred);
        if (front)
            g.fillRect (r.removeFromBottom (2).reduced (12, 0));
    }

    int getTabButtonBestWidth (juce::TabBarButton& b, int) override
    {
        const int text = juce::GlyphArrangement::getStringWidthInt (Theme::caps (12.5f), b.getButtonText());
        if (auto* extra = b.getExtraComponent())
            return text + 20 + 6 + extra->getWidth() + 14;
        return text + 40;
    }

    juce::Rectangle<int> getTabButtonExtraComponentBounds (const juce::TabBarButton&, juce::Rectangle<int>& area, juce::Component& c) override
    {
        auto r = area.removeFromRight (c.getWidth() + 14).withTrimmedRight (14);
        return r.withSizeKeepingCentre (c.getWidth(), c.getHeight());
    }

    void drawTabbedButtonBarBackground (juce::TabbedButtonBar& bar, juce::Graphics& g) override
    {
        g.setColour (Theme::panelEdge);
        g.fillRect (10, bar.getHeight() - 1, bar.getWidth() - 14, 1);
    }

    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}

    juce::Font getComboBoxFont (juce::ComboBox&) override { return Theme::font (13.0f); }
    juce::Font getPopupMenuFont() override { return Theme::font (14.0f); }

    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> pos, juce::Rectangle<int> parent) override
    {
        const auto layout = tooltipLayout (text);
        const int w = (int) std::ceil (layout.getWidth()) + tooltipPadding * 2;
        const int h = (int) std::ceil (layout.getHeight()) + tooltipPadding * 2;
        return juce::Rectangle<int> (pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 24,
                                     pos.y > parent.getCentreY() ? pos.y - (h + 6) : pos.y + 6, w, h).constrainedWithin (parent);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h) override
    {
        const auto r = juce::Rectangle<float> ((float) w, (float) h);
        g.setColour (Theme::panelTop);
        g.fillRect (r);
        g.setColour (Theme::accent.withAlpha (0.6f));
        g.drawRect (r, 1.0f);
        tooltipLayout (text).draw (g, r.reduced ((float) tooltipPadding));
    }

private:
    static constexpr int tooltipPadding = 8;

    static juce::TextLayout tooltipLayout (const juce::String& text)
    {
        juce::AttributedString s;
        s.append (text, Theme::font (13.5f), Theme::text);
        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths (s, 320.0f);
        return layout;
    }
};

//==============================================================================
namespace Format
{
inline juce::String value (int index, double v)
{
    const auto& d = P::defs()[(size_t) index];
    const juce::String unit = P::u8 (d.unit);

    if (index == P::StartPos)
        return juce::String (v * 100.0, 1) + " %";
    if (index == P::LoopBpm)
        return v < 0.5 ? juce::String ("Auto") : juce::String (v, 2) + " BPM";
    if (index == P::SliceStart)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int key = juce::roundToInt (v);
        return juce::String (names[key % 12]) + juce::String (key / 12 - 2);
    }

    // Note: juce::String (double, 0) prints every decimal, so whole numbers use roundToInt.
    if (unit == "ms")
        return v >= 1000.0 ? juce::String (v / 1000.0, 2) + " s"
             : v < 10.0    ? juce::String (v, 1) + " ms"
                           : juce::String (juce::roundToInt (v)) + " ms";
    if (unit == "Hz")
        return v >= 1000.0 ? juce::String (v / 1000.0, 2) + " kHz"
             : v < 10.0    ? juce::String (v, 2) + " Hz"
                           : juce::String (juce::roundToInt (v)) + " Hz";
    if (unit == "st" || unit == "ct")
        return (v > 0 ? "+" : "") + juce::String (juce::roundToInt (v)) + " " + unit;
    if (unit.isNotEmpty())
        return juce::String (v, 1) + " " + unit;
    if (d.min >= 0.0f && d.max <= 1.0f)
        return juce::String (juce::roundToInt (v * 100.0)) + " %";
    if (d.min == -1.0f && d.max == 1.0f)
        return (v > 0 ? "+" : "") + juce::String (juce::roundToInt (v * 100.0)) + " %";
    if (d.step >= 1.0f)
        return juce::String (juce::roundToInt (v));
    return juce::String (v, 2);
}

inline double parse (int index, const juce::String& text)
{
    const auto& d = P::defs()[(size_t) index];
    const juce::String unit = P::u8 (d.unit);
    const double v = text.retainCharacters ("0123456789.,-+").replaceCharacter (',', '.').getDoubleValue();
    if (index == P::LoopBpm && text.trim().startsWithIgnoreCase ("a"))
        return 0.0;
    if (index == P::SliceStart && text.trim().isNotEmpty() && ! juce::CharacterFunctions::isDigit (text.trim()[0]))
    {
        // A note name as Ableton writes it: C3 is 60.
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const auto t = text.trim().toUpperCase();
        for (int n = 11; n >= 0; --n)
            if (t.startsWith (names[n]))
                return (double) (n + 12 * (t.substring ((int) std::strlen (names[n])).getIntValue() + 2));
    }

    if (unit == "ms" && text.trimEnd().endsWithIgnoreCase (" s"))
        return v * 1000.0;
    if (unit == "Hz" && text.containsIgnoreCase ("k"))
        return v * 1000.0;
    if (unit.isEmpty() && ((d.min >= 0.0f && d.max <= 1.0f) || (d.min == -1.0f && d.max == 1.0f)))
        return v / 100.0;
    return v;
}
}

//==============================================================================
class Knob : public juce::Component, public juce::SettableTooltipClient
{
public:
    Knob (juce::AudioProcessorValueTreeState& state, int index, juce::String labelText = {},
          juce::Slider::SliderStyle style = juce::Slider::RotaryHorizontalVerticalDrag)
    {
        const auto& d = P::defs()[(size_t) index];
        setComponentID ("p:" + juce::String (index));      // the guides find a control by its parameter
        setTooltip (Help::forParam (index));
        slider.setTooltip (getTooltip());       // before the text box exists: it copies the tooltip
        slider.setSliderStyle (style);
        slider.setTextBoxStyle (style == juce::Slider::LinearHorizontal ? juce::Slider::TextBoxRight : juce::Slider::TextBoxBelow,
                                false, 72, 17);
        slider.setColour (juce::Slider::trackColourId, Theme::accent);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, d.id, slider);
        slider.textFromValueFunction = [index] (double v) { return Format::value (index, v); };
        slider.valueFromTextFunction = [index] (const juce::String& s) { return Format::parse (index, s); };
        slider.updateText();
        slider.setPopupDisplayEnabled (false, false, nullptr);

        // The knobs set from the sample have no fixed default to go back to.
        if (! followsSample (index))
            slider.setDoubleClickReturnValue (true, d.def);
        addAndMakeVisible (slider);

        label.setText (labelText.isNotEmpty() ? labelText : P::u8 (d.name), juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (Theme::font (12.0f));
        label.setColour (juce::Label::textColourId, Theme::dim);
        label.setMinimumHorizontalScale (0.8f);
        label.setBorderSize ({});
        label.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);
        showLabel = style != juce::Slider::LinearHorizontal;
        label.setVisible (showLabel);
    }

    void setAccent (juce::Colour c) { slider.setColour (juce::Slider::trackColourId, c); }

    // Where the modulation takes the value now (0..1 of the knob's travel), or -1: none. Only drawn: the value
    // itself, its automation and its MIDI Learn stay as they are.
    void setModulation (float position)
    {
        const auto old = slider.getProperties()["modPos"];
        const float before = old.isVoid() ? -1.0f : (float) old;
        if (std::abs (before - position) < 0.002f)
            return;
        slider.getProperties().set ("modPos", position);
        slider.repaint();
    }
    int parameterIndex() const noexcept { return getComponentID().fromFirstOccurrenceOf ("p:", false, false).getIntValue(); }

    void resized() override
    {
        auto r = getLocalBounds();
        if (showLabel)
            label.setBounds (r.removeFromTop (15));
        slider.setBounds (r);
    }

    juce::Slider slider;

private:
    static bool followsSample (int index)
    {
        return index == P::StartPos || index == P::AmpA || index == P::AmpCurve || index == P::AmpD
            || index == P::AmpS || index == P::AmpFade || index == P::AmpR;
    }

    juce::Label label;
    bool showLabel = true;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class Toggle : public juce::Component
{
public:
    Toggle (juce::AudioProcessorValueTreeState& state, int index, juce::String text = {})
    {
        const auto& d = P::defs()[(size_t) index];
        setComponentID ("p:" + juce::String (index));
        button.setButtonText (text.isNotEmpty() ? text : P::u8 (d.name));
        button.setClickingTogglesState (true);
        button.setTooltip (Help::forParam (index));
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, d.id, button);
        addAndMakeVisible (button);
    }

    void resized() override { button.setBounds (getLocalBounds().withSizeKeepingCentre (getWidth(), std::min (getHeight(), 26))); }

    juce::TextButton button;

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

// Padlock that keeps a group of settings when a new sample comes in.
class LockToggle : public juce::Button
{
public:
    LockToggle (juce::AudioProcessorValueTreeState& state, int index) : juce::Button (P::u8 (P::defs()[(size_t) index].name))
    {
        setClickingTogglesState (true);
        setTooltip (Help::forParam (index));
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, P::defs()[(size_t) index].id, *this);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const bool locked = getToggleState();
        const auto colour = locked ? Theme::accent2 : (highlighted ? Theme::text : Theme::dim);
        const auto r = getLocalBounds().toFloat().withSizeKeepingCentre (12.0f, 15.0f);

        // Shackle: closed when locked, swung open otherwise.
        juce::Path shackle;
        const float left = r.getX() + 2.0f, right = r.getRight() - 2.0f, top = r.getY(), base = r.getY() + 7.0f;
        shackle.startNewSubPath (left, base);
        shackle.lineTo (left, top + 3.0f);
        shackle.quadraticTo (left, top, (left + right) * 0.5f, top);
        shackle.quadraticTo (right, top, right, top + 3.0f);
        shackle.lineTo (right, locked ? base : top + 4.0f);
        g.setColour (colour);
        g.strokePath (shackle, juce::PathStrokeType (1.6f));
        g.fillRoundedRectangle (r.withTop (base), 2.0f);
    }

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

class Choice : public juce::Component
{
public:
    Choice (juce::AudioProcessorValueTreeState& state, int index, bool withLabel = true, juce::String labelText = {})
    {
        const auto& d = P::defs()[(size_t) index];
        setComponentID ("p:" + juce::String (index));
        box.addItemList (P::choicesOf (index), 1);
        box.setTooltip (Help::forParam (index));
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, d.id, box);
        addAndMakeVisible (box);

        label.setTooltip (box.getTooltip());
        label.setText (labelText.isNotEmpty() ? labelText : P::u8 (d.name), juce::dontSendNotification);
        label.setFont (Theme::font (12.0f));
        label.setColour (juce::Label::textColourId, Theme::dim);
        label.setJustificationType (juce::Justification::centredLeft);
        label.setBorderSize ({});
        addAndMakeVisible (label);
        label.setVisible (withLabel);
        hasLabel = withLabel;
    }

    void resized() override
    {
        const int labelH = hasLabel ? 16 : 0;
        auto r = getLocalBounds().withSizeKeepingCentre (getWidth(), std::min (getHeight(), labelH + 28));
        if (hasLabel)
            label.setBounds (r.removeFromTop (labelH));
        box.setBounds (r);
    }

    juce::ComboBox box;

private:
    juce::Label label;
    bool hasLabel = true;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
};

//==============================================================================
// Rounded panel with a title. Its controls go in rows that always fill the panel: a row
// has a fixed height or shares what is free, and its visible controls share the width.
// A row with nothing visible takes no room, so hiding controls never leaves a hole.
class Section : public juce::Component
{
public:
    explicit Section (juce::String titleText, juce::Colour colour = Theme::accent)
        : title (std::move (titleText)), accent (colour)
    {
        setComponentID ("s:" + title);                      // the guides find a panel by its title
    }

    void setTitle (const juce::String& text) { title = text; repaint(); }
    juce::Colour getAccent() const noexcept { return accent; }

    // The module's colour on a control: knob arcs and values, buttons on, a menu's arrow and outline.
    static void tintWith (juce::Component& c, juce::Colour colour)
    {
        if (auto* k = dynamic_cast<Knob*> (&c))
            k->setAccent (colour);
        else if (auto* t = dynamic_cast<Toggle*> (&c))
        {
            t->button.setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.18f));
            t->button.setColour (juce::TextButton::textColourOnId, colour);
        }
        else if (auto* ch = dynamic_cast<Choice*> (&c))
        {
            ch->box.setColour (juce::ComboBox::arrowColourId, colour);
            ch->box.setColour (juce::ComboBox::focusedOutlineColourId, colour.withAlpha (0.7f));
        }
        else if (auto* b = dynamic_cast<juce::TextButton*> (&c))
        {
            b->setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.18f));
            b->setColour (juce::TextButton::textColourOnId, colour);
        }
        else
            for (auto* child : c.getChildren())         // a display with its own knob (LFO's amount)
                tintWith (*child, colour);
    }
    void tint (juce::Component& c) { tintWith (c, accent); }
    void setAccent (juce::Colour c) { accent = c; repaint(); }

    void row (int fixedHeight = 0, float weight = 1.0f) { rows.push_back ({ fixedHeight, weight, {} }); }

    // maxHeight keeps a short control, like a button, at its size in the middle of a tall row.
    template <typename C>
    C& add (std::unique_ptr<C> c, float weight = 1.0f, int maxHeight = 0)
    {
        if (rows.empty())
            row();
        auto& ref = *c;
        addAndMakeVisible (ref);
        tint (ref);
        rows.back().items.push_back ({ std::move (c), weight, maxHeight });
        return ref;
    }

    // A control on the title line, at its right end.
    template <typename C>
    C& addToTitle (std::unique_ptr<C> c, int width)
    {
        auto& ref = *c;
        addAndMakeVisible (ref);
        tint (ref);
        titleItems.push_back ({ std::move (c), (float) width, 0 });
        return ref;
    }

    void clear() { rows.clear(); }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setGradientFill (juce::ColourGradient (Theme::panelTop, 0.0f, 0.0f, Theme::panel, 0.0f, 120.0f, false));
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (Theme::panelEdge);
        g.drawRoundedRectangle (r, 7.0f, 1.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (11.0f, 10.0f, 3.0f, 12.0f, 1.5f);
        g.setColour (Theme::text);
        g.setFont (Theme::caps (12.0f));
        g.drawText (title, 20, 7, getWidth() - 28, 18, juce::Justification::centredLeft);
    }

    void resized() override
    {
        int right = getWidth() - pad;
        for (auto& item : titleItems)
        {
            right -= (int) item.weight;
            item.comp->setBounds (right, 5, (int) item.weight, 22);
            right -= gap;
        }

        auto area = getLocalBounds().reduced (pad, 0).withTrimmedTop (titleHeight).withTrimmedBottom (pad);

        int fixed = 0, shown = 0;
        float shared = 0.0f;
        for (const auto& r : rows)
        {
            if (! isShown (r))
                continue;
            ++shown;
            fixed += r.height;
            shared += r.height > 0 ? 0.0f : r.weight;
        }

        const int spare = area.getHeight() - fixed - gap * std::max (0, shown - 1);

        for (const auto& r : rows)
        {
            if (! isShown (r))
                continue;

            auto line = area.removeFromTop (r.height > 0 ? r.height : juce::roundToInt ((float) spare * r.weight / shared));
            area.removeFromTop (gap);

            float total = 0.0f;
            int count = 0;
            for (const auto& item : r.items)
            {
                if (item.comp->isVisible())
                {
                    total += item.weight;
                    ++count;
                }
            }

            const int room = line.getWidth() - gap * (count - 1);
            for (const auto& item : r.items)
            {
                if (! item.comp->isVisible())
                    continue;

                auto cell = line.removeFromLeft (juce::roundToInt ((float) room * item.weight / total));
                line.removeFromLeft (gap);
                item.comp->setBounds (item.maxHeight > 0 ? cell.withSizeKeepingCentre (cell.getWidth(), std::min (item.maxHeight, cell.getHeight()))
                                                         : cell);
            }
        }
    }

private:
    static constexpr int pad = 9, gap = 6, titleHeight = 31;

    struct Item { std::unique_ptr<juce::Component> comp; float weight; int maxHeight; };
    struct Row { int height; float weight; std::vector<Item> items; };

    static bool isShown (const Row& r)
    {
        return std::any_of (r.items.begin(), r.items.end(), [] (const Item& i) { return i.comp->isVisible(); });
    }

    juce::String title;
    juce::Colour accent;
    std::vector<Row> rows;
    std::vector<Item> titleItems;
};
