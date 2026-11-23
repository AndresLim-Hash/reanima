#pragma once

#include "Params.h"
#include "SampleAnalysis.h"
#include "DspUtil.h"
#include <atomic>
#include <memory>
#include <vector>

class Voice;
struct LoopStretcher;

// LOOP "Rubber Band": the loop stretched by Rubber Band R3 offline (at the file's rate), for one tempo and pitch.
// Made by the processor in the background; the engine reads it.
struct RubberRender
{
    std::vector<float> l, r;
    int length = 0;                         // samples (the loop's length over the tempo)
    double tempo = 1.0, pitch = 1.0;
    const SampleData* sample = nullptr;
    int loopLength = 0, loopStart = 0;      // the part of the file rendered (Ventana)
};
class FxRack;
class Mechanism;
namespace Drone { class Shared; class Stream; }

// Everything that makes sound: voices, MIDI handling, FX and master stage.
// Used by the processor in real time and offline to render the drag-out file.
class SynthEngine
{
public:
    static constexpr float minReshapedRelease = 0.06f;  // seconds; below this a sustained sound stops with a knock

    static constexpr int maxVoices = 32;
    static constexpr int physicalVoices = maxVoices + 8;   // spares so stolen voices can fade out

    SynthEngine();
    ~SynthEngine();

    void prepare (double sampleRate);
    void reset();

    // Message thread (or offline): creates the effect modules the rack needs.
    void prepareFx (const ParamSnapshot& params);

    // Audio thread. Stops all voices if the sample changes and returns the previous
    // sample so the caller can release it off the audio thread.
    std::shared_ptr<const SampleData> setSample (std::shared_ptr<const SampleData> newSample);

    // Audio thread. The same sound analysed further (another engine needs more of it): the notes that sound
    // finish with the analysis they started with, new ones take the new one, so nothing is cut. Returns the
    // sample that was current, which the caller must keep alive (and free off the audio thread) while
    // previousInUse is true. Not possible while an earlier one is still in use: canUpgrade().
    std::shared_ptr<const SampleData> upgradeSample (std::shared_ptr<const SampleData> newSample);
    bool canUpgrade() const noexcept { return previous == nullptr; }
    std::atomic<bool> previousInUse { false };

