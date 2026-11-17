#include "PluginEditor.h"
#include "FactoryPresets.h"
#include "Mechanism.h"

namespace
{
// The grid of the window: the pages on the left, the output on the right.
constexpr int margin = 10, gap = 8, headerHeight = 60, tabDepth = 30;
constexpr int railWidth = 78;
constexpr int railX = ReanimaEditor::designW - margin - railWidth;
constexpr int pageWidth = railX - gap - margin;
constexpr int pageHeight = ReanimaEditor::designH - headerHeight - tabDepth - margin - gap;

// The preset card takes the room from just after the brand (REANIMA and its line end near x = 120) to the sample card.
const juce::Rectangle<int> presetCard { 132, 9, 332, 42 };
const juce::Rectangle<int> sampleCard { 472, 9, 340, 42 };
const juce::Rectangle<int> engineCard { 820, 9, margin + pageWidth - 820, 42 };
constexpr int engineLabelWidth = 64;
constexpr float minScale = 0.6f, maxScale = 2.0f;

// Settings of the editor that are not part of a project, like its size.
juce::PropertiesFile& settings()
{
    static juce::PropertiesFile file ([]
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Reanima";
        o.folderName = "Blackpanda/Reanima";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        return o;
    }());
    return file;
}
}

//==============================================================================
void HeaderBar::paint (juce::Graphics& g)
{
    g.setColour (Theme::text);
    g.setFont (Theme::font (24.0f, true).withExtraKerningFactor (0.14f));
    g.drawText ("REANIMA", 13, 6, 180, 30, juce::Justification::centredLeft);

    g.setColour (Theme::accent);
    g.setFont (Theme::font (10.0f, true).withExtraKerningFactor (0.1f));
    g.drawFittedText (P::u8 ("BLACKPANDA  ·  " JucePlugin_VersionString), 14, 35, 180, 14, juce::Justification::centredLeft, 1, 0.7f);

    for (const auto card : { presetCard, sampleCard, engineCard })
    {
        g.setColour (Theme::well);
        g.fillRoundedRectangle (card.toFloat(), 7.0f);
        g.setColour (Theme::panelEdge);
        g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 7.0f, 1.0f);
    }

    g.setColour (Theme::dim);
    g.setFont (Theme::caps (11.0f));
    g.drawText ("MOTOR", engineCard.withWidth (engineLabelWidth).withTrimmedLeft (12), juce::Justification::centredLeft);
}

//==============================================================================
void drawHeart (juce::Graphics& g, juce::Rectangle<float> r, bool on, bool over)
{
    const float side = std::min (r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre (side, side * 0.9f);
    const float w = r.getWidth(), h = r.getHeight(), x = r.getX(), y = r.getY();
    juce::Path heart;
    heart.startNewSubPath (x + w * 0.5f, y + h);
    heart.cubicTo (x - w * 0.15f, y + h * 0.45f, x + w * 0.15f, y - h * 0.15f, x + w * 0.5f, y + h * 0.25f);
    heart.cubicTo (x + w * 0.85f, y - h * 0.15f, x + w * 1.15f, y + h * 0.45f, x + w * 0.5f, y + h);
    heart.closeSubPath();
    const auto colour = juce::Colour (0xffff5c7a);
    if (on)
    {
        g.setColour (colour);
        g.fillPath (heart);
    }
    g.setColour (on ? colour : (over ? Theme::text : Theme::dim));
    g.strokePath (heart, juce::PathStrokeType (1.3f));
}

namespace
{
juce::PopupMenu::Item presetItem (const juce::String& presetName, int id, bool isCurrent, juce::String shownAs = {},
                                  std::function<void()> deleter = {})
{
    if (shownAs.isEmpty())
        shownAs = presetName;
    juce::PopupMenu::Item item (shownAs);
    item.itemID = id;
    item.isTicked = isCurrent;
    item.customComponent = new PresetItem (presetName, shownAs, isCurrent, std::move (deleter));
    return item;
}
}

//==============================================================================
PresetBar::PresetBar (ReanimaProcessor& p) : proc (p)
{
    previous.setTooltip ("Preset anterior.");
    next.setTooltip ("Preset siguiente.");
    save.setTooltip (P::u8 ("Guarda el sonido actual como preset, con un nombre."));
    name.setColour (juce::TextButton::textColourOffId, Theme::text);
    previous.onClick = [this] { step (-1); };
    next.onClick = [this] { step (1); };
    name.onClick = [this] { showMenu(); };
    save.onClick = [this] { askName(); };
    dice.setTooltip (P::u8 ("Dado: un sonido al azar con motores, envolvente, timbre, filtro, modulación y efectos; se iguala de volumen "
                            "y se descarta si saldría mudo. Respeta tu sample, Snap a C y el candado de la envolvente. Deshacer lo recupera."));
    dice.onClick = [this] { proc.randomize(); refresh(); };
    for (auto* b : { &previous, &name, &next, &save })
        addAndMakeVisible (b);
    addAndMakeVisible (dice);
    refresh();
}

void PresetBar::resized()
{
    auto r = getLocalBounds();
    previous.setBounds (r.removeFromLeft (24));
    r.removeFromLeft (4);
    save.setBounds (r.removeFromRight (66));
    r.removeFromRight (4);
    dice.setBounds (r.removeFromRight (26));
    r.removeFromRight (4);
    next.setBounds (r.removeFromRight (24));
    r.removeFromRight (4);
    name.setBounds (r);
}

void PresetBar::refresh()
{
    const auto current = proc.getPresetName();
    if (current != shown || name.getButtonText().isEmpty())
    {
        if (! shown.isEmpty() || name.getButtonText().isNotEmpty())
            countUse (current);         // a preset chosen (not the one an instance opens with)
        shown = current;
        name.setButtonText (current.isNotEmpty() ? current : P::u8 ("Sin preset"));

        // A factory preset says what it is for.
        juce::String tip = P::u8 ("Presets. Los de fábrica son plantillas: cambian motor, envolvente, timbre y efectos y conservan tu sample. "
                                  "Los tuyos se guardan en Documentos/Blackpanda/Reanima/Presets con el sample, los mandos y los efectos.");
        for (int i = 0; i < (int) Factory::presets().size(); ++i)
            if (ReanimaProcessor::factoryPresetName (i) == current)
            {
                const auto& f = Factory::presets()[(size_t) i];
                tip = P::u8 (f.category) + P::u8 (" · ") + P::u8 (f.why) + P::u8 ("\nPara: ") + P::u8 (f.use);
            }
        name.setTooltip (tip);
    }
}

juce::StringArray PresetBar::favourites()
{
    return juce::StringArray::fromLines (settings().getValue ("favourites"));
}

void PresetBar::setFavourite (const juce::String& presetName, bool on)
{
    auto list = favourites();
    list.removeEmptyStrings();
    if (on)
        list.addIfNotAlreadyThere (presetName);
    else
    {
        list.removeString (presetName);
        // Taken out by hand: the automatic marking must not put it back.
        auto dismissed = juce::StringArray::fromLines (settings().getValue ("favouritesDismissed"));
        dismissed.addIfNotAlreadyThere (presetName);
        dismissed.removeEmptyStrings();
        settings().setValue ("favouritesDismissed", dismissed.joinIntoString ("\n"));
    }
    settings().setValue ("favourites", list.joinIntoString ("\n"));
    settings().saveIfNeeded();
}

juce::PropertiesFile& PresetBar::settingsFile() { return settings(); }

bool PresetBar::autoFavourites() { return settings().getBoolValue ("autoFavourites", true); }

// Every preset chosen or saved counts once here (only on this computer, nothing is sent anywhere); with
// automatic marking on, one used five times becomes a favourite, unless it was taken out by hand.
void PresetBar::countUse (const juce::String& presetName)
{
    if (presetName.isEmpty() || presetName == P::u8 ("Dado"))
        return;
    const auto key = "uses_" + juce::String::toHexString (presetName.hashCode64());
    const int uses = settings().getIntValue (key, 0) + 1;
    settings().setValue (key, uses);
    if (autoFavourites() && uses >= 5 && ! juce::StringArray::fromLines (settings().getValue ("favouritesDismissed")).contains (presetName))
    {
        auto list = favourites();
        list.removeEmptyStrings();
        list.addIfNotAlreadyThere (presetName);
        settings().setValue ("favourites", list.joinIntoString ("\n"));
    }
    settings().saveIfNeeded();
}

void PresetBar::apply (const juce::String& presetName)
{
    for (int i = 0; i < (int) Factory::presets().size(); ++i)
        if (ReanimaProcessor::factoryPresetName (i) == presetName)
        {
            proc.applyFactoryPreset (i);
            return;
        }
    for (const auto& f : proc.getPresets())
        if (f.getFileNameWithoutExtension() == presetName)
        {
            proc.loadPreset (f);
            return;
        }
}

// The factory templates come first, then the user's presets.
void PresetBar::step (int direction)
{
    const auto presets = proc.getPresets();
    const int factory = (int) Factory::presets().size(), total = factory + presets.size();

    int index = -1;
    for (int i = 0; i < total; ++i)
        if ((i < factory ? ReanimaProcessor::factoryPresetName (i) : presets[i - factory].getFileNameWithoutExtension()) == proc.getPresetName())
            index = i;
    index = index < 0 ? (direction > 0 ? 0 : total - 1) : (index + direction + total) % total;
    if (index < factory)
        proc.applyFactoryPreset (index);
    else
        proc.loadPreset (presets[index - factory]);
    refresh();
}

void PresetBar::showMenu()
{
    const auto presets = proc.getPresets();
    auto favs = favourites();
    favs.removeEmptyStrings();
    // A favourite whose preset is gone (deleted, or its file moved) is not offered.
    for (int i = favs.size(); --i >= 0;)
    {
        bool exists = false;
        for (int k = 0; k < (int) Factory::presets().size() && ! exists; ++k)
            exists = ReanimaProcessor::factoryPresetName (k) == favs[i];
        for (const auto& f : presets)
            exists = exists || f.getFileNameWithoutExtension() == favs[i];
        if (! exists)
            favs.remove (i);
    }
    auto menu = buildMenu (favs, presets);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&name), [this, presets, favs] (int result)
    {
        if (result >= 5000 && result < 5000 + favs.size())
            apply (favs[result - 5000]);
        else if (result == 4999)
        {
            settings().setValue ("autoFavourites", ! autoFavourites());
            settings().saveIfNeeded();
        }
        else if (result >= 1 && result <= presets.size())
            proc.loadPreset (presets[result - 1]);
        else if (result >= 2000 && result < 2000 + (int) Factory::presets().size())
            proc.applyFactoryPreset (result - 2000);
        else if (result == 1000)
            askName();
        else if (result == 1001)
        {
            proc.getPresetFolder().createDirectory();
            proc.getPresetFolder().startAsProcess();
        }
        refresh();
    });
}

