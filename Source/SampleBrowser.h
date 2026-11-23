#pragma once

#include "PluginProcessor.h"

// A sample browser inside Reanima. Ableton cannot drop a sample from its Splice tab onto a third-party plug-in
// (Splice and Ableton document it: only onto tracks, Simpler and Sampler), so the Splice samples are reached
// from here: the folders are read from Live's own configuration (Library.cfg of the newest Live: where it puts
// what it downloads from Splice, its User Library and the user's Places, such as the Splice app's folder), never
// from a fixed path; folders of one's own can be added. A click hears a file (the preview, without touching the
// loaded sample); a double click (or Enter) loads it.
class SampleBrowser : public juce::Component, private juce::ListBoxModel, private juce::Timer
{
public:
    explicit SampleBrowser (ReanimaProcessor& p);
    ~SampleBrowser() override;

    void resized() override;
    void paint (juce::Graphics& g) override;
    void visibilityChanged() override;
    bool keyPressed (const juce::KeyPress& key) override;
    std::function<void()> onClose;

    // Where the folders come from (public for the tests).
    struct Source { juce::String name; juce::File folder; bool favourite = false, recent = false; };
    static std::vector<Source> liveSources (const juce::File& libraryCfg);
    static juce::File newestLiveLibraryCfg();
    std::vector<Source> sources;
    void openSource (int index);
    void openFolder (const juce::File& folder);
    int entryCount() const noexcept { return (int) entries.size(); }

private:
    struct Entry { juce::File file; bool folder = false; };

    int getNumRows() override { return (int) entries.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int row) override;
    void selectedRowsChanged (int row) override;
    void timerCallback() override;

    void rebuildSources();
    void showSourceMenu();
    void addFavourite();
    void listFolder();
    void startSearch (const juce::String& text);
    void startRecent();
    void activate (int row);
    void preview (int row);
    void loadRow (int row);

    ReanimaProcessor& proc;
    juce::ListBox sourceList { "fuentes" }, fileList { "archivos" };
    struct SourceModel : juce::ListBoxModel
    {
        SampleBrowser& owner;
        explicit SourceModel (SampleBrowser& o) : owner (o) {}
        int getNumRows() override { return (int) owner.sources.size() + 1; }
        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
        void listBoxItemClicked (int row, const juce::MouseEvent& e) override;
    } sourceModel { *this };

    juce::TextButton upButton { juce::String::fromUTF8 ("\xe2\x86\x91") }, closeButton { "Cerrar" }, listenButton;
    juce::TextEditor search;
    juce::Label pathLabel, status;

    juce::File folder, root;
    std::vector<Entry> entries;
    int currentSource = -1;
    bool listen = true;
    juce::String pendingSearch;
    int searchCountdown = 0;

    // Searches and the list of recent files run on a thread of their own; their result comes back by message.
    juce::ThreadPool pool { 1 };
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>> (false);
    juce::String loadedPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleBrowser)
};
