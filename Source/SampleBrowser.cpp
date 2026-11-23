#include "SampleBrowser.h"
#include "PluginEditor.h"
#include "Look.h"

namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

juce::PropertiesFile& prefs() { return PresetBar::settingsFile(); }

juce::StringArray favouriteFolders()
{
    return juce::StringArray::fromTokens (prefs().getValue ("browserFavourites"), "|", "");
}

// "Live 12.4.5" -> 12.4.5 as a number that sorts (12004005)
long long liveVersion (const juce::String& name)
{
    const auto parts = juce::StringArray::fromTokens (name.fromFirstOccurrenceOf ("Live ", false, false), ".", "");
    long long v = 0;
    for (int i = 0; i < 3; ++i)
        v = v * 1000 + (i < parts.size() ? parts[i].getIntValue() : 0);
    return v;
}
}

//==============================================================================
juce::File SampleBrowser::newestLiveLibraryCfg()
{
   #if JUCE_WINDOWS
    const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Ableton");
   #elif JUCE_MAC
    const auto base = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Preferences/Ableton");
   #else
    const juce::File base;
   #endif
    juce::File best;
    long long bestVersion = -1;
    for (const auto& dir : base.findChildFiles (juce::File::findDirectories, false, "Live *"))
    {
        auto cfg = dir.getChildFile ("Preferences").getChildFile ("Library.cfg");
        if (! cfg.existsAsFile())
            cfg = dir.getChildFile ("Library.cfg");
        if (cfg.existsAsFile() && liveVersion (dir.getFileName()) > bestVersion)
        {
            bestVersion = liveVersion (dir.getFileName());
            best = cfg;
        }
    }
    return best;
}

// From Library.cfg: where Live puts what it downloads from its Splice tab (the User Library's Samples/Splice, or a
// folder of the user's choosing), the User Library, and the folders the user added to Live's browser (Places).
std::vector<SampleBrowser::Source> SampleBrowser::liveSources (const juce::File& cfg)
{
    std::vector<Source> out;
    const auto xml = juce::XmlDocument::parse (cfg);
    if (xml == nullptr)
        return out;
    juce::File userLibrary;
    juce::String spliceMode, splicePath;
    std::vector<Source> places;
    std::function<void (const juce::XmlElement&)> walk = [&] (const juce::XmlElement& e)
    {
        if (e.hasTagName ("ProjectPath") && userLibrary == juce::File())
            userLibrary = juce::File (e.getStringAttribute ("Value"));
        else if (e.hasTagName ("SpliceDownloadFolderModeMember"))
            spliceMode = e.getStringAttribute ("Value");
        else if (e.hasTagName ("CustomSpliceDownloadPathMember"))
            splicePath = e.getStringAttribute ("Value");
        else if (e.hasTagName ("UserFolderInfo"))
            places.push_back ({ e.getStringAttribute ("DisplayName"), juce::File (e.getStringAttribute ("Path").replaceCharacter ('\\', '/')) });
        for (auto* child : e.getChildIterator())
            walk (*child);
    };
    walk (*xml);
    const auto spliceDownloads = spliceMode == "Custom" && splicePath.isNotEmpty() ? juce::File (splicePath)
                                                                                  : userLibrary.getChildFile ("Samples").getChildFile ("Splice");
    if (spliceDownloads.isDirectory())
        out.push_back ({ u8 ("Splice (descargas de Live)"), spliceDownloads });
    for (auto& p : places)
        if (p.folder.isDirectory())
        {
            if (p.name.isEmpty())
                p.name = p.folder.getFileName();
            out.push_back (p);
        }
    if (userLibrary.isDirectory())
        out.push_back ({ "User Library", userLibrary });
    return out;
}

