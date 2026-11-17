#include "FxRack.h"
#include "DspUtil.h"
#include "Filters.h"
#include <cstring>

using namespace Dsp;

namespace Fx
{
namespace
{
constexpr const char* divisions = "1/32|1/16T|1/16|1/8T|1/8|1/4T|1/4|1/2|1 comp|2 comp|4 comp";

using PS = ParamSpec;

PS knob (const char* n, float mn, float mx, float df, const char* unit = "", float centre = 0, float step = 0)
{
    PS s { n };
    s.min = mn; s.max = mx; s.def = df; s.unit = unit; s.step = step;
    if (centre > mn && centre < mx) { s.curve = PS::Skew; s.centre = centre; }
    return s;
}

PS sym (const char* n, float mx, float df, const char* unit)
{
    PS s { n };
    s.min = -mx; s.max = mx; s.def = df; s.unit = unit; s.curve = PS::Symmetric;
    return s;
}

PS choice (const char* n, const char* choices, int df = 0)
{
    PS s { n };
    s.kind = PS::Choice; s.choices = choices; s.def = (float) df;
    s.numChoices = 1 + (int) std::count (choices, choices + std::strlen (choices), '|');
    return s;
}

PS toggle (const char* n, bool df)
{
    PS s { n };
    s.kind = PS::Toggle; s.def = df ? 1.0f : 0.0f;
    return s;
}

std::vector<TypeSpec> buildSpecs()
{
    std::vector<TypeSpec> t ((size_t) NumTypes);

    t[Empty] = { "—", 1.0f, {} };

    t[HyperDim] = { "Hyper/Dimension", 1.0f, {
        knob ("Velocidad", 0.05f, 10, 0.7f, "Hz", 1),
        knob ("Detune", 0, 1, 0.3f),
        knob ("Voces", 1, 7, 4, "", 0, 1),
        knob ("Mix Hyper", 0, 1, 0.5f),
        knob ("Tamaño Dim.", 0, 1, 0.5f),
        knob ("Mix Dim.", 0, 1, 0.5f) } };

    t[Distortion] = { "Distorsión", 1.0f, {
        choice ("Modo", "Tube|Soft Clip|Hard Clip|Diode 1|Diode 2|Lin. Fold|Sin Fold|Zero-Square|Downsample|Asym|Rectify|X-Shaper|X-Shaper Asym|Sine Shaper|Stomp Box|Overdrive"),
        knob ("Drive", 0, 1, 0.3f),
        knob ("Bias DC", -1, 1, 0),
        choice ("Filtro", "Off|Pre|Post"),
        choice ("Tipo filtro", "LP|BP|HP"),
        knob ("Frecuencia", 20, 20000, 2000, "Hz", 1000),
        knob ("Q", 0.3f, 10, 0.7f, "", 1) } };

    t[Flanger] = { "Flanger", 1.0f, {
        knob ("Velocidad", 0.01f, 10, 0.3f, "Hz", 0.5f),
        knob ("Profundidad", 0, 1, 0.6f),
        knob ("Feedback", -0.95f, 0.95f, 0.5f),
        knob ("Fase estéreo", 0, 180, 90, "°") } };

    t[Phaser] = { "Phaser", 1.0f, {
        knob ("Velocidad", 0.01f, 10, 0.4f, "Hz", 0.5f),
        knob ("Profundidad", 0, 1, 0.6f),
        knob ("Frecuencia", 50, 10000, 800, "Hz", 800),
        knob ("Feedback", -0.95f, 0.95f, 0.4f),
        knob ("Fase estéreo", 0, 180, 90, "°"),
        choice ("Etapas", "2|4|6|8|12", 2) } };

    t[Chorus] = { "Chorus", 0.5f, {
        knob ("Velocidad", 0.01f, 10, 0.8f, "Hz", 1),
        knob ("Delay 1", 1, 30, 7, "ms"),
        knob ("Delay 2", 1, 30, 11, "ms"),
        knob ("Profundidad", 0, 1, 0.4f),
        knob ("Feedback", 0, 0.95f, 0.1f),
        knob ("Filtro LP", 200, 20000, 8000, "Hz", 3000) } };

    t[Delay] = { "Delay", 0.3f, {
        toggle ("Sync", true),
        knob ("Tiempo L", 1, 2000, 375, "ms", 300),
        knob ("Tiempo R", 1, 2000, 375, "ms", 300),
        choice ("División L", divisions, 4),
        choice ("División R", divisions, 4),
        toggle ("Link", true),
        knob ("Feedback", 0, 0.98f, 0.4f),
        choice ("Modo", "Normal|Ping-Pong|Tap→Delay", 1),
        knob ("Filtro", 100, 15000, 2000, "Hz", 1500),
        knob ("Ancho filtro", 0.2f, 8, 6, "oct") } };

    t[Compressor] = { "Compresor", 1.0f, {
        knob ("Threshold", -60, 0, -18, "dB"),
        knob ("Ratio", 1, 20, 4, ":1", 4),
        knob ("Ataque", 0.1f, 100, 10, "ms", 10),
        knob ("Release", 5, 1000, 120, "ms", 120),
        knob ("Ganancia", 0, 30, 0, "dB"),
        toggle ("Multibanda", false),
        knob ("Upward", 0, 1, 0.5f) } };

    t[Reverb] = { "Reverb", 0.3f, {
        choice ("Tipo", "Plate|Hall|Vintage|Nitrous|Basin", 1),
        knob ("Tamaño", 0, 1, 0.6f),
        knob ("Decay", 0.1f, 20, 2.5f, "s", 2),
        knob ("Pre-delay", 0, 250, 20, "ms"),
        knob ("Amortig.", 0, 1, 0.4f),
        knob ("Low cut", 20, 1000, 120, "Hz", 120),
        knob ("High cut", 1000, 20000, 9000, "Hz", 6000),
        knob ("Ancho", 0, 1, 1) } };

    t[EQ] = { "EQ", 1.0f, {
        choice ("Banda 1", "Low Shelf|Peak|Low Cut"),
        knob ("Freq 1", 20, 20000, 150, "Hz", 1000),
        knob ("Gain 1", -24, 24, 0, "dB"),
        knob ("Q 1", 0.1f, 10, 0.7f, "", 1),
        choice ("Banda 2", "High Shelf|Peak|High Cut"),
        knob ("Freq 2", 20, 20000, 5000, "Hz", 1000),
        knob ("Gain 2", -24, 24, 0, "dB"),
        knob ("Q 2", 0.1f, 10, 0.7f, "", 1) } };

    t[Filter] = { "Filtro", 1.0f, {
        choice ("Tipo", "Lowpass|Highpass|Bandpass|Notch|Morph|DJ|Comb|Vowel"),
        choice ("Pendiente", "12 dB|24 dB", 1),
        choice ("Circuito", "Clean|OSR|MS2|SMP|PRD"),
        knob ("Frecuencia", 20, 20000, 2000, "Hz", 1000),
        knob ("Resonancia", 0, 1, 0.2f),
        knob ("Drive", 0, 24, 0, "dB"),
        knob ("Morph/DJ", 0, 1, 0.5f) } };

    t[Bode] = { "Bode", 1.0f, {
        sym ("Desplaz.", 2000, 0, "Hz"),
        knob ("Spread", 0, 1, 0),
        knob ("Feedback", 0, 0.9f, 0) } };

    t[Utility] = { "Utility", 1.0f, {
        knob ("Ganancia", -24, 24, 0, "dB"),
        knob ("Pan", -1, 1, 0),
        knob ("Ancho", 0, 2, 1),
        knob ("Graves mono", 0, 500, 0, "Hz", 120),
        toggle ("Invertir L", false),
        toggle ("Invertir R", false) } };

    t[SplitLH] = { "Splitter L/H", 1.0f, {
        knob ("Cruce", 40, 16000, 600, "Hz", 600),
        knob ("Nivel graves", -24, 12, 0, "dB"),
        knob ("Nivel agudos", -24, 12, 0, "dB"),
        toggle ("Procesar graves", true),
        toggle ("Procesar agudos", true) } };

    t[SplitLMH] = { "Splitter L/M/H", 1.0f, {
        knob ("Cruce 1", 40, 8000, 250, "Hz", 250),
        knob ("Cruce 2", 200, 16000, 3000, "Hz", 3000),
        knob ("Nivel graves", -24, 12, 0, "dB"),
        knob ("Nivel medios", -24, 12, 0, "dB"),
        knob ("Nivel agudos", -24, 12, 0, "dB"),
        toggle ("Procesar graves", true),
        toggle ("Procesar medios", true),
        toggle ("Procesar agudos", true) } };

    t[SplitMS] = { "Splitter M/S", 1.0f, {
        knob ("Nivel mid", -24, 12, 0, "dB"),
        knob ("Nivel side", -24, 12, 0, "dB"),
        toggle ("Procesar mid", true),
        toggle ("Procesar side", true) } };

    return t;
}
} // namespace

const TypeSpec& spec (int type)
{
    static const auto specs = buildSpecs();
    return specs[(size_t) juce::jlimit (0, NumTypes - 1, type)];
}

juce::StringArray typeNames()
{
    juce::StringArray names;
    for (int i = 0; i < NumTypes; ++i)
        names.add (juce::String::fromUTF8 (spec (i).name));
    return names;
}

juce::StringArray choicesOf (const ParamSpec& s)
{
    return juce::StringArray::fromTokens (juce::String::fromUTF8 (s.choices), "|", "");
}

int choiceIndex (const ParamSpec& s, float norm)
{
    return juce::jlimit (0, std::max (0, s.numChoices - 1), (int) (norm * (float) s.numChoices));
}

float toValue (const ParamSpec& s, float norm)
{
    norm = juce::jlimit (0.0f, 1.0f, norm);
    switch (s.kind)
    {
        case ParamSpec::Choice: return (float) choiceIndex (s, norm);
        case ParamSpec::Toggle: return norm > 0.5f ? 1.0f : 0.0f;
        case ParamSpec::Knob:   break;
    }

    float v;
    if (s.curve == ParamSpec::Symmetric)
    {
        const float x = norm * 2.0f - 1.0f;
        v = (x < 0.0f ? -1.0f : 1.0f) * x * x * s.max;
    }
    else if (s.curve == ParamSpec::Skew)
    {
        const float skew = std::log (0.5f) / std::log ((s.centre - s.min) / (s.max - s.min));
        v = s.min + (s.max - s.min) * std::pow (norm, 1.0f / skew);
    }
    else
        v = s.min + (s.max - s.min) * norm;

    if (s.step > 0.0f)
        v = s.min + std::round ((v - s.min) / s.step) * s.step;
    return v;
}

float toNorm (const ParamSpec& s, float value)
{
    switch (s.kind)
    {
        case ParamSpec::Choice: return (value + 0.5f) / (float) std::max (1, s.numChoices);
        case ParamSpec::Toggle: return value > 0.5f ? 1.0f : 0.0f;
        case ParamSpec::Knob:   break;
    }

    value = juce::jlimit (s.min, s.max, value);
    if (s.curve == ParamSpec::Symmetric)
    {
        const float x = std::sqrt (std::abs (value) / s.max);
        return 0.5f + 0.5f * (value < 0.0f ? -x : x);
    }
    if (s.curve == ParamSpec::Skew)
    {
        const float skew = std::log (0.5f) / std::log ((s.centre - s.min) / (s.max - s.min));
        return std::pow ((value - s.min) / (s.max - s.min), skew);
    }
    return (value - s.min) / (s.max - s.min);
}

juce::String format (const ParamSpec& s, float norm)
{
    if (s.kind == ParamSpec::Choice)
        return choicesOf (s)[choiceIndex (s, norm)];
    if (s.kind == ParamSpec::Toggle)
        return norm > 0.5f ? "On" : "Off";

    const float v = toValue (s, norm);
    const juce::String unit = juce::String::fromUTF8 (s.unit);

    // juce::String (double, 0) prints every decimal, so whole numbers use roundToInt.
    auto num = [] (float x) { return std::abs (x) < 10.0f ? juce::String (x, 1) : juce::String (juce::roundToInt (x)); };

    if (unit == "ms")
        return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " s" : num (v) + " ms";
    if (unit == "Hz")
        return std::abs (v) >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
    if (unit.isNotEmpty())
        return num (v) + " " + unit;
    if (s.step >= 1.0f)
        return juce::String (juce::roundToInt (v));
    if (s.min >= 0.0f && s.max <= 1.0f)
        return juce::String (juce::roundToInt (v * 100.0f)) + " %";
    if (s.min == -1.0f && s.max == 1.0f)
        return juce::String (juce::roundToInt (v * 100.0f)) + " %";
    return juce::String (v, 2);
}

//==============================================================================
namespace
{
// Removes DC with its corner at 5 Hz, far below anything that should be heard.
struct DcBlocker
{
    float x1[2] {}, y1[2] {}, pole = 0.999f;

