#pragma once

#include "PluginProcessor.h"

// Waveform of the loaded sample with its detected envelope, the analysis landmarks
// (peak, sustain zone, loop, transient), the start point and the user's ADSR overlay,
// both draggable. The wheel zooms around the mouse; dragging the background scrolls.
// It is cached as an image; only the playheads redraw while notes play, on a transparent
// sibling placed on top of it.
class WaveformView : public juce::Component, public juce::TooltipClient
{
public:
    explicit WaveformView (ReanimaProcessor& p);

    class Playheads : public juce::Component
    {
    public:
        explicit Playheads (WaveformView& view);
        void update();
        void paint (juce::Graphics& g) override;

    private:
        WaveformView& owner;
        std::array<float, SynthEngine::physicalVoices> drawn {};
        std::array<int, SynthEngine::physicalVoices> drawnSlice {};
        int drawnCount = 0;
        std::vector<int> litSlices;         // sized once (128): no allocation while playing
        juce::Colour colourFor (int slice) const;
    };

    Playheads playheads { *this };

    void sampleChanged();
    void paint (juce::Graphics& g) override;
    void resized() override;
    juce::String getTooltip() override;

    void mouseMove (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseExit (const juce::MouseEvent&) override { setHover (-1); }

    void setDropHighlight (bool on) { if (dropHighlight != on) { dropHighlight = on; repaint(); } }

    // Visible part of the timeline: 1 shows everything.
    float getZoom() const noexcept { return zoom; }
    void zoomAround (double seconds, float newZoom);

private:
    enum Handle { HAttack, HDecay, HFade, HRelease, NumHandles, HStart = NumHandles, HPan, HSlice };

    struct Geometry
    {
        juce::Rectangle<float> area;
        double viewSeconds = 1.0;                   // the whole timeline
        double first = 0.0, shown = 1.0;            // the part on screen
        double startSeconds = 0.0, holdSeconds = 0.3;
        float a = 0, d = 0, s = 0, fade = 0, r = 0, curve = 1;

        float xFor (double seconds) const { return area.getX() + (float) ((seconds - first) / shown) * area.getWidth(); }
        float yFor (float level) const { return area.getBottom() - juce::jlimit (0.0f, 1.0f, level) * area.getHeight(); }
        double secondsFor (float x) const { return first + (x - area.getX()) / area.getWidth() * shown; }
        float levelFor (float y) const { return juce::jlimit (0.0f, 1.0f, (area.getBottom() - y) / area.getHeight()); }
        bool visible (float x) const { return x >= area.getX() - 1.0f && x <= area.getRight() + 1.0f; }
    };

    Geometry geometry (bool useFrozenView) const;
    juce::Rectangle<float> startTab (const Geometry& g) const;
    juce::Point<float> handlePos (const Geometry& g, int handle) const;
    int handleAt (juce::Point<float> p) const;
    void setHover (int h);
    float param (int index) const { return proc.paramValue (index); }
    void setParam (int index, float value);
    void setGesture (int handle, bool starting);
    void setStartFromMouse (const juce::MouseEvent& e, const Geometry& g);
    void buildOverview (const SampleData& s, const Geometry& g);
    void paintStart (juce::Graphics& gfx, const SampleData& s, const Geometry& g);
    void updateZoomButtons();

    ReanimaProcessor& proc;
    std::shared_ptr<const SampleData> sample;
    std::vector<std::pair<float, float>> overview;
    double overviewFirst = -1.0, overviewShown = -1.0;
    float samplePeak = 1.0f;
    int hover = -1, dragging = -1;
    double frozenView = -1.0, frozenHold = 0.3;
    float dragStartValue = 0.0f;
    double panFrom = 0.0;
    bool dropHighlight = false;

    // SLICE: the marker under the mouse (its index in the slices; the first, at 0, never moves).
    bool slicing() const;
    int sliceAt (juce::Point<float> p) const;
    int snapToHit (int at, const Geometry& g) const;
    int hoverSlice = -1, dragSlice = -1;
    std::vector<int> dragMarks;

    float zoom = 1.0f;
    double viewFirst = 0.0;
    juce::TextButton zoomOut { P::u8 ("−") }, zoomIn { "+" }, zoomAll { "Todo" };
};
