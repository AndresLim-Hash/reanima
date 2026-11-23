#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <memory>
#include <vector>

// The key of a sound, as far as it can be told, with how sure it is (0..1):
//  - from its name, when the file says it (Splice and most libraries: "..._F#m", "..._Dmin", "..._C");
//  - from its audio: a chromagram (the energy of the spectrum's peaks, 55 Hz to 2 kHz, folded on the 12 pitch
//    classes) set against the major and minor key profiles of Krumhansl and Kessler, the 24 keys;
//  - from the notes played (a histogram of pitch classes, weighted by length and velocity, fading with time).
// A key that is not clear (a drum, noise, a chord that fits several keys) comes out with a low confidence: the
// caller shows it as a suggestion and does not apply it on its own.
namespace KeyDetect
{
struct Estimate
{
    int root = -1;              // 0 = C ... 11 = B; -1: none
    bool minor = false;
    float confidence = 0.0f;    // 0..1
    bool fromName = false;
    std::array<float, 12> chroma {};
    juce::String text() const;  // "La menor", "Fa# mayor", "?" ...
};

Estimate fromName (const juce::String& fileName);
Estimate fromChroma (const std::array<float, 12>& chroma, float tonality = 1.0f);
Estimate fromAudio (const juce::AudioBuffer<float>& audio, double sampleRate);

// The notes played: each note-on adds its velocity, each block the histogram fades (half in `halfLife` s).
struct MidiHistogram
{
    std::array<float, 12> weight {};
    int notes = 0;
    void noteOn (int note, float velocity) noexcept { weight[(size_t) (note % 12)] += velocity; ++notes; }
    void fade (double seconds, double halfLife = 30.0) noexcept;
    Estimate estimate() const;
};

// A chord: its root, its kind (by `chordKinds`), the note in the bass (a slash chord when it is not the root) and
// how sure (from notes played: 1 when every note fits; from the sound: the fit against the next best).
struct Chord
{
    int root = -1, kind = 0, bass = -1;
    float confidence = 0.0f;
    juce::String name() const;                          // "Lam7", "Do/Mi", "Sol5"; "" for none
    juce::String degreeIn (int keyRoot) const;          // "vi7", "IV", "bVII", "V/3"; "" for none
    bool operator== (const Chord& o) const noexcept { return root == o.root && kind == o.kind && bass == o.bass; }
    bool operator!= (const Chord& o) const noexcept { return ! (*this == o); }
};
struct ChordKind { const char* suffix; std::array<int, 5> intervals; int count; bool minorish; };
const std::vector<ChordKind>& chordKinds();
// The notes held (MIDI numbers, any order): the chord they make, the lowest in the bass. Fewer than 3 different
// notes is no chord (a fifth alone gives the "5" chord).
Chord chordFromNotes (const std::vector<int>& notes);
// A chromagram (the sound of the last moment): the triad or seventh it fits best.
Chord chordFromChroma (const std::array<float, 12>& chroma);

// The sound coming in (the sidechain), a little at a time (message thread): frames of 8192 at half overlap, the
// chroma and the energy fading like the notes' histogram. The confidence grows with the pitched seconds heard.
class AudioStream
{
public:
    AudioStream();
    ~AudioStream();
    void reset (double sampleRate);
    void push (const float* mono, int numSamples);
    void fade (double seconds, double halfLife = 30.0) noexcept;
    Estimate estimate() const;
    double secondsHeard() const noexcept { return heardSeconds; }
    // The chord of the last moment: the same frames, fading in half a second; none after a moment of silence.
    Chord chord() const;
    static constexpr double fullAfter = 20.0;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    double rate = 0.0, peaksEnergy = 0.0, allEnergy = 0.0, heardSeconds = 0.0;
    std::array<float, 12> chroma {}, recent {};
    double recentSeconds = 0.0;
};

const char* noteName (int pitchClass);      // "Do", "Do#", ... "Si"

// How often an estimate from the sound with this confidence gave the key or its relative (the same notes),
// measured on 1129 Splice samples whose name says their key (research/tonalidad_2026-11-22): 0..1.
float measuredReliability (float confidence) noexcept;
}