    void prepare (double sr) noexcept { pole = 1.0f - (float) (twoPi * 5.0 / sr); }

    float process (int ch, float x) noexcept
    {
        const float y = x - x1[ch] + pole * y1[ch];
        x1[ch] = x;
        y1[ch] = y;
        return y;
    }
    void reset() noexcept { x1[0] = x1[1] = y1[0] = y1[1] = 0.0f; }
};

//------------------------------------------------------------------------------
class HyperDimModule final : public Module
{
public:
    void prepare (double sr, int) override
    {
        sampleRate = sr;
        for (auto& d : lines) d.prepare ((int) (sr * 0.06));
        for (auto& d : dim) d.prepare ((int) (sr * 0.05));
        juce::Random rng (11);
        for (auto& ph : phases) ph = rng.nextDouble();
        reset();
    }

    void reset() override { for (auto& d : lines) d.reset(); for (auto& d : dim) d.reset(); }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        const float rate = value (HyperDim, 0, norm), detune = value (HyperDim, 1, norm);
        const int voices = (int) value (HyperDim, 2, norm);
        const float hyperMix = value (HyperDim, 3, norm), size = value (HyperDim, 4, norm), dimMix = value (HyperDim, 5, norm);
        const auto& sn = sine();
        const float sr = (float) sampleRate;
        const float baseDelay = 0.012f * sr, depth = (0.0005f + detune * 0.004f) * sr;
        const float dA = (0.006f + size * 0.018f) * sr, dB = (0.009f + size * 0.025f) * sr;

