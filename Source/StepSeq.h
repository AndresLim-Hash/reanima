#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <limits>

// The arpeggiator and step sequencer (ARP · SEQ): one step engine in two modes.
//  - Arp: each step plays the next note of the chord held (Estilo: up, down, ... ; Octavas), moved by the step's
//    Nota lane in degrees of the key;
//  - Seq: each step's Nota lane is a degree of the key, counted from the key pressed (which transposes the phrase).
// The lanes (Paso, Nota, Octava, Velocidad, Gate, Probabilidad, Repetición) each have their own length: lanes of
// different lengths drift against each other (polyrhythm). The steps follow the DAW's bars when it plays (Sync:
// DAW), else a clock of their own that starts with the first key. Generated notes go to Reanima's engine; the
// keys pressed do not sound themselves.
namespace StepSeq
{
constexpr int maxSteps = 16;
enum Lane { On, Note, Octave, Velocity, Gate, Chance, Ratchet, numLanes };

struct LaneRange { float lo, hi, def; bool whole; };
LaneRange laneRange (int lane) noexcept;
const char* laneName (int lane) noexcept;

struct Pattern
{
    std::array<std::array<float, maxSteps>, numLanes> value {};
    std::array<int, numLanes> length {};
    Pattern() noexcept { clear(); }
    void clear() noexcept;
    float at (int lane, int step) const noexcept { return value[(size_t) lane][(size_t) step]; }
    juce::String toString() const;
    static Pattern fromString (const juce::String& text);      // a bad text gives the default pattern
    bool operator== (const Pattern& o) const noexcept { return value == o.value && length == o.length; }
};

// New values for a lane (or all, lane < 0), in the key (the Nota lane: mostly chord degrees), `density` 0..1
// of the steps on.
void generate (Pattern& p, int lane, float density, juce::Random& rng);

// The pattern, written by the message thread and read by the audio thread (a seqlock: the reader retries a
// copy caught half-written, and keeps its last one if it keeps failing).
class SharedPattern
{
public:
    void write (const Pattern& p) noexcept;
    bool read (Pattern& out) const noexcept;
    Pattern latest() const noexcept { Pattern p; read (p); return p; }
private:
    std::atomic<unsigned> version { 0 };
    Pattern data;
};

struct Settings
{
    bool on = false, seq = false, latch = false, sync = true;
    int style = 0, direction = 0, octaves = 1;
    double stepBeats = 0.25, freeBpm = 120.0;
    float swing = 0.0f, gate = 1.0f;
    int keyRoot = 0;
    bool keyMinor = false;
};

double rateBeats (int choice) noexcept;     // the Ritmo choice in beats (1/16: 0.25)

// The scale degree `degrees` away from `note` (snapped to the key first; 0: the note itself, unchanged).
int moveInKey (int note, int degrees, int keyRoot, bool minor) noexcept;

class Player
{
public:
    // The step lane positions (for the grid's playheads): the step each lane is on, -1 when stopped.
    std::array<std::atomic<int>, numLanes> shownStep {};
    std::atomic<int> heldShown { 0 };

    Player() { for (auto& s : shownStep) s.store (-1); }
    void prepare (double sampleRate);
    void reset() noexcept;          // everything off, the clock stopped (no notes out: call flush first)
    // Takes the block's MIDI, writes what the engine plays into `out` (cleared first). Off: `out` = `in`.
    // ppq < 0 or ! playing: no transport.
    void process (const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples, const Settings& s, const Pattern& pattern,
                  bool hostPlaying, double ppq, double hostBpm);
    // The chord last held (for the MIDI clip), as notes and velocities; count 0: none yet.
    struct Chord { std::array<int, 16> note {}; std::array<int, 16> velocity {}; int count = 0; };
    Chord lastChord() const noexcept;
    void setSeed (juce::int64 seed) noexcept { rng.setSeed (seed); }
    bool active() const noexcept { return wasOn; }      // on, or notes of its own still to end

private:
    struct Held { int note, velocity; juce::uint32 order; };
    struct Pending { juce::int64 at; int note; bool on; int velocity; };
    double rate = 48000.0;
    std::array<Held, 16> held {};
    int heldCount = 0;
    bool keysDown = false;          // with Latch: a key is down now (a new press after all released starts a chord anew)
    std::array<bool, 128> physicallyDown {};
    juce::uint32 pressCounter = 0;
    std::array<Pending, 512> pending {};
    int pendingCount = 0;
    std::array<int, 128> sounding {};
    juce::int64 now = 0;            // samples since prepare
    bool running = false, wasOn = false, wasHost = false;
    double freeBeat = 0.0, lastPpqEnd = -1.0;
    juce::int64 arpIndex = 0, lastFired = std::numeric_limits<juce::int64>::min();
    juce::Random rng { 12345 };
    std::atomic<unsigned> chordVersion { 0 };
    Chord chordCopy;

    void addPending (juce::int64 at, int note, bool on, int velocity) noexcept;
    void allOff (juce::MidiBuffer& out, int at) noexcept;
    void playStep (juce::int64 step, juce::int64 atSample, double stepSamples, const Settings& s, const Pattern& p);
    int arpNote (const Settings& s) noexcept;
    void keepChord() noexcept;
};
}