juce::PopupMenu PresetBar::buildMenu (const juce::StringArray& favs, const juce::Array<juce::File>& presets)
{
    const auto& factory = Factory::presets();
    juce::PopupMenu menu;

    // The categories, each a submenu; the one in use is ticked, and so is its category. FAVS comes first, a
    // category like the others holding only the presets marked with the heart.
    menu.addSectionHeader (P::u8 ("CATEGORÍAS · se aplican a tu sample"));
    {
        juce::PopupMenu sub;
        bool current = false;
        for (int i = 0; i < favs.size(); ++i)
        {
            auto item = presetItem (favs[i], 5000 + i, favs[i] == proc.getPresetName());
            current = current || item.isTicked;
            sub.addItem (std::move (item));
        }
        if (favs.isEmpty())
            sub.addItem (-1, P::u8 ("Pulsa el corazón de un preset para guardarlo aquí"), false);
        sub.addSeparator();
        sub.addItem (4999, P::u8 ("Marcar automáticamente los más usados"), true, autoFavourites());
        menu.addSubMenu (P::u8 ("♥ FAVS"), sub, true, nullptr, current);
    }
    for (const auto* category : Factory::categories())
    {
        juce::PopupMenu sub;
        bool current = false;
        for (int i = 0; i < (int) factory.size(); ++i)
        {
            if (juce::String (factory[(size_t) i].category) != juce::String (category))
                continue;
            const auto presetName = ReanimaProcessor::factoryPresetName (i);
            auto item = presetItem (presetName, 2000 + i, presetName == proc.getPresetName(), P::u8 (factory[(size_t) i].name));
            current = current || item.isTicked;
            sub.addItem (std::move (item));
        }
        if (sub.getNumItems() > 0)
            menu.addSubMenu (P::u8 (category), sub, true, nullptr, current);
    }

    menu.addSectionHeader ("TUS PRESETS");
    for (int i = 0; i < presets.size(); ++i)
    {
        const auto file = presets[i];
        menu.addItem (presetItem (file.getFileNameWithoutExtension(), i + 1, file.getFileNameWithoutExtension() == proc.getPresetName(), {},
                                  [safe = juce::Component::SafePointer<PresetBar> (this), file]
                                  {
                                      if (safe != nullptr)
                                          safe->confirmDelete (file);
                                  }));
    }
    if (presets.isEmpty())
        menu.addItem (-1, P::u8 ("Todavía no has guardado ninguno"), false);
    menu.addSeparator();
    menu.addItem (1000, P::u8 ("Guardar como…"));
    menu.addItem (1001, "Abrir la carpeta de presets");

    return menu;
}

void PresetBar::confirmDelete (const juce::File& file)
{
    const auto presetName = file.getFileNameWithoutExtension();
    auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::WarningIcon, P::u8 ("Borrar preset"),
                       P::u8 ("¿Borrar el preset «") + presetName + P::u8 ("»?\n\nEl archivo va a la papelera de tu ordenador. Los proyectos que lo "
                       "usan siguen sonando igual: guardan todo su sonido dentro."), P::u8 ("Borrar"), P::u8 ("Cancelar"), this);
    juce::AlertWindow::showAsync (options, [safe = juce::Component::SafePointer<PresetBar> (this), file] (int result)
    {
        if (safe == nullptr || result != 1)
            return;
        if (! removePreset (safe->proc, file))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Reanima", P::u8 ("No se pudo borrar el preset."));
        safe->refresh();
    });
}

bool PresetBar::removePreset (ReanimaProcessor& p, const juce::File& file)
{
    const auto name = file.getFileNameWithoutExtension();
    if (! p.deletePreset (file))
        return false;
    auto list = favourites();
    list.removeString (name);
    list.removeEmptyStrings();
    settings().setValue ("favourites", list.joinIntoString ("\n"));
    auto dismissed = juce::StringArray::fromLines (settings().getValue ("favouritesDismissed"));
    dismissed.removeString (name);
    dismissed.removeEmptyStrings();
    settings().setValue ("favouritesDismissed", dismissed.joinIntoString ("\n"));
    settings().removeValue ("uses_" + juce::String::toHexString (name.hashCode64()));
    settings().saveIfNeeded();
    return true;
}

void PresetBar::askName()
{
    auto* window = new juce::AlertWindow ("Guardar preset", P::u8 ("Nombre del preset:"), juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("name", proc.getPresetName());
    window->addButton ("Guardar", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancelar", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, window] (int result)
    {
        if (result == 1 && ! proc.savePreset (window->getTextEditorContents ("name")))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Reanima", "No se pudo guardar el preset.");
        refresh();
    }), true);
}

//==============================================================================
//==============================================================================
ReanimaEditor::ReanimaEditor (ReanimaProcessor& p)
    : AudioProcessorEditor (&p), proc (p), presetBar (p), fxPage (p), waveform (p)
{
    setLookAndFeel (&look);
    auto& state = proc.apvts;

    content.addAndMakeVisible (header);

    loadButton.setButtonText ("Cargar");
    loadButton.setTooltip (P::u8 ("Abre un archivo de audio. También puedes arrastrarlo sobre la forma de onda."));
    loadButton.setColour (juce::TextButton::textColourOffId, Theme::text);
    loadButton.onClick = [this] { openFileChooser(); };
    content.addAndMakeVisible (loadButton);

    sampleLabel.setFont (Theme::font (15.0f, true));
    sampleLabel.setBorderSize ({});
    rootLabel.setFont (Theme::font (12.5f));
    rootLabel.setBorderSize ({});
    content.addAndMakeVisible (sampleLabel);
    content.addAndMakeVisible (rootLabel);

    rootLabel.setMinimumHorizontalScale (0.7f);
    sampleLabel.setMinimumHorizontalScale (0.7f);
    content.addAndMakeVisible (presetBar);

    snapToggle = make<Toggle> (state, P::SnapC, "Snap a C");
    // Snap a C is the user's choice: it stays through presets, the dice and saving (setSnapPreference).
    snapToggle->button.onClick = [this] { proc.setSnapPreference (snapToggle->button.getToggleState()); updateSampleInfo(); };
    snapToggle->button.setTooltip (P::u8 ("Afina el sample al Do más cercano. Si lo activas, se queda activado aunque cambies de preset, "
                                          "uses el dado o vuelvas a abrir el proyecto."));
    content.addAndMakeVisible (*snapToggle);

    engineChoice = make<Choice> (state, P::Engine, false);
    content.addAndMakeVisible (*engineChoice);

    tabs.setTabBarDepth (tabDepth);
    tabs.setOutline (0);
    tabs.addTab ("SAMPLE", Theme::bg, &samplePage, false);
    tabs.addTab (P::u8 ("EDICIÓN"), Theme::bg, &editPage, false);
    tabs.addTab (P::u8 ("FILTRO · MOD"), Theme::bg, &filterPage, false);
    tabs.addTab ("MATRIZ", Theme::bg, &matrixPage, false);
    tabs.addTab ("FX", Theme::bg, &fxPage, false);
    const char* tabHelp[] = { "Sample, motor, timbre, envolvente y voz.", "Recorte, fundidos, bucle, ganancia y envolvente de pitch.", "Filtro, segunda envolvente, macros y LFO.",
                              "Conecta fuentes de modulación con destinos.", "Efectos: cadena Main y dos buses en paralelo." };
    for (int i = 0; i < tabs.getNumTabs(); ++i)
    {
        auto* tab = tabs.getTabbedButtonBar().getTabButton (i);
        tab->setTooltip (P::u8 (tabHelp[i]));
        // Its own guide, beside the name: owned by the tab.
        auto* dot = new GuideDot();
        dot->setComponentID ("tabguide:" + juce::String (i));
        dot->setTooltip (P::u8 ("Guía de esta pestaña, paso a paso: Fundamentos, Práctica y Avanzado. No cambia ningún ajuste."));
        dot->onClick = [this, i]
        {
            if (guide->isRunning() && guide->tourName() == TabGuides::tabName (i))
                guide->stop();
            else
                startTabGuide (i);
        };
        tab->setExtraComponent (dot, juce::TabBarButton::afterText);
    }
    content.addAndMakeVisible (tabs);

    // Undo and redo, on the free end of the tab row: there on every page.
    undoButton.setTooltip (P::u8 ("Deshace el último cambio: un mando, un preset, una edición del sample o un sample cargado."));
    redoButton.setTooltip (P::u8 ("Vuelve a hacer lo deshecho."));
    undoButton.onClick = [this] { proc.undo(); };
    redoButton.onClick = [this] { proc.redo(); };
    helpButton.setTooltip (P::u8 ("Ayuda: el tour de bienvenida, la guía rápida y la guía de cada pestaña. Con una guía abierta, la cierra."));
    helpButton.setColour (juce::TextButton::buttonColourId, Theme::accent.withAlpha (0.3f));
    helpButton.onClick = [this]
    {
        if (guide->isRunning())
        {
            guide->stop();
            return;
        }
        juce::PopupMenu menu;
        menu.addItem (1, "Tour de bienvenida");
        menu.addItem (2, P::u8 ("Guía rápida (12 pasos)"));
        menu.addSeparator();
        for (int t = 0; t < TabGuides::numTabs; ++t)
            menu.addItem (10 + t, P::u8 ("Guía de ") + TabGuides::tabName (t));
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&helpButton), [this] (int r)
        {
            if (r == 1) startWelcome();
            else if (r == 2) startQuickGuide();
            else if (r >= 10) startTabGuide (r - 10);
        });
    };
    for (auto* b : { &undoButton, &redoButton, &helpButton })
    {
        b->setColour (juce::TextButton::textColourOffId, Theme::text);
        content.addAndMakeVisible (b);
    }

    // A/B: two settings of the sound, heard at the same loudness when "= nivel" is on.
    slotA.setTooltip (P::u8 ("Ajuste A. Pulsa B para guardar aquí lo que suena y probar otra cosa en B; vuelve a A cuando quieras comparar. No toca el sample."));
    slotB.setTooltip (P::u8 ("Ajuste B. La primera vez empieza igual que A; lo que cambies queda en B. Pulsa A y B para comparar."));
    matchButton.setTooltip (P::u8 ("Iguala la sonoridad de A y B (medida como la norma EBU R128): el más fuerte se baja al nivel del otro, nunca se sube. Así ganas por sonar mejor, no por sonar más fuerte."));
    slotA.onClick = [this] { proc.compareSelect (0); };
    slotB.onClick = [this] { proc.compareSelect (1); };
    matchButton.setClickingTogglesState (true);
    matchButton.setToggleState (proc.matchLevels.load(), juce::dontSendNotification);
    matchButton.onClick = [this] { proc.matchLevels.store (matchButton.getToggleState()); };
    for (auto* b : { &slotA, &slotB, &matchButton })
    {
        b->setColour (juce::TextButton::textColourOffId, Theme::text);
        b->setColour (juce::TextButton::buttonOnColourId, Theme::accent.withAlpha (0.55f));
        content.addAndMakeVisible (b);
    }

    buildPages();
    buildGuide();
    addAndMakeVisible (content);
    sendLookAndFeelChange();    // the controls were built before they had a parent to take it from

    proc.addChangeListener (this);

    // The size the user chose last time, or as large as fits the screen up to 100 %: on a 4K
    // display at 300 % Windows offers only 1280 x 720 points.
    setResizable (true, true);
    setResizeLimits ((int) (designW * minScale), (int) (designH * minScale), (int) (designW * maxScale), (int) (designH * maxScale));
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) designW / designH);
    savedScale = (float) settings().getDoubleValue ("uiScale", 0.0);
    setScale (savedScale > 0.0f ? savedScale : screenFitScale(), false);

    waveform.sampleChanged();
    editView->sampleChanged();
    updateSampleInfo();
    timerCallback();
    startTimerHz (30);

    // The first openings: the welcome tour, once the window is up.
    if (offerWelcome && ! proc.welcomeShown() && ! welcomeNever())
        juce::Timer::callAfterDelay (400, [safe = juce::Component::SafePointer<ReanimaEditor> (this)]
        {
            if (safe != nullptr && ! safe->guide->isRunning())
                safe->startWelcome();
        });
}

