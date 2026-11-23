#pragma once

#include "PluginProcessor.h"
#include "Look.h"
#include "WaveformView.h"
#include "FxPage.h"
#include "Displays.h"
#include "EditPage.h"
#include "TabGuides.h"

// A die: a random sound (ReanimaProcessor::randomize).
class DiceButton : public juce::Button
{
public:
    DiceButton() : juce::Button ("Dado") {}
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (3.0f);
        r = r.withSizeKeepingCentre (std::min (r.getWidth(), r.getHeight()), std::min (r.getWidth(), r.getHeight()));
        g.setColour (Theme::accent.withAlpha (down ? 0.5f : over ? 0.3f : 0.15f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (over ? Theme::text : Theme::accent);
        g.drawRoundedRectangle (r, 4.0f, 1.2f);
        const float d = r.getWidth() * 0.13f;
        for (const auto [x, y] : { std::pair { 0.27f, 0.27f }, { 0.73f, 0.27f }, { 0.5f, 0.5f }, { 0.27f, 0.73f }, { 0.73f, 0.73f } })
            g.fillEllipse (r.getX() + r.getWidth() * x - d * 0.5f, r.getY() + r.getHeight() * y - d * 0.5f, d, d);
    }
};

// A heart, filled when on: a favourite preset (drawn beside each preset in the list).
void drawHeart (juce::Graphics& g, juce::Rectangle<float> area, bool on, bool over);

// Preset name with the list behind it, previous and next, and saving the current sound.
class PresetBar : public juce::Component
{
public:
    explicit PresetBar (ReanimaProcessor& p);
    void resized() override;
    void refresh();

private:
    void step (int direction);
    void showMenu();
    void askName();
    void apply (const juce::String& presetName);        // a factory or a user preset, by the name shown

public:
    // Favourites and uses: preferences of this computer (not of a project), kept by name.
    static juce::StringArray favourites();
    static void setFavourite (const juce::String& presetName, bool on);
    static void countUse (const juce::String& presetName);
    static bool autoFavourites();
    static juce::PropertiesFile& settingsFile();
    // The list of presets: FAVS first, then the factory categories, then the user's own.
    juce::PopupMenu buildMenu (const juce::StringArray& favs, const juce::Array<juce::File>& presets);
    // Deleting one of the user's presets: asks first (confirmDelete), then removePreset, which also takes
    // it out of the favourites and of the counted uses. The sound loaded stays as it is.
    void confirmDelete (const juce::File& file);
    static bool removePreset (ReanimaProcessor& p, const juce::File& file);

private:
    ReanimaProcessor& proc;
    juce::TextButton previous { P::u8 ("◀") }, next { P::u8 ("▶") }, name, save { "Guardar" };
    DiceButton dice;
    juce::String shown;
};

// A preset in the list with its own heart: a click on the heart marks or unmarks it as a favourite and
// leaves the list open; a click anywhere else loads the preset.
class PresetItem : public juce::PopupMenu::CustomComponent, public juce::SettableTooltipClient
{
public:
    // onDelete: the preset is one of the user's and can be deleted (a trash can beside the heart).
    PresetItem (juce::String presetName, juce::String shownAs, bool isCurrent, std::function<void()> deleter = {})
        : juce::PopupMenu::CustomComponent (false), preset (std::move (presetName)), label (std::move (shownAs)), current (isCurrent),
          onDelete (std::move (deleter))
    {
        setTitle (label);
        setTooltip (onDelete ? P::u8 ("Corazón: favorito (aparece en ♥ FAVS). Papelera: borra este preset tuyo, tras confirmarlo; "
                                      "los proyectos que lo usan siguen sonando igual.")
                             : P::u8 ("Pulsa el corazón para marcarlo como favorito o quitarlo. Los favoritos aparecen en la categoría ♥ FAVS "
                                      "y se guardan en este ordenador, sea cual sea el proyecto."));
    }

