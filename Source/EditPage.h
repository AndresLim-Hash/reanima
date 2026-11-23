#pragma once

#include "Displays.h"

// The sample as a whole, for editing. Dragging over the waveform selects a part to cut, copy,
// silence or process; a click places the point where Pegar inserts. The lines (start, end,
// fades, loop) can be dragged; the wheel zooms around the mouse and Mayús + arrastrar scrolls.
class EditView : public ParamView
{
public:
    explicit EditView (ReanimaProcessor& p);

    void sampleChanged();
    void paint (juce::Graphics& g) override;
    void resized() override { overview.clear(); }
    juce::String getTooltip() override;

    void mouseMove (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent&) override { setHover (None); }
    bool keyPressed (const juce::KeyPress& key) override;

    // Editing, on the selection (or at the insertion point for Pegar).
    bool hasSelection() const noexcept { return sample != nullptr && selectionEnd > selectionStart; }
    void runEdit (ReanimaProcessor::Edit what, float amount = 0.0f);
    void copy();
    void selectAll();
    bool snapsToZero = true;

    // Imán: the edges (selection, lines) go to the grid while dragging; Alt lets them free. The grid is the
    // sample's beats (a loop's own, or the host's tempo from the file's start), or seconds without a tempo.
    // Saved with the project (a view setting).
    bool snapsToGrid() const;
    void setSnapsToGrid (bool on);
    // Resolution: 0 Auto (as fine as the zoom leaves room for), else one of gridChoices() (in beats).
    int gridChoice() const;
    void setGridChoice (int choice);
    struct GridChoice { const char* name; double beats; };
    static const std::vector<GridChoice>& gridChoices();
    // The grid now: its step (in samples, > 0), where it starts, whether it counts beats, a name ("1/16", "10 ms").
    struct Grid { double step = 0.0, origin = 0.0, beat = 0.0; bool musical = false; juce::String name; };
    Grid grid() const;
    // A position (fraction of the file) put on the nearest grid line (exactly on a sample).
    double snapped (double fraction) const;
    // For the tests: the view's zoom and its width as the grid sees them.
    void setViewForTests (double firstFraction, double shownFraction) { first = firstFraction; shown = shownFraction; }
    double selectionStartForTests() const { return selectionStart; }
    double selectionEndForTests() const { return selectionEnd; }
    std::function<void()> onMagnetChanged;      // the toolbar's button follows the G key

private:
    enum Marker { None = -1, Start, End, LoopStart, LoopEnd, FadeInEnd, FadeOutStart, Pan, Select, SelStart, SelEnd, Hit, NumMarkers };
    // LOOP: the cuts along the bottom of the waveform (the strip), as in SAMPLE: a click turns one off or on, a
    // double click in the strip adds one.
    juce::Rectangle<float> hitStrip() const;
    int hitAt (juce::Point<float> p, bool& isCut) const;
    int hoverHit = -1;
    bool hoverHitIsCut = false;
    double beatSamples() const;         // samples per beat, 0 without a tempo
    double place (double fraction, const juce::MouseEvent& e) const;    // snapped unless Alt (or the magnet is off)

    juce::Rectangle<float> area() const;
    float xFor (double fraction) const;
    double fractionFor (float x) const;
    double markerFraction (int m) const;
    int markerAt (const juce::MouseEvent& e) const;
    void setHover (int m);
    void setFraction (int m, double fraction);
    void setValue (int index, float value);
    void buildOverview();
    double zeroCrossingNear (double fraction) const;

    std::shared_ptr<const SampleData> sample;
    std::vector<std::pair<float, float>> overview;
    double overviewFirst = -1.0, overviewShown = -1.0;
    float peak = 1.0f;
    double first = 0.0, shown = 1.0;        // the visible part of the file, as fractions
    double panFrom = 0.0;
    int hover = None, dragging = None;

    double selectionStart = 0.0, selectionEnd = 0.0, anchor = 0.0, insertAt = -1.0;     // fractions
};

// The edit commands, over the waveform of EDICIÓN.
class EditToolbar : public juce::Component
{
public:
    EditToolbar (ReanimaProcessor& p, EditView& view);
    void resized() override;
    void refresh();

private:
    juce::TextButton& add (const juce::String& text, const char* tip, std::function<void()> action, float weight = 1.0f);

    ReanimaProcessor& proc;
    EditView& view;
    std::vector<std::pair<std::unique_ptr<juce::TextButton>, float>> buttons;
    juce::TextButton *trim = nullptr, *remove = nullptr, *silence = nullptr, *copyButton = nullptr, *cut = nullptr, *paste = nullptr,
                     *reverse = nullptr, *fadeIn = nullptr, *fadeOut = nullptr, *normalize = nullptr, *gain = nullptr,
                     *seamless = nullptr, *zero = nullptr, *magnet = nullptr, *gridMenu = nullptr;
};

// Pitch envelope as a curve: where the note starts and how it comes back.
class PitchEnvelopeShape : public ParamView
{
public:
    explicit PitchEnvelopeShape (ReanimaProcessor& p);
    void paint (juce::Graphics& g) override;
};