ReanimaEditor::~ReanimaEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

Section& ReanimaEditor::addSection (juce::Component& page, juce::String title, juce::Rectangle<int> bounds, juce::Colour c)
{
    sections.push_back (std::make_unique<Section> (std::move (title), c));
    auto& s = *sections.back();
    page.addAndMakeVisible (s);
    s.setBounds (bounds);
    return s;
}

void ReanimaEditor::buildPages()
{
    auto& st = proc.apvts;

    auto& rail = addSection (content, "SALIDA", { railX, margin, railWidth, designH - 2 * margin }, Theme::silver);

    // The master volume sits on the meter it moves; the rest of the output below.
    rail.row (82);
    rail.add (make<Knob> (st, P::Volume));
    rail.row();
    meter = &rail.add (make<OutputMeter>());
    rail.row (82);
    rail.add (make<Knob> (st, P::Pan));
    rail.row (82);
    rail.add (make<Knob> (st, P::MonoBass));
    rail.row (28);
    rail.add (make<Toggle> (st, P::MonoOut));
    rail.row (24);
    sizeButton = &rail.add (make<juce::TextButton> ("100 %"));
    sizeButton->setTooltip (P::u8 ("Tamaño de la ventana. También puedes arrastrar la esquina inferior derecha. Se recuerda para la próxima vez."));
    sizeButton->setColour (juce::TextButton::textColourOffId, Theme::dim);
    sizeButton->onClick = [this] { showSizeMenu(); };

    buildSamplePage();
    buildEditPage();
    buildFilterPage();
    buildMatrixPage();

    for (auto& s : sections)
        s->resized();
}

// The tour of the guide. Each step points at the real control it talks about.
void ReanimaEditor::buildGuide()
{
    const auto of = [this] (juce::Component* c) { return [this, c] { return content.getLocalArea (c, c->getLocalBounds()); }; };
    const auto fixed = [] (juce::Rectangle<int> r) { return [r] { return r; }; };
    const auto pageArea = [this] { return tabs.getBounds().withTrimmedTop (tabDepth); };
    using Step = GuideOverlay::Step;

    std::vector<Step> steps {
        { P::u8 ("Guía rápida"), P::u8 ("Reanima trata tu sample como un sintetizador: detecta su nota y su envolvente, y te deja rehacerlo "
                                        "a tu gusto. Son 12 pasos cortos. Esc la cierra; las flechas avanzan."), {}, 0 },
        { P::u8 ("1 · Carga un sample"), P::u8 ("Ya tienes un sample de demostración para practicar. Pulsa Cargar o arrastra un WAV, AIFF, FLAC, MP3 u "
                                                "OGG sobre la forma de onda para usar el tuyo. Debajo del nombre verás su nota. Snap a C lo afina al Do más cercano para que el teclado suene en tono."), fixed (sampleCard), 0 },
        { P::u8 ("2 · Prepáralo"), P::u8 ("La línea INICIO marca dónde empieza la nota: arrastra su pestaña. La curva naranja es tu ADSR y sus "
                                          "puntos también se arrastran. La rueda hace zoom."), of (&waveform), 0 },
        { P::u8 ("3 · Elige el motor"), P::u8 ("Natural: A Reconstrucción (suena como el sample) o D Híbrido (pianos y baterías: conserva el golpe).\n"
                                               "Sintético: C Resíntesis (armónicos, notas infinitas) o B Reenvolvente (pads).\n"
                                               "Creativo: E Granular (texturas) o F Drone Maker (drones y ambientes).\n"
                                               "Warp (panel MOTOR) cambia el tono sin acelerar el sonido: Tonos para notas, Textura para pads."), fixed (engineCard), 0 },
        { P::u8 ("4 · Dale forma"), P::u8 ("Ataque, Decay, Sustain y Release. Remodelar respeta el carácter del sample; Sintetizador lo vuelve "
                                           "un tono plano que tu ADSR modela como en un Minimoog. Golpe da pegada a un sample suave. "
                                           "Detectado vuelve al original."), of (ampPanel), 0 },
        { P::u8 ("5 · Hazlo real"), P::u8 ("Fuerza → brillo y Apagado hacen que responda como un instrumento. Mecanismo añade el gesto de "
                                           "un instrumento real: piano, púa, arco, tecla o soplo. El menú del título dice cuánto encaja "
                                           "cada uno con tu sample (» el recomendado). Entrada y Salida ajustan el golpe al pulsar y al "
                                           "soltar, Adaptar lo funde con el sample y On/Off lo apaga para comparar."), of (timbrePanel), 0 },
        { P::u8 ("6 · Hazlo grande"), P::u8 ("Voces y Detune engordan el sonido como en Serum. Mono y Glide para bajos y leads; Transponer y "
                                             "Fino para afinar."), of (voicePanel), 0 },
        { P::u8 ("7 · O empieza por un preset"), P::u8 ("Los presets de fábrica aplican un sonido completo a tu sample: Piano de verdad, Pad "
                                                        "infinito, 808 desde un bombo… Guardar crea los tuyos."), fixed (presetCard), 0 },
        { P::u8 ("8 · Edita el sample"), P::u8 ("Arrastra sobre la onda para seleccionar y después recorta, borra, copia o pega sin salir del "
                                                "plugin. El archivo original no se toca."), [this] { return content.getLocalArea (&editPage, editToolbar->getBounds().getUnion (editView->getBounds())); }, 1 },
        { P::u8 ("9 · Filtro y movimiento"), P::u8 ("Filtro con su envolvente, tres LFO, cuatro macros y la expresión de tu teclado (aftertouch, "
                                                    "rueda, CC11). En MATRIZ conectas cualquier fuente con cualquier destino."), pageArea, 2 },
        { P::u8 ("10 · Efectos"), P::u8 ("Una cadena Main y dos buses en paralelo, como en Serum. Arrastra las ranuras para cambiar el orden."), pageArea, 4 },
        { P::u8 ("11 · Prueba sin miedo"), P::u8 ("A y B guardan dos ajustes para compararlos; con = nivel suenan igual de fuerte, "
                                                  "así eliges por cómo suenan y no por el volumen. Deshacer y Rehacer recuperan cualquier "
                                                  "cambio. El botón ? abre esta guía cuando quieras."),
          [this] { return slotA.getBounds().getUnion (helpButton.getBounds()); }, 0 },
        { P::u8 ("12 · Termina"), P::u8 ("El Volumen empieza en -6 dB para que quepan acordes y efectos: súbelo sin que el medidor llegue al "
                                         "rojo. Al guardar el proyecto de Ableton se guarda todo, también el sample: abre igual en cualquier ordenador."),
          [this] { return guideTargets.rect ("s:SALIDA"); }, 0 },
    };

    quickSteps = std::move (steps);
    guide = std::make_unique<GuideOverlay> ([this] (int tab) { tabs.setCurrentTabIndex (tab); });
    content.addChildComponent (*guide);
    guide->setBounds (0, 0, designW, designH);

    // What the guides of the tabs point at: found among what is on screen now (the visible page, the
    // controls of the engine chosen), so a step follows its control wherever the layout puts it.
    const auto visible = [this] (juce::Component* c)
    {
        for (auto* p = c; p != nullptr && p != &content; p = p->getParentComponent())
            if (! p->isVisible())
                return false;
        return c != nullptr;
    };
    // A thing by its key: "text:<what a button says>", or its component ID.
    const auto find = [this] (const juce::String& key) -> juce::Component*
    {
        const bool byText = key.startsWith ("text:");
        const auto wanted = byText ? key.substring (5) : key;
        std::function<juce::Component* (juce::Component&)> walk = [&] (juce::Component& parent) -> juce::Component*
        {
            for (auto* c : parent.getChildren())
            {
                if (! c->isVisible() || c == guide.get())
                    continue;
                if (byText ? (dynamic_cast<juce::Button*> (c) != nullptr && dynamic_cast<juce::Button*> (c)->getButtonText() == wanted)
                           : c->getComponentID() == wanted)
                    return c;
                if (auto* found = walk (*c))
                    return found;
            }
            return nullptr;
        };
        return walk (content);
    };
    // The panel a thing belongs to: the nearest that holds it among the panels (sections), the rows of
    // the matrix, the slots of the effects and the bar of the editing tools; or the thing itself.
    const auto groupOf = [this] (juce::Component* c)
    {
        for (auto* p = c; p != nullptr && p != &content; p = p->getParentComponent())
        {
            const auto id = p->getComponentID();
            if (id.startsWith ("s:") || id.startsWith ("fxslot:") || id.startsWith ("mrow:") || id == "edit-toolbar" || id == "fx-editor")
                return p;
        }
        return c;
    };
    const auto bounds = [this] (juce::Component* c) { return c != nullptr ? content.getLocalArea (c, c->getLocalBounds()) : juce::Rectangle<int>(); };
    guideTargets.rect = [find, bounds] (const juce::String& key) { return bounds (find (key)); };
    guideTargets.group = [find, bounds, groupOf] (const juce::String& key)
    {
        auto* c = find (key);
        return c != nullptr ? bounds (groupOf (c)) : juce::Rectangle<int>();
    };
    const auto area = [this, visible] (juce::Component* c)
    {
        return [this, visible, c] { return visible (c) ? content.getLocalArea (c, c->getLocalBounds()) : juce::Rectangle<int>(); };
    };

    guideTargets.areas = {
        { "sampleCard", fixed (sampleCard) },
        { "engineCard", fixed (engineCard) },
        { "waveform", area (&waveform) },
        { "editView", area (editView) },
        { "ab", [this] { return slotA.getBounds().getUnion (matchButton.getBounds()); } },
    };
}

