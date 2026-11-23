#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

// Every plugin parameter, by index. The audio thread reads them into a flat
// snapshot (ParamSnapshot) once per block; the UI attaches by string id.
namespace P
{
enum Id : int
{
    // Engine / sample
    Engine, SustainMode, SustainType, Stretch, Flatten, GrainMs, Spray, GranSpeed, Breath,
    TransientMs, TransientGain, StartPos, Reverse, Transpose, Fine, SnapC,
    Bits, Downsample, Variation,

    // Timbre follows the envelope (bow/breath attack, darker release, velocity brightness)
    Bow, RelDark, VelBright, RelNoise,

    // Engine C (resynthesis)
    Partials, NoiseLevel,

    // Amp envelope (reconstructed)
    AmpA, AmpCurve, AmpD, AmpS, AmpFade, AmpR, VelAmp,

    // Voicing
    PolyMode, Glide, Unison, UniDetune, UniBlend, UniWidth, Volume, Pan, MonoOut, EnvLock, EnvMode, Punch, AtAmount, VibDepth, VibRate, BendRange,
    GrainPos, GrainDensity, GrainPitch, GrainReverse, GrainWidth, GrainMix,
    EndPos, FadeIn, FadeOut, LoopOn, LoopStart, LoopEnd, LoopXfade, SampleGain, Normalize, PitchEnvAmt, PitchEnvTime,
    MechType, MechAdapt, MechDown, MechUp, MechOn, WarpMode,

    // Engine F (drone maker)
    DroneStretch, DroneFreeze, DroneSize, DroneHarm, DroneHarmWidth, DroneTonal, DroneSpread,
    DroneSub2, DroneSub1, DroneFifth, DroneUp1, DroneUp2, DroneShift, DroneWidth, DroneCurveOn,

    // Filter (Auto Filter clone)
    FltOn, FltType, FltSlope, FltCircuit, FltFreq, FltRes, FltDrive, FltMorph,
    FltKey, FltEnvAmt, FltFollow, FltFolAtk, FltFolRel, FltMix,

    // Envelope 2
    E2A, E2D, E2S, E2R,

    // LFOs (6 params each). Suavidad: how gently the LFO moves (a glide over its steps; with Deriva, how little
    // quick wobble it has); 0 leaves every shape as it was before it existed.
    Lfo1Rate, Lfo1Sync, Lfo1Div, Lfo1Shape, Lfo1Retrig, Lfo1Smooth,
    Lfo2Rate, Lfo2Sync, Lfo2Div, Lfo2Shape, Lfo2Retrig, Lfo2Smooth,
    Lfo3Rate, Lfo3Sync, Lfo3Div, Lfo3Shape, Lfo3Retrig, Lfo3Smooth,

    Macro1, Macro2, Macro3, Macro4,

    // Mod matrix (3 params per slot)
    M1Src, M1Dst, M1Amt, M2Src, M2Dst, M2Amt, M3Src, M3Dst, M3Amt, M4Src, M4Dst, M4Amt,
    M5Src, M5Dst, M5Amt, M6Src, M6Dst, M6Amt, M7Src, M7Dst, M7Amt, M8Src, M8Dst, M8Amt,

    // FX rack routing (Serum 2 style: main chain + two parallel buses)
    FxMainLevel, FxBus1Send, FxBus2Send, FxBus1Level, FxBus2Level, FxBus1ToMain, FxBus2ToMain,

    // Output: the lows made mono below this frequency (0: off). Engines B and D: how the sample hands over
    // to its granular sustain (time, and how much of the granular part there is in the end).
    MonoBass, GrainFade, GrainBlend,

    // How the sample plays: as a note (one shot) or as a loop that follows the host's tempo.
    PlayMode, LoopWarp, LoopSync, LoopBeats, LoopAlgo,
    // LOOP: NORMAL (warped to the host) or SLICE (a slice per key from C3), the loop's own tempo (0: detected),
    // and how the slices are found.
    LoopKind, LoopBpm, SliceBy, SliceDivision, SliceSensitivity, SliceStart,
    // LOOP Normal: plays by itself with the host's transport (keys then only move its pitch). SLICE: a slice plays
    // on to the end of the sample (Thru), and the fade before its end.
    LoopPlay, SliceThru, SliceFade,
    // LOOP: Envolvente and the filter's envelope start again at each hit (Normal) or follow each slice with its
    // own release (Slice).
    LoopHitEnv,
    // Por golpe: Mecanismo strikes again at each hit as well (TIMBRE).
    MechPerHit,
    // LOOP Normal: the part of the loop that plays (Ventana, in beats; Completo: all of it) and where it starts
    // (Posición, in beats from the loop's start).
    LoopWinLen, LoopWinPos,
    // The key of the music (the arpeggiator and sequencer follow it): where it comes from (set by hand, estimated
    // from the sample, or from the notes played) and, by hand, its tonic and scale.
    KeySource, KeyRoot, KeyScale,
    // ARP · SEQ: the step engine (StepSeq.h): on, mode, the arpeggio's order and the sequence's direction, the
    // step's length, octaves, swing, gate, latch, the DAW's tempo or its own, and how full Generar makes it.
    ArpOn, ArpMode, ArpStyle, SeqDir, ArpRate, ArpOctaves, ArpSwing, ArpGate, ArpLatch, ArpSync, ArpBpm, ArpDensity,
    // Engine E: grains placed a whole number of the note's periods apart (Coherente), so that they add up in phase.
    GrainCoherent,
    // Filter type Armónico: how much the harmonics of the note in its region are raised (+) or lowered (-), and how
    // wide that region is (octaves around Frecuencia). Morph/DJ is then the balance of odd and even harmonics.
    FltHarmGain, FltHarmRegion,
    // Engines B and D: the sustain takes on the colour the sample itself had at that moment (0: as before, still).
    SustainMotion,
    // The sidechain as a source of the matrix: how much it is raised, how its level is followed, and what of it.
    ScSens, ScAttack, ScRelease, ScResponse,