        for (int i = 0; i < n; ++i)
        {
            const float l = L[i], r = R[i];
            lines[0].push (l);
            lines[1].push (r);

            float hl = 0.0f, hr = 0.0f;
            for (int v = 0; v < voices; ++v)
            {
                phases[(size_t) v] += rate * (1.0 + 0.13 * v) / sampleRate;
                const float mod = sn (phases[(size_t) v]);
                const float d = baseDelay + (float) v * 0.0011f * sr + mod * depth;
                const float pan = voices > 1 ? (float) v / (float) (voices - 1) : 0.5f;
                hl += lines[0].read (d) * (1.0f - pan) * 2.0f;
                hr += lines[1].read (d) * pan * 2.0f;
            }
            const float norm1 = 1.0f / (float) voices;
            float outL = l + (hl * norm1 - l) * hyperMix;
            float outR = r + (hr * norm1 - r) * hyperMix;

            // Dimension: adds decorrelated side content; mono sum unchanged.
            const float mid = 0.5f * (outL + outR);
            dim[0].push (mid);
            dim[1].push (mid);
            const float side = (dim[0].read (dA) - dim[1].read (dB)) * 0.7f * dimMix;
            outL += side;
            outR -= side;

            L[i] = outL;
            R[i] = outR;
        }
    }

private:
    double sampleRate = 48000.0;
    std::array<DelayLine, 2> lines, dim;
    std::array<double, 7> phases {};
};

//------------------------------------------------------------------------------
class DistortionModule final : public Module
{
public:
    void prepare (double sr, int maxBlock) override
    {
        sampleRate = sr;
        dc.prepare (sr);

        // The harmonics of a clipper go far above the audio band: 8x at 44.1 and 48 kHz, 4x above.
        using OS = juce::dsp::Oversampling<float>;
        const size_t stages = sr < 60000.0 ? 3 : 2;
        for (auto* os : { &oversampling, &dryPath })
        {
            *os = std::make_unique<OS> (2, stages, OS::filterHalfBandPolyphaseIIR, true);
            (*os)->initProcessing ((size_t) std::max (1, maxBlock));
        }
        reset();
    }

    void reset() override
    {
        if (oversampling) oversampling->reset();
        if (dryPath) dryPath->reset();
        filter.reset();
        dc.reset();
        holdCount = 0.0f;
        held[0] = held[1] = 0.0f;
    }

    static float shape (int mode, float x) noexcept
    {
        switch (mode)
        {
            case 0:  return x >= 0.0f ? std::tanh (x) : std::exp (std::max (x, -20.0f)) - 1.0f;                 // Tube
            case 1:  return std::tanh (x);                                                                     // Soft Clip
            case 2:  return juce::jlimit (-1.0f, 1.0f, x);                                                      // Hard Clip
            case 3:  return x >= 0.0f ? 1.0f - std::exp (-x) : -0.3f * (1.0f - std::exp (x));                   // Diode 1
            case 4:  return x >= 0.0f ? std::tanh (x) : 0.3f * std::tanh (3.0f * x);                            // Diode 2
            case 5:  { const float t = (x + 1.0f) * 0.25f; return 4.0f * std::abs (t - Dsp::floorFast (t + 0.5f)) - 1.0f; } // Lin. Fold
            case 6:  return std::sin (x * pi * 0.5f);                                                          // Sin Fold
            case 7:  return std::tanh (x * std::abs (x));                                                      // Zero-Square
            case 9:  return std::tanh (x + 0.3f) - 0.2913f;                                                    // Asym
            case 10: return std::abs (std::tanh (x));                                                          // Rectify
            case 11: { const float c = juce::jlimit (-1.5f, 1.5f, x); return c - (4.0f / 27.0f) * c * c * c; }  // X-Shaper
            case 12: { const float c = juce::jlimit (-1.5f, 1.5f, x + 0.25f); return c - (4.0f / 27.0f) * c * c * c - 0.2477f; }
            case 13: return std::sin (x);                                                                      // Sine Shaper
            case 14: return std::tanh (3.0f * x) * 0.7f + juce::jlimit (-1.0f, 1.0f, x) * 0.3f;                 // Stomp Box
            case 15: return x >= 0.0f ? std::tanh (x * 1.2f) : std::tanh (x * 0.9f);                           // Overdrive
            default: return x;
        }
    }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        const int mode = (int) value (Distortion, 0, norm);
        const float drive = value (Distortion, 1, norm);
        const float bias = value (Distortion, 2, norm) * 0.5f;
        const int filterMode = (int) value (Distortion, 3, norm);
        const int filterType = (int) value (Distortion, 4, norm);
        const float gain = dbToGain (drive * 36.0f);
        const float comp = 1.0f / std::sqrt (gain);

        if (filterMode != 0)
        {
            const Biquad::Kind kinds[] = { Biquad::LowPass, Biquad::BandPass, Biquad::HighPass };
            filter.set (kinds[filterType], sampleRate, value (Distortion, 5, norm), value (Distortion, 6, norm));
        }

        if (filterMode == 1)
            for (int i = 0; i < n; ++i) { L[i] = filter.process (0, L[i]); R[i] = filter.process (1, R[i]); }

        if (mode == 8)   // Downsample: sample and hold, no oversampling
        {
            const float factor = std::max (1.0f, (1.0f + drive * 31.0f) * (float) (sampleRate / 44100.0));
            for (int i = 0; i < n; ++i)
            {
                if (holdCount <= 0.0f) { held[0] = L[i]; held[1] = R[i]; holdCount += factor; }
                holdCount -= 1.0f;
                L[i] = held[0];
                R[i] = held[1];
            }
        }
        else
        {
            float* channels[] = { L, R };
            juce::dsp::AudioBlock<float> block (channels, 2, (size_t) n);
            auto up = oversampling->processSamplesUp (block);
            for (size_t ch = 0; ch < 2; ++ch)
            {
                float* d = up.getChannelPointer (ch);
                for (size_t i = 0; i < up.getNumSamples(); ++i)
                    d[i] = shape (mode, d[i] * gain + bias) * comp;
            }
            oversampling->processSamplesDown (block);
            for (int i = 0; i < n; ++i) { L[i] = dc.process (0, L[i]); R[i] = dc.process (1, R[i]); }
        }

        if (filterMode == 2)
            for (int i = 0; i < n; ++i) { L[i] = filter.process (0, L[i]); R[i] = filter.process (1, R[i]); }
    }

