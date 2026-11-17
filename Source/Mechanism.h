#pragma once

#include "DspUtil.h"
#include "SampleAnalysis.h"
#include <array>
#include <vector>

// Recordings of instrument mechanisms, without their notes: a piano's key and damper and its
// sustain pedal, the hammer strikes of real pianos (taken from their notes), a pick on a string and the fingers stopping it, a bow biting and leaving the
// string, an organ key and a harpsichord jack, the tongue and air of a flute, a clarinet and a
// tenor sax. They come from CC0 libraries (see Resources/Mechanism/CREDITOS.txt), are kept
// inside the plugin and decoded once.
class MechanismLibrary
{
public:
    enum Event { Down, Up, PedalDown, PedalUp, NumEvents };
    static constexpr int numBands = SampleData::Excitation::numBands;
    using Colour = std::array<float, numBands>;

    struct Clip
    {
        int note = 60, layer = 1;              // the key it was recorded on, and how hard (1 to 4)
        juce::AudioBuffer<float> audio;        // stereo, at clipRate
        float gain = 1.0f;                     // brings it to 0 dB RMS over its loudest 50 ms
        float sustainGain = 1.0f;              // brings what follows its first 0.2 s (a bow alone) to 0 dB RMS
        bool tuned = false;                    // keeps a little of its note: tuned to the one that sounds
        float peak = 1.0f;                     // its largest sample
        int peakAt = 0;                        // the moment it strikes: its first strong peak, in samples
        float peakSign = 1.0f;                 // and the polarity of that peak
    };

    static constexpr double clipRate = 48000.0;

    // Built the first time it is asked for: never on the audio thread (SynthEngine::prepare asks).
    static const MechanismLibrary& get();

    // type: a Mechanism::Type with recordings. A set that a "real" type lacks (its release, a pedal)
    // is that of the gesture it is a version of: only the strike differs.
    const std::vector<Clip>& clips (int type, int event) const noexcept { return sets[(size_t) setOf (type, event)][(size_t) event]; }

    // The average colour of a set, in the bands of SampleData::Excitation (dB, mean removed).
    const Colour& colour (int type, int event) const noexcept { return colours[(size_t) setOf (type, event)][(size_t) event]; }

    static constexpr int numSets = 10;      // piano, pick, bow, key, breath; real piano, pluck, bow, breath, guitar pick

private:
    MechanismLibrary();
    // Mechanism::RealPiano, RealPick, RealBow, RealBreath and RealGuitar (6 to 10) have sets 5 to 9, and
    // fall back on those of Piano, Pick, Bow, Breath and Pick.
    int setOf (int type, int event) const noexcept
    {
        static constexpr int classic[] { 0, 1, 2, 4, 1 };
        if (type >= 6 && type <= 10)
            return sets[(size_t) type - 1][(size_t) event].empty() ? classic[type - 6] : type - 1;
        return juce::jlimit (0, 4, type);
    }
    std::array<std::array<std::vector<Clip>, NumEvents>, numSets> sets;
    std::array<std::array<Colour, NumEvents>, numSets> colours {};
};

// The gestures of an instrument around its note: what sounds when the key goes down and when it
// comes back up, played from the recordings above, and for the bow and the breath, the noise of
// the bow on the string and of the air for as long as the note sounds.
//
// They are joined to the sample as if they set it going, not laid on top of it (see DESIGN.md,
// "Mecanismo"):
//  - in time: the strike of each recording lands on the first peak of the sample's attack, with
//    the same polarity, so that both make one attack and not two;
//  - in colour: each recording takes the colour of the sample's attack ("Adaptar"), as an
//    excitation takes the colour of the body it sets going;
//  - in register: nothing under the note the sample plays (under its lowest register when it has
//    no note), which is what made the pick sound like a bass of its own;
//  - in pitch: what is left of the recordings' own notes meets the note that sounds, and a short
//    resonance at that note, like the string or the tube it excites, lets the noise ring in it;
//  - in level and envelope: harder notes make louder mechanisms; a slow attack softens a strike
//    and lets a bow or a breath swell with it; the air and the bow follow the note's own level,
//    relatively stronger when it is soft; the release of a short note is quieter.
class Mechanism
{
public:
    // The "real" types (0.9.5, at the end of the menu so that saved projects keep their choice) play as
    // the gesture they are a version of, with strikes taken from real notes of that instrument family
    // (Iowa, VSCO, Karoryfer): a piano's hammer, the finger pluck of a harp and of a pizzicato ("Pulsado
    // real"), the bow of a violin and of a cello, the tongue and breath of flute, clarinet, horn, oboe,
    // bassoon and trumpet, and a pick on acoustic and electric guitars ("Púa guitarra").
    enum Type { Piano, Pick, Bow, Key, Breath, FromSample, RealPiano, RealPick, RealBow, RealBreath, RealGuitar, NumTypes };