bool ReanimaEditor::welcomeNever() { return settings().getBoolValue ("welcomeNever", false); }

void ReanimaEditor::setWelcomeNever (bool never)
{
    settings().setValue ("welcomeNever", never);
    settings().saveIfNeeded();
}

// The first time: what is where, and how to start. Short; the guides of each tab go deeper.
void ReanimaEditor::startWelcome()
{
    using Step = GuideOverlay::Step;
    const auto fixed = [] (juce::Rectangle<int> r) { return [r] { return r; }; };
    const auto page = [this] { return tabs.getBounds().withTrimmedTop (tabDepth); };
    const auto tabButton = [this] (int i)
    {
        return [this, i]
        {
            auto* b = tabs.getTabbedButtonBar().getTabButton (i);
            return std::vector<juce::Rectangle<int>> { b != nullptr ? content.getLocalArea (b, b->getLocalBounds()) : juce::Rectangle<int>() };
        };
    };
    const char* tabText[] = {
        "SAMPLE: la forma de onda, el motor, el timbre (con Mecanismo), la envolvente y la voz. Aquí se decide cómo suena.",
        "EDICIÓN: recorta, corta, pega, funde y haz bucles en el sample sin tocar el archivo original.",
        "FILTRO · MOD: el filtro con su envolvente, tres LFO, cuatro macros y la expresión de tu teclado.",
        "MATRIZ: conecta cualquier fuente de movimiento (LFO, velocidad, rueda, azar...) con lo que quieras mover.",
        "FX: una cadena MAIN y dos buses en paralelo, con reverbs, delays, distorsiones y más." };
    std::vector<Step> steps {
        { "Bienvenido a Reanima", P::u8 ("Reanima convierte un solo sample en un instrumento. En un minuto: dónde está cada cosa y cómo empezar. "
                                         "Puedes cerrar este tour cuando quieras."), {}, 0 },
        { "Presets y dado", P::u8 ("206 presets por categorías, que se aplican a tu sample. El dado crea un sonido al azar con un clic; "
                                   "Deshacer lo recupera."), fixed (presetCard), 0 },
        { "Tu sample", P::u8 ("Ya hay un sample de demostración. Carga el tuyo con Cargar o arrastrándolo. Snap a C lo afina al Do y se "
                              "queda activado aunque cambies de preset."), fixed (sampleCard), 0 },
        { "El motor", P::u8 ("Seis formas de rehacer el sample: A suena como el original, C lo resintetiza, E lo deshace en granos, "
                             "F lo convierte en un drone..."), fixed (engineCard), 0 } };
    for (int t = 0; t < TabGuides::numTabs; ++t)
    {
        Step s { TabGuides::tabName (t), P::u8 (tabText[t]), page, t };
        s.focus = tabButton (t);
        steps.push_back (std::move (s));
    }
    steps.push_back ({ "Salida", P::u8 ("El volumen empieza en -6 dB para dejar sitio a acordes y efectos; Graves mono y Mono ordenan la imagen estéreo."),
                       [this] { return guideTargets.rect ("s:SALIDA"); }, 0 });
    steps.push_back ({ P::u8 ("Cómo empezar"), P::u8 ("Toca, prueba presets o el dado, compara con A/B y deshaz cuando quieras. El ? de cada "
                                                     "pestaña abre su guía; el ? de aquí arriba, este tour."),
                       [this] { return slotA.getBounds().getUnion (helpButton.getBounds()); }, 0 });
    guide->start (std::move (steps), "Bienvenida", true);
    guide->onFinished = [this] (bool never)
    {
        proc.setWelcomeShown();
        if (never)
            setWelcomeNever (true);
    };
}

void ReanimaEditor::startQuickGuide()
{
    guide->start (quickSteps, P::u8 ("Guía rápida"));
}

void ReanimaEditor::startTabGuide (int tab)
{
    tabs.setCurrentTabIndex (tab);
    guide->start (TabGuides::forTab (tab, guideTargets), TabGuides::tabName (tab));
}

