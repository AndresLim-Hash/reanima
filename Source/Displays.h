#pragma once

#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"
#include "Filters.h"
#include "Look.h"

// A view of some parameters that redraws only when one of them changes.
class ParamView : public juce::Component, public juce::SettableTooltipClient
{
public:
    ParamView (ReanimaProcessor& p, std::vector<int> watchedParams)
        : proc (p), watched (std::move (watchedParams)), last (watched.size(), -1.0e9f) {}

    void update();

protected:
    virtual void refresh() {}
    float value (int index) const { return proc.paramValue (index); }

    ReanimaProcessor& proc;

private:
    std::vector<int> watched;
    std::vector<float> last;
};

// Stereo output meter: the solid bar has VU ballistics, the pale bar above it is the
// instantaneous peak, the line holds the last peak. Clicking clears the clip lights.
class OutputMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    OutputMeter();

    void update (const ReanimaProcessor::MeterReading& reading);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent&) override;

    float vuDb (int channel) const { return levels[(size_t) channel].vu; }
    float peakDb (int channel) const { return levels[(size_t) channel].peak; }

private:
    struct Level
    {
        float rms = 0.0f, vu = minDb, peak = minDb, hold = minDb, holdSeconds = 0.0f;
        bool clipped = false;
    };

    static constexpr float minDb = -60.0f, maxDb = 6.0f;

    juce::Rectangle<float> barArea (int channel) const;
    float yFor (float db) const;

    std::array<Level, 2> levels;
    float highest = minDb;
    double lastUpdate = 0.0;
    juce::String drawnState;
};

// The real response of the filter: an impulse through the same code the voices use.
class FilterCurve : public ParamView
{
public:
    explicit FilterCurve (ReanimaProcessor& p);
    void paint (juce::Graphics& g) override;

private:
    void refresh() override { stale = true; }
    void measure();

    static constexpr int fftOrder = 13, fftSize = 1 << fftOrder, numPoints = 240;
    static constexpr double rate = 44100.0;

    VoiceFilter filter;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> work = std::vector<float> ((size_t) fftSize * 2);
    std::array<float, numPoints> response {};
    bool stale = true;      // measured when painted, so a hidden page costs nothing
};

class EnvelopeShape : public ParamView
{
public:
    explicit EnvelopeShape (ReanimaProcessor& p);
    void paint (juce::Graphics& g) override;
};

class LfoShape : public ParamView
{
public:
    LfoShape (ReanimaProcessor& p, int shapeParam);
    void paint (juce::Graphics& g) override;

private:
    const int shape;
};

// Engine F's drawn filter: a gain from 20 Hz to 20 kHz, drawn with the mouse. Double click
// makes it flat again. Drawing switches it on.
class DroneCurveView : public ParamView
{
public:
    explicit DroneCurveView (ReanimaProcessor& p);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

private:
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (8.0f, 8.0f); }
    float xFor (float hz) const;
    float yFor (float db) const;
    void drawTo (juce::Point<float> p);

    juce::Point<float> last;
};

// One line of the modulation matrix. It lights up when it connects something.
class MatrixRow : public ParamView
{
public:
    MatrixRow (ReanimaProcessor& p, int slot);
    void paint (juce::Graphics& g) override;
    void resized() override;

    // Shared with the titles above the columns.
    static constexpr int numberWidth = 42, boxWidth = 300, arrowWidth = 40, margin = 10;

private:
    const int slot, base;
    Choice source, destination;
    Knob amount;
};

// Under TIMBRE: how well each gesture of Mecanismo suits the sample and the attack it is
// played with, written in the menu next to each one, the one that suits best, and what speaks
// against the chosen one. With the On/Off of Mecanismo, to compare.
class MechanismAdvice : public juce::Component, public juce::SettableTooltipClient
{
public:
    MechanismAdvice (ReanimaProcessor& p, juce::ComboBox& typeMenu);
    ~MechanismAdvice() override;
    void update();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void refreshMenuText();

    ReanimaProcessor& proc;
    juce::ComboBox& menu;
    juce::TextButton onOff { "On" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    juce::String text;
    bool warning = false;
    std::shared_ptr<const SampleData> lastSample;
    std::array<float, 6> lastValues {};
};

class MatrixTitles : public juce::Component
{
public:
    void paint (juce::Graphics& g) override;
};
