#include "FxPage.h"

namespace
{
constexpr int margin = 10, gap = 8, chainsHeight = 344;
const char* chainNames[3] = { "MAIN", "BUS 1", "BUS 2" };

int chainOf (int slot) { return slot < P::fxMainSlots ? 0 : 1 + (slot - P::fxMainSlots) / P::fxBusSlots; }

// Each chain keeps its colour everywhere, its effects too: MAIN blue, BUS 1 green, BUS 2 orange.
juce::Colour chainColour (int chain) { return chain == 0 ? Theme::accent : chain == 1 ? Theme::good : Theme::accent2; }
int indexOf (int slot) { return slot - FxRack::chainStart (chainOf (slot)); }
}

//==============================================================================
class FxPage::Strip : public juce::Component, public juce::DragAndDropTarget
{
public:
    Strip (FxPage& p, int s) : page (p), slot (s)
    {
        setComponentID ("fxslot:" + juce::String (s));     // the FX guide points at the first slots
        auto& st = page.getProcessor().apvts;
        typeBox.addItemList (Fx::typeNames(), 1);
        typeBox.setTooltip (Help::forFxType (Fx::Empty));
        typeBox.onChange = [this]
        {
            if (refreshing)
                return;
            page.getProcessor().setFxSlotType (slot, typeBox.getSelectedItemIndex());
            page.select (slot);
        };
        addAndMakeVisible (typeBox);

        onButton.setButtonText ("On");
        onButton.setClickingTogglesState (true);
        onButton.setTooltip (Help::forParam (P::fxOn (slot)));
        onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (st, P::defs()[(size_t) P::fxOn (slot)].id, onButton);
        addAndMakeVisible (onButton);

        mix.setTooltip (Help::forParam (P::fxMix (slot)));
        mix.setSliderStyle (juce::Slider::LinearBar);
        mix.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
        mix.setColour (juce::Slider::textBoxTextColourId, Theme::text);
        mix.setColour (juce::Slider::trackColourId, colour.withAlpha (0.55f));
        onButton.setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.22f));
        onButton.setColour (juce::TextButton::textColourOnId, colour);
        mixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (st, P::defs()[(size_t) P::fxMix (slot)].id, mix);
        mix.textFromValueFunction = [] (double v) { return "Mix " + juce::String (juce::roundToInt (v * 100.0)) + " %"; };
        mix.updateText();
        addAndMakeVisible (mix);
        refresh();
    }

    void refresh()
    {
        const int type = juce::roundToInt (page.getProcessor().paramValue (P::fxType (slot)));
        if (typeBox.getSelectedItemIndex() != type)
        {
            refreshing = true;
            typeBox.setSelectedItemIndex (type, juce::dontSendNotification);
            typeBox.setTooltip (Help::forFxType (type));
            refreshing = false;
        }
        const bool used = type != Fx::Empty;
        onButton.setVisible (used);
        mix.setVisible (used && ! Fx::isSplitter (type));
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool sel = page.getSelected() == slot;
        g.setColour (dragOver ? colour.withAlpha (0.25f) : (sel ? colour.withAlpha (0.1f) : Theme::well.withAlpha (0.35f)));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (sel ? colour : Theme::panelEdge);
        g.drawRoundedRectangle (r, 5.0f, 1.0f);

        g.setColour (Theme::dim);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 2; ++col)
                g.fillEllipse (7.0f + (float) col * 5.0f, (float) getHeight() * 0.5f - 6.0f + (float) row * 5.0f, 2.5f, 2.5f);
        g.setFont (Theme::font (12.0f));
        g.drawText (juce::String (indexOf (slot) + 1), 20, 0, 14, getHeight(), juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (5, 0);
        r.removeFromLeft (32);
        const auto line = [] (juce::Rectangle<int> cell) { return cell.withSizeKeepingCentre (cell.getWidth(), 28); };
        typeBox.setBounds (line (r.removeFromLeft (136)));
        r.removeFromLeft (6);
        onButton.setBounds (line (r.removeFromLeft (38)));
        r.removeFromLeft (6);
        mix.setBounds (line (r));
    }

    void mouseDown (const juce::MouseEvent&) override { page.select (slot); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mouseDownPosition.x > 32 || e.getDistanceFromDragStart() < 4)
            return;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            if (! container->isDragAndDropActive())
                container->startDragging (juce::var (slot), this);
    }

    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.isInt(); }
    void itemDragEnter (const SourceDetails&) override { dragOver = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { dragOver = false; repaint(); }

    void itemDropped (const SourceDetails& d) override
    {
        dragOver = false;
        const int from = (int) d.description;
        page.getProcessor().moveFxSlot (from, chainOf (slot), indexOf (slot));
        page.select (slot);
        repaint();
    }

private:
    FxPage& page;
    const int slot;
    const juce::Colour colour = chainColour (chainOf (slot));
    juce::ComboBox typeBox;
    juce::TextButton onButton;
    juce::Slider mix;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachment;
    bool refreshing = false, dragOver = false;
};

