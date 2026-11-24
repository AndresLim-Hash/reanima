#pragma once

#include "SampleAnalysis.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <memory>
#include <vector>

// Engine F, the drone maker. The sample is read through very long FFT frames whose phases are
// drawn at random, so its rhythm and its attack disappear and only its colour is left; the
// reading can move a thousand times slower than the sample or stop (the idea of Paul Nasca's
// PaulStretch). Between analysis and synthesis the spectrum can keep only the harmonics of the
// note, lean towards its tone or its noise, be smeared, copied to other octaves, shifted in
// frequency and shaped by a drawn curve.
namespace Drone
{
constexpr int minOrder = 10, maxOrder = 16;
constexpr int numSizes = 6;                 // the "Tamaño" choice
constexpr int curvePoints = 16;             // drawn filter: gains in dB from 20 Hz to 20 kHz
constexpr float curveMinDb = -48.0f, curveMaxDb = 12.0f;
constexpr int numIntervals = 5;             // -2 oct, -1 oct, fifth, +1 oct, +2 oct
constexpr float intervalRatios[numIntervals] = { 0.25f, 0.5f, 1.5f, 2.0f, 4.0f };
constexpr int maxCopies = 1 + numIntervals + 16;

// The partials of a note at their true frequency (approved for A/B 23-11): on by default; off for A/B renders.
void setTrueFrequency (bool on) noexcept;

// FFT order for a "Tamaño" choice: the same length in seconds at any sample rate, up to 2^16.
int orderFor (int sizeChoice, double hostRate) noexcept;

// Frequency of point i of the drawn curve, and the curve's gain at any frequency.
float curveFrequency (int point) noexcept;
float curveGainDb (const float* curve, float hz) noexcept;

struct Settings
{
    int order = 14;
    double hostRate = 48000.0;
    double rateRatio = 1.0;                 // source samples per output sample at the original pitch
    double advance = 0.0;                   // source samples the reading moves per output sample
    double regionStart = 0.0, regionEnd = 1.0;
    float fundamentalHz = 0.0f;             // of the sample; 0 when it has no clear note
    float harmonics = 0.0f, harmonicWidthCents = 25.0f;
    float tonal = 0.0f, spread = 0.0f;
    float shiftHz = 0.0f, width = 1.0f;
    int numCopies = 1;
    std::array<float, maxCopies> copyRatio {}, copyLevel {};
    const float* curve = nullptr;           // curvePoints values in dB, or none
    const float* colour = nullptr;          // Seguir: curvePoints values in dB that change the colour, never the level
    bool keepFormants = false;              // Warp Tonos: the copies move their partials, not the envelope
};

using Complex = juce::dsp::Complex<float>;

// The transforms, windows and scratch space every stream shares. Built off the audio thread;
// the streams use them one after another on the audio thread.
class Shared
{
public:
    Shared();
    const juce::dsp::FFT& fft (int order) const noexcept { return *ffts[(size_t) (order - minOrder)]; }
    const float* window (int order) const noexcept { return windows[(size_t) (order - minOrder)].data(); }

    std::vector<Complex> transform;         // 2^maxOrder
    std::vector<Complex> twiddle;           // e^(-2 pi i k / 2^maxOrder), k < 2^(maxOrder-1): the FFT done pass by pass
    std::vector<float> spectrum, gains;     // 2^(maxOrder-1) + 1
    std::vector<double> sums;

    // Per-bin weights that depend only on the note, a setting and the size, not on the frame: worked out
    // when one of those changes and kept (with harmonics on, the logarithm and exponential of every bin
    // were most of a long frame's time, and the heaviest stage of all).
    std::vector<float> harmonicKeep, curveGain, colourGain;
    float keepF0 = -1.0f, keepWidth = -1.0f, keepBinHz = -1.0f, curveBinHz = -1.0f;
    int keepBins = 0, curveBins = 0;
    std::array<float, curvePoints> curveKey {};

private:
    std::array<std::unique_ptr<juce::dsp::FFT>, maxOrder - minOrder + 1> ffts;
    std::array<std::vector<float>, maxOrder - minOrder + 1> windows;
};

// One note of drone. Each frame is prepared ahead of time in small stages, which the engine hands
// out over the blocks before the frame is due: the most urgent stream first. The long FFTs are done
// in place a pass at a time (one pass of a 2^16 transform is a fraction of a millisecond), so that no
// block has to do a whole one: at 64-sample buffers a whole one took longer than the block itself.
// Both channels share one complex transform.
class Stream
{
public:
    Stream();

    // The first frame is due `delay` samples later.
    void start (double sourcePos, int order, int delay, juce::int64 seed) noexcept;

    // Blocks to spare before the frame being prepared is late; a large number when it is ready.
    int slack (int blockSize) const noexcept;
    // When the frame being prepared is due (samples), and how far along it is; none prepared: no deadline.
    int deadline() const noexcept { return stage == 0 ? std::numeric_limits<int>::max() : untilFrame; }
    int progress() const noexcept { return stage; }
    // Stages a block this stream needs from now on to have its frame ready in time, spread evenly.
    double demand (int blockSize) const noexcept
    {
        return stage == 0 ? 0.0 : (double) (numStages() - stage + 1) / (double) std::max (1, untilFrame / std::max (1, blockSize));
    }

    // Runs the next stage of the frame being prepared.
    void step (const SampleData& s, const Settings& d, Shared& shared) noexcept;

    // Adds num samples, each scaled by gain[i].
    void render (const SampleData& s, const Settings& d, Shared& shared, const float* gain, float* outL, float* outR, int num) noexcept;

    double position() const noexcept { return pos; }

    bool inUse = false;

private:
    static constexpr int ringSize = 2 << maxOrder;
    int numStages() const noexcept { return 2 * order + 10; }

    void read (const SampleData& s, const Settings& d, Shared& shared) noexcept;
    void fftPass (int pass, bool inverse, const Shared& shared) noexcept;      // 0: reorder; 1..order: butterflies
    void magnitudes() noexcept;
    void shape (int ch, const Settings& d, Shared& shared) noexcept;
    void build (const Settings& d, int half) noexcept;      // in two halves: the heaviest stage
    juce::Random buildPhases, buildApart;                   // carried from one half to the other
    void synthesise (Shared& shared) noexcept;
    void scheduleNext (const Settings& d) noexcept;

    std::array<std::vector<float>, 2> acc, mag;

    // The tonal peaks of the frame being built (fractional bin, phase in cycles at the frame's middle, the right
    // channel's offset), and of the one before, to carry each partial's phase on at its own frequency.
    struct Track { float pos; double theta; float apart, sweep, rate; };
    std::vector<Track> tracks, lastTracks;
    std::vector<int> peakOf;                // per bin: the track whose peak it belongs to, or -1
    int lastSize = 0;
    bool seededForPeaks = false;
    void findPeaks (const Settings& d, juce::Random& apartRng) noexcept;
    std::vector<Complex> frame;             // the windowed input, then the spectrum to play
    int order = 14, size = 1 << 14, hop = 1 << 12;
    int readIdx = 0, nextAt = 0, untilFrame = 0, stage = 0;
    double pos = 0.0;
    float level = 1.0f;                     // brings the part being read to the level of the loudest
    juce::Random rng;
    juce::int64 frameSeed = 1;
};
} // namespace Drone
