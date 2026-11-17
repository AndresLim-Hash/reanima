#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <memory>
#include <vector>

// Amplitude envelope shape shared by the detected model and the user's ADSR.
// Times in seconds. Held level:  attack (t/a)^curve, then
//   s * 10^(-fade*t'/20) + (1 - s) * e^(-5 t'/d)
struct AdsrShape
{
    float attack = 0.005f, curve = 1.0f, decay = 0.3f, sustain = 0.5f, fadeDbPerSec = 0.0f, release = 0.3f;

    float heldLevel (double t) const noexcept;
};

// An analysed sample. Immutable once built; shared between UI and audio thread.
struct SampleData
{
    juce::AudioBuffer<float> audio;     // 2 channels, padded with zeros at the end
    int length = 0;                     // valid samples
    float peakLevel = 1.0f;             // largest absolute sample
    double sampleRate = 44100.0;
    juce::String name;
    bool reversed = false;
    bool forAllEngines = false;         // has the models of engines B, C and D too

    // Envelope (RMS, normalised so the peak is 1) at envHop resolution.
    std::vector<float> env;
    int envHop = 64;
    float peakRms = 1.0f;

    // Landmarks, in samples.
    double onset = 0, peak = 0, knee = 0, sustainPos = 0, usableEnd = 0;

    AdsrShape detected;
    // What versions up to 0.9.5 detected, when it was different: they ended the attack of a note that keeps
    // swelling at its loudest moment, seconds later, and their decay fit could let the sustain fall faster
    // than the decay (Sustain then worked backwards). A project saved then with the envelope as detected
    // gets the new detection, which plays the file just the same.
    AdsrShape earlierDetected;
    bool hasEarlierDetected = false;
    float transientMs = 20.0f;

    // How the sound is set going, for Mecanismo to know which instrument gestures suit it and to
    // join them to it (see Mechanism).
    struct Excitation
    {
        // Bands of colour: everything under 177 Hz, the octaves around 250 Hz to 4 kHz, and
        // everything over 5.7 kHz.
        static constexpr int numBands = 7;
        static constexpr float bandEdges[numBands + 1] { 20.0f, 177.0f, 355.0f, 710.0f, 1420.0f, 2840.0f, 5680.0f, 20000.0f };

        float attackMs = 5.0f;          // the detected attack
        float transientDb = 0.0f;       // first 15 ms against 60 to 250 ms after the onset
        float sustain = 0.0f;           // 0 dies away, 1 holds: the level 400 ms after the peak
        float noisiness = 0.0f;         // spectral flatness of the body, 0 a pure tone, 1 noise
        float tonal = 0.0f;             // share of the body's energy on the harmonics of its note
        float centroidHz = 1000.0f;     // brightness of the attack
        float lowEdgeHz = 40.0f;        // the lowest the sound reaches (3 % of its energy below)
        float transientAt = 0.0f;       // seconds from the onset to the first peak of the attack
        float riseAt = 0.0f;            // seconds from the onset to where the attack reaches a tenth of that peak
        float transientSign = 1.0f;     // its polarity
        std::array<float, numBands> attackColour {};    // dB per band, mean removed
    };

    Excitation excitation;

    bool pitched = false;
    float rootMidi = 60.0f;             // fractional MIDI note (C3 = 60)

    // Loop or one shot (SampleAnalysis, detectLoop), and what LOOP needs to play it in time.
    struct LoopInfo
    {
        bool likely = false;            // a loop, clear enough to switch to LOOP by itself
        float confidence = 0.0f;        // 0..1
        int beats = 0;                  // its musical length (0: none found)
        int length = 0;                 // the samples those beats take: the whole file, or less when a tail follows
        double bpm = 0.0;               // its tempo: those beats over that length
        double nameBpm = 0.0;           // the tempo its name gives, if any
        bool fromName = false;          // the beats came from the name's tempo
        bool tailAlive = false;         // it still sounds at its last sample
        bool rhythmic = false;          // hits (warped as Beats) rather than a texture (Tonal)
        float percussive = 0.0f;        // share of its energy in hits (median filtering of its spectrum), 0..1
        std::vector<int> slices;        // where Beats cuts it, from 0
        std::vector<std::pair<int, float>> hits;    // every onset found and how strong (SLICE's sensitivity)
    };
    LoopInfo loopInfo;
    // The file as it is, without the micro-fades at its ends that every other reading gets: a loop goes on
    // from its last sample into its first. Empty for files over 70 s (LOOP then reads the audio above).
    juce::AudioBuffer<float> loopAudio;

    // Sines + noise model for engine C (and the additive freeze).
    struct PartialModel
    {
        static constexpr int numBands = 24;

        int hop = 256;                  // samples between frames; frame f is centred at f * hop
        int numFrames = 0, numPartials = 0;
        int lastFrame = 0;              // last frame with audible energy
        float refFreq = 100.0f;         // lowest strong partial, reference for brightness
        std::vector<float> amp, freq, phase;    // [frame * numPartials + k]; phase in cycles
        std::vector<float> pan;                 // per partial, -1 (L) .. 1 (R)
        std::vector<float> decayDbPerSec;       // per partial, natural decay of its tail
        std::array<float, numBands + 1> bandEdges {};
        std::vector<float> noise;               // [frame * numBands + b], per-bin RMS magnitude

        bool valid() const noexcept { return numPartials > 0 && numFrames > 1; }
    };

    PartialModel partials;