    void getIdealSize (int& w, int& h) override
    {
        w = textLeft + juce::GlyphArrangement::getStringWidthInt (font(), label) + 14 + heartWidth + (onDelete ? trashWidth : 0);
        h = 24;
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        const bool lit = isItemHighlighted() && ! overHeart;
        if (lit)
        {
            g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
            g.fillRect (r);
        }
        if (current)
        {
            g.setColour (Theme::accent);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ textLeft * 0.5f, r.getHeight() * 0.5f }));
        }
        g.setColour (findColour (lit ? juce::PopupMenu::highlightedTextColourId : juce::PopupMenu::textColourId));
        g.setFont (font());
        g.drawFittedText (label, r.withTrimmedLeft (textLeft).withTrimmedRight (heartWidth + (onDelete ? trashWidth : 0) + 6),
                          juce::Justification::centredLeft, 1);
        if (onDelete)
        {
            if (overTrash)
            {
                g.setColour (Theme::well);
                g.fillRoundedRectangle (trashArea().toFloat().reduced (2.0f), 4.0f);
            }
            drawTrash (g, trashArea().toFloat().reduced (8.0f, 5.0f), overTrash ? Theme::danger : Theme::dim);
        }

        const bool on = PresetBar::favourites().contains (preset);
        if (overHeart)
        {
            g.setColour (Theme::well);
            g.fillRoundedRectangle (heartArea().toFloat().reduced (2.0f), 4.0f);
        }
        drawHeart (g, heartArea().toFloat().reduced (6.0f, 5.0f), on, overHeart);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setOverHeart (heartArea().contains (e.getPosition()));
        setOverTrash (onDelete && trashArea().contains (e.getPosition()));
    }
    void mouseExit (const juce::MouseEvent&) override
    {
        setOverHeart (false);
        setOverTrash (false);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()))
            return;
        if (heartArea().contains (e.getPosition()))
        {
            PresetBar::setFavourite (preset, ! PresetBar::favourites().contains (preset));
            repaint();
        }
        else if (onDelete && trashArea().contains (e.getPosition()))
        {
            auto deleter = onDelete;            // the menu (and this item) goes away first
            juce::PopupMenu::dismissAllActiveMenus();
            juce::MessageManager::callAsync (deleter);
        }
        else
            triggerMenuItem();
    }
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::returnKey)
        {
            triggerMenuItem();
            return true;
        }
        return false;
    }

    juce::Rectangle<int> heartArea() const { return getLocalBounds().removeFromRight (heartWidth); }
    juce::Rectangle<int> trashArea() const { return getLocalBounds().withTrimmedRight (heartWidth).removeFromRight (trashWidth); }
    bool canDelete() const noexcept { return (bool) onDelete; }