//==============================================================================
SampleBrowser::SampleBrowser (ReanimaProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    listen = prefs().getBoolValue ("browserListen", true);

    sourceList.setModel (&sourceModel);
    sourceList.setRowHeight (24);
    sourceList.setColour (juce::ListBox::backgroundColourId, Theme::well);
    addAndMakeVisible (sourceList);

    fileList.setModel (this);
    fileList.setRowHeight (22);
    fileList.setColour (juce::ListBox::backgroundColourId, Theme::well);
    fileList.setWantsKeyboardFocus (true);
    addAndMakeVisible (fileList);

    upButton.setTooltip (u8 ("Sube a la carpeta de arriba."));
    upButton.onClick = [this] { if (folder.getParentDirectory() != folder && folder != root) openFolder (folder.getParentDirectory()); };
    addAndMakeVisible (upButton);

    search.setTextToShowWhenEmpty (u8 ("Buscar en esta fuente..."), Theme::dim);
    search.setFont (Theme::font (13.0f));
    search.onTextChange = [this] { pendingSearch = search.getText().trim(); searchCountdown = 9; };     // 0.3 s after the last key
    search.onReturnKey = [this] { searchCountdown = 1; };
    addAndMakeVisible (search);

    listenButton.setButtonText (u8 ("Escuchar"));
    listenButton.setTooltip (u8 ("Al seleccionar un archivo, se oye tal cual (sus primeros 15 s, a -6 dB), sin cambiar el sample cargado. "
                                 "Doble clic o Intro lo carga."));
    listenButton.setClickingTogglesState (true);
    listenButton.setToggleState (listen, juce::dontSendNotification);
    listenButton.onClick = [this]
    {
        listen = listenButton.getToggleState();
        prefs().setValue ("browserListen", listen);
        if (! listen)
            proc.stopPreview();
    };
    addAndMakeVisible (listenButton);

    closeButton.onClick = [this] { proc.stopPreview(); if (onClose) onClose(); };
    addAndMakeVisible (closeButton);

    for (auto* l : { &pathLabel, &status })
    {
        l->setFont (Theme::font (12.0f));
        l->setColour (juce::Label::textColourId, Theme::dim);
        l->setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (*l);
    }
    rebuildSources();
    startTimerHz (30);
}

SampleBrowser::~SampleBrowser()
{
    cancel->store (true);
    pool.removeAllJobs (true, 2000);
    proc.stopPreview();
}

void SampleBrowser::rebuildSources()
{
    sources.clear();
    for (const auto& s : liveSources (newestLiveLibraryCfg()))
        sources.push_back (s);
    // Splice first (what it is for), then its most recent files, then the rest of Live's folders.
    const auto isSplice = [] (const Source& s) { return s.name.containsIgnoreCase ("splice") || s.folder.getFullPathName().containsIgnoreCase ("splice"); };
    const auto firstOther = std::stable_partition (sources.begin(), sources.end(), isSplice);
    if (firstOther != sources.begin())
        sources.insert (firstOther, { u8 ("Recientes de Splice"), {}, false, true });
    for (const auto& f : favouriteFolders())
        if (juce::File (f).isDirectory())
            sources.push_back ({ juce::File (f).getFileName(), juce::File (f), true });
    sourceList.updateContent();
    sourceList.repaint();
    if (currentSource < 0 && ! sources.empty())
        openSource (0);
}

void SampleBrowser::visibilityChanged()
{
    if (isVisible())
    {
        rebuildSources();       // Live's folders may have changed while it was closed
        fileList.grabKeyboardFocus();
    }
    else
        proc.stopPreview();
}

void SampleBrowser::openSource (int index)
{
    if (index < 0 || index >= (int) sources.size())
        return;
    currentSource = index;
    sourceList.selectRow (index, true, true);
    search.clear();
    pendingSearch.clear();
    if (sources[(size_t) index].recent)
    {
        root = folder = juce::File();
        startRecent();
        return;
    }
    root = sources[(size_t) index].folder;
    openFolder (root);
}

void SampleBrowser::openFolder (const juce::File& f)
{
    folder = f;
    listFolder();
}