    // The oversampling filters shift the phase, so the dry signal goes through the same ones.
    void alignDry (float* L, float* R, int n) override
    {
        float* channels[] = { L, R };
        juce::dsp::AudioBlock<float> block (channels, 2, (size_t) n);
        dryPath->processSamplesUp (block);
        dryPath->processSamplesDown (block);
    }

private:
    double sampleRate = 48000.0;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling, dryPath;
    Biquad filter;
    DcBlocker dc;
    float holdCount = 0.0f, held[2] {};
};

//------------------------------------------------------------------------------
class FlangerModule final : public Module
{
public:
    void prepare (double sr, int) override { sampleRate = sr; for (auto& d : lines) d.prepare ((int) (sr * 0.02)); reset(); }
    void reset() override { for (auto& d : lines) d.reset(); fb[0] = fb[1] = 0.0f; }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        const float rate = value (Flanger, 0, norm), depth = value (Flanger, 1, norm);
        const float feedback = value (Flanger, 2, norm), phaseOffset = value (Flanger, 3, norm) / 360.0f;
        const float sr = (float) sampleRate;
        const auto& sn = sine();
        float* io[] = { L, R };

        for (int i = 0; i < n; ++i)
        {
            phase += rate / sampleRate;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float lfo = 0.5f + 0.5f * sn (phase + (ch == 1 ? phaseOffset : 0.0f));
                const float d = (0.0005f + depth * 0.007f * lfo) * sr;
                const float x = io[ch][i];
                lines[(size_t) ch].push (x + fb[ch] * feedback);
                const float y = lines[(size_t) ch].read (d);
                fb[ch] = y;
                io[ch][i] = 0.5f * (x + y);
            }
        }
    }

private:
    double sampleRate = 48000.0, phase = 0.0;
    std::array<DelayLine, 2> lines;
    float fb[2] {};
};

//------------------------------------------------------------------------------
class PhaserModule final : public Module
{
public:
    void prepare (double sr, int) override { sampleRate = sr; reset(); }
    void reset() override { for (auto& ch : state) ch.fill (0.0f); fb[0] = fb[1] = 0.0f; }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        const float rate = value (Phaser, 0, norm), depth = value (Phaser, 1, norm), freq = value (Phaser, 2, norm);
        const float feedback = value (Phaser, 3, norm), phaseOffset = value (Phaser, 4, norm) / 360.0f;
        static const int stageCounts[] = { 2, 4, 6, 8, 12 };
        const int stages = stageCounts[(int) value (Phaser, 5, norm)];
        const auto& sn = sine();
        float* io[] = { L, R };

        for (int i = 0; i < n; ++i)
        {
            phase += rate / sampleRate;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float lfo = sn (phase + (ch == 1 ? phaseOffset : 0.0f));
                const float fc = juce::jlimit (20.0f, (float) sampleRate * 0.45f, freq * std::exp2 (depth * 3.0f * lfo));
                const float t = std::tan (pi * fc / (float) sampleRate);
                const float a = (t - 1.0f) / (t + 1.0f);
                const float x = io[ch][i];
                float y = x + fb[ch] * feedback;
                auto& st = state[(size_t) ch];
                for (int s = 0; s < stages; ++s)
                {
                    const float out = a * y + st[(size_t) s];
                    st[(size_t) s] = y - a * out;
                    y = out;
                }
                fb[ch] = y;
                io[ch][i] = 0.5f * (x + y);
            }
        }
    }

private:
    double sampleRate = 48000.0, phase = 0.0;
    std::array<std::array<float, 12>, 2> state {};
    float fb[2] {};
};

//------------------------------------------------------------------------------
class ChorusModule final : public Module
{
public:
    void prepare (double sr, int) override { sampleRate = sr; for (auto& d : lines) d.prepare ((int) (sr * 0.05)); reset(); }
    void reset() override { for (auto& d : lines) d.reset(); lp.reset(); fb[0] = fb[1] = 0.0f; }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        const float rate = value (Chorus, 0, norm);
        const float d1 = value (Chorus, 1, norm) * 0.001f * (float) sampleRate;
        const float d2 = value (Chorus, 2, norm) * 0.001f * (float) sampleRate;
        const float depth = value (Chorus, 3, norm) * 0.003f * (float) sampleRate;
        const float feedback = value (Chorus, 4, norm);
        lp.set (Biquad::LowPass, sampleRate, value (Chorus, 5, norm), 0.707f);
        const auto& sn = sine();
        float* io[] = { L, R };

        for (int i = 0; i < n; ++i)
        {
            phase += rate / sampleRate;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float x = io[ch][i];
                lines[(size_t) ch].push (x + fb[ch] * feedback);
                const float off = ch == 0 ? 0.0f : 0.25f;
                const float a = lines[(size_t) ch].read (std::max (1.0f, d1 + depth * (1.0f + sn (phase + off))));
                const float b = lines[(size_t) ch].read (std::max (1.0f, d2 + depth * (1.0f + sn (phase + off + 0.5))));
                const float wet = lp.process (ch, 0.5f * (a + b));
                fb[ch] = wet;
                io[ch][i] = wet;
            }
        }
    }

private:
    double sampleRate = 48000.0, phase = 0.0;
    std::array<DelayLine, 2> lines;
    Biquad lp;
    float fb[2] {};
};

//------------------------------------------------------------------------------
class DelayModule final : public Module
{
public:
    void prepare (double sr, int) override
    {
        sampleRate = sr;
        for (auto& d : lines) d.prepare ((int) (sr * 6.0));
        tap.prepare ((int) (sr * 6.0));
        reset();
    }

    void reset() override
    {
        for (auto& d : lines) d.reset();
        tap.reset();
        hp.reset();
        lp.reset();
        timeL = timeR = -1.0f;
        fb[0] = fb[1] = 0.0f;
    }