void ReanimaEditor::buildSamplePage()
{
    auto& st = proc.apvts;
    constexpr int A = P::EngineRebuild, B = P::EngineReenvelope, C = P::EngineResynth, D = P::EngineHybrid, E = P::EngineGranular,
                  F = P::EngineDrone;

    // Each engine shows only the controls it uses.
    const auto onlyFor = [this] (juce::Component& c, std::initializer_list<int> engines) -> juce::Component&
    {
        int mask = 0;
        for (const int e : engines)
            mask |= 1 << e;
        engineControls.push_back ({ &c, mask });
        return c;
    };
    const auto sustained = [this]
    {
        // Sintetizador holds the note too.
        return (int) proc.paramValue (P::SustainType) == P::SustainHold
            || ((int) proc.paramValue (P::EnvMode) == P::EnvSynth && (int) proc.paramValue (P::Engine) <= P::EngineHybrid);
    };
    const auto granularSustain = [this] { return (int) proc.paramValue (P::SustainMode) == P::SustainGranular; };

    samplePage.addAndMakeVisible (waveform);
    samplePage.addAndMakeVisible (waveform.playheads);

    auto& motor = addSection (samplePage, "MOTOR", {});
    motorPanel = &motor;
    playToggle = &motor.addToTitle (make<Toggle> (st, P::PlayMode, "ONE SHOT"), 86);
    motor.row (46);
    // In E Warp only sets how fast the cloud travels: dimmed while it stands still.
    warpChoice = &motor.add (make<Choice> (st, P::WarpMode));
    dimUnless (onlyFor (*warpChoice, { A, B, C, D, E }),
               [this] { return (int) proc.paramValue (P::Engine) != P::EngineGranular || proc.paramValue (P::GranSpeed) != 0.0f; });
    onlyFor (motor.add (make<Choice> (st, P::SustainType)), { A, B, C, D });
    onlyFor (motor.add (make<Choice> (st, P::SustainMode)), { B, D });
    motor.row (28);
    motor.add (make<Toggle> (st, P::Reverse));
    onlyFor (motor.add (make<Toggle> (st, P::Stretch)), { B, D });
    motor.row();
    dimUnless (onlyFor (motor.add (make<Knob> (st, P::Breath)), { A, B, C, D }), sustained);
    onlyFor (motor.add (make<Knob> (st, P::Variation)), { A, B, C, D });
    onlyFor (motor.add (make<Knob> (st, P::StartPos)), { A, B, C, D });
    onlyFor (motor.add (make<Knob> (st, P::TransientMs)), { C, D });
    onlyFor (motor.add (make<Knob> (st, P::TransientGain)), { C, D });
    onlyFor (motor.add (make<Knob> (st, P::GrainMix)), { A, B, C, D });
    motor.row();
    onlyFor (motor.add (make<Knob> (st, P::GrainMs)), { B, D });
    dimUnless (onlyFor (motor.add (make<Knob> (st, P::Spray)), { B, D }), granularSustain);
    dimUnless (onlyFor (motor.add (make<Knob> (st, P::GranSpeed)), { B, D }), granularSustain);
    onlyFor (motor.add (make<Knob> (st, P::Flatten)), { B, D });
    onlyFor (motor.add (make<Knob> (st, P::GrainFade)), { B, D });
    onlyFor (motor.add (make<Knob> (st, P::GrainBlend)), { B, D });
    onlyFor (motor.add (make<Knob> (st, P::Partials)), { C });
    onlyFor (motor.add (make<Knob> (st, P::NoiseLevel)), { C });

    // Engine E: the grain cloud.
    motor.row();
    onlyFor (motor.add (make<Knob> (st, P::GrainPos)), { E, F });
    for (const int id : { P::GrainMs, P::GrainDensity, P::Spray })
        onlyFor (motor.add (make<Knob> (st, id)), { E });
    motor.row();
    for (const int id : { P::GranSpeed, P::GrainPitch, P::GrainReverse, P::GrainWidth })
        onlyFor (motor.add (make<Knob> (st, id)), { E });

    // Engine F: how the sample is read. Its spectrum has a panel of its own, beside the waveform.
    dimUnless (onlyFor (motor.add (make<Knob> (st, P::DroneStretch)), { F }), [this] { return proc.paramValue (P::DroneFreeze) < 0.5f; });
    onlyFor (motor.add (make<Knob> (st, P::DroneWidth)), { F });
    motor.row (46);
    onlyFor (motor.add (make<Choice> (st, P::WarpMode)), { F });   // F has room for it here, beside its size
    onlyFor (motor.add (make<Choice> (st, P::DroneSize)), { F });
    onlyFor (motor.add (make<Toggle> (st, P::DroneFreeze), 1.0f, 28), { F });

    // LOOP: the loop player instead of the engine: how it follows the tempo and what it found.
    // Normal: the loop round and round, warped to the project. Slice: a slice per key from Primera nota.
    const auto warping = [this] { return proc.paramValue (P::LoopWarp) > 0.5f; };
    motor.row (46);
    onlyFor (motor.add (make<Choice> (st, P::LoopKind)), { loopPanel, slicePanel });
    onlyFor (motor.add (make<Choice> (st, P::LoopBeats)), { loopPanel });
    dimUnless (onlyFor (motor.add (make<Choice> (st, P::LoopAlgo)), { loopPanel }), warping);
    onlyFor (motor.add (make<Choice> (st, P::SliceBy)), { slicePanel });
    dimUnless (onlyFor (motor.add (make<Choice> (st, P::SliceDivision)), { slicePanel }),
               [this] { return (int) proc.paramValue (P::SliceBy) == P::SliceGrid; });
    motor.row (28);
    onlyFor (motor.add (make<Toggle> (st, P::LoopWarp, "Warp")), { loopPanel });
    dimUnless (onlyFor (motor.add (make<Toggle> (st, P::LoopSync, "Tempo del host")), { loopPanel }), warping);
    onlyFor (motor.add (make<Toggle> (st, P::LoopPlay, "Play")), { loopPanel });
    onlyFor (motor.add (make<Toggle> (st, P::SliceThru, "Thru")), { slicePanel });
    onlyFor (motor.add (make<Toggle> (st, P::LoopHitEnv, "Por golpe")), { loopPanel, slicePanel });
    motor.row();
    onlyFor (motor.add (make<Knob> (st, P::LoopBpm)), { loopPanel, slicePanel });
    dimUnless (onlyFor (motor.add (make<Knob> (st, P::SliceSensitivity)), { slicePanel }),
               [this] { return (int) proc.paramValue (P::SliceBy) == P::SliceTransients; });
    dimUnless (onlyFor (motor.add (make<Knob> (st, P::SliceSensitivity)), { loopPanel }),
               [this] { return proc.paramValue (P::LoopHitEnv) > 0.5f && (int) proc.paramValue (P::SliceBy) == P::SliceTransients; });
    onlyFor (motor.add (make<Knob> (st, P::SliceStart)), { slicePanel });
    onlyFor (motor.add (make<Knob> (st, P::SliceFade)), { slicePanel });
    motor.row (44);
    auto info = make<juce::Label>();
    info->setFont (Theme::font (12.0f));
    info->setColour (juce::Label::textColourId, Theme::dim);
    info->setJustificationType (juce::Justification::topLeft);
    info->setMinimumHorizontalScale (1.0f);
    loopInfo = &motor.add (std::move (info));
    onlyFor (*loopInfo, { loopPanel, slicePanel });

    auto& spectrum = addSection (samplePage, "DRONE MAKER", {}, Theme::teal);
    dronePanel = &spectrum;
    spectrum.addToTitle (make<Toggle> (st, P::DroneCurveOn, "Filtro dibujado"), 122);
    spectrum.row (0, 1.3f);
    droneCurve = &spectrum.add (make<DroneCurveView> (proc));
    views.push_back (droneCurve);
    spectrum.row();
    for (const int id : { P::DroneHarm, P::DroneHarmWidth, P::DroneTonal, P::DroneSpread, P::DroneShift })
    {
        auto& knob = spectrum.add (make<Knob> (st, id));
        if (id == P::DroneHarmWidth)
            dimUnless (knob, [this] { return proc.paramValue (P::DroneHarm) > 0.0f; });
    }
    spectrum.row();
    for (const int id : { P::DroneSub2, P::DroneSub1, P::DroneFifth, P::DroneUp1, P::DroneUp2 })
        spectrum.add (make<Knob> (st, id));

    auto& timbre = addSection (samplePage, "TIMBRE", {}, Theme::good);
    timbrePanel = &timbre;
    auto& mechMenu = timbre.addToTitle (make<Choice> (st, P::MechType, false), 150);
    dimUnless (mechMenu, [this] { return proc.paramValue (P::RelNoise) > 0.0f && proc.paramValue (P::MechOn) > 0.5f; });
    // Five to a row: the longest names are shortened on the panel (the tooltip and the host keep them).
    const auto timbreKnob = [&] (int id, const char* label = nullptr) -> Knob&
    {
        auto& knob = timbre.add (make<Knob> (st, id, label != nullptr ? P::u8 (label) : juce::String()));
        return knob;
    };
    timbre.row();
    timbreKnob (P::VelBright, "Brillo");
    // In engine A the attack is the only thing that darkens: a slowed one.
    dimUnless (timbreKnob (P::Bow), [this]
    {
        const auto sample = proc.getSample();
        return (int) proc.paramValue (P::Engine) != P::EngineRebuild
            || (sample != nullptr && proc.paramValue (P::AmpA) * 0.001f > 3.0f * sample->detected.attack);
    });
    timbreKnob (P::RelDark);
    timbreKnob (P::Bits);
    timbreKnob (P::Downsample, "Muestreo");

    // Mecanismo: how much, the strike when the key goes down and when it comes back up, and how
    // much it takes from the sample.
    timbre.row();
    const auto mechanismOn = [this] { return proc.paramValue (P::RelNoise) > 0.0f && proc.paramValue (P::MechOn) > 0.5f; };
    dimUnless (timbreKnob (P::RelNoise), [this] { return proc.paramValue (P::MechOn) > 0.5f; });
    dimUnless (timbreKnob (P::MechDown), mechanismOn);
    dimUnless (timbreKnob (P::MechUp), mechanismOn);
    dimUnless (timbreKnob (P::MechAdapt), [this, mechanismOn] { return mechanismOn() && (int) proc.paramValue (P::MechType) != Mechanism::FromSample; });
    timbre.row (22);
    // Listening apart, to judge Mecanismo by ear: everything, the note alone, the strike alone. Not saved.
    listenButton = &timbre.add (std::make_unique<juce::TextButton> (P::u8 ("Oír todo")), 0.42f);
    listenButton->setComponentID ("listen");
    listenButton->setTooltip (P::u8 ("Para juzgar Mecanismo de oído: todo, solo la nota (sin Mecanismo) o solo el golpe "
                                     "(la nota en silencio). No se guarda en el proyecto."));
    listenButton->onClick = [this]
    {
        const int next = (proc.engine.listen.load() + 1) % 3;
        proc.engine.listen.store (next);
        const char* names[] { "Oír todo", "Oír nota", "Oír golpe" };
        listenButton->setButtonText (P::u8 (names[next]));
        listenButton->setToggleState (next != SynthEngine::ListenAll, juce::dontSendNotification);
    };
    {
        const char* names[] { "Oír todo", "Oír nota", "Oír golpe" };
        const int now = juce::jlimit (0, 2, proc.engine.listen.load());
        listenButton->setButtonText (P::u8 (names[now]));
        listenButton->setToggleState (now != SynthEngine::ListenAll, juce::dontSendNotification);
        listenButton->setColour (juce::TextButton::buttonOnColourId, Theme::good.withAlpha (0.55f));
    }
    mechAdvice = &timbre.add (std::make_unique<MechanismAdvice> (proc, mechMenu.box));

    auto& amp = addSection (samplePage, "ENVOLVENTE", {}, Theme::accent2);
    ampPanel = &amp;
    amp.addToTitle (make<LockToggle> (st, P::EnvLock), 26);
    const auto rebuildEngine = [this] { return (int) proc.paramValue (P::Engine) == P::EngineRebuild; };
    amp.row (28);
    const auto oneShot = [this] { return (int) proc.paramValue (P::PlayMode) != P::PlayLoop; };
    dimUnless (amp.add (make<Choice> (st, P::EnvMode, false), 1.7f),
               [this, oneShot] { return oneShot() && (int) proc.paramValue (P::Engine) <= P::EngineHybrid; });
    auto reset = make<juce::TextButton> ("Detectado");
    reset->setTooltip (P::u8 ("Vuelve a poner los mandos ADSR en los valores detectados en el sample. Con ellos y en modo Remodelar, el motor A toca el archivo tal cual."));
    reset->onClick = [this] { proc.resetEnvelopeToDetected(); };
    amp.add (std::move (reset));

    // In LOOP the envelope is a gate: Ataque and Release are its fades; the rest waits dimmed.
    amp.row();
    for (const int id : { P::AmpA, P::AmpD, P::AmpS, P::AmpR })
    {
        auto& knob = amp.add (make<Knob> (st, id));
        if (id == P::AmpD || id == P::AmpS)
            dimUnless (knob, oneShot);
    }
    amp.row();
    auto& curve = amp.add (make<Knob> (st, P::AmpCurve));
    dimUnless (curve, oneShot);
    auto& fade = amp.add (make<Knob> (st, P::AmpFade));
    dimUnless (fade, [this, oneShot] { return oneShot() && (int) proc.paramValue (P::SustainType) != P::SustainHold; });
    auto& punch = amp.add (make<Knob> (st, P::Punch));
    dimUnless (punch, [this, rebuildEngine, oneShot]
    {
        const auto sample = proc.getSample();
        return oneShot() && rebuildEngine() && sample != nullptr && proc.paramValue (P::AmpA) * 0.001f < sample->detected.attack;
    });
    amp.add (make<Knob> (st, P::VelAmp));

    auto& voice = addSection (samplePage, P::u8 ("VOZ · AFINACIÓN"), {}, Theme::violet);
    voicePanel = &voice;
    voice.row (46);
    voice.add (make<Choice> (st, P::PolyMode));
    voice.row();
    voice.add (make<Knob> (st, P::Glide));
    voice.add (make<Knob> (st, P::Transpose));
    voice.add (make<Knob> (st, P::Fine));
    voice.row();
    dimUnless (voice.add (make<Knob> (st, P::Unison)), oneShot);
    for (const int id : { P::UniDetune, P::UniBlend, P::UniWidth })
        dimUnless (voice.add (make<Knob> (st, id)), [this, oneShot] { return oneShot() && proc.paramValue (P::Unison) > 1.5f; });
}

namespace
{
struct MidiDrag : juce::TextButton
{
    explicit MidiDrag (ReanimaProcessor& p) : juce::TextButton (P::u8 ("EXPORTAR MIDI")), proc (p)
    {
        setTooltip (P::u8 ("Arrastra este botón a una pista MIDI de Ableton (u otro DAW): crea un clip con una nota por trozo, "
                           "en su sitio y con su duración, que vuelve a tocar el loop entero. Luego reordena, borra o copia sus "
                           "notas. Clic: guardarlo como archivo .mid."));
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || e.getDistanceFromDragStart() < 6)
            return;
        const auto file = proc.writeSliceMidi();
        if (! file.existsAsFile())
            return;
        dragging = true;
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                    [safe = juce::Component::SafePointer<MidiDrag> (this)]
                                                                    { if (safe != nullptr) safe->dragging = false; });
    }
    void clicked() override
    {
        if (dragging)
            return;
        const auto file = proc.writeSliceMidi();
        if (! file.existsAsFile())
            return;
        chooser = std::make_unique<juce::FileChooser> (P::u8 ("Guardar el clip MIDI"),
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (file.getFileName()), "*.mid");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [file] (const juce::FileChooser& fc)
                              {
                                  const auto target = fc.getResult();
                                  if (target != juce::File())
                                      file.copyFileTo (target.withFileExtension ("mid"));
                              });
    }
    ReanimaProcessor& proc;
    bool dragging = false;
    std::unique_ptr<juce::FileChooser> chooser;
};