void SampleBrowser::listFolder()
{
    cancel->store (true);
    cancel = std::make_shared<std::atomic<bool>> (false);
    entries.clear();
    for (const auto& d : folder.findChildFiles (juce::File::findDirectories | juce::File::ignoreHiddenFiles, false))
        entries.push_back ({ d, true });
    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b) { return a.file.getFileName().compareNatural (b.file.getFileName()) < 0; });
    std::vector<Entry> files;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles, false))
        if (SampleAnalysis::isAudioFile (f))
            files.push_back ({ f, false });
    std::sort (files.begin(), files.end(), [] (const Entry& a, const Entry& b) { return a.file.getFileName().compareNatural (b.file.getFileName()) < 0; });
    entries.insert (entries.end(), files.begin(), files.end());
    pathLabel.setText (folder.getRelativePathFrom (root.getParentDirectory()), juce::dontSendNotification);
    status.setText (juce::String ((int) files.size()) + u8 (" archivos de audio"), juce::dontSendNotification);
    upButton.setEnabled (folder != root);
    fileList.updateContent();
    fileList.deselectAllRows();
    fileList.scrollToEnsureRowIsOnscreen (0);
    repaint();
}

// Every audio file under the source whose name has all the words, up to 500, on the pool's thread.
void SampleBrowser::startSearch (const juce::String& text)
{
    if (text.isEmpty())
    {
        if (root.isDirectory())
            listFolder();
        return;
    }
    if (! root.isDirectory())
        return;
    cancel->store (true);
    auto stop = cancel = std::make_shared<std::atomic<bool>> (false);
    status.setText (u8 ("Buscando..."), juce::dontSendNotification);
    const auto words = juce::StringArray::fromTokens (text, " ", "");
    const auto from = root;
    juce::Component::SafePointer<SampleBrowser> self (this);
    pool.addJob ([self, stop, words, from]
    {
        std::vector<juce::File> found;
        for (const auto& entry : juce::RangedDirectoryIterator (from, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles))
        {
            if (stop->load() || found.size() >= 500)
                break;
            const auto name = entry.getFile().getFileName();
            bool all = SampleAnalysis::isAudioFile (entry.getFile());
            for (const auto& w : words)
                all = all && name.containsIgnoreCase (w);
            if (all)
                found.push_back (entry.getFile());
        }
        juce::MessageManager::callAsync ([self, stop, found]
        {
            if (self == nullptr || stop->load())
                return;
            self->entries.clear();
            for (const auto& f : found)
                self->entries.push_back ({ f, false });
            self->status.setText (juce::String ((int) found.size()) + (found.size() >= 500 ? u8 (" (los primeros 500)") : juce::String())
                                  + u8 (" encontrados"), juce::dontSendNotification);
            self->fileList.updateContent();
            self->fileList.deselectAllRows();
            self->repaint();
        });
    });
}

// The newest audio files of every Splice folder (Live's downloads, the Splice app's): the last 100 brought in.
void SampleBrowser::startRecent()
{
    cancel->store (true);
    auto stop = cancel = std::make_shared<std::atomic<bool>> (false);
    std::vector<juce::File> roots;
    for (const auto& s : sources)
        if (! s.recent && (s.name.containsIgnoreCase ("splice") || s.folder.getFullPathName().containsIgnoreCase ("splice")))
            roots.push_back (s.folder);
    entries.clear();
    fileList.updateContent();
    pathLabel.setText (u8 ("Recientes de Splice"), juce::dontSendNotification);
    status.setText (u8 ("Buscando los más recientes..."), juce::dontSendNotification);
    upButton.setEnabled (false);
    juce::Component::SafePointer<SampleBrowser> self (this);
    pool.addJob ([self, stop, roots]
    {
        std::vector<std::pair<juce::Time, juce::File>> all;
        for (const auto& r : roots)
            for (const auto& entry : juce::RangedDirectoryIterator (r, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles))
            {
                if (stop->load())
                    return;
                if (SampleAnalysis::isAudioFile (entry.getFile()))
                    all.push_back ({ entry.getModificationTime(), entry.getFile() });
            }
        std::sort (all.begin(), all.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });
        if (all.size() > 100)
            all.resize (100);
        juce::MessageManager::callAsync ([self, stop, all]
        {
            if (self == nullptr || stop->load())
                return;
            self->entries.clear();
            for (const auto& [when, f] : all)
                self->entries.push_back ({ f, false });
            self->status.setText (juce::String ((int) all.size()) + u8 (" más recientes"), juce::dontSendNotification);
            self->fileList.updateContent();
            self->repaint();
        });
    });
}