    void process (float* L, float* R, int n, const float* norm, double bpm) override
    {
        const bool sync = value (Delay, 0, norm) > 0.5f;
        const bool link = value (Delay, 5, norm) > 0.5f;
        auto seconds = [&] (int msIndex, int divIndex)
        {
            return sync ? beatsForDivision ((int) value (Delay, divIndex, norm)) * 60.0 / bpm : value (Delay, msIndex, norm) / 1000.0;
        };
        const double maxT = sampleRate * 5.9;
        const float targetL = (float) juce::jlimit (1.0, maxT, seconds (1, 3) * sampleRate);
        const float targetR = link ? targetL : (float) juce::jlimit (1.0, maxT, seconds (2, 4) * sampleRate);
        if (timeL < 0.0f) { timeL = targetL; timeR = targetR; }

        const float feedback = value (Delay, 6, norm);
        const int mode = (int) value (Delay, 7, norm);
        const float centre = value (Delay, 8, norm), width = value (Delay, 9, norm);
        hp.set (Biquad::HighPass, sampleRate, centre / std::exp2 (width * 0.5f), 0.707f);
        lp.set (Biquad::LowPass, sampleRate, centre * std::exp2 (width * 0.5f), 0.707f);
        const float smooth = 1.0f - std::exp (-1.0f / (0.05f * (float) sampleRate));

        for (int i = 0; i < n; ++i)
        {
            timeL += (targetL - timeL) * smooth;
            timeR += (targetR - timeR) * smooth;
            float inL = L[i], inR = R[i];

            if (mode == 2)   // Tap→Delay: the input first passes a single tap of the L time
            {
                tap.push (0.5f * (inL + inR));
                inL = inR = tap.read (timeL);
            }

            const float dl = lines[0].read (mode == 2 ? timeR : timeL);
            const float dr = lines[1].read (timeR);
            const float fl = lp.process (0, hp.process (0, dl));
            const float fr = lp.process (1, hp.process (1, dr));

            if (mode == 1)
            {
                lines[0].push ((inL + inR) * 0.5f + feedback * fr);
                lines[1].push (feedback * fl);
            }
            else
            {
                lines[0].push (inL + feedback * fl);
                lines[1].push (inR + feedback * fr);
            }

            L[i] = mode == 2 ? fl + inL : fl;
            R[i] = mode == 2 ? fr + inR : fr;
        }
    }

private:
    double sampleRate = 48000.0;
    std::array<DelayLine, 2> lines;
    DelayLine tap;
    Biquad hp, lp;
    float timeL = -1.0f, timeR = -1.0f, fb[2] {};
};

//------------------------------------------------------------------------------
class CompressorModule final : public Module
{
public:
    void prepare (double sr, int) override
    {
        sampleRate = sr;
        const juce::dsp::ProcessSpec spec { sr, 1, 2 };
        for (auto& x : xover) x.prepare (spec);
        xover[0].setCutoffFrequency (88.0f);
        xover[1].setCutoffFrequency (2500.0f);
        allpass.prepare (spec);
        allpass.setType (juce::dsp::LinkwitzRileyFilterType::allpass);
        allpass.setCutoffFrequency (2500.0f);
        reset();
    }

    void reset() override
    {
        for (auto& x : xover) x.reset();
        allpass.reset();
        env.fill (-120.0f);
    }

    float compute (int band, float level, float threshold, float ratio, float upward, float atk, float rel) noexcept
    {
        const float db = 20.0f * std::log10 (std::max (level, 1.0e-6f));
        auto& e = env[(size_t) band];
        e = db > e ? db + (e - db) * atk : db + (e - db) * rel;

        float target = 0.0f;
        if (e > threshold)
            target = (threshold - e) * (1.0f - 1.0f / ratio);
        else if (upward > 0.0f && e > -70.0f)
        {
            const float upThreshold = threshold - 12.0f;
            if (e < upThreshold)
                target = std::min (24.0f, (upThreshold - e) * upward * 0.6f);
        }
        return dbToGain (target);
    }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        const float threshold = value (Compressor, 0, norm), ratio = value (Compressor, 1, norm);
        const float atk = std::exp (-1.0f / (value (Compressor, 2, norm) * 0.001f * (float) sampleRate));
        const float rel = std::exp (-1.0f / (value (Compressor, 3, norm) * 0.001f * (float) sampleRate));
        const float makeup = dbToGain (value (Compressor, 4, norm));
        const bool multiband = value (Compressor, 5, norm) > 0.5f;
        const float upward = multiband ? value (Compressor, 6, norm) : 0.0f;

        for (int i = 0; i < n; ++i)
        {
            if (! multiband)
            {
                const float g = compute (0, std::max (std::abs (L[i]), std::abs (R[i])), threshold, ratio, 0.0f, atk, rel) * makeup;
                L[i] *= g;
                R[i] *= g;
                continue;
            }

            float bandsL[3], bandsR[3];
            for (int ch = 0; ch < 2; ++ch)
            {
                float* b = ch == 0 ? bandsL : bandsR;
                float low, rest, mid, high;
                xover[0].processSample (ch, ch == 0 ? L[i] : R[i], low, rest);
                xover[1].processSample (ch, rest, mid, high);
                b[0] = allpass.processSample (ch, low); b[1] = mid; b[2] = high;
            }

            float outL = 0.0f, outR = 0.0f;
            for (int b = 0; b < 3; ++b)
            {
                const float g = compute (b, std::max (std::abs (bandsL[b]), std::abs (bandsR[b])), threshold, ratio, upward, atk, rel);
                outL += bandsL[b] * g;
                outR += bandsR[b] * g;
            }
            L[i] = outL * makeup;
            R[i] = outR * makeup;
        }
    }

private:
    double sampleRate = 48000.0;
    std::array<juce::dsp::LinkwitzRileyFilter<float>, 2> xover;
    juce::dsp::LinkwitzRileyFilter<float> allpass;
    std::array<float, 3> env {};
};

//------------------------------------------------------------------------------
// 8-line feedback delay network with input diffusion. The five types change line
// lengths, diffusion, damping, modulation and colouring (own designs inspired by the
// character the names suggest, not models of Serum's algorithms).
class ReverbModule final : public Module
{
public:
    void prepare (double sr, int) override
    {
        sampleRate = sr;
        for (auto& d : lines) d.prepare ((int) (sr * 0.5));
        for (auto& d : diffusers) d.prepare ((int) (sr * 0.05));
        predelay.prepare ((int) (sr * 0.3));
        reset();
    }

    void reset() override
    {
        for (auto& d : lines) d.reset();
        for (auto& d : diffusers) d.reset();
        predelay.reset();
        damp.fill (0.0f);
        lowCut.reset();
        highCut.reset();
        holdCount = 0.0f;
        held[0] = held[1] = 0.0f;
    }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        const int type = (int) value (Reverb, 0, norm);
        const float size = value (Reverb, 1, norm), decay = value (Reverb, 2, norm);
        const float pre = value (Reverb, 3, norm) * 0.001f * (float) sampleRate;
        const float dampAmt = value (Reverb, 4, norm), width = value (Reverb, 7, norm);
        lowCut.set (Biquad::HighPass, sampleRate, value (Reverb, 5, norm), 0.707f);
        highCut.set (Biquad::LowPass, sampleRate, value (Reverb, 6, norm), 0.707f);