    // How the note plays, for the mechanism to join it.
    struct Note
    {
        float velocity = 1.0f;
        float sounding = 60.0f;         // the pitch the note sounds at, in MIDI
        float speed = 1.0f;             // how fast the sample is read (its pitch ratio)
        float attackMs = 5.0f;          // the attack the envelope gives the note
        float adapt = 0.75f;            // Adaptar: how much the mechanism takes from the sample
        float downGain = 1.0f;          // Entrada: the level of what sounds when the key goes down
        float upGain = 1.0f;            // Salida: the level of what sounds when it comes back up
        double startPos = 0.0;          // where the note starts in the sample
    };

    void prepare (double sr);           // not on the audio thread
    void seed (juce::int64 s) noexcept { rng.setSeed (s); }
    void reset() noexcept;
    bool busy() const noexcept;

    // amount: the Mecanismo knob, 0 to 1.
    void keyDown (int type, float amount, const SampleData& s, const Note& note) noexcept;
    void keyUp (int type, float amount, const SampleData& s, const Note& note) noexcept;
    void pedal (bool down, float amount, const SampleData& s, float gain = 1.0f) noexcept;

    // Adds what sounds to l and r. tone: the RMS of the note itself over these samples, which
    // the bow and the breath follow.
    void process (float* l, float* r, int num, const SampleData& s, float tone = 0.0f) noexcept;

    // How well each type suits a sample, played with an attack of attackMs and held (Sostenido)
    // or not: 0 to 1 each, the best of them, and in a few words what speaks against each one.
    struct Fit
    {
        std::array<float, NumTypes> score {};
        std::array<const char*, NumTypes> against {};
        int best = 0;
        bool byName = false;        // the best was suggested by the file name (SampleData::mechanismHint)
    };
    static Fit fit (const SampleData& s, float attackMs, float sustain, bool held) noexcept;

    // What a file name says the sample is: the type whose recordings come from that instrument (a real
    // one), Tecla for an organ, or -1 when it names none, or names a synth (then the sound decides).
    // word: the word that decided. Not on the audio thread.
    static int nameHint (const juce::String& fileName, juce::String& word);

private:
    static constexpr int maxEq = 10;    // up to three high-passes and seven bands of colour
    using Clip = MechanismLibrary::Clip;

    struct Chain
    {
        std::array<Dsp::Biquad, maxEq> stage;
        int count = 0;
        void reset() noexcept { for (auto& b : stage) b.reset(); }
        void process (float& x, float& y) noexcept
        {
            for (int i = 0; i < count; ++i)
            {
                x = stage[(size_t) i].process (0, x);
                y = stage[(size_t) i].process (1, y);
            }
        }
    };

    struct Player
    {
        bool active = false, fromSample = false, zeroPhase = false;
        const Clip* clip = nullptr;
        double pos = 0.0, step = 1.0, end = 0.0;
        double fadeOut = 0.0;           // samples of the recording over which it fades before its end (0: none)
        int wait = 0;                   // samples before it starts, to land on the sample's attack
        float gain = 0.0f, ramp = 1.0f, rampStep = 1.0f;
        Chain chain;
        Dsp::Biquad filter;             // "Del sample"
    };

    struct Grain
    {
        const Clip* clip = nullptr;
        double pos = 0.0, step = 1.0;
        int age = 0, length = 0;
        float gain = 0.0f;
    };

    void play (int type, int event, float amount, const SampleData& s, const Note& note, float scale = 1.0f) noexcept;
    void playSample (bool down, float amount, const SampleData& s, const Note& note) noexcept;
    const Clip* choose (int type, int event, float velocity, float sounding, bool remember) noexcept;
    void setChain (Chain& chain, int type, int event, const SampleData& s, const Note& note) const noexcept;
    void setResonance (int type, const SampleData& s, const Note& note) noexcept;
    void startGrain (Grain& g) noexcept;
    Player& freePlayer() noexcept;

    double rate = 48000.0;
    double heldFor = 0.0;                   // samples since the key went down
    std::array<Player, 4> players;
    const Clip* last[NumTypes][MechanismLibrary::NumEvents] {};
    juce::Random rng { 0x3c4a };

    // The noise of the bow or the air for as long as the note sounds: grains of the recordings.
    bool texture = false;
    int textureType = 0;
    Note textureNote;
    float textureLevel = 0.0f, toneLevel = 0.0f;
    std::array<Grain, 4> grains;
    int grainLength = 1, sinceGrain = 0, nextGrain = 0;
    std::vector<float> grainWindow;
    Chain textureChain;

    // The string or tube the noise rings in: a short comb at the note that sounds.
    std::vector<float> combL, combR;
    int combPos = 0;
    double combDelay = 0.0;
    float combFeedback = 0.0f, combWet = 0.0f, combNorm = 1.0f, combLowL = 0.0f, combLowR = 0.0f, ringing = 0.0f;
    Dsp::Biquad combLow;                    // what it rings under the note, taken away

    std::array<float, 64> mixL {}, mixR {};
};