void SampleBrowser::addFavourite()
{
    auto chooser = std::make_shared<juce::FileChooser> (u8 ("Añadir una carpeta de samples"), folder);
    juce::Component::SafePointer<SampleBrowser> self (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [self, chooser] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (self == nullptr || ! dir.isDirectory())
            return;
        auto list = favouriteFolders();
        list.addIfNotAlreadyThere (dir.getFullPathName());
        prefs().setValue ("browserFavourites", list.joinIntoString ("|"));
        self->rebuildSources();
        for (int i = 0; i < (int) self->sources.size(); ++i)
            if (self->sources[(size_t) i].favourite && self->sources[(size_t) i].folder == dir)
                self->openSource (i);
    });
}

void SampleBrowser::showSourceMenu()
{
    if (currentSource < 0 || currentSource >= (int) sources.size() || ! sources[(size_t) currentSource].favourite)
        return;
    juce::PopupMenu menu;
    menu.addItem (1, u8 ("Quitar de favoritas"));
    const auto which = sources[(size_t) currentSource].folder;
    juce::Component::SafePointer<SampleBrowser> self (this);
    menu.showMenuAsync ({}, [self, which] (int r)
    {
        if (r != 1 || self == nullptr)
            return;
        auto list = favouriteFolders();
        list.removeString (which.getFullPathName());
        prefs().setValue ("browserFavourites", list.joinIntoString ("|"));
        self->currentSource = -1;
        self->rebuildSources();
    });
}

void SampleBrowser::SourceModel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    const bool add = row == (int) owner.sources.size();
    if (selected && ! add)
    {
        g.setColour (Theme::accent.withAlpha (0.22f));
        g.fillRect (0, 0, width, height);
    }
    g.setFont (Theme::font (13.0f));
    if (add)
    {
        g.setColour (Theme::accent);
        g.drawText (u8 ("+ Añadir carpeta..."), 8, 0, width - 12, height, juce::Justification::centredLeft);
        return;
    }
    const auto& s = owner.sources[(size_t) row];
    g.setColour (s.name.containsIgnoreCase ("splice") ? Theme::good : Theme::text);
    g.drawText ((s.favourite ? u8 ("\xe2\x98\x85 ") : juce::String()) + s.name, 8, 0, width - 12, height, juce::Justification::centredLeft, true);
}

void SampleBrowser::SourceModel::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (row == (int) owner.sources.size())
    {
        owner.addFavourite();
        return;
    }
    owner.openSource (row);
    if (e.mods.isPopupMenu())
        owner.showSourceMenu();
}

void SampleBrowser::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= (int) entries.size())
        return;
    const auto& e = entries[(size_t) row];
    if (selected)
    {
        g.setColour (Theme::accent.withAlpha (0.25f));
        g.fillRect (0, 0, width, height);
    }
    const bool loaded = ! e.folder && e.file.getFullPathName() == loadedPath;
    g.setFont (Theme::font (13.0f));
    g.setColour (e.folder ? Theme::accent : loaded ? Theme::good : Theme::text);
    const juce::String name = e.folder ? u8 ("\xe2\x96\xb8 ") + e.file.getFileName() : e.file.getFileName();
    g.drawText (name, 8, 0, width - 90, height, juce::Justification::centredLeft, true);
    if (! e.folder)
    {
        g.setColour (Theme::dim);
        g.setFont (Theme::font (11.0f));
        const auto kb = e.file.getSize() / 1024;
        g.drawText (kb >= 1024 ? juce::String (kb / 1024.0, 1) + " MB" : juce::String (kb) + " KB", width - 84, 0, 76, height, juce::Justification::centredRight);
    }
}