    void process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, const ParamSnapshot& params, double bpm);
    // Audio thread, before process(): the sidechain's audio for the coming block (null: none), for the matrix's
    // Sidechain source and the Impronta effect. It must stay valid until process() returns.
    void setSidechain (const float* left, const float* right, int length) noexcept;
    // The Sidechain source as the voices last read it (0..1), for the panel.
    std::atomic<float> sideShown { 0.0f };
    // The host's transport at the start of the next block: playing, and where (quarter notes from the start).
    void setTransport (bool playing, double ppq) noexcept { hostPlaying = playing; hostPpq = ppq; }

    // The drawn filter of engine F: Drone::curvePoints gains in dB. Audio thread, before process().
    void setDroneCurve (const float* gainsDb) noexcept;

    // What is heard, to judge Mecanismo by ear (set from the panel, never saved): everything, the note
    // alone (no Mecanismo) or Mecanismo alone (the note muted; its level still drives the bow and the air).
    enum Listen { ListenAll, ListenSample, ListenMechanism };
    std::atomic<int> listen { ListenAll };
    // Tests: each note's random seed from its key only (ReanimaAudit estres), so that it can be set against itself alone.
    std::atomic<bool> keySeeds { false };

    // Playback position (in samples) of each voice for the UI, or -1.
    std::array<std::atomic<float>, physicalVoices> playheads;
    // SLICE: the slice each voice plays (-1: none), beside its playhead; the window reads both.
    std::array<std::atomic<int>, physicalVoices> playSlices;

    // Key that plays the sample at its root (after "Snap a C").
    // Do3: the key that plays the sample as it is, or moved to its nearest C with Snap a C.
    static constexpr int rootKey = 60;

    // Semitones the sample moves at the root key: 0, or up to C with Snap a C.
    static float snapShift (const SampleData& s, bool snap) noexcept;

    // Analysis thread, while the sample is built: tries Warp's Tonos on it and sets tonesHold and
    // tonesGrid. Where Tonos would miss the note, engines A, B and D play it as Textura.
    static void checkTones (SampleData& s);
    static constexpr float tonesPass = 0.7f;     // least share on the right harmonics (set on 176 one shots of the library)

    // Under Warp Tonos, whether this sample is played as Textura instead (engines A, B and D).
    static bool tonesFallBack (const SampleData& s, int engine) noexcept;

    // LOOP Normal's Ventana: the part of the loop that plays (samples of the file), from its length in beats and its
    // Posición; the whole loop with Completo, without a tempo, or in SLICE.
    struct LoopWindow { int start = 0, length = 0; bool on = false; };
    static LoopWindow loopWindow (const SampleData& s, const ParamSnapshot& p) noexcept;
    static double loopBpmOf (const SampleData& s, const ParamSnapshot& p) noexcept;

    // The matrix's modulation as the knobs show it (their outer ring): that of the last note played (its LFOs,
    // envelopes, velocity, macros...), or with no note sounding the macros alone. Audio thread writes, UI reads.
    std::array<std::atomic<float>, P::NumDests> modShown {};
    // The destination that moves a knob's parameter (DstNone: none), and its value moved by `amount` of that
    // destination, in the parameter's own units, as the voices apply it.
    static int destOf (int paramId) noexcept;
    static float modulated (int paramId, float value, float amount) noexcept;

    // The Mecanismo knob, or 0 while its On/Off is off.
    static float mechanismAmount (const ParamSnapshot& p) noexcept;

    // SLICE: where each slice starts (samples of the sample), set on the message thread, read at note-on.
    static constexpr int maxSlices = 128;
    void setSlices (const std::vector<int>& starts) noexcept;
    // The cuts in time order, and which slice each key plays (slot k from Primera nota: slice order[k]).
    void setSlices (const std::vector<int>& cuts, const std::vector<int>& order) noexcept;
    int sliceCount() const noexcept { return slicesUsed.load (std::memory_order_acquire); }
    // LOOP Beats: its cuts as the user left them (hits turned off or added, HitEdits), for the sample they were
    // worked out on; empty: the analysis's own (SampleData::LoopInfo::slices). Message thread.
    static constexpr int maxBeatCuts = 1024;
    void setBeatCuts (const SampleData* owner, const std::vector<int>& cuts) noexcept;
    // Audio thread: the cuts Beats plays for this sample.
    const std::vector<int>& beatSlices (const SampleData& s) const noexcept;
    // Por golpe in TIMBRE: a hit's peak over its first 15 ms, and the peak that counts as full (Brillo darkens the
    // weaker hits as softer keys): that of the loop's strong hits (three quarters of its hits stay under). Message thread.
    static float hitPeak (const SampleData& s, int at) noexcept;
    std::atomic<float> hitStrengthRef { 1.0f };
    // LOOP "Rubber Band": what a voice asks for (tempo, pitch), and the render ready (set on the message thread).
    std::atomic<double> rubberWantTempo { 1.0 }, rubberWantPitch { 1.0 };
    std::atomic<int> rubberWantStart { 0 }, rubberWantLength { 0 };     // the part of the loop (Ventana)
    std::atomic<bool> rubberWanted { false };
    std::atomic<const RubberRender*> rubberReady { nullptr };
    // SLICE diagnosis for the window: the last key received and the slice its voice took (-1: none).
    std::atomic<int> lastSliceNote { -1 }, lastSliceIndex { -1 }, lastSliceFrom { 0 }, lastSliceTo { 0 };