    // FX slots follow: type, on, mix and fxParamsPerSlot generic values each.
    FxSlotBase
};

constexpr int fxMainSlots = 6, fxBusSlots = 4;
constexpr int numFxSlots = fxMainSlots + 2 * fxBusSlots;
constexpr int fxParamsPerSlot = 10;
constexpr int fxSlotStride = 3 + fxParamsPerSlot;
constexpr int Count = FxSlotBase + numFxSlots * fxSlotStride;

constexpr int fxType (int slot) noexcept { return FxSlotBase + slot * fxSlotStride; }
constexpr int fxOn (int slot) noexcept { return fxType (slot) + 1; }
constexpr int fxMix (int slot) noexcept { return fxType (slot) + 2; }
constexpr int fxParam (int slot, int k) noexcept { return fxType (slot) + 3 + k; }

constexpr int numLfos = 3;
constexpr int lfoStride = Lfo2Rate - Lfo1Rate;
constexpr int numSlots = 8;
constexpr int slotStride = M2Src - M1Src;

enum EngineMode { EngineRebuild, EngineReenvelope, EngineResynth, EngineHybrid, EngineGranular, EngineDrone };
enum SustainTypeId { SustainNatural, SustainHold };
enum SustainModeId { SustainLoop, SustainGranular, SustainFreeze };
enum EnvModeId { EnvReshape, EnvSynth };
enum WarpModeId { WarpRepitch, WarpTones, WarpTexture };
enum PlayModeId { PlayOneShot, PlayLoop };
// 2 was Tonal (overlapping grains) up to 0.9.5: measured, its grains beat against each other (the peaks of a
// chord moved, a note smeared into its neighbours), so a project saved with it now plays Complex.
enum LoopAlgoId { LoopAuto, LoopBeatsAlgo, LoopComplex, LoopComplexPro, LoopRubberBand };
enum LoopKindId { LoopNormal, LoopSlice };
enum SliceById { SliceTransients, SliceGrid, SliceManual };
// Loop length choices (LoopBeats), in beats; 0 = as detected.
constexpr int maxSlices = 128;
constexpr int loopBeatChoices[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };
// Ventana: its length in beats for each choice (0: Completo, the whole loop).
constexpr double loopWindowBeats[] = { 0.0, 16.0, 8.0, 4.0, 2.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125 };
enum PolyModeId { Poly, Mono, Legato };
enum FilterType { LP, HP, BP, Notch, Morph, DJ, Comb, Vowel, Harmonic };
enum FilterCircuit { Clean, OSR, MS2, SMP, PRD };
enum LfoShape { Sine, Triangle, SawUp, SawDown, Square, SampleHold, Drift };
// What the sidechain source follows: its level, the level of its lows, mids or highs, or its brightness.
enum ScResponseId { ScLevel, ScLows, ScMids, ScHighs, ScBright };

enum ModSource
{
    SrcNone, SrcLfo1, SrcLfo2, SrcLfo3, SrcEnv2, SrcAmpEnv, SrcVelocity, SrcNote,
    SrcModWheel, SrcAftertouch, SrcMacro1, SrcMacro2, SrcMacro3, SrcMacro4, SrcRandom, SrcSidechain,
    NumSources
};

enum ModDest
{
    DstNone, DstPitch, DstVolume, DstPan, DstCutoff, DstRes, DstDrive, DstMorph,
    DstStart, DstSpray, DstGrain, DstDetune, DstLfo1Rate, DstLfo2Rate, DstTransient, DstBow, DstBreath, DstFine,
    NumDests
};

struct Def
{
    enum Kind { Float, Choice, Bool };

    const char* id;
    const char* name;           // UTF-8
    Kind kind;
    float min = 0, max = 1, def = 0;
    float centre = 0;           // skew centre, 0 = linear
    float step = 0;
    const char* choices = "";   // "a|b|c"
    const char* unit = "";
};

const std::array<Def, Count>& defs();
juce::String fxSlotName (int slot);   // "Main 1", "Bus 2 · 3"
juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
juce::StringArray choicesOf (int index);

inline juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
} // namespace P

using ParamSnapshot = std::array<float, P::Count>;