        //                                 Plate  Hall   Vintage Nitrous Basin
        static const float lengthScale[] = { 0.55f, 1.0f,  0.8f,   0.7f,   1.8f };
        static const float diffusion[]   = { 0.75f, 0.6f,  0.5f,   0.7f,   0.8f };
        static const float modDepth[]    = { 2.0f,  6.0f,  18.0f,  25.0f,  8.0f };
        static const float dampBias[]    = { 0.0f,  0.15f, 0.45f,  -0.2f,  0.5f };
        static const float baseMs[8] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f };

        const float sr = (float) sampleRate;
        const float scale = (0.35f + size * 1.65f) * lengthScale[type];
        float lengths[8], gains[8];
        for (int k = 0; k < 8; ++k)
        {
            lengths[k] = std::min (baseMs[k] * scale * 0.001f * sr, sr * 0.48f);
            gains[k] = std::pow (10.0f, -3.0f * (lengths[k] / sr) / std::max (0.1f, decay));
        }

        const float dampCoef = juce::jlimit (0.0f, 0.95f, dampAmt * 0.8f + dampBias[type]);
        const float diff = diffusion[type];
        static const float diffMs[4] = { 4.7f, 3.6f, 12.7f, 9.3f };
        const auto& sn = sine();

        for (int i = 0; i < n; ++i)
        {
            float in = 0.5f * (L[i] + R[i]);
            predelay.push (in);
            in = pre > 1.0f ? predelay.read (pre) : in;

            for (int a = 0; a < 4; ++a)
            {
                const float d = diffMs[a] * (0.5f + size) * 0.001f * sr * (type == 4 ? 2.0f : 1.0f);
                const float delayed = diffusers[(size_t) a].read (std::max (1.0f, d));
                const float v = in + diff * delayed;
                diffusers[(size_t) a].push (v);
                in = delayed - diff * v;
            }

            modPhase += 0.37 / sampleRate;
            float outs[8];
            for (int k = 0; k < 8; ++k)
            {
                const float mod = (k & 1) ? modDepth[type] * sn (modPhase + k * 0.125) : 0.0f;
                outs[k] = lines[(size_t) k].read (std::max (1.0f, lengths[k] + mod));
            }

            // Hadamard 8x8 (fast Walsh-Hadamard), normalised.
            float h[8];
            for (int k = 0; k < 8; ++k) h[k] = outs[k];
            for (int len = 1; len < 8; len <<= 1)
                for (int s = 0; s < 8; s += len * 2)
                    for (int j = s; j < s + len; ++j)
                    {
                        const float a = h[j], b = h[j + len];
                        h[j] = a + b;
                        h[j + len] = a - b;
                    }

            for (int k = 0; k < 8; ++k)
            {
                float v = h[k] * 0.35355339f * gains[k];
                damp[(size_t) k] = v + (damp[(size_t) k] - v) * dampCoef;
                v = damp[(size_t) k];
                if (type == 3)
                    v = fastTanh (v * 1.3f) / 1.3f;   // Nitrous: gritty loop
                lines[(size_t) k].push (v + (k < 4 ? in : -in) * 0.5f);
            }

            float wetL = (outs[0] + outs[2] + outs[4] + outs[6]) * 1.6f;
            float wetR = (outs[1] + outs[3] + outs[5] + outs[7]) * 1.6f;

            if (type == 2)   // Vintage: reduced rate and resolution on the wet signal
            {
                if (holdCount <= 0.0f) { held[0] = wetL; held[1] = wetR; holdCount += std::max (1.0f, sr / 22050.0f); }
                holdCount -= 1.0f;
                wetL = std::round (held[0] * 2048.0f) / 2048.0f;
                wetR = std::round (held[1] * 2048.0f) / 2048.0f;
            }

            wetL = highCut.process (0, lowCut.process (0, wetL));
            wetR = highCut.process (1, lowCut.process (1, wetR));

            const float mid = 0.5f * (wetL + wetR), side = 0.5f * (wetL - wetR) * width;
            L[i] = mid + side;
            R[i] = mid - side;
        }
    }

private:
    double sampleRate = 48000.0, modPhase = 0.0;
    std::array<DelayLine, 8> lines;
    std::array<DelayLine, 4> diffusers;
    DelayLine predelay;
    std::array<float, 8> damp {};
    Biquad lowCut, highCut;
    float holdCount = 0.0f, held[2] {};
};

//------------------------------------------------------------------------------
class EqModule final : public Module
{
public:
    void prepare (double sr, int) override { sampleRate = sr; reset(); }
    void reset() override { b1.reset(); b2.reset(); }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        static const Biquad::Kind k1[] = { Biquad::LowShelf, Biquad::Peak, Biquad::HighPass };
        static const Biquad::Kind k2[] = { Biquad::HighShelf, Biquad::Peak, Biquad::LowPass };
        b1.set (k1[(int) value (EQ, 0, norm)], sampleRate, value (EQ, 1, norm), value (EQ, 3, norm), value (EQ, 2, norm));
        b2.set (k2[(int) value (EQ, 4, norm)], sampleRate, value (EQ, 5, norm), value (EQ, 7, norm), value (EQ, 6, norm));

        for (int i = 0; i < n; ++i)
        {
            L[i] = b2.process (0, b1.process (0, L[i]));
            R[i] = b2.process (1, b1.process (1, R[i]));
        }
    }

private:
    double sampleRate = 48000.0;
    Biquad b1, b2;
};

//------------------------------------------------------------------------------
class FilterModule final : public Module
{
public:
    void prepare (double sr, int) override { filter.prepare (sr); }
    void reset() override { filter.reset(); }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        filter.set ((int) value (Filter, 0, norm), (int) value (Filter, 1, norm), (int) value (Filter, 2, norm),
                    value (Filter, 3, norm), value (Filter, 4, norm), value (Filter, 5, norm), value (Filter, 6, norm), 1.0f);
        for (int i = 0; i < n; ++i)
            filter.process (L[i], R[i]);
    }

private:
    VoiceFilter filter;
};

//------------------------------------------------------------------------------
// Frequency shifter: Hilbert transform (two allpass chains, 90° apart) and
// single-sideband modulation.
class BodeModule final : public Module
{
public:
    void prepare (double sr, int) override { sampleRate = sr; reset(); }
    void reset() override { for (auto& c : chains) c = {}; fb[0] = fb[1] = 0.0f; }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        const float shift = value (Bode, 0, norm), spread = value (Bode, 1, norm), feedback = value (Bode, 2, norm);
        const float shifts[2] = { shift, shift * (1.0f - 2.0f * spread) };
        float* io[] = { L, R };
        const auto& sn = sine();

        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                auto& c = chains[(size_t) ch];
                const float x = io[ch][i] + fb[ch] * feedback;
                const float re = c.delayed;
                const float im = c.pathB.process (x);
                c.delayed = c.pathA.process (x);

                c.phase += shifts[ch] / sampleRate;
                c.phase -= Dsp::floorFast (c.phase);
                const float y = re * sn (c.phase + 0.25) + im * sn (c.phase);
                fb[ch] = y;
                io[ch][i] = y;
            }
        }
    }

private:
    struct AllpassChain
    {
        std::array<float, 4> a2 {};
        std::array<float, 4> x1 {}, x2 {}, y1 {}, y2 {};

        float process (float x) noexcept
        {
            for (size_t s = 0; s < 4; ++s)
            {
                const float y = a2[s] * (x + y2[s]) - x2[s];
                x2[s] = x1[s]; x1[s] = x;
                y2[s] = y1[s]; y1[s] = y;
                x = y;
            }
            return x;
        }
    };

    struct Channel
    {
        AllpassChain pathA, pathB;
        float delayed = 0.0f;
        double phase = 0.0;

        Channel()
        {
            static const float a[] = { 0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f };
            static const float b[] = { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f };
            for (size_t s = 0; s < 4; ++s) { pathA.a2[s] = a[s] * a[s]; pathB.a2[s] = b[s] * b[s]; }
        }
    };

    double sampleRate = 48000.0;
    std::array<Channel, 2> chains;
    float fb[2] {};
};