//==============================================================================
// One parameter of the selected slot: a knob, a menu or a switch depending on the
// effect's definition. The generic slot parameter stores a 0..1 value.
class FxPage::ParamControl : public juce::Component
{
public:
    ParamControl (ReanimaProcessor& p, int slot, int type, int k, const Fx::ParamSpec& s) : spec (s)
    {
        const auto colour = chainColour (chainOf (slot));
        box.setColour (juce::ComboBox::arrowColourId, colour);
        button.setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.22f));
        button.setColour (juce::TextButton::textColourOnId, colour);
        const auto* id = P::defs()[(size_t) P::fxParam (slot, k)].id;
        auto* param = p.apvts.getParameter (id);

        const auto help = Help::forFxParam (type, k);
        label.setTooltip (help);
        slider.setTooltip (help);
        box.setTooltip (help);
        button.setTooltip (help);

        label.setText (juce::String::fromUTF8 (spec.name), juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (Theme::font (13.0f));
        label.setColour (juce::Label::textColourId, Theme::dim);
        addAndMakeVisible (label);

        if (spec.kind == Fx::ParamSpec::Knob)
        {
            slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 16);
            slider.setColour (juce::Slider::trackColourId, colour);
            slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (p.apvts, id, slider);
            slider.textFromValueFunction = [this] (double v) { return Fx::format (spec, (float) v); };
            slider.valueFromTextFunction = [this] (const juce::String& t)
            {
                return (double) Fx::toNorm (spec, t.retainCharacters ("0123456789.,-").replaceCharacter (',', '.').getFloatValue());
            };
            slider.setDoubleClickReturnValue (true, Fx::toNorm (spec, spec.def));
            slider.updateText();
            addAndMakeVisible (slider);
        }
        else if (spec.kind == Fx::ParamSpec::Choice)
        {
            box.addItemList (Fx::choicesOf (spec), 1);
            attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float v)
            {
                box.setSelectedItemIndex (Fx::choiceIndex (spec, v), juce::dontSendNotification);
            });
            box.onChange = [this] { attachment->setValueAsCompleteGesture (Fx::toNorm (spec, (float) box.getSelectedItemIndex())); };
            attachment->sendInitialUpdate();
            addAndMakeVisible (box);
        }
        else
        {
            button.setButtonText (juce::String::fromUTF8 (spec.name));
            button.setClickingTogglesState (true);
            attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float v)
            {
                button.setToggleState (v > 0.5f, juce::dontSendNotification);
            });
            button.onClick = [this] { attachment->setValueAsCompleteGesture (button.getToggleState() ? 1.0f : 0.0f); };
            attachment->sendInitialUpdate();
            label.setVisible (false);
            addAndMakeVisible (button);
        }
    }

    void resized() override
    {
        // A short block in the middle of the cell, so a menu lines up with the knobs beside it.
        constexpr int knobMax = 116, labelH = 18;
        auto r = getLocalBounds();
        if (spec.kind == Fx::ParamSpec::Toggle)
        {
            button.setBounds (r.withSizeKeepingCentre (std::min (r.getWidth() - 8, 110), 28));
            return;
        }

        if (spec.kind == Fx::ParamSpec::Knob)
        {
            r = r.withSizeKeepingCentre (r.getWidth(), std::min (r.getHeight(), knobMax + labelH + 20));
            label.setBounds (r.removeFromTop (labelH));
            slider.setBounds (r);
        }
        else
        {
            r = r.withSizeKeepingCentre (std::min (r.getWidth() - 8, 150), labelH + 28);
            label.setBounds (r.removeFromTop (labelH));
            box.setBounds (r);
        }
    }

private:
    const Fx::ParamSpec spec;
    juce::Label label;
    juce::Slider slider;
    juce::ComboBox box;
    juce::TextButton button;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