    // What keeps a note alive in engines B and D: a spectrally equalised seamless loop
    // (Loop and Granular) and a breathing additive/spectral freeze, both built from a
    // region of the sample. "early" sits at the sustain point (Sostenido), "late" near
    // the end of the sample (Natural).
    struct SustainSource
    {
        juce::AudioBuffer<float> loop, freeze;
        double start = 0, end = 0;
        float model = 1.0f, env = 1.0f;     // detected model and measured envelope over the region
        float level = 1.0f;                 // RMS of the loop, against the loudest part of the sample
    };

    SustainSource early, late;

    // Engine A: long seamless reconstructions of the sound (see Reconstruct.h).
    struct Bed
    {
        static constexpr int pad = 48;      // wrapped samples before and after the signal
        juce::AudioBuffer<float> audio;
        int length = 0;
        int origin = 0;             // bed position that lines up with the start of the file
        float coherence = 0.0f;     // correlation with the sample once lined up, 0 to 1
    };

    Bed bedBright, bedDark;                 // from the body right after the attack / later in the decay
    // The two beds side by side, frame by frame (bright L, bright R, dark L, dark R), for Warp: its grains
    // read all four at one position, and this way every read touches one place in memory instead of four.
    std::vector<float> bedQuad;
    double bedStart = 0, bedEnd = 0;
    double period = 0.0;                    // samples per cycle when pitched, else 0

    // The start of every cycle of a pitched sample (empty if it has no note), following the
    // period as it drifts (vibrato, the pitch drop of an 808): what Warp's Tonos lays its grains on.
    std::vector<double> marks;
    double cycle = 0.0;                     // how often the wave repeats: the period, or a half, third or quarter of it

    // Whether Tonos moves this sample to the note asked for, tried on loading (SynthEngine::checkTones).
    // A chord, voices detuned against each other or cycles that do not repeat alike cannot be laid out
    // cycle by cycle: such a sample plays Tonos as Textura. tonesGrid is the share of what Tonos played
    // that fell on the harmonics it should have, the worse of 7 and 12 semitones up (1: not tried).
    // Mecanismo "Del sample" adds the sample's own attack, high-passed, in step with the note: the same
    // signal twice, so a filter that shifted its phase would hollow the sum under its cutoff (about
    // -2.6 dB around 500 Hz with a 1 kHz high-pass). The first seconds are filtered here forwards and
    // backwards, with no shift at all (4th order, 1 kHz).
    juce::AudioBuffer<float> attackHigh;

    // What the file name says the sample is, for Mecanismo to suggest (Mechanism::nameHint): a
    // Mechanism::Type, or -1; and the word that said it.
    int mechanismHint = -1;
    juce::String mechanismHintWord;

    bool tonesHold = true;
    float tonesGrid = 1.0f;

    // Detected model sampled every modelHop samples (floored at -60 dB) for fast lookup.
    static constexpr int modelHop = 16;
    std::vector<float> modelTable;

    float envAt (double pos) const noexcept;
    float modelAt (double pos) const noexcept { return detected.heldLevel ((pos - onset) / sampleRate); }

    float modelFast (double pos) const noexcept
    {
        const double f = pos > 0.0 ? pos / modelHop : 0.0;
        const auto i = (size_t) f;
        if (i + 1 >= modelTable.size())
            return std::max (1.0e-3f, modelAt (pos));
        const float frac = (float) (f - (double) i);
        return modelTable[i] + frac * (modelTable[i + 1] - modelTable[i]);
    }

    // Where a note starts for a value of "Inicio" (fraction of the file): always on a whole
    // sample, and exactly on the detected onset when it is that close to it.
    double startPosition (double fraction) const noexcept
    {
        const double pos = std::round (juce::jlimit (0.0, 0.99, fraction) * length);
        return std::abs (pos - onset) <= 1.0 ? onset : pos;
    }

    // Root key used when "Snap a C" moves the sample to the nearest C.
    int nearestC() const noexcept;
    int rootKey() const noexcept { return pitched ? juce::roundToInt (rootMidi) : 60; }

    // Posición of engines E and F: a fraction of the part that sounds, from the onset to the
    // usable end, so that the silence a file is padded with is never where they read.
    double soundingPosition (double fraction) const noexcept
    {
        const double end = std::max (onset + 1.0, usableEnd);
        return onset + juce::jlimit (0.0, 1.0, fraction) * (end - onset);
    }
};

namespace SampleAnalysis
{
// Reads an audio file (max 2 channels, max 2 minutes). Returns false on failure.
bool loadFile (const juce::File& file, juce::AudioBuffer<float>& out, double& sampleRate);

// Only the loop detection of analyse (the same, with a plain envelope): for measuring it over a library.
SampleData::LoopInfo analyseLoop (const juce::AudioBuffer<float>& source, double sr, const juce::String& name);

// The files loadFile can read on this computer, from the formats it has (WAV, AIFF, FLAC, Ogg and MP3
// everywhere; M4A, CAF and the rest of Core Audio's on macOS): "*.wav;*.aif;..." and one file.
juce::String audioWildcard();
bool isAudioFile (const juce::File& file);
// A file named by a drag that arrives as text: a path, or a file:// URL (one per line; the first audio file).
juce::File audioFileFromText (const juce::String& text);

// Engine A only needs the landmarks and the beds, a fraction of the work. The models of the
// other engines are left out when `forAllEngines` is false.
std::shared_ptr<SampleData> analyse (const juce::AudioBuffer<float>& source, double sampleRate,
                                     const juce::String& name, bool reverse, bool forAllEngines = true);

juce::String midiNoteName (float midi);   // Ableton naming: 60 = C3
}
