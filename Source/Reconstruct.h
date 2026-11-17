#pragma once

#include "SampleAnalysis.h"

// Offline reconstruction of what a sample would sound like if it kept playing: a long,
// seamless stereo "bed" built from a region of the sample in the frequency domain.
// Tonal peaks stay phase coherent (with a small jitter proportional to their frequency),
// everything else keeps its spectrum but gets new phases, and the stereo image of every
// bin is kept. The bed is played as plain audio, so no grains or oscillators are involved.
namespace Reconstruct
{
struct BedOptions
{
    double seconds = 8.0;
    float jitterCents = 3.0f;       // spread of the tonal peaks
    float tonalDb = 12.0f;          // prominence above the local floor to count as tonal
    float targetRms = 1.0f;         // RMS of the mono mix of the result
    double period = 0.0;            // samples per cycle when the pitch is known
    juce::int64 seed = 1;
};

struct Region { int start = 0, end = 0; };

// Removes the level changes of a region, keeping the balance between channels.
juce::AudioBuffer<float> flattenedRegion (const juce::AudioBuffer<float>& audio, double sr, Region r);

juce::AudioBuffer<float> makeBed (const juce::AudioBuffer<float>& audio, double sr, Region r, const BedOptions& options);

// Bright body right after the attack, and darker region further into the decay.
Region earlyRegion (const SampleData& s);
Region lateRegion (const SampleData& s);

// Fills in the two beds of an analysed sample.
void buildBeds (SampleData& s);
}