//------------------------------------------------------------------------------
class UtilityModule final : public Module
{
public:
    void prepare (double sr, int) override
    {
        const juce::dsp::ProcessSpec spec { sr, 1, 2 };
        xover.prepare (spec);
        reset();
    }

    void reset() override { xover.reset(); }

    void process (float* L, float* R, int n, const float* norm, double) override
    {
        const float gain = dbToGain (value (Utility, 0, norm));
        const float pan = value (Utility, 1, norm), width = value (Utility, 2, norm);
        const float monoBelow = value (Utility, 3, norm);
        const float invL = value (Utility, 4, norm) > 0.5f ? -1.0f : 1.0f;
        const float invR = value (Utility, 5, norm) > 0.5f ? -1.0f : 1.0f;
        float gL, gR;
        panGains (pan, gL, gR);
        gL *= gain * invL;
        gR *= gain * invR;
        if (monoBelow > 1.0f)
            xover.setCutoffFrequency (monoBelow);

        for (int i = 0; i < n; ++i)
        {
            float l = L[i], r = R[i];
            if (monoBelow > 1.0f)
            {
                float lowL, highL, lowR, highR;
                xover.processSample (0, l, lowL, highL);
                xover.processSample (1, r, lowR, highR);
                const float mono = 0.5f * (lowL + lowR);
                l = mono + highL;
                r = mono + highR;
            }
            const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * width;
            L[i] = (mid + side) * gL;
            R[i] = (mid - side) * gR;
        }
    }

private:
    juce::dsp::LinkwitzRileyFilter<float> xover;
};
} // namespace

std::unique_ptr<Module> create (int type)
{
    switch (type)
    {
        case HyperDim:   return std::make_unique<HyperDimModule>();
        case Distortion: return std::make_unique<DistortionModule>();
        case Flanger:    return std::make_unique<FlangerModule>();
        case Phaser:     return std::make_unique<PhaserModule>();
        case Chorus:     return std::make_unique<ChorusModule>();
        case Delay:      return std::make_unique<DelayModule>();
        case Compressor: return std::make_unique<CompressorModule>();
        case Reverb:     return std::make_unique<ReverbModule>();
        case EQ:         return std::make_unique<EqModule>();
        case Filter:     return std::make_unique<FilterModule>();
        case Bode:       return std::make_unique<BodeModule>();
        case Utility:    return std::make_unique<UtilityModule>();
        default:         return {};
    }
}
} // namespace Fx

//==============================================================================
FxRack::FxRack() = default;
FxRack::~FxRack() = default;

namespace
{
// Effects run on short blocks whatever the host uses, so that their parameters never step
// audibly. Their tails are given this long of silence before the rack goes to sleep.
constexpr int rackBlock = 128;
constexpr double sleepAfterSeconds = 7.0;      // longer than the longest delay
constexpr float silence = 1.0e-6f;

void ramp (float* x, int n, float from, float to) noexcept
{
    const float step = (to - from) / (float) n;
    for (int i = 0; i < n; ++i)
    {
        from += step;
        x[i] *= from;
    }
}
}

void FxRack::prepare (double sr)
{
    sampleRate = sr;
    for (auto* b : { &dry, &bus1, &bus2, &scratch, &bypass })
        b->setSize (2, rackBlock);

    const juce::dsp::ProcessSpec spec { sr, (juce::uint32) rackBlock, 2 };
    for (auto& slot : slots)
    {
        for (auto& x : slot.xover)
            x.prepare (spec);
        slot.xover[2].setType (juce::dsp::LinkwitzRileyFilterType::allpass);
        for (auto& m : slot.owned)
            m->prepare (sr, rackBlock);
    }
    prepared = true;
}

void FxRack::reset()
{
    for (auto& slot : slots)
    {
        for (auto& x : slot.xover)
            x.reset();
        for (auto& m : slot.owned)
            m->reset();
        slot.running = neverRan;
    }
    silentSamples = 0;
}

void FxRack::ensureModules (const ParamSnapshot& p)
{
    for (int s = 0; s < P::numFxSlots; ++s)
    {
        const int type = juce::jlimit (0, Fx::NumTypes - 1, (int) p[(size_t) P::fxType (s)]);
        auto& slot = slots[(size_t) s];
        if (type == Fx::Empty || Fx::isSplitter (type) || slot.live[(size_t) type].load() != nullptr)
            continue;

        if (auto m = Fx::create (type))
        {
            m->prepare (sampleRate, rackBlock);
            slot.live[(size_t) type].store (m.get());
            slot.owned.push_back (std::move (m));
        }
    }
}

bool FxRack::chainActive (int chain, const ParamSnapshot& p) const
{
    for (int s = chainStart (chain); s < chainStart (chain) + chainSize (chain); ++s)
        if (((int) p[(size_t) P::fxType (s)] != Fx::Empty && p[(size_t) P::fxOn (s)] > 0.5f) || slots[(size_t) s].running > Fx::Empty)
            return true;
    return false;
}

void FxRack::split (Slot& slot, int type, const float* norm, float* L, float* R, int n)
{
    float* byL = bypass.getWritePointer (0);
    float* byR = bypass.getWritePointer (1);

    if (type == Fx::SplitMS)
    {
        const float gm = dbToGain (Fx::value (type, 0, norm)), gs = dbToGain (Fx::value (type, 1, norm));
        const bool pm = Fx::value (type, 2, norm) > 0.5f, ps = Fx::value (type, 3, norm) > 0.5f;
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (L[i] + R[i]) * gm, s = 0.5f * (L[i] - R[i]) * gs;
            const float keepM = pm ? m : 0.0f, keepS = ps ? s : 0.0f;
            byL[i] += (m - keepM) + (s - keepS);
            byR[i] += (m - keepM) - (s - keepS);
            L[i] = keepM + keepS;
            R[i] = keepM - keepS;
        }
        return;
    }

    const bool three = type == Fx::SplitLMH;
    const float x1 = Fx::value (type, 0, norm);
    const float x2 = three ? std::max (x1 * 1.2f, Fx::value (type, 1, norm)) : 0.0f;
    const int levelBase = three ? 2 : 1, procBase = three ? 5 : 3, numBands = three ? 3 : 2;

    float gains[3], process[3];
    for (int b = 0; b < numBands; ++b)
    {
        gains[b] = dbToGain (Fx::value (type, levelBase + b, norm));
        process[b] = Fx::value (type, procBase + b, norm) > 0.5f ? 1.0f : 0.0f;
    }

    slot.xover[0].setCutoffFrequency (x1);
    if (three)
    {
        slot.xover[1].setCutoffFrequency (x2);
        slot.xover[2].setCutoffFrequency (x2);
    }

    float* io[] = { L, R };
    float* by[] = { byL, byR };
    for (int ch = 0; ch < 2; ++ch)
    {
        for (int i = 0; i < n; ++i)
        {
            float b[3];
            float low, rest;
            slot.xover[0].processSample (ch, io[ch][i], low, rest);
            if (three)
            {
                b[0] = slot.xover[2].processSample (ch, low);
                slot.xover[1].processSample (ch, rest, b[1], b[2]);
            }
            else
            {
                b[0] = low;
                b[1] = rest;
            }

            float keep = 0.0f, pass = 0.0f;
            for (int k = 0; k < numBands; ++k)
            {
                const float v = b[k] * gains[k];
                keep += v * process[k];
                pass += v * (1.0f - process[k]);
            }
            io[ch][i] = keep;
            by[ch][i] += pass;
        }
    }
}