private:
    void handleMidi (const juce::MidiMessage& m, const ParamSnapshot& p);
    void noteOn (int note, float velocity, const ParamSnapshot& p, int channel = 1);
    void noteOff (int note, const ParamSnapshot& p, int channel = 1);
    void renderVoices (juce::AudioBuffer<float>& buffer, int start, int num, const ParamSnapshot& p);
    // The matrix's Sidechain source over samples [start, start + num) of the block.
    void followSidechain (int start, int num, const ParamSnapshot& p) noexcept;
    Voice* allocateVoice();
    Drone::Stream* acquireDrone() noexcept;
    LoopStretcher* acquireStretcher() noexcept;

    // LOOP Complex / Complex Pro and Beats's tails: a stretcher (Bungee, Signalsmith) per note, made in prepare(),
    // never on the audio thread. One for every voice, stolen ones fading out included: with fewer (there were 8),
    // the 9th note sounding at once, release tails counted, found none and played as Textura, broken until
    // an earlier note ended (ReanimaAudit estres).
    static constexpr int numStretchers = physicalVoices;
    std::vector<std::unique_ptr<LoopStretcher>> stretchers;
    std::atomic<bool> stretchersReady { false };
    void buildStretchers (double sr);

    // Engine F needs long buffers; they are made the first time it is chosen, off the audio thread. One for
    // every voice (with 16, the 17th note at once had none and was left empty).
    static constexpr int numDroneStreams = physicalVoices;
    std::unique_ptr<Drone::Shared> droneShared;
    std::vector<std::unique_ptr<Drone::Stream>> droneStreams;
    std::atomic<bool> dronesReady { false };
    std::array<float, 16> droneCurve {};
    std::array<std::atomic<int>, maxSlices> sliceStarts {};
    std::array<std::atomic<int>, maxSlices> sliceEnds {}, sliceOriginal {}, cutPoints {};      // end -1: the sample's end
    std::atomic<int> slicesUsed { 0 };
    // Beats's cuts: written on the message thread under a sequence number (odd while writing), copied by the
    // audio thread at the start of a block when it has moved on and is even (never torn, nothing allocated).
    std::array<std::atomic<int>, maxBeatCuts> beatCutsShared {};
    std::atomic<int> beatCutsCount { 0 };
    std::atomic<const SampleData*> beatCutsOwner { nullptr };
    std::atomic<unsigned> beatCutsSeq { 0 };
    std::vector<int> beatCutsLive;              // the audio thread's copy (room for maxBeatCuts)
    const SampleData* beatCutsLiveOwner = nullptr;
    unsigned beatCutsLiveSeq = 0;
    void takeBeatCuts() noexcept;
    double stretcherRate = 0.0;

    std::vector<std::unique_ptr<Voice>> voices;
    std::unique_ptr<FxRack> fx;
    std::unique_ptr<Mechanism> pedalNoise;     // the sustain pedal of a piano, under Mecanismo
    std::shared_ptr<const SampleData> sample;
    std::shared_ptr<const SampleData> previous;     // the analysis older notes still play (see upgradeSample)
    const SampleData& sampleFor (const Voice& v) const noexcept;

    double sampleRate = 44100.0;
    double bpm = 120.0;
    bool tempoKnown = false;        // the host gave its tempo (LOOP follows it only then)
    // LOOP with Play con el host: the one voice the transport plays, where the host is (quarter notes, at the
    // chunk being rendered), and the key that tunes it.
    bool hostPlaying = false;
    double hostPpq = 0.0, transportPpqNow = 0.0;
    Voice* transportVoice = nullptr;
    int grainBudget = 0;            // LOOP Complex: grains the notes starting in this block may still compute
    int transportNote = 60;
    void manageTransport (const ParamSnapshot& p);
    std::vector<int> heldNotes;
    bool pedalDown = false;
    float modWheel = 0, aftertouch = 0, breath = 0, expression = 1, pitchBend = 0;     // pitch bend from -1 to 1

    // MPE: each note on a channel of its own, with its own bend (in semitones of that channel's range), its
    // pressure and its timbre (CC74). Found from the notes (several channels at once) or from an MPE
    // configuration message; until then every channel acts as one keyboard, as before.
    bool mpe = false;
    std::array<float, 17> chanBend {}, chanPressure {}, chanTimbre {}, chanBendRange {};
    std::array<bool, 17> timbreSeen {};
    std::array<int, 17> rpnMsb {}, rpnLsb {};
    float lastNotePitch = -1.0f;
    int lastBlock = 512;
    float limiterGain = 1.0f;
    Dsp::Biquad bassHigh1, bassHigh2, bassAll;     // Graves mono: the side above the cut, the centre in the same phase
    float bassMonoHz = -1.0f;
    float masterGainL = 1.0f, masterGainR = 1.0f, monoMix = 0.0f;
    bool masterGainsReady = false;
    juce::uint32 noteCounter = 0;

    // The sidechain (setSidechain) and its follower: the level of what Respuesta listens to, and of its highs for Brillo.
    const float* sideL = nullptr;
    const float* sideR = nullptr;
    int sideLen = 0;
    float sideValue = 0.0f;
    float sideEnv = 0.0f, sideHighEnv = 0.0f;
    int sideResponse = -1;
    Dsp::Biquad sideBand, sideHigh;

    std::array<double, P::numLfos> globalLfoPhase {};
    std::array<float, P::numLfos> globalSampleHold {};
    juce::Random rng { 0x5eed };

    friend class Voice;
};