void SampleBrowser::activate (int row)
{
    if (row < 0 || row >= (int) entries.size())
        return;
    if (entries[(size_t) row].folder)
        openFolder (entries[(size_t) row].file);
    else
        loadRow (row);
}

void SampleBrowser::preview (int row)
{
    if (! listen || row < 0 || row >= (int) entries.size() || entries[(size_t) row].folder)
        return;
    proc.previewFile (entries[(size_t) row].file);
}

void SampleBrowser::loadRow (int row)
{
    const auto f = entries[(size_t) row].file;
    proc.stopPreview();
    proc.loadSample (f);
    loadedPath = f.getFullPathName();
    status.setText (u8 ("Cargado: ") + f.getFileName(), juce::dontSendNotification);
    fileList.repaint();
}

void SampleBrowser::listBoxItemClicked (int row, const juce::MouseEvent&) { preview (row); }
void SampleBrowser::listBoxItemDoubleClicked (int row, const juce::MouseEvent&) { activate (row); }
void SampleBrowser::returnKeyPressed (int row) { activate (row); }
void SampleBrowser::selectedRowsChanged (int row) { preview (row); }     // the arrow keys hear each one

bool SampleBrowser::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        proc.stopPreview();
        if (onClose)
            onClose();
        return true;
    }
    if (key == juce::KeyPress::backspaceKey && ! search.hasKeyboardFocus (true))
    {
        upButton.triggerClick();
        return true;
    }
    return false;
}

void SampleBrowser::timerCallback()
{
    if (searchCountdown > 0 && --searchCountdown == 0)
        startSearch (pendingSearch);
}

void SampleBrowser::paint (juce::Graphics& g)
{
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    g.setColour (Theme::panelEdge);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 8.0f, 1.0f);
    g.setColour (Theme::text);
    g.setFont (Theme::caps (12.5f));
    g.drawText (u8 ("EXPLORADOR DE SAMPLES"), 14, 8, 260, 20, juce::Justification::centredLeft);
    g.setColour (Theme::dim);
    g.setFont (Theme::font (11.5f));
    g.drawText (u8 ("Clic: escuchar · doble clic o Intro: cargar · Esc: cerrar"), 280, 8, getWidth() - 300, 20, juce::Justification::centredRight);
    if (sources.empty())
    {
        g.drawFittedText (u8 ("No encuentro la configuración de Ableton Live. Añade tus carpetas de samples (la de Splice, por ejemplo) con «+ Añadir carpeta»."),
                          sourceList.getBounds().reduced (8).withTrimmedTop (30), juce::Justification::topLeft, 6);
    }
}

void SampleBrowser::resized()
{
    auto r = getLocalBounds().reduced (12);
    r.removeFromTop (26);
    auto bottom = r.removeFromBottom (30);
    closeButton.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (8);
    listenButton.setBounds (bottom.removeFromRight (100));
    status.setBounds (bottom);
    r.removeFromBottom (6);
    sourceList.setBounds (r.removeFromLeft (220));
    r.removeFromLeft (10);
    auto top = r.removeFromTop (28);
    upButton.setBounds (top.removeFromLeft (32));
    top.removeFromLeft (6);
    search.setBounds (top.removeFromRight (std::min (300, top.getWidth() / 2)));
    pathLabel.setBounds (top);
    r.removeFromTop (6);
    fileList.setBounds (r);
}