void FxRack::processChain (int chain, float* L, float* R, int n, const ParamSnapshot& p, double bpm)
{
    bypass.clear (0, n);
    bool anyBypass = false;

    for (int s = chainStart (chain); s < chainStart (chain) + chainSize (chain); ++s)
    {
        const int type = juce::jlimit (0, Fx::NumTypes - 1, (int) p[(size_t) P::fxType (s)]);
        const bool on = p[(size_t) P::fxOn (s)] > 0.5f;
        auto& slot = slots[(size_t) s];

        if (type == Fx::Empty || (! on && slot.running != type))
        {
            slot.running = Fx::Empty;
            continue;
        }

        const float* norm = &p[(size_t) P::fxParam (s, 0)];

        if (Fx::isSplitter (type))
        {
            slot.running = on ? type : Fx::Empty;
            if (on)
            {
                split (slot, type, norm, L, R, n);
                anyBypass = true;
            }
            continue;
        }

        auto* module = slot.live[(size_t) type].load();
        if (module == nullptr)
            continue;

        // An effect comes in and goes out through its mix, and it does not bring back
        // the echoes it had when it was last in use.
        const float mix = on ? p[(size_t) P::fxMix (s)] : 0.0f;
        if (slot.running == neverRan)
        {
            slot.mix = mix;
        }
        else if (slot.running != type)
        {
            module->reset();
            slot.mix = 0.0f;
        }
        slot.running = on ? type : Fx::Empty;

        const bool blend = mix < 0.999f || slot.mix < 0.999f;
        if (blend)
        {
            scratch.copyFrom (0, 0, L, n);
            scratch.copyFrom (1, 0, R, n);
        }

        module->process (L, R, n, norm, bpm);

        if (blend)
        {
            float* dl = scratch.getWritePointer (0);
            float* dr = scratch.getWritePointer (1);
            module->alignDry (dl, dr, n);
            const float step = (mix - slot.mix) / (float) n;
            float m = slot.mix;
            for (int i = 0; i < n; ++i)
            {
                m += step;
                L[i] = dl[i] + (L[i] - dl[i]) * m;
                R[i] = dr[i] + (R[i] - dr[i]) * m;
            }
        }
        slot.mix = mix;
    }

    if (anyBypass)
    {
        juce::FloatVectorOperations::add (L, bypass.getReadPointer (0), n);
        juce::FloatVectorOperations::add (R, bypass.getReadPointer (1), n);
    }
}

void FxRack::process (juce::AudioBuffer<float>& buffer, const ParamSnapshot& p, double bpm)
{
    if (! prepared || buffer.getNumChannels() < 2)
        return;

    const bool mainOn = chainActive (0, p);
    const bool busOn[2] = { p[P::FxBus1Send] > 0.0f && chainActive (1, p), p[P::FxBus2Send] > 0.0f && chainActive (2, p) };
    const int total = buffer.getNumSamples();

    if (! mainOn && ! busOn[0] && ! busOn[1] && std::abs (p[P::FxMainLevel] - 1.0f) < 1.0e-6f && std::abs (mainLevel - 1.0f) < 1.0e-6f)
        return;

    // Nothing coming in and the tails gone: the rack sleeps until the next sound.
    if (buffer.getMagnitude (0, total) >= silence)
        silentSamples = 0;
    else if (silentSamples >= (int) (sleepAfterSeconds * sampleRate))
        return;

    for (int start = 0; start < total; start += rackBlock)
    {
        const int n = std::min (rackBlock, total - start);
        processBlock (buffer.getWritePointer (0, start), buffer.getWritePointer (1, start), n, p, bpm, busOn, mainOn);
    }

    if (buffer.getMagnitude (0, total) < silence)
        silentSamples += total;
    else
        silentSamples = 0;
}

void FxRack::processBlock (float* L, float* R, int n, const ParamSnapshot& p, double bpm, const bool* busOn, bool mainOn)
{
    dry.copyFrom (0, 0, L, n);
    dry.copyFrom (1, 0, R, n);

    juce::AudioBuffer<float>* busBuffers[2] = { &bus1, &bus2 };
    const float sends[2] = { p[P::FxBus1Send], p[P::FxBus2Send] };
    const float levels[2] = { p[P::FxBus1Level], p[P::FxBus2Level] };

    for (int b = 0; b < 2; ++b)
    {
        if (! busOn[b])
        {
            sendLevel[b] = 0.0f;
            busLevel[b] = levels[b];
            continue;
        }

        auto& bb = *busBuffers[b];
        for (int ch = 0; ch < 2; ++ch)
        {
            bb.copyFrom (ch, 0, dry, ch, 0, n);
            ramp (bb.getWritePointer (ch), n, sendLevel[b], sends[b]);
        }
        processChain (b + 1, bb.getWritePointer (0), bb.getWritePointer (1), n, p, bpm);
        ramp (bb.getWritePointer (0), n, busLevel[b], levels[b]);
        ramp (bb.getWritePointer (1), n, busLevel[b], levels[b]);
        sendLevel[b] = sends[b];
        busLevel[b] = levels[b];
    }

    // Main input: dry signal plus the buses routed into it.
    ramp (L, n, mainLevel, p[P::FxMainLevel]);
    ramp (R, n, mainLevel, p[P::FxMainLevel]);
    mainLevel = p[P::FxMainLevel];

    const bool toMain[2] = { p[P::FxBus1ToMain] > 0.5f, p[P::FxBus2ToMain] > 0.5f };
    auto addBus = [&] (int b)
    {
        juce::FloatVectorOperations::add (L, busBuffers[b]->getReadPointer (0), n);
        juce::FloatVectorOperations::add (R, busBuffers[b]->getReadPointer (1), n);
    };

    for (int b = 0; b < 2; ++b)
        if (busOn[b] && toMain[b])
            addBus (b);

    if (mainOn)
        processChain (0, L, R, n, p, bpm);

    for (int b = 0; b < 2; ++b)
        if (busOn[b] && ! toMain[b])
            addBus (b);
}