private:
    static constexpr int textLeft = 22, heartWidth = 30, trashWidth = 28;
    static void drawTrash (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
    {
        const float side = std::min (r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre (side * 0.8f, side);
        juce::Path p;
        const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
        p.addLineSegment ({ x - w * 0.1f, y + h * 0.2f, x + w * 1.1f, y + h * 0.2f }, 1.0f);       // lid
        p.addLineSegment ({ x + w * 0.35f, y + h * 0.05f, x + w * 0.65f, y + h * 0.05f }, 1.0f);   // handle
        juce::Path can;
        can.startNewSubPath (x + w * 0.08f, y + h * 0.3f);
        can.lineTo (x + w * 0.18f, y + h);
        can.lineTo (x + w * 0.82f, y + h);
        can.lineTo (x + w * 0.92f, y + h * 0.3f);
        g.setColour (colour);
        g.fillPath (p);
        g.strokePath (can, juce::PathStrokeType (1.3f));
        for (const float k : { 0.38f, 0.62f })
            g.drawLine (x + w * k, y + h * 0.42f, x + w * k, y + h * 0.86f, 1.0f);
    }
    void setOverTrash (bool on)
    {
        if (on != overTrash)
        {
            overTrash = on;
            repaint();
        }
    }
    static juce::Font font() { return Theme::font (14.0f); }
    void setOverHeart (bool on)
    {
        if (on != overHeart)
        {
            overHeart = on;
            repaint();
        }
    }

    juce::String preset, label;      // the name the favourites keep, and the one shown
    bool current = false, overHeart = false, overTrash = false;
    std::function<void()> onDelete;
};

// Brand and the cards behind the preset, sample and engine controls.
class HeaderBar : public juce::Component
{
public:
    HeaderBar() { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override;
};

class ReanimaEditor : public juce::AudioProcessorEditor,
                      public juce::FileDragAndDropTarget,
                      public juce::TextDragAndDropTarget,
                      public juce::DragAndDropContainer,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    explicit ReanimaEditor (ReanimaProcessor&);
    ~ReanimaEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { waveform.setDropHighlight (true); }
    void fileDragExit (const juce::StringArray&) override { waveform.setDropHighlight (false); }
    void filesDropped (const juce::StringArray& files, int, int) override;
    // Some hosts and browsers hand over a dragged file as its path or a file:// URL in text.
    bool isInterestedInTextDrag (const juce::String& text) override { return SampleAnalysis::audioFileFromText (text) != juce::File(); }
    void textDragEnter (const juce::String&, int, int) override { waveform.setDropHighlight (true); }
    void textDragExit (const juce::String&) override { waveform.setDropHighlight (false); }
    void textDropped (const juce::String& text, int, int) override;

    static constexpr int designW = 1180, designH = 760;

    // The guides: the quick one (header "?") and the one of each tab (the "?" beside its name).
    void startQuickGuide();
    // The welcome tour, shown by itself the first time a new instance's window opens; closing it marks the
    // instance (ReanimaProcessor::welcomeShown). "No volver a mostrar" is this computer's choice: no new instance
    // opens it either; the ? button always can. Tools that open editors can turn it off.
    void startWelcome();
    static inline bool offerWelcome = true;
    static bool welcomeNever();
    static void setWelcomeNever (bool never);
    void startTabGuide (int tab);
    GuideOverlay& getGuide() noexcept { return *guide; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateModulationRings();
    void updateSampleInfo();
    void buildPages();
    void buildSamplePage();
    void buildEditPage();
    void buildFilterPage();
    void buildMatrixPage();
    void showEngineControls (int engine);
    void buildGuide();
    void openFileChooser();
    void setScale (float scale, bool remember);
    void showSizeMenu();
    static float screenFitScale();

    template <typename C, typename... Args>
    std::unique_ptr<C> make (Args&&... args) { return std::make_unique<C> (std::forward<Args> (args)...); }

    Section& addSection (juce::Component& page, juce::String title, juce::Rectangle<int> bounds, juce::Colour c = Theme::accent);

    // A control that only matters while its condition holds is dimmed the rest of the time.
    template <typename Condition>
    void dimUnless (juce::Component& control, Condition condition) { dimRules.push_back ({ &control, condition }); }

    // A control that only exists in one mode of its panel (the filter's Armónico) is shown only then, and the panel
    // lays its row out again.
    template <typename Condition>
    juce::Component& showOnly (Section& panel, juce::Component& control, Condition condition)
    {
        showRules.push_back ({ &panel, &control, condition });
        return control;
    }
    struct ShowRule { Section* panel; juce::Component* control; std::function<bool()> shown; };
    std::vector<ShowRule> showRules;

    // The parameters the waveform draws; it repaints when one of them changes.
    static constexpr int watched[] = { P::AmpA, P::AmpCurve, P::AmpD, P::AmpS, P::AmpFade, P::AmpR, P::Engine,
                                       P::SustainMode, P::SustainType, P::TransientMs, P::SnapC, P::StartPos,
                                       P::GrainPos, P::GrainMs, P::Spray, P::EnvMode, P::DroneStretch, P::DroneFreeze,
                                       P::DroneSize, P::PlayMode, P::LoopKind, P::LoopBeats, P::LoopBpm, P::SliceBy,
                                       P::SliceDivision, P::SliceSensitivity, P::SliceStart, P::LoopHitEnv, P::LoopWinLen, P::LoopWinPos };

    ReanimaProcessor& proc;
    ReanimaLook look;
    juce::TooltipWindow tooltips { this, 500 };

    juce::Component content;
    HeaderBar header;
    juce::Label sampleLabel, rootLabel;
    PresetBar presetBar;
    juce::TextButton loadButton;
    // The sample browser (Splice's folders among them), over the tabs while it is open.
    struct FolderButton : juce::Button
    {
        FolderButton() : juce::Button ("explorar") { setClickingTogglesState (true); }
        void paintButton (juce::Graphics& g, bool over, bool down) override;
    } browseButton;
    std::unique_ptr<class SampleBrowser> browser;
    void showBrowser (bool show);
    std::unique_ptr<Toggle> snapToggle;
    std::unique_ptr<Choice> engineChoice;
    OutputMeter* meter = nullptr;
    juce::TextButton* sizeButton = nullptr;
    juce::TextButton undoButton { "Deshacer" }, redoButton { "Rehacer" }, helpButton { "?" };
    juce::TextButton slotA { "A" }, slotB { "B" }, matchButton { "= nivel" };
    juce::TextButton* listenButton = nullptr;     // TIMBRE: hear everything, the note alone or Mecanismo alone
    std::unique_ptr<GuideOverlay> guide;
    std::vector<GuideOverlay::Step> quickSteps;
    TabGuides::Targets guideTargets;
    float savedScale = 0.0f;
    int scaleSettleTicks = -1;          // counts down after a drag-resize, then the size is remembered

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    juce::Component samplePage, editPage, filterPage, matrixPage, arpPage;
    void buildArpPage();
    EditView* editView = nullptr;
    std::unique_ptr<EditView> editViewOwner;
    std::unique_ptr<EditToolbar> editToolbar;
    FxPage fxPage;
    WaveformView waveform;
    std::vector<std::unique_ptr<Section>> sections;
    Section *motorPanel = nullptr, *timbrePanel = nullptr, *ampPanel = nullptr, *voicePanel = nullptr, *dronePanel = nullptr;
    DroneCurveView* droneCurve = nullptr;
    MechanismAdvice* mechAdvice = nullptr;
    Choice* warpChoice = nullptr;       // Warp of engines A to E: its Tonos says when the sample plays it as Textura
    Toggle* playToggle = nullptr;       // ONE SHOT / LOOP, in the title of MOTOR
    juce::Label* loopInfo = nullptr;    // what LOOP found and how it plays
    static constexpr int loopPanel = 6; // the controls MOTOR shows in LOOP, as if it were a seventh engine
    static constexpr int slicePanel = 7; // ... and in LOOP with Slice
    std::vector<std::unique_ptr<juce::Component>> editExtras;      // EDICIÓN: the order of the slices, the exports
    juce::String loopText() const;
    static void refreshFonts (juce::Component& c);
    std::vector<std::pair<juce::Component*, int>> engineControls;   // a control and the engines it belongs to, as bits
    std::vector<ParamView*> views;

    struct DimRule { juce::Component* control; std::function<bool()> inUse; };
    std::vector<DimRule> dimRules;

    std::unique_ptr<juce::FileChooser> chooser;
    bool wasLoading = false;
    int shownEngine = -1;
    std::array<float, std::size (watched)> lastWatched {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReanimaEditor)
};