// EDICIÓN: the slices in the order the keys play them (Primera nota upwards), one tile each: drag a tile to
// another place to change the order; double click: the file's order again. The order is what the keys, the
// MIDI clip and EXPORTAR AUDIO use.
struct SliceOrderStrip : juce::Component, juce::SettableTooltipClient, juce::Timer
{
    explicit SliceOrderStrip (ReanimaProcessor& p) : proc (p)
    {
        setTooltip (P::u8 ("Orden de los trozos (LOOP en Slice): cada casilla es una tecla, desde Primera nota, con el número del trozo "
                           "que toca. Arrastra una casilla a otro sitio para cambiar el orden; doble clic: el orden del archivo. "
                           "Las teclas, EXPORTAR MIDI y EXPORTAR AUDIO siguen este orden."));
        startTimerHz (10);
    }
    void timerCallback() override
    {
        const auto o = proc.getSliceOrder();
        const bool on = (int) proc.paramValue (P::PlayMode) == P::PlayLoop && (int) proc.paramValue (P::LoopKind) == P::LoopSlice;
        if (o != order || on != active || juce::roundToInt (proc.paramValue (P::SliceStart)) != first)
        {
            order = o;
            active = on;
            first = juce::roundToInt (proc.paramValue (P::SliceStart));
            repaint();
        }
    }
    int slotAt (float x) const
    {
        if (order.empty())
            return -1;
        return juce::jlimit (0, (int) order.size() - 1, (int) (x / ((float) getWidth() / (float) order.size())));
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (r, 4.0f);
        if (! active || order.size() < 2)
        {
            g.setColour (Theme::dim);
            g.setFont (Theme::font (12.0f));
            g.drawText (P::u8 ("ORDEN DE LOS TROZOS: pon LOOP en Slice (MOTOR) para cortarlo y ordenar sus trozos aquí."), r.reduced (8.0f, 0.0f),
                        juce::Justification::centredLeft);
            return;
        }
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const float w = r.getWidth() / (float) order.size();
        for (size_t k = 0; k < order.size(); ++k)
        {
            const auto tile = juce::Rectangle<float> (r.getX() + w * (float) k, r.getY(), w, r.getHeight()).reduced (std::min (1.0f, w * 0.1f), 2.0f);
            const bool moved = order[k] != (int) k, dragged = (int) k == from;
            g.setColour ((dragged ? Theme::accent : moved ? Theme::accent2 : Theme::good).withAlpha (dragged ? 0.55f : 0.22f));
            g.fillRoundedRectangle (tile, 2.0f);
            g.setColour (Theme::text);
            g.setFont (Theme::font (w > 34.0f ? 11.0f : 9.0f, true));
            if (w > 14.0f)
                g.drawText (juce::String (order[k] + 1), tile.withTrimmedBottom (tile.getHeight() * 0.45f), juce::Justification::centred);
            g.setColour (Theme::dim);
            g.setFont (Theme::font (9.0f));
            const int key = first + (int) k;
            if (w > 24.0f && key <= 127)
                g.drawText (juce::String (names[key % 12]) + juce::String (key / 12 - 2), tile.withTrimmedTop (tile.getHeight() * 0.5f),
                            juce::Justification::centred);
        }
        if (from >= 0 && to >= 0 && to != from)
        {
            g.setColour (Theme::accent);
            const float x = r.getX() + w * (float) (to > from ? to + 1 : to);
            g.fillRect (x - 1.5f, r.getY(), 3.0f, r.getHeight());
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        from = active ? slotAt (e.position.x) : -1;
        to = from;
        repaint();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (from < 0)
            return;
        to = slotAt (e.position.x);
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (from >= 0 && to >= 0 && to != from)
        {
            auto o = order;
            const int moved = o[(size_t) from];
            o.erase (o.begin() + from);
            o.insert (o.begin() + to, moved);
            proc.setSliceOrder (o);
        }
        from = to = -1;
        timerCallback();
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (! active)
            return;
        std::vector<int> o (order.size());
        for (size_t k = 0; k < o.size(); ++k)
            o[k] = (int) k;
        proc.setSliceOrder (o);
    }
    ReanimaProcessor& proc;
    std::vector<int> order;
    bool active = false;
    int first = 60, from = -1, to = -1;
};

// EXPORTAR AUDIO: format, rate and bits, then where; what the plug-in plays, rendered offline.
struct AudioExport : juce::TextButton
{
    explicit AudioExport (ReanimaProcessor& p) : juce::TextButton (P::u8 ("EXPORTAR AUDIO")), proc (p)
    {
        setTooltip (P::u8 ("Graba lo que suena en el plugin, con todo su proceso (motor, envolventes, filtros, efectos, buses, "
                           "modulación y salida): en Slice, los trozos en el orden de la tira, al tempo del proyecto; en Loop "
                           "Normal, un loop entero a su tempo; en One shot, la nota con su cola. Elige formato, frecuencia y bits."));
    }
    void clicked() override
    {
        auto* w = new juce::AlertWindow (P::u8 ("Exportar audio"), P::u8 ("El resultado final, tal como suena."), juce::MessageBoxIconType::NoIcon);
        w->addComboBox ("format", { "WAV", "AIFF", "FLAC" }, P::u8 ("Formato"));
        w->addComboBox ("rate", { "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz" }, P::u8 ("Frecuencia"));
        w->addComboBox ("bits", { "16 bits", "24 bits", "32 bits (coma flotante, WAV)" }, P::u8 ("Profundidad"));
        const double hostRate = proc.getSampleRate();
        w->getComboBoxComponent ("rate")->setSelectedItemIndex (hostRate > 92000.0 ? 3 : hostRate > 60000.0 ? 2 : hostRate > 46000.0 ? 1 : 0);
        w->getComboBoxComponent ("bits")->setSelectedItemIndex (1);
        w->addButton (P::u8 ("Exportar"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (P::u8 ("Cancelar"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<AudioExport> safe (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result)
        {
            if (result != 1 || safe == nullptr)
                return;
            const int format = w->getComboBoxComponent ("format")->getSelectedItemIndex();
            const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
            const double rate = rates[juce::jlimit (0, 3, w->getComboBoxComponent ("rate")->getSelectedItemIndex())];
            const int bits = w->getComboBoxComponent ("bits")->getSelectedItemIndex() == 0 ? 16 : w->getComboBoxComponent ("bits")->getSelectedItemIndex() == 1 ? 24 : 32;
            safe->save (format, rate, bits);
        }), true);
    }
    void save (int format, double rate, int bits)
    {
        const char* ext[] = { ".wav", ".aif", ".flac" };
        const auto s = proc.getSample();
        const auto name = juce::File::createLegalFileName (s != nullptr ? s->name : juce::String ("Reanima")) + " (Reanima)" + ext[format];
        chooser = std::make_unique<juce::FileChooser> (P::u8 ("Exportar audio"),
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (name),
                                                       juce::String ("*") + ext[format]);
        juce::Component::SafePointer<AudioExport> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe, format, rate, bits, e = juce::String (ext[format])] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr || fc.getResult() == juce::File())
                                      return;
                                  juce::String error;
                                  const auto target = fc.getResult().withFileExtension (e);
                                  if (! safe->proc.renderExport (target, format, rate, bits, error))
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, P::u8 ("Exportar audio"), error);
                              });
    }
    ReanimaProcessor& proc;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace

void ReanimaEditor::buildEditPage()
{
    auto& st = proc.apvts;
    constexpr int toolbarHeight = 28, viewY = gap + toolbarHeight + 6, viewHeight = 300, stripHeight = 54;
    constexpr int panelsY = viewY + viewHeight + 6 + stripHeight + gap, panelsHeight = pageHeight - panelsY;
    const auto rebuildEngine = [this] { return (int) proc.paramValue (P::Engine) == P::EngineRebuild; };

    auto view = make<EditView> (proc);
    editView = view.get();
    editPage.addAndMakeVisible (*view);
    view->setBounds (margin, viewY, pageWidth, viewHeight);
    views.push_back (view.get());
    editViewOwner = std::move (view);

    editToolbar = std::make_unique<EditToolbar> (proc, *editView);
    editToolbar->setComponentID ("edit-toolbar");
    editPage.addAndMakeVisible (*editToolbar);
    editToolbar->setBounds (margin, gap, pageWidth, toolbarHeight);

    // The order of the slices and the two exports, under the waveform.
    {
        constexpr int buttonWidth = 150;
        const int y = viewY + viewHeight + 6, stripWidth = pageWidth - 2 * (buttonWidth + gap);
        auto strip = std::make_unique<SliceOrderStrip> (proc);
        strip->setBounds (margin, y, stripWidth, stripHeight);
        editPage.addAndMakeVisible (*strip);
        auto audio = std::make_unique<AudioExport> (proc);
        audio->setBounds (margin + stripWidth + gap, y, buttonWidth, stripHeight);
        editPage.addAndMakeVisible (*audio);
        auto midi = std::make_unique<MidiDrag> (proc);
        midi->setBounds (margin + stripWidth + buttonWidth + 2 * gap, y, buttonWidth, stripHeight);
        editPage.addAndMakeVisible (*midi);
        editExtras.push_back (std::move (strip));
        editExtras.push_back (std::move (audio));
        editExtras.push_back (std::move (midi));
    }

    constexpr int trimWidth = 410, loopWidth = 330;
    auto& trim = addSection (editPage, "RECORTE Y NIVEL", { margin, panelsY, trimWidth, panelsHeight });
    trim.addToTitle (make<Toggle> (st, P::Normalize), 90);
    trim.row();
    for (const int id : { P::EndPos, P::FadeIn, P::FadeOut })
        dimUnless (trim.add (make<Knob> (st, id), 1.0f, 110), rebuildEngine);
    trim.add (make<Knob> (st, P::SampleGain), 1.0f, 110);

    auto& loop = addSection (editPage, "BUCLE", { margin + trimWidth + gap, panelsY, loopWidth, panelsHeight }, Theme::good);
    loop.addToTitle (make<Toggle> (st, P::LoopOn, "On"), 54);
    loop.row();
    for (const int id : { P::LoopStart, P::LoopEnd, P::LoopXfade })
    {
        auto& knob = loop.add (make<Knob> (st, id), 1.0f, 110);
        dimUnless (knob, [this, rebuildEngine] { return rebuildEngine() && proc.paramValue (P::LoopOn) > 0.5f; });
    }

    const int pitchX = margin + trimWidth + loopWidth + 2 * gap;
    auto& pitch = addSection (editPage, "ENVOLVENTE DE PITCH", { pitchX, panelsY, margin + pageWidth - pitchX, panelsHeight }, Theme::accent2);
    pitch.row();
    views.push_back (&pitch.add (make<PitchEnvelopeShape> (proc), 1.6f));
    pitch.add (make<Knob> (st, P::PitchEnvAmt), 1.0f, 110);
    dimUnless (pitch.add (make<Knob> (st, P::PitchEnvTime), 1.0f, 110), [this] { return proc.paramValue (P::PitchEnvAmt) != 0.0f; });
}

