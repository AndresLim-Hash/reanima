#include "Params.h"
#include <string>
#include <vector>

namespace P
{
namespace
{
constexpr const char* lfoDivs = "1/32|1/16T|1/16|1/8T|1/8|1/4T|1/4|1/2|1 comp|2 comp|4 comp";
constexpr const char* lfoShapes = "Seno|Triángulo|Sierra ↑|Sierra ↓|Cuadrada|S&H";
constexpr const char* sources = "—|LFO 1|LFO 2|LFO 3|Env 2|Env Amp|Velocidad|Nota|Mod Wheel|Aftertouch|Macro 1|Macro 2|Macro 3|Macro 4|Aleatorio";
constexpr const char* dests = "—|Tono|Volumen|Pan|Frecuencia filtro|Resonancia|Drive filtro|Morph filtro|Inicio|Spray|Grano|Detune unison|Vel. LFO 1|Vel. LFO 2|Transitorio|Arco/aliento|Respiración";

using K = Def::Kind;

std::array<Def, Count> buildDefs()
{
    std::array<Def, Count> d {};

    auto f = [&] (int i, const char* id, const char* name, float mn, float mx, float df, float centre = 0, const char* unit = "", float step = 0)
    { d[(size_t) i] = { id, name, K::Float, mn, mx, df, centre, step, "", unit }; };
    auto c = [&] (int i, const char* id, const char* name, const char* choices, int df = 0)
    { d[(size_t) i] = { id, name, K::Choice, 0, 0, (float) df, 0, 0, choices, "" }; };
    auto b = [&] (int i, const char* id, const char* name, bool df)
    { d[(size_t) i] = { id, name, K::Bool, 0, 1, df ? 1.0f : 0.0f, 0, 0, "", "" }; };

    c (Engine, "engine", "Motor", "A · Reconstrucción|B · Reenvolvente|C · Resíntesis|D · Híbrido|E · Granular|F · Drone Maker");
    c (SustainMode, "sustainMode", "Modo sustain", "Loop|Granular|Freeze", 1);
    c (SustainType, "sustainType", "Sustain", "Natural (pedal)|Sostenido");
    b (Stretch, "stretch", "Stretch", true);
    f (Flatten, "flatten", "Aplanar", 0, 1, 0);
    f (GrainMs, "grainMs", "Grano", 10, 1000, 80, 100, "ms");
    f (Spray, "spray", "Spray", 0, 1, 0.3f);
    f (GranSpeed, "granSpeed", "Deriva", -1, 1, 0);
    f (Breath, "breath", "Respiración", 0, 1, 0.35f);
    f (TransientMs, "transientMs", "Transitorio", 2, 200, 20, 30, "ms");
    f (TransientGain, "transientGain", "Gan. trans.", -24, 12, 0, 0, "dB");
    f (StartPos, "startPos", "Inicio", 0, 1, 0);
    b (Reverse, "reverse", "Reverse", false);
    f (Transpose, "transpose", "Transponer", -48, 48, 0, 0, "st", 1);
    f (Fine, "fine", "Fino", -100, 100, 0, 0, "ct");
    b (SnapC, "snapC", "Snap a C", false);
    f (Bits, "bits", "Bits", 1, 24, 24, 0, "", 1);
    f (Downsample, "downsample", "Downsample", 1, 32, 1, 4, "x");
    f (Variation, "variation", "Variación", 0, 1, 0);

    f (Bow, "bow", "Arco/aliento", 0, 1, 0.6f);
    f (RelDark, "relDark", "Apagado", 0, 1, 0.5f);
    f (VelBright, "velBright", "Fuerza → brillo", 0, 1, 0.5f);
    f (RelNoise, "relNoise", "Mecanismo", 0, 1, 0);
    f (Partials, "partials", "Parciales", 4, 128, 64, 0, "", 1);
    f (NoiseLevel, "noiseLevel", "Ruido (C)", 0, 2, 1);

    f (AmpA, "ampA", "Ataque", 0.1f, 20000, 5, 300, "ms");
    f (AmpCurve, "ampCurve", "Curva", 0.2f, 5, 1, 1);
    f (AmpD, "ampD", "Decay", 1, 30000, 300, 1000, "ms");
    f (AmpS, "ampS", "Sustain", 0, 1, 0.5f);
    f (AmpFade, "ampFade", "Caída sus.", 0, 120, 0, 15, "dB/s");
    f (AmpR, "ampR", "Release", 1, 30000, 300, 1000, "ms");
    f (VelAmp, "velAmp", "Vel → Amp", 0, 1, 0.7f);

    c (PolyMode, "polyMode", "Modo", "Poly|Mono|Legato");
    f (Glide, "glide", "Glide", 0, 5000, 0, 300, "ms");
    f (Unison, "unison", "Voces", 1, 16, 1, 0, "", 1);
    f (UniDetune, "uniDetune", "Detune", 0, 100, 25, 0, "%");
    f (UniBlend, "uniBlend", "Blend", 0, 1, 0.5f);
    f (UniWidth, "uniWidth", "Ancho", 0, 1, 0.8f);
    f (Volume, "volume", "Volumen", -60, 12, -6, -6, "dB");     // room for chords and effects
    f (Pan, "pan", "Pan", -1, 1, 0);
    b (MonoOut, "monoOut", "Mono", false);
    b (EnvLock, "envLock", "Fijar ADSR", false);
    c (EnvMode, "envMode", "Modo ADSR", "Remodelar el sample|Sintetizador");
    f (Punch, "punch", "Golpe", 0, 1, 0.5f);
    f (AtAmount, "atAmount", "Aftertouch", 0, 1, 0.5f);
    f (VibDepth, "vibDepth", "Vibrato", 0, 1, 0.5f);
    f (VibRate, "vibRate", "Vel. vibrato", 3, 9, 5.5f, 0, "Hz");
    f (BendRange, "bendRange", "Bend", 0, 24, 2, 0, "st", 1);
    f (GrainPos, "grainPos", "Posición", 0, 1, 0.3f);
    f (GrainDensity, "grainDensity", "Densidad", 2, 200, 30, 30, "/s");
    f (GrainPitch, "grainPitch", "Pitch aleat.", 0, 12, 0, 2, "st");
    f (GrainReverse, "grainReverse", "Al revés", 0, 1, 0);
    f (GrainWidth, "grainWidth", "Ancho granos", 0, 1, 0.5f);
    f (GrainMix, "grainMix", "Textura", 0, 1, 0);

    f (EndPos, "endPos", "Fin", 0, 1, 1);
    f (FadeIn, "fadeIn", "Fundido entrada", 0, 1000, 0, 100, "ms");
    f (FadeOut, "fadeOut", "Fundido salida", 0, 2000, 0, 200, "ms");
    b (LoopOn, "loopOn", "Bucle", false);
    f (LoopStart, "loopStart", "Inicio bucle", 0, 1, 0.3f);
    f (LoopEnd, "loopEnd", "Fin bucle", 0, 1, 0.7f);
    f (LoopXfade, "loopXfade", "Cruce", 0, 500, 30, 60, "ms");
    f (SampleGain, "sampleGain", "Ganancia", -24, 24, 0, 0, "dB");
    b (Normalize, "normalize", "Normalizar", false);
    f (PitchEnvAmt, "pitchEnvAmt", "Pitch env.", -48, 48, 0, 0, "st");
    f (PitchEnvTime, "pitchEnvTime", "Tiempo pitch", 1, 5000, 120, 200, "ms");
    c (MechType, "mechType", "Tipo mecanismo", "Piano|Púa|Arco|Tecla|Soplo|Del sample|Piano real|Pulsado real|Arco real|Soplo real|Púa guitarra");
    f (MechAdapt, "mechAdapt", "Adaptar", 0, 1, 0.75f);
    f (MechDown, "mechDown", "Entrada", -60, 12, 0, -12, "dB");
    f (MechUp, "mechUp", "Salida", -60, 12, 0, -12, "dB");
    b (MechOn, "mechOn", "Mecanismo on", true);
    c (WarpMode, "warpMode", "Warp", "Re-Pitch|Tonos|Textura");

    f (DroneStretch, "droneStretch", "Estirar", 1, 1000, 8, 30, "x");
    b (DroneFreeze, "droneFreeze", "Congelar", false);
    c (DroneSize, "droneSize", "Tamaño FFT", "0,04 s|0,09 s|0,17 s|0,34 s|0,7 s|1,4 s", 3);
    f (DroneHarm, "droneHarm", "Armónicos", 0, 1, 0);
    f (DroneHarmWidth, "droneHarmWidth", "Ancho arm.", 1, 200, 25, 30, "ct");
    f (DroneTonal, "droneTonal", "Tono/ruido", -1, 1, 0);
    f (DroneSpread, "droneSpread", "Spread", 0, 1, 0);
    f (DroneSub2, "droneSub2", "-2 oct", 0, 1, 0);
    f (DroneSub1, "droneSub1", "-1 oct", 0, 1, 0);
    f (DroneFifth, "droneFifth", "Quinta", 0, 1, 0);
    f (DroneUp1, "droneUp1", "+1 oct", 0, 1, 0);
    f (DroneUp2, "droneUp2", "+2 oct", 0, 1, 0);
    f (DroneShift, "droneShift", "Desplazar", -1000, 1000, 0, 0, "Hz");
    f (DroneWidth, "droneWidth", "Estéreo", 0, 1, 1);
    b (DroneCurveOn, "droneCurveOn", "Filtro dibujado", false);

    b (FltOn, "fltOn", "Filtro", false);
    c (FltType, "fltType", "Tipo", "Lowpass|Highpass|Bandpass|Notch|Morph|DJ|Comb|Vowel");
    c (FltSlope, "fltSlope", "Pendiente", "12 dB|24 dB", 1);
    c (FltCircuit, "fltCircuit", "Circuito", "Clean|OSR|MS2|SMP|PRD");
    f (FltFreq, "fltFreq", "Frecuencia", 20, 20000, 2000, 1000, "Hz");
    f (FltRes, "fltRes", "Resonancia", 0, 1, 0.2f);
    f (FltDrive, "fltDrive", "Drive", 0, 24, 0, 0, "dB");
    f (FltMorph, "fltMorph", "Morph/DJ", 0, 1, 0.5f);
    f (FltKey, "fltKey", "Keytrack", 0, 1, 0);
    f (FltEnvAmt, "fltEnvAmt", "Env 2 →", -1, 1, 0);
    f (FltFollow, "fltFollow", "Follower", -1, 1, 0);
    f (FltFolAtk, "fltFolAtk", "Fol. ataque", 0.1f, 100, 5, 10, "ms");
    f (FltFolRel, "fltFolRel", "Fol. release", 5, 1000, 100, 150, "ms");
    f (FltMix, "fltMix", "Dry/Wet", 0, 1, 1);

    f (E2A, "e2A", "Ataque", 0.1f, 20000, 5, 300, "ms");
    f (E2D, "e2D", "Decay", 1, 30000, 400, 1000, "ms");
    f (E2S, "e2S", "Sustain", 0, 1, 0.3f);
    f (E2R, "e2R", "Release", 1, 30000, 300, 1000, "ms");

    static const char* lfoIds[numLfos][5] = {
        { "lfo1Rate", "lfo1Sync", "lfo1Div", "lfo1Shape", "lfo1Retrig" },
        { "lfo2Rate", "lfo2Sync", "lfo2Div", "lfo2Shape", "lfo2Retrig" },
        { "lfo3Rate", "lfo3Sync", "lfo3Div", "lfo3Shape", "lfo3Retrig" } };

    for (int l = 0; l < numLfos; ++l)
    {
        const int base = Lfo1Rate + l * lfoStride;
        f (base + 0, lfoIds[l][0], "Velocidad", 0.01f, 30, 2, 2, "Hz");
        b (base + 1, lfoIds[l][1], "Sync", false);
        c (base + 2, lfoIds[l][2], "División", lfoDivs, 6);
        c (base + 3, lfoIds[l][3], "Forma", lfoShapes);
        b (base + 4, lfoIds[l][4], "Retrig", true);
    }

    f (Macro1, "macro1", "Macro 1", 0, 1, 0);
    f (Macro2, "macro2", "Macro 2", 0, 1, 0);
    f (Macro3, "macro3", "Macro 3", 0, 1, 0);
    f (Macro4, "macro4", "Macro 4", 0, 1, 0);

    static const char* slotIds[numSlots][3] = {
        { "m1Src", "m1Dst", "m1Amt" }, { "m2Src", "m2Dst", "m2Amt" }, { "m3Src", "m3Dst", "m3Amt" }, { "m4Src", "m4Dst", "m4Amt" },
        { "m5Src", "m5Dst", "m5Amt" }, { "m6Src", "m6Dst", "m6Amt" }, { "m7Src", "m7Dst", "m7Amt" }, { "m8Src", "m8Dst", "m8Amt" } };

    for (int s = 0; s < numSlots; ++s)
    {
        const int base = M1Src + s * slotStride;
        c (base + 0, slotIds[s][0], "Fuente", sources);
        c (base + 1, slotIds[s][1], "Destino", dests);
        f (base + 2, slotIds[s][2], "Cantidad", -1, 1, 0);
    }

    f (FxMainLevel, "fxMainLevel", "Main nivel", 0, 1, 1);
    f (FxBus1Send, "fxBus1Send", "Envío Bus 1", 0, 1, 0);
    f (FxBus2Send, "fxBus2Send", "Envío Bus 2", 0, 1, 0);
    f (FxBus1Level, "fxBus1Level", "Nivel Bus 1", 0, 1, 1);
    f (FxBus2Level, "fxBus2Level", "Nivel Bus 2", 0, 1, 1);
    b (FxBus1ToMain, "fxBus1ToMain", "Bus 1 → Main", false);
    b (FxBus2ToMain, "fxBus2ToMain", "Bus 2 → Main", false);
    f (MonoBass, "monoBass", "Graves mono", 0, 300, 100, 100, "Hz");
    f (GrainFade, "grainFade", "Fundido gran.", 10, 3000, 150, 300, "ms");
    f (GrainBlend, "grainBlend", "Mezcla gran.", 0, 1, 1);
    c (PlayMode, "playMode", "Reproducción", "One shot|Loop");
    b (LoopWarp, "loopWarp", "Warp loop", true);
    b (LoopSync, "loopSync", "Tempo del host", true);
    c (LoopBeats, "loopBeats", "Duración", "Auto|1 tiempo|2 tiempos|3 tiempos|1 compás|6 tiempos|2 compases|3 compases|4 compases|6 compases|8 compases|12 compases|16 compases");
    c (LoopAlgo, "loopAlgo", "Calidad", "Auto|Beats|Complex|Complex Pro|Rubber Band");
    c (LoopKind, "loopKind", "Modo loop", "Normal|Slice");
    f (LoopBpm, "loopBpm", "BPM original", 0, 250, 0, 0, "", 0.01f);
    c (SliceBy, "sliceBy", "Cortes", "Transitorios|Rejilla|A mano");
    c (SliceDivision, "sliceDivision", "División", "1 compás|1/2|1/4|1/8|1/16|1/32", 3);
    f (SliceSensitivity, "sliceSensitivity", "Sensibilidad", 0, 1, 0.5f);
    f (SliceStart, "sliceStart", "Primera nota", 24, 96, 60, 0, "", 1);
    b (LoopPlay, "loopPlay", "Play con el host", false);
    b (SliceThru, "sliceThru", "Thru", false);
    f (SliceFade, "sliceFade", "Fundido", 0, 100, 2, 10, "ms");
    b (LoopHitEnv, "loopHitEnv", "Por golpe", false);

    // Slot ids and names must outlive the defs, so they live in static storage.
    static std::vector<std::string> strings;
    strings.reserve ((size_t) numFxSlots * fxSlotStride * 2);
    auto keep = [] (std::string str) { strings.push_back (std::move (str)); return strings.back().c_str(); };
    static const char* types = "—|Hyper/Dimension|Distorsión|Flanger|Phaser|Chorus|Delay|Compresor|Reverb|EQ|Filtro|Bode|Utility|Splitter L/H|Splitter L/M/H|Splitter M/S";

    for (int slot = 0; slot < numFxSlots; ++slot)
    {
        const std::string sid = "fx" + std::to_string (slot);
        const std::string sname = fxSlotName (slot).toStdString();
        c (fxType (slot), keep (sid + "Type"), keep (sname + " tipo"), types);
        b (fxOn (slot), keep (sid + "On"), keep (sname + " on"), true);
        f (fxMix (slot), keep (sid + "Mix"), keep (sname + " mix"), 0, 1, 1);
        for (int k = 0; k < fxParamsPerSlot; ++k)
            f (fxParam (slot, k), keep (sid + "P" + std::to_string (k + 1)), keep (sname + " P" + std::to_string (k + 1)), 0, 1, 0.5f);
    }

    return d;
}
} // namespace

juce::String fxSlotName (int slot)
{
    if (slot < fxMainSlots)
        return "Main " + juce::String (slot + 1);
    const int bus = (slot - fxMainSlots) / fxBusSlots;
    return "Bus " + juce::String (bus + 1) + u8 (" · ") + juce::String ((slot - fxMainSlots) % fxBusSlots + 1);
}

const std::array<Def, Count>& defs()
{
    static const auto d = buildDefs();
    return d;
}

juce::StringArray choicesOf (int index)
{
    return juce::StringArray::fromTokens (u8 (defs()[(size_t) index].choices), "|", "");
}

// The panels of the plugin give a knob its context; a host's automation list does not.
static juce::String hostName (int index)
{
    const auto name = u8 (defs()[(size_t) index].name);
    if (index > FltOn && index <= FltMix)
        return "Filtro " + name;
    if (index >= E2A && index <= E2R)
        return "Env 2 " + name;
    if (index >= Lfo1Rate && index < Macro1)
        return "LFO " + juce::String (1 + (index - Lfo1Rate) / lfoStride) + " " + name;
    if (index >= M1Src && index < FxMainLevel)
        return "Mod " + juce::String (1 + (index - M1Src) / slotStride) + " " + name;
    return name;
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int i = 0; i < Count; ++i)
    {
        const auto& d = defs()[(size_t) i];
        jassert (d.id != nullptr);
        const juce::ParameterID pid { d.id, i >= MechAdapt && i <= WarpMode ? 2 : 1 };      // 2: added in 0.9
        const auto name = hostName (i);

        switch (d.kind)
        {
            case Def::Choice:
                layout.add (std::make_unique<juce::AudioParameterChoice> (pid, name, choicesOf (i), (int) d.def));
                break;

            case Def::Bool:
                layout.add (std::make_unique<juce::AudioParameterBool> (pid, name, d.def > 0.5f));
                break;

            case Def::Float:
            {
                juce::NormalisableRange<float> range (d.min, d.max, d.step);
                if (d.centre > d.min && d.centre < d.max)
                    range.setSkewForCentre (d.centre);

                layout.add (std::make_unique<juce::AudioParameterFloat> (
                    pid, name, range, d.def,
                    juce::AudioParameterFloatAttributes().withLabel (u8 (d.unit))));
                break;
            }
        }
    }

    return layout;
}
} // namespace P