//==============================================================================
FxPage::FxPage (ReanimaProcessor& p) : proc (p), editor ({}, chainColour (0)), master (p)
{
    editor.setComponentID ("fx-editor");
    auto& st = proc.apvts;

    for (int c = 0; c < 3; ++c)
    {
        chains[(size_t) c] = std::make_unique<Section> (chainNames[c], chainColour (c));
        addAndMakeVisible (*chains[(size_t) c]);
    }

    chains[0]->addToTitle (std::make_unique<Knob> (st, P::FxMainLevel, juce::String(), juce::Slider::LinearHorizontal), 190);

    const auto busRouting = [&] (Section& bus, int send, int level, int toMain, juce::Colour colour)
    {
        bus.row (84);
        bus.add (std::make_unique<Knob> (st, send, P::u8 ("Envío"))).setAccent (colour);
        bus.add (std::make_unique<Knob> (st, level, "Nivel")).setAccent (colour);
        auto& route = bus.add (std::make_unique<Toggle> (st, toMain, P::u8 ("→ Main")), 1.5f, 28);
        route.button.setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.22f));
        route.button.setColour (juce::TextButton::textColourOnId, colour);
    };
    busRouting (*chains[1], P::FxBus1Send, P::FxBus1Level, P::FxBus1ToMain, chainColour (1));
    busRouting (*chains[2], P::FxBus2Send, P::FxBus2Level, P::FxBus2ToMain, chainColour (2));

    for (int s = 0; s < P::numFxSlots; ++s)
    {
        auto& chain = *chains[(size_t) chainOf (s)];
        chain.row();
        strips.push_back (&chain.add (std::make_unique<Strip> (*this, s)));
    }

    addAndMakeVisible (editor);
    addAndMakeVisible (master);

    rebuildEditor();
    startTimerHz (10);
}

FxPage::~FxPage()
{
    stopTimer();
}

void FxPage::select (int slot)
{
    if (selected == slot)
        return;
    selected = slot;
    shownType = -1;
    rebuildEditor();
    for (auto* s : strips)
        s->repaint();
}

void FxPage::timerCallback()
{
    if (! isShowing())
        return;

    for (auto* s : strips)
        s->refresh();
    if (juce::roundToInt (proc.paramValue (P::fxType (selected))) != shownType)
        rebuildEditor();
}

void FxPage::rebuildEditor()
{
    shownType = juce::roundToInt (proc.paramValue (P::fxType (selected)));
    const auto& spec = Fx::spec (shownType);
    const int count = std::min ((int) spec.params.size(), P::fxParamsPerSlot);

    editor.clear();
    editor.setAccent (chainColour (chainOf (selected)));
    editor.setTitle (P::fxSlotName (selected) + (shownType == Fx::Empty ? juce::String() : P::u8 ("  ·  ") + juce::String::fromUTF8 (spec.name)));

    const auto note = [this] (const juce::String& text, float size)
    {
        auto label = std::make_unique<juce::Label>();
        label->setText (text, juce::dontSendNotification);
        label->setFont (Theme::font (size));
        label->setColour (juce::Label::textColourId, Theme::dim);
        label->setJustificationType (juce::Justification::centred);
        editor.add (std::move (label));
    };

    if (count == 0)
    {
        editor.row();
        note (P::u8 ("Elige un efecto en el menú de esta ranura"), 15.0f);
    }

    // Up to eight controls fit on one line; more go on two even lines.
    const int perRow = count > 8 ? (count + 1) / 2 : std::max (1, count);
    for (int k = 0; k < count; ++k)
    {
        if (k % perRow == 0)
            editor.row();
        editor.add (std::make_unique<ParamControl> (proc, selected, shownType, k, spec.params[(size_t) k]));
    }

    editor.row (20);
    note (P::u8 ("Arrastra desde ⋮⋮ para reordenar o mover entre cadenas. Envío manda la señal en paralelo a cada bus; → Main la pasa después por Main. "
                 "Tras un splitter, los efectos siguientes sólo procesan las bandas marcadas como Procesar."), 12.0f);
    editor.sendLookAndFeelChange();     // the new controls were built outside the window
    editor.resized();
}

void FxPage::resized()
{
    const int width = getWidth() - 2 * margin;
    const int chainWidth = (width - 2 * gap) / 3;

    for (int c = 0; c < 3; ++c)
        chains[(size_t) c]->setBounds (margin + c * (chainWidth + gap), gap, c == 2 ? width - 2 * (chainWidth + gap) : chainWidth, chainsHeight);

    // the selected slot's editor, and MASTER (what every chain goes through last) at its right
    const int masterWidth = 372, bottom = getHeight() - chainsHeight - 2 * gap - margin;
    editor.setBounds (margin, gap + chainsHeight + gap, width - masterWidth - gap, bottom);
    master.setBounds (margin + width - masterWidth, gap + chainsHeight + gap, masterWidth, bottom);
}