void ReanimaEditor::buildFilterPage()
{
    auto& st = proc.apvts;
    constexpr int top = gap, topHeight = 316, bottomY = top + topHeight + gap, bottomHeight = pageHeight - topHeight - gap;
    constexpr int env2Width = 400, filterWidth = pageWidth - env2Width - gap;

    auto& flt = addSection (filterPage, "FILTRO", { margin, top, filterWidth, topHeight });
    flt.addToTitle (make<Toggle> (st, P::FltOn, "On"), 54);
    flt.row (46);
    flt.add (make<Choice> (st, P::FltType));
    dimUnless (flt.add (make<Choice> (st, P::FltSlope)), [this]
    {
        const int type = (int) proc.paramValue (P::FltType);
        return type != P::Comb && type != P::Vowel;
    });
    flt.add (make<Choice> (st, P::FltCircuit));
    flt.row();
    views.push_back (&flt.add (make<FilterCurve> (proc)));
    flt.row (84);
    flt.add (make<Knob> (st, P::FltFreq));
    flt.add (make<Knob> (st, P::FltRes));
    dimUnless (flt.add (make<Knob> (st, P::FltDrive)), [this] { return (int) proc.paramValue (P::FltCircuit) != P::Clean; });
    dimUnless (flt.add (make<Knob> (st, P::FltMorph)), [this]
    {
        const int type = (int) proc.paramValue (P::FltType);
        return type == P::Morph || type == P::DJ || type == P::Comb || type == P::Vowel;
    });
    flt.add (make<Knob> (st, P::FltMix));
    flt.add (make<Knob> (st, P::FltKey));
    flt.add (make<Knob> (st, P::FltEnvAmt));
    flt.add (make<Knob> (st, P::FltFollow));
    for (const int id : { P::FltFolAtk, P::FltFolRel })
        dimUnless (flt.add (make<Knob> (st, id)), [this] { return proc.paramValue (P::FltFollow) != 0.0f; });

    auto& env2 = addSection (filterPage, "ENV 2", { margin + filterWidth + gap, top, env2Width, topHeight }, Theme::accent2);
    env2.row();
    views.push_back (&env2.add (make<EnvelopeShape> (proc)));
    env2.row (84);
    for (const int id : { P::E2A, P::E2D, P::E2S, P::E2R })
        env2.add (make<Knob> (st, id));

    constexpr int sideWidth = 206, lfoWidth = (pageWidth - 2 * sideWidth - 4 * gap) / 3;
    for (int l = 0; l < P::numLfos; ++l)
    {
        const int base = P::Lfo1Rate + l * P::lfoStride;
        auto& lfo = addSection (filterPage, "LFO " + juce::String (l + 1),
                                { margin + l * (lfoWidth + gap), bottomY, lfoWidth, bottomHeight },
                                l == 0 ? Theme::violet : l == 1 ? Theme::teal : Theme::warning);
        lfo.row();
        views.push_back (&lfo.add (make<LfoShape> (proc, base + 3)));
        lfo.row (46);
        lfo.add (make<Choice> (st, base + 3));
        auto& division = lfo.add (make<Choice> (st, base + 2));
        lfo.row (84);
        auto& rate = lfo.add (make<Knob> (st, base + 0));
        lfo.add (make<Toggle> (st, base + 1), 1.0f, 28);
        lfo.add (make<Toggle> (st, base + 4), 1.0f, 28);

        dimUnless (rate, [this, base] { return proc.paramValue (base + 1) < 0.5f; });
        dimUnless (division, [this, base] { return proc.paramValue (base + 1) > 0.5f; });
    }

    const int sideX = margin + 3 * (lfoWidth + gap);
    auto& macros = addSection (filterPage, "MACROS", { sideX, bottomY, sideWidth, bottomHeight }, Theme::good);
    for (const int first : { P::Macro1, P::Macro3 })
    {
        macros.row();
        macros.add (make<Knob> (st, first), 1.0f, 116);
        macros.add (make<Knob> (st, first + 1), 1.0f, 116);
    }

    // What a keyboard player expects from a real instrument.
    auto& expression = addSection (filterPage, P::u8 ("EXPRESIÓN MIDI"), { sideX + sideWidth + gap, bottomY, margin + pageWidth - sideX - sideWidth - gap, bottomHeight }, Theme::pink);
    expression.row();
    expression.add (make<Knob> (st, P::AtAmount), 1.0f, 116);
    expression.add (make<Knob> (st, P::BendRange), 1.0f, 116);
    expression.row();
    expression.add (make<Knob> (st, P::VibDepth), 1.0f, 116);
    expression.add (make<Knob> (st, P::VibRate), 1.0f, 116);
}

void ReanimaEditor::buildMatrixPage()
{
    auto& matrix = addSection (matrixPage, P::u8 ("MATRIZ DE MODULACIÓN"), { margin, gap, pageWidth, pageHeight }, Theme::good);
    matrix.row (16);
    matrix.add (make<MatrixTitles>());

    for (int slot = 0; slot < P::numSlots; ++slot)
    {
        matrix.row();
        views.push_back (&matrix.add (make<MatrixRow> (proc, slot)));
    }

    auto help = make<juce::Label>();
    help->setText (P::u8 ("Cada fila conecta una fuente con un destino. Alcance al 100 %: Tono ±24 st · Volumen ±24 dB · Frecuencia ±60 st · "
                          "Drive ±24 dB. Inicio y Spray se suman al valor del mando."),
                   juce::dontSendNotification);
    help->setFont (Theme::font (12.5f));
    help->setColour (juce::Label::textColourId, Theme::dim);
    help->setJustificationType (juce::Justification::centred);
    matrix.row (24);
    matrix.add (std::move (help));
}

// Each engine shows its own controls, and the panels share the width by what they hold.
void ReanimaEditor::showEngineControls (int engine)
{
    for (const auto& [control, engines] : engineControls)
        control->setVisible ((engines >> engine) & 1);

    // Engine F shares the top with its spectrum panel.
    constexpr int waveformHeight = 372, panelsY = gap + waveformHeight + gap, panelsHeight = pageHeight - waveformHeight - gap;
    const bool drone = engine == P::EngineDrone;
    const int waveformWidth = drone ? 500 : pageWidth;
    waveform.setBounds (margin, gap, waveformWidth, waveformHeight);
    waveform.playheads.setBounds (waveform.getBounds());
    dronePanel->setVisible (drone);
    dronePanel->setBounds (margin + waveformWidth + gap, gap, pageWidth - waveformWidth - gap, waveformHeight);
    dronePanel->resized();

    // Motor, Timbre and Envolvente; Voz takes the rest.
    const std::array<int, 3> widths = engine == P::EngineRebuild  ? std::array<int, 3> { 270, 250, 290 }
                                    : engine >= P::EngineGranular ? std::array<int, 3> { 300, 250, 260 }
                                                                  : std::array<int, 3> { 340, 230, 250 };
    int x = margin;
    int i = 0;
    for (auto* panel : { motorPanel, timbrePanel, ampPanel })
    {
        panel->setBounds (x, panelsY, widths[(size_t) i], panelsHeight);
        panel->resized();
        x += widths[(size_t) i++] + gap;
    }
    voicePanel->setBounds (x, panelsY, margin + pageWidth - x, panelsHeight);
    voicePanel->resized();
}

float ReanimaEditor::screenFitScale()
{
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    if (display == nullptr)
        return 1.0f;
    const auto room = display->userBounds;    // in points, after the system scale and the taskbar
    const float fit = std::min ((float) (room.getHeight() - 120) / designH, (float) (room.getWidth() - 60) / designW);
    return juce::jlimit (minScale, 1.0f, fit);
}

// Every label's font again at the window's scale (Theme::onScreen keeps text at least 8 px on the screen): its
// design size is kept in the label the first time.
void ReanimaEditor::refreshFonts (juce::Component& c)
{
    for (auto* child : c.getChildren())
    {
        if (auto* l = dynamic_cast<juce::Label*> (child))
        {
            auto& props = l->getProperties();
            if (! props.contains ("designFont"))
                props.set ("designFont", l->getFont().getHeight());
            const float design = (float) props["designFont"];
            l->setFont (l->getFont().withHeight (Theme::onScreen (design)));
            l->setMinimumHorizontalScale (0.75f);
        }
        refreshFonts (*child);
    }
}

void ReanimaEditor::setScale (float scale, bool remember)
{
    scale = juce::jlimit (minScale, maxScale, scale);
    setSize (juce::roundToInt (designW * scale), juce::roundToInt (designH * scale));
    if (remember)
    {
        savedScale = scale;
        settings().setValue ("uiScale", (double) scale);
        settings().saveIfNeeded();
    }
}

void ReanimaEditor::showSizeMenu()
{
    juce::PopupMenu menu;
    const float current = (float) getWidth() / designW;
    menu.addItem (1, P::u8 ("Ajustar a la pantalla"));
    menu.addSeparator();
    const int sizes[] = { 60, 75, 90, 100, 125, 150, 200 };
    for (const int s : sizes)
        menu.addItem (100 + s, juce::String (s) + " %", true, std::abs (current * 100.0f - (float) s) < 0.5f);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (sizeButton), [this] (int r)
    {
        if (r == 1)
            setScale (screenFitScale(), true);
        else if (r > 100)
            setScale ((float) (r - 100) / 100.0f, true);
    });
}

void ReanimaEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::bg);
}

void ReanimaEditor::resized()
{
    const float scale = (float) getWidth() / (float) designW;
    if (std::abs (Theme::uiScale() - scale) > 1.0e-4f)
    {
        Theme::uiScale() = scale;
        refreshFonts (content);
    }
    content.setTransform (juce::AffineTransform::scale (scale));
    content.setBounds (0, 0, designW, designH);
    if (sizeButton != nullptr)
        sizeButton->setButtonText (juce::String (juce::roundToInt (scale * 100.0f)) + " %");
    if (savedScale > 0.0f && std::abs (scale - savedScale) > 0.005f)
        scaleSettleTicks = 15;      // a drag of the corner: remembered once it stops

    header.setBounds (0, 0, railX, headerHeight);

    presetBar.setBounds (presetCard.reduced (7, 8));

    auto card = sampleCard.reduced (7, 6);
    loadButton.setBounds (card.removeFromLeft (70));
    snapToggle->setBounds (card.removeFromRight (78));
    card.reduce (10, -3);
    sampleLabel.setBounds (card.removeFromTop (card.getHeight() / 2 + 2));
    rootLabel.setBounds (card);

    engineChoice->setBounds (engineCard.withTrimmedLeft (engineLabelWidth).reduced (7, 6));
    tabs.setBounds (0, headerHeight, railX - 4, designH - headerHeight);
    helpButton.setBounds (railX - 4 - margin - 28, headerHeight + 2, 28, tabDepth - 6);
    redoButton.setBounds (helpButton.getX() - 6 - 86, headerHeight + 2, 86, tabDepth - 6);
    undoButton.setBounds (redoButton.getX() - 6 - 92, headerHeight + 2, 92, tabDepth - 6);
    matchButton.setBounds (undoButton.getX() - 14 - 104, headerHeight + 2, 104, tabDepth - 6);
    slotB.setBounds (matchButton.getX() - 4 - 30, headerHeight + 2, 30, tabDepth - 6);
    slotA.setBounds (slotB.getX() - 4 - 30, headerHeight + 2, 30, tabDepth - 6);
}

void ReanimaEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    waveform.sampleChanged();
    editView->sampleChanged();
    updateSampleInfo();
    presetBar.refresh();
    droneCurve->repaint();
}

// What LOOP found and how it plays now, for the line under its controls.
juce::String ReanimaEditor::loopText() const
{
    const auto s = proc.getSample();
    if (s == nullptr)
        return {};
    const auto& info = s->loopInfo;
    const int chosen = P::loopBeatChoices[(size_t) juce::jlimit (0, 12, (int) proc.paramValue (P::LoopBeats))];
    const int beats = chosen > 0 ? chosen : info.beats;
    const auto length = [] (int b)
    {
        if (b % 4 == 0)
            return b == 4 ? P::u8 ("1 compás") : juce::String (b / 4) + P::u8 (" compases");
        return b == 1 ? P::u8 ("1 tiempo") : juce::String (b) + P::u8 (" tiempos");
    };
    juce::String text;
    if (beats > 0)
    {
        const double seconds = (info.length > 0 ? info.length : s->length) / s->sampleRate;
        text << P::u8 ("Loop de ") << length (beats) << P::u8 (" a ") << juce::String (beats * 60.0 / seconds, 1) << " BPM";
        if (proc.paramValue (P::LoopBpm) > 0.0f)
            text = P::u8 ("Loop a ") + juce::String (proc.paramValue (P::LoopBpm), 2) + P::u8 (" BPM (BPM original escrito)");
        else if (chosen == 0)
            text << (info.fromName ? P::u8 (" (tempo del nombre)") : P::u8 (" (detectado)"));
    }
    else if (proc.paramValue (P::LoopBpm) > 0.0f)
        text << P::u8 ("Loop a ") << juce::String (proc.paramValue (P::LoopBpm), 2) << P::u8 (" BPM (BPM original escrito)");
    else
        text << P::u8 ("Sin tempo detectado: elige su Duración para que siga el proyecto.");
    const double host = proc.getHostBpm();
    const bool warp = proc.paramValue (P::LoopWarp) > 0.5f, follow = proc.paramValue (P::LoopSync) > 0.5f;
    if ((int) proc.paramValue (P::LoopKind) == P::LoopSlice)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const auto note = [] (int k) { return juce::String (names[((k % 12) + 12) % 12]) + juce::String (k / 12 - 2); };
        const int count = (int) proc.getSlices().size(), first = juce::roundToInt (proc.paramValue (P::SliceStart));
        const int last = std::min (127, first + count - 1);
        text << "\n" << juce::String (count) << (count == 1 ? P::u8 (" trozo") : P::u8 (" trozos")) << P::u8 (", de ") << note (first)
             << P::u8 (" a ") << note (last) << P::u8 (". Cada tecla toca su trozo entero.");
        // The last key received and the slice its voice took (what the engine did, not what the window guesses).
        const int key = proc.engine.lastSliceNote.load (std::memory_order_acquire);
        if (key >= 0)
        {
            const int index = proc.engine.lastSliceIndex.load();
            text << P::u8 ("\nÚltima tecla ") << note (key) << " (MIDI " << key << P::u8 (") → ")
                 << (index >= 0 ? P::u8 ("trozo ") + juce::String (index + 1) : P::u8 ("ninguno (fuera de rango)"));
        }
        return text;
    }
    text << "\n";
    if (! warp)
        text << P::u8 ("Sin Warp: como un sampler clásico, la tecla cambia tono y velocidad.");
    else if (follow && beats > 0 && host > 0.0)
        text << P::u8 ("Suena a ") << juce::String (host, 1) << P::u8 (" BPM, el tempo del proyecto. La tecla cambia sólo el tono.");
    else
        text << P::u8 ("Suena a su tempo. La tecla cambia sólo el tono.");
    if ((int) proc.paramValue (P::LoopAlgo) == P::LoopAuto)
        text << (info.rhythmic ? P::u8 ("\nAuto: Beats (tiene golpes).") : P::u8 ("\nAuto: Complex (sin golpes claros)."));
    return text;
}

void ReanimaEditor::timerCallback()
{
    meter->update (proc.takeMeterReading());

    // Each engine has its own controls; LOOP has its own instead of the engine's.
    const bool looping = (int) proc.paramValue (P::PlayMode) == P::PlayLoop;
    const int engine = ! looping ? (int) proc.paramValue (P::Engine)
                     : (int) proc.paramValue (P::LoopKind) == P::LoopSlice ? slicePanel : loopPanel;
    if (engine != shownEngine)
    {
        shownEngine = engine;
        showEngineControls (engine);
        playToggle->button.setButtonText (looping ? "LOOP" : "ONE SHOT");
        engineChoice->setAlpha (looping ? 0.38f : 1.0f);
    }
    if (looping && loopInfo != nullptr)
    {
        const auto text = loopText();
        if (text != loopInfo->getText())
            loopInfo->setText (text, juce::dontSendNotification);
    }

    for (auto* view : views)
        view->update();
    if (mechAdvice != nullptr)
        mechAdvice->update();

    for (const auto& rule : dimRules)
        rule.control->setAlpha (rule.inUse() ? 1.0f : 0.38f);

    // A sample that Tonos cannot lay out cycle by cycle plays it as Textura in A, B and D: the menu says so.
    if (warpChoice != nullptr)
    {
        const auto s = proc.getSample();
        const auto text = s != nullptr && SynthEngine::tonesFallBack (*s, engine) ? P::u8 ("Tonos → Textura") : juce::String ("Tonos");
        auto& box = warpChoice->box;
        if (box.getItemText (P::WarpTones) != text)
        {
            box.changeItemText (P::WarpTones + 1, text);
            box.setSelectedId (box.getSelectedId(), juce::dontSendNotification);
        }
    }

    waveform.playheads.update();
    editToolbar->refresh();
    undoButton.setEnabled (proc.canUndo());
    redoButton.setEnabled (proc.canRedo());
    slotA.setToggleState (proc.compareStarted() && proc.compareSlot() == 0, juce::dontSendNotification);
    slotB.setToggleState (proc.compareStarted() && proc.compareSlot() == 1, juce::dontSendNotification);
    {
        const float trim = proc.compareTrimDb();
        const auto text = proc.matchLevels.load() && trim < -0.05f ? P::u8 ("= nivel ") + juce::String (trim, 1) + " dB" : P::u8 ("= nivel");
        if (matchButton.getButtonText() != text)
            matchButton.setButtonText (text);
    }

    if (scaleSettleTicks > 0 && --scaleSettleTicks == 0)
        setScale ((float) getWidth() / designW, true);

    std::array<float, std::size (watched)> values;
    for (size_t i = 0; i < values.size(); ++i)
        values[i] = proc.paramValue (watched[i]);

    if (values != lastWatched)
    {
        waveform.repaint();
        updateSampleInfo();
        lastWatched = values;
    }

    const bool loading = proc.isLoading();
    if (loading != wasLoading)
    {
        wasLoading = loading;
        updateSampleInfo();
    }

    const auto error = proc.takeLoadError();
    if (error.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Reanima", error);
}

void ReanimaEditor::updateSampleInfo()
{
    const auto s = proc.getSample();
    rootLabel.setColour (juce::Label::textColourId, Theme::accent);

    if (proc.isLoading())
    {
        sampleLabel.setText (proc.sampleFileName(), juce::dontSendNotification);
        rootLabel.setText (P::u8 ("Analizando…"), juce::dontSendNotification);
        return;
    }

    if (proc.sampleIsMissing())
    {
        sampleLabel.setText (proc.sampleFileName(), juce::dontSendNotification);
        rootLabel.setColour (juce::Label::textColourId, Theme::accent2);
        rootLabel.setText (P::u8 ("No se encuentra el archivo"), juce::dontSendNotification);
        return;
    }

    if (s == nullptr)
    {
        sampleLabel.setText ("Sin sample", juce::dontSendNotification);
        rootLabel.setText ({}, juce::dontSendNotification);
        return;
    }

    sampleLabel.setText (s->name, juce::dontSendNotification);

    if (! s->pitched)
    {
        rootLabel.setText (P::u8 ("Sin nota clara · Do3 lo toca tal cual"), juce::dontSendNotification);
        return;
    }

    const int key = s->rootKey();
    const int cents = juce::roundToInt ((s->rootMidi - (float) key) * 100.0f);
    juce::String text = P::u8 ("Nota ") + SampleAnalysis::midiNoteName ((float) key) + " " + (cents >= 0 ? "+" : "") + juce::String (cents) + " ct";

    if (proc.paramValue (P::SnapC) > 0.5f)
        text << P::u8 ("  → ") << SampleAnalysis::midiNoteName ((float) s->nearestC())
             << juce::String::formatted (" (%+.2f st)", (double) SynthEngine::snapShift (*s, true)) << P::u8 (" en Do3");
    else
        text << P::u8 ("  · Do3 lo toca tal cual");

    rootLabel.setText (text, juce::dontSendNotification);
}

void ReanimaEditor::openFileChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Cargar sample", juce::File(), SampleAnalysis::audioWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file.existsAsFile())
                                  proc.loadSample (file);
                          });
}

// Files dragged onto any part of the window: from the explorer or Finder, from the browser of Ableton Live
// (its own library, Places and the Splice samples it lists, which are files on disk once downloaded) and from
// the Splice app. The first file it can read loads; the sound is kept inside the project, so it plays even if
// the file is later moved or was a temporary copy.
bool ReanimaEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (SampleAnalysis::isAudioFile (juce::File (f)))
            return true;
    return false;
}

void ReanimaEditor::filesDropped (const juce::StringArray& files, int, int)
{
    waveform.setDropHighlight (false);
    for (const auto& f : files)
    {
        const juce::File file (f);
        if (SampleAnalysis::isAudioFile (file))
        {
            proc.loadSample (file);
            return;
        }
    }
}

void ReanimaEditor::textDropped (const juce::String& text, int, int)
{
    waveform.setDropHighlight (false);
    const auto file = SampleAnalysis::audioFileFromText (text);
    if (file != juce::File())
        proc.loadSample (file);
}
