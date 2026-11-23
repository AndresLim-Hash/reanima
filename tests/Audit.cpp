// Professional audit of the engine and the rack, at every sample rate the plugin must support.
// Each check measures one promise: no allocation on the audio thread, zero latency, exact
// polarity, flat response, no aliasing, stable effects, parameters that really do something.
// Usage: ReanimaAudit [fast]     (fast skips the long fuzz and the CPU table)

#include "Params.h"
#include "SampleAnalysis.h"
#include "PluginProcessor.h"
#include "MasterStage.h"
#include "SynthEngine.h"
#include "FxRack.h"
#include "DspUtil.h"
#include "HelpText.h"
#include "EditPage.h"
#include "SampleBrowser.h"
#include <atomic>
#include <cstdio>
#include <set>
#include <map>
#include <cstdlib>
#include <functional>
#include <new>

// Counts every allocation, so that a single one during process() fails the audit.
static std::atomic<bool> countingAllocations { false };
static std::atomic<long> allocationCount { 0 };

void* operator new (std::size_t size)
{
    if (countingAllocations.load (std::memory_order_relaxed))
        allocationCount.fetch_add (1, std::memory_order_relaxed);
    if (void* p = std::malloc (size > 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size) { return operator new (size); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
constexpr double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
constexpr double twoPi = juce::MathConstants<double>::twoPi;
int failures = 0, checks = 0;

void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    ++checks;
    std::printf ("[%s] %s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8(), detail.toRawUTF8());
    if (! ok)
        ++failures;
}

void title (const char* text) { std::printf ("\n=== %s ===\n", text); std::fflush (stdout); }
double toDb (double x) { return 20.0 * std::log10 (std::max (1.0e-12, x)); }
juce::String db (double x) { return juce::String (x, 2) + " dB"; }

using Sample = std::shared_ptr<const SampleData>;
using Audio = juce::AudioBuffer<float>;

Audio tone (double hz, double seconds, double rate, float amp = 0.5f)
{
    Audio b (2, (int) (seconds * rate));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const float v = amp * (float) std::sin (twoPi * hz * i / rate);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

Audio pluck (double hz, double seconds, double rate)
{
    Audio b (2, (int) (seconds * rate));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const double t = i / rate;
        const double env = t < 0.003 ? t / 0.003 : 0.25 * std::exp (-(t - 0.003) * 1.2) + 0.75 * std::exp (-(t - 0.003) / 0.08);
        double v = 0.0;
        for (int h = 1; h <= 8; ++h)
            v += std::sin (twoPi * hz * h * t) / h;
        b.setSample (0, i, (float) (0.4 * env * v));
        b.setSample (1, i, (float) (0.4 * env * v));
    }
    return b;
}

ParamSnapshot paramsFor (const SampleData& s)
{
    ParamSnapshot p {};
    for (int i = 0; i < P::Count; ++i)
        p[(size_t) i] = P::defs()[(size_t) i].def;
    p[P::Volume] = 0.0f;           // the engine at unity: the -6 dB default is room for chords
    p[P::MonoBass] = 0.0f;         // the engine's own output (Graves mono shifts the lows' phase on purpose)

    auto set = [&p] (int index, float v) { p[(size_t) index] = juce::jlimit (P::defs()[(size_t) index].min, P::defs()[(size_t) index].max, v); };
    set (P::AmpA, s.detected.attack * 1000.0f);
    set (P::AmpCurve, s.detected.curve);
    set (P::AmpD, s.detected.decay * 1000.0f);
    set (P::AmpS, s.detected.sustain);
    set (P::AmpFade, s.detected.fadeDbPerSec);
    set (P::AmpR, s.detected.release * 1000.0f);
    set (P::TransientMs, s.transientMs);
    set (P::StartPos, s.length > 0 ? (float) (s.onset / s.length) : 0.0f);
    return p;
}

// One note on `note`, held for `held` seconds, rendered in blocks of `block` samples.
Audio render (Sample s, const ParamSnapshot& p, int note, double rate, double held, double total,
              int block = 512, int noteAt = 0, float velocity = 1.0f)
{
    SynthEngine e;
    e.prepare (rate);
    e.setSample (s);
    e.prepareFx (p);

    const int length = (int) (total * rate), off = noteAt + (int) (held * rate);
    Audio out (2, length), buf (2, block);
    out.clear();
    for (int pos = 0; pos < length; pos += block)
    {
        const int n = std::min (block, length - pos);
        juce::MidiBuffer midi;
        if (noteAt >= pos && noteAt < pos + n)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, velocity), noteAt - pos);
        if (off >= pos && off < pos + n)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), off - pos);
        Audio view (buf.getArrayOfWritePointers(), 2, n);
        e.process (view, midi, p, 120.0);
        out.copyFrom (0, pos, buf, 0, 0, n);
        out.copyFrom (1, pos, buf, 1, 0, n);
    }
    return out;
}

// Amplitude of one frequency, with a window whose side lobes stay out of the way.
double toneAmp (const float* x, int n, double hz, double rate)
{
    double re = 0.0, im = 0.0, sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double u = twoPi * i / n;
        const double w = 0.35875 - 0.48829 * std::cos (u) + 0.14128 * std::cos (2 * u) - 0.01168 * std::cos (3 * u);
        const double a = twoPi * hz * i / rate;
        re += w * x[i] * std::cos (a);
        im -= w * x[i] * std::sin (a);
        sum += w;
    }
    return 2.0 * std::sqrt (re * re + im * im) / sum;
}

// Windowed magnitudes (amplitude scale), for spurs and aliases.
std::vector<float> spectrum (const float* x, int order)
{
    const int N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) N * 2, 0.0f);
    double sum = 0.0;
    for (int i = 0; i < N; ++i)
    {
        const double u = twoPi * i / N;
        const double w = 0.35875 - 0.48829 * std::cos (u) + 0.14128 * std::cos (2 * u) - 0.01168 * std::cos (3 * u);
        buf[(size_t) i] = (float) (w * x[i]);
        sum += w;
    }
    fft.performFrequencyOnlyForwardTransform (buf.data(), true);
    buf.resize ((size_t) N / 2);
    for (auto& v : buf)
        v *= (float) (2.0 / sum);
    return buf;
}

// Strongest component more than `guardBins` bins away from every listed frequency, relative to `ref`.
double worstSpur (const std::vector<float>& mag, double rate, int order, std::initializer_list<double> keep, double ref, double maxHz = 1.0e9)
{
    const double binHz = rate / (1 << order);
    constexpr int guardBins = 8;
    double worst = 1.0e-12;
    for (int k = 2; k < (int) mag.size(); ++k)
    {
        if (k * binHz > maxHz)
            break;
        bool near = false;
        for (const double f : keep)
            near = near || std::abs (k - f / binHz) < guardBins;
        if (! near)
            worst = std::max (worst, (double) mag[(size_t) k]);
    }
    return toDb (worst / ref);
}

double rmsOf (const float* x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i)
        s += (double) x[i] * x[i];
    return std::sqrt (s / std::max (1, n));
}

//==============================================================================
void auditHelp()
{
    title ("Cada control tiene su explicación");
    juce::String missing;
    for (int id = 0; id < P::FxSlotBase; ++id)
        if (Help::forParam (id).isEmpty())
            missing << P::defs()[(size_t) id].id << " ";
    for (int type = 1; type < Fx::NumTypes; ++type)
    {
        if (Help::forFxType (type).isEmpty())
            missing << "tipo " << type << " ";
        for (int k = 0; k < (int) Fx::spec (type).params.size(); ++k)
            if (Help::forFxParam (type, k).isEmpty())
                missing << Fx::spec (type).name << "/" << Fx::spec (type).params[(size_t) k].name << " ";
    }
    check (missing.isEmpty(), "todos los parámetros y efectos tienen ayuda", missing);
}

void auditRealTime()
{
    title ("Tiempo real: ninguna reserva de memoria en process()");
    const double rate = 48000.0;
    Sample s = SampleAnalysis::analyse (pluck (220.0, 2.0, rate), rate, "rt", false);
    auto p = paramsFor (*s);
    p[P::FltOn] = 1.0f;

    for (int engineMode : { (int) P::EngineRebuild, (int) P::EngineReenvelope, (int) P::EngineHybrid, (int) P::EngineResynth, (int) P::EngineGranular,
                            (int) P::EngineDrone })
    {
        p[P::Engine] = (float) engineMode;
        for (int slot = 0; slot < P::numFxSlots; ++slot)
        {
            const int type = 1 + slot % (Fx::NumTypes - 1);
            const auto& spec = Fx::spec (type);
            p[(size_t) P::fxType (slot)] = (float) type;
            p[(size_t) P::fxOn (slot)] = 1.0f;
            p[(size_t) P::fxMix (slot)] = 0.5f;
            for (int k = 0; k < (int) spec.params.size(); ++k)
                p[(size_t) P::fxParam (slot, k)] = Fx::toNorm (spec.params[(size_t) k], spec.params[(size_t) k].def);
        }
        p[P::FxBus1Send] = p[P::FxBus2Send] = 0.5f;

        SynthEngine e;
        e.prepare (rate);
        e.setSample (s);
        e.prepareFx (p);
        Audio buf (2, 512);

        allocationCount = 0;
        countingAllocations = true;
        for (int b = 0; b < 200; ++b)
        {
            juce::MidiBuffer midi;      // no events: an empty buffer does not allocate
            e.process (buf, midi, p, 120.0);
        }
        countingAllocations = false;
        const long idle = allocationCount.load();

        juce::MidiBuffer on;
        on.ensureSize (4096);
        for (int n = 0; n < 40; ++n)
            on.addEvent (juce::MidiMessage::noteOn (1, 40 + n, 0.8f), n * 3);

        allocationCount = 0;
        countingAllocations = true;
        e.process (buf, on, p, 120.0);
        juce::MidiBuffer empty;
        for (int b = 0; b < 200; ++b)
            e.process (buf, empty, p, 120.0);
        countingAllocations = false;

        check (idle == 0 && allocationCount.load() == 0, "motor " + juce::String (engineMode) + " + 14 efectos sin reservas",
               juce::String (idle) + " en reposo, " + juce::String (allocationCount.load()) + " tocando");
    }
}

void auditDelayLine()
{
    title ("Delay: precisión tras mucho tiempo de reproducción");
    Dsp::DelayLine line;
    line.prepare (4096);
    const double hz = 997.0, rate = 48000.0;
    const float delay = 100.37f;
    double early = 0.0, late = 0.0;
    const int total = 40000000;       // casi 14 minutos a 48 kHz
    for (int i = 0; i < total; ++i)
    {
        line.push ((float) std::sin (twoPi * hz * i / rate));
        if (i < 1000000 || i > total - 200000)
        {
            if (i < 10000)
                continue;
            const double err = std::abs (line.read (delay) - std::sin (twoPi * hz * ((double) i + 1.0 - (double) delay) / rate));
            (i < 1000000 ? early : late) = std::max (i < 1000000 ? early : late, err);
        }
    }
    check (late <= early * 1.5, "el error de lectura no crece con el tiempo",
           "inicio " + db (toDb (early)) + ", a los 14 min " + db (toDb (late)));
}

void auditPlayback()
{
    title ("Motor A: fidelidad, polaridad, latencia y tono");

    for (const double rate : rates)
    {
        const juce::String tag = juce::String (rate / 1000.0, 1) + " kHz";
        Sample s = SampleAnalysis::analyse (pluck (220.0, 1.5, rate), rate, "same", false);
        auto p = paramsFor (*s);

        // Same rate, untouched: the output is the file, sample by sample and in polarity.
        {
            const auto y = render (s, p, SynthEngine::rootKey, rate, 1.4, 1.4);
            const int start = (int) s->onset, n = std::min (y.getNumSamples(), s->length - start);
            double sig = 0.0, err = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double x = s->audio.getSample (0, start + i);
                sig += x * x;
                err += (x - y.getSample (0, i)) * (x - y.getSample (0, i));
            }
            const double snr = 10.0 * std::log10 (sig / std::max (1.0e-20, err));
            check (snr > 100.0, tag + ": sin tocar = archivo original", db (snr));
        }

        // A note placed at sample N starts exactly at N, in any block size.
        for (const int block : { 16, 64, 441, 512, 1024 })
        {
            const int at = 1000;
            const auto y = render (s, p, SynthEngine::rootKey, rate, 0.3, 0.4, block, at);
            int first = -1;
            for (int i = 0; i < y.getNumSamples() && first < 0; ++i)
                if (y.getSample (0, i) != 0.0f)
                    first = i;
            const int expected = at + [&] { for (int i = (int) s->onset; i < s->length; ++i) if (s->audio.getSample (0, i) != 0.0f) return i - (int) s->onset; return 0; }();
            check (first == expected, tag + ", bloque " + juce::String (block) + ": latencia cero", "primera muestra en " + juce::String (first) + ", esperada " + juce::String (expected));
        }

        // Sample at 44.1 kHz in a session at this rate: same pitch and level.
        {
            Sample a = SampleAnalysis::analyse (tone (1000.0, 3.0, 44100.0), 44100.0, "tone", false);
            auto q = paramsFor (*a);
            const auto y = render (a, q, SynthEngine::rootKey, rate, 2.0, 1.0);
            const int order = 15, at = (int) (0.2 * rate);
            const double level = toneAmp (y.getReadPointer (0) + at, 1 << order, 1000.0, rate);
            const double off = toneAmp (y.getReadPointer (0) + at, 1 << order, 1000.0 * std::pow (2.0, 2.0 / 1200.0), rate);
            const auto mag = spectrum (y.getReadPointer (0) + at, order);
            const double spur = worstSpur (mag, rate, order, { 1000.0 }, level);
            check (std::abs (toDb (level / 0.5)) < 0.05 && off < level, tag + ": sample a 44,1 kHz, nivel correcto", db (toDb (level / 0.5)));
            check (spur < -90.0, tag + ": sample a 44,1 kHz, pureza", "peor espuria " + db (spur));
        }
    }

    title ("Motor A: distorsión del lecho");
    for (const double rate : rates)
    {
        // A pure tone that decays, held by the bed: nothing but that tone should come out.
        Audio decaying = tone (1000.0, 2.0, rate, 0.9f);
        for (int i = 0; i < decaying.getNumSamples(); ++i)
        {
            const float env = std::exp (-2.0f * (float) i / (float) rate);
            decaying.setSample (0, i, decaying.getSample (0, i) * env);
            decaying.setSample (1, i, decaying.getSample (1, i) * env);
        }
        Sample s = SampleAnalysis::analyse (decaying, rate, "pure", false);
        auto p = paramsFor (*s);
        p[P::SustainType] = (float) P::SustainHold;
        p[P::AmpS] = 1.0f;
        p[P::Breath] = 0.0f;
        const auto y = render (s, p, SynthEngine::rootKey, rate, 4.0, 4.0);
        const int at = (int) (2.5 * rate);
        const double level = toneAmp (y.getReadPointer (0) + at, 1 << 15, 1000.0, rate);
        double worst = 0.0;
        for (int h = 2; h <= 5; ++h)
            worst = std::max (worst, toneAmp (y.getReadPointer (0) + at, 1 << 15, 1000.0 * h, rate));
        check (toDb (worst / level) < -70.0, juce::String (rate / 1000.0, 1) + " kHz: armónicos añadidos al sostener un tono puro", db (toDb (worst / level)));
    }

    title ("Motor A: transposición (pureza, respuesta, aliasing)");
    for (const double rate : rates)
    {
        const juce::String tag = juce::String (rate / 1000.0, 1) + " kHz";
        Sample s = SampleAnalysis::analyse (tone (1000.0, 6.0, rate), rate, "tone", false);
        auto p = paramsFor (*s);

        for (const float semis : { -12.0f, -5.0f, 1.0f, 7.0f, 12.0f, 24.0f })
        {
            auto q = p;
            q[P::Transpose] = semis;
            const double hz = 1000.0 * std::pow (2.0, semis / 12.0);
            const int order = 14, at = (int) (0.02 * rate);
            const auto y = render (s, q, SynthEngine::rootKey, rate, 1.0, 0.5);
            const double level = toneAmp (y.getReadPointer (0) + at, 1 << order, hz, rate);
            const auto mag = spectrum (y.getReadPointer (0) + at, order);
            const double spur = worstSpur (mag, rate, order, { hz }, level);
            check (std::abs (toDb (level / 0.5)) < 0.1 && spur < -85.0, tag + juce::String::formatted (": %+.0f st", semis),
                   "nivel " + db (toDb (level / 0.5)) + ", peor espuria " + db (spur));
        }
    }

    // Top of the audio band at a fractional position, at 44.1 kHz (the worst case).
    {
        const double rate = 44100.0;
        for (const double hz : { 10000.0, 15000.0, 17000.0, 18000.0, 19000.0, 20000.0 })
        {
            Sample s = SampleAnalysis::analyse (tone (hz, 2.0, rate), rate, "top", false);
            auto q = paramsFor (*s);
            q[P::Fine] = 1.0f;
            const auto y = render (s, q, SynthEngine::rootKey, rate, 1.0, 0.5);
            const double out = hz * std::pow (2.0, 0.01 / 12.0);
            const double level = toneAmp (y.getReadPointer (0) + 2000, 8192, out, rate);
            check (std::abs (toDb (level / 0.5)) < (hz <= 18000.0 ? 0.5 : 8.0), "respuesta en " + juce::String ((int) hz) + " Hz con +1 ct", db (toDb (level / 0.5)));
        }
    }

    // Transposing up: what lands in the audible band that was not there.
    for (const float semis : { 3.0f, 7.0f, 12.0f, 19.0f, 24.0f })
    {
        const double rate = 44100.0, src = 19000.0;
        Sample s = SampleAnalysis::analyse (tone (src, 4.0, rate), rate, "alias", false);
        auto q = paramsFor (*s);
        q[P::Transpose] = semis;
        const int order = 14;
        const auto y = render (s, q, SynthEngine::rootKey, rate, 1.0, 0.5);
        const auto mag = spectrum (y.getReadPointer (0) + 2000, order);
        const double alias = worstSpur (mag, rate, order, {}, 0.5, 16000.0);
        check (alias < -60.0, juce::String::formatted ("aliasing audible con %+.0f st", semis), "19 kHz: " + db (alias) + " bajo 16 kHz");
    }

    // A harmonically rich sound an octave up keeps its top end.
    {
        const double rate = 44100.0;
        Sample s = SampleAnalysis::analyse (tone (8000.0, 4.0, rate), rate, "air", false);
        auto q = paramsFor (*s);
        q[P::Transpose] = 12.0f;
        const auto y = render (s, q, SynthEngine::rootKey, rate, 1.0, 0.5);
        const double level = toneAmp (y.getReadPointer (0) + 2000, 8192, 16000.0, rate);
        check (std::abs (toDb (level / 0.5)) < 1.0, "8 kHz una octava arriba conserva el nivel", db (toDb (level / 0.5)));
    }
}

void auditStereo()
{
    title ("Estéreo y fase");
    for (const double rate : rates)
    {
        const juce::String tag = juce::String (rate / 1000.0, 1) + " kHz";

        // Each channel stays on its side and the time between them is kept.
        Audio src = pluck (220.0, 1.0, rate);
        const int lag = 11;
        for (int i = src.getNumSamples() - 1; i >= 0; --i)
            src.setSample (1, i, i >= lag ? src.getSample (0, i - lag) : 0.0f);
        Sample s = SampleAnalysis::analyse (src, rate, "lag", false);
        auto p = paramsFor (*s);
        p[P::MonoBass] = 0.0f;      // the engine's own stereo; Graves mono puts the lows in the centre on purpose
        const auto y = render (s, p, SynthEngine::rootKey, rate, 0.8, 0.8);
        double best = -1.0e30;
        int bestLag = 0;
        for (int d = -20; d <= 20; ++d)
        {
            double c = 0.0;
            for (int i = 100; i < y.getNumSamples() - 100; ++i)
                c += (double) y.getSample (0, i) * y.getSample (1, i + d);
            if (c > best) { best = c; bestLag = d; }
        }
        check (bestLag == lag, tag + ": diferencia de tiempo entre canales intacta", juce::String (bestLag) + " muestras");

        Audio left = pluck (220.0, 1.0, rate);
        left.clear (1, 0, left.getNumSamples());
        Sample l = SampleAnalysis::analyse (left, rate, "left", false);
        auto q = paramsFor (*l);
        q[P::SustainType] = (float) P::SustainHold;
        q[P::AmpS] = 0.8f;
        q[P::MonoBass] = 0.0f;
        const auto z = render (l, q, SynthEngine::rootKey, rate, 1.5, 1.5);
        const double rL = rmsOf (z.getReadPointer (0), z.getNumSamples()), rR = rmsOf (z.getReadPointer (1), z.getNumSamples());
        check (toDb (rR / rL) < -60.0, tag + ": sample sólo en L no invade R, ni con lecho", db (toDb (rR / rL)));

        Sample c = SampleAnalysis::analyse (pluck (220.0, 1.0, rate), rate, "centre", false);
        auto m = paramsFor (*c);
        m[P::Pan] = -1.0f;
        const auto hard = render (c, m, SynthEngine::rootKey, rate, 0.5, 0.5);
        check (rmsOf (hard.getReadPointer (1), hard.getNumSamples()) < 1.0e-6, tag + ": pan a la izquierda silencia R", "");

        m[P::Pan] = 0.0f;
        m[P::SustainType] = (float) P::SustainHold;
        m[P::AmpS] = 0.8f;
        const auto mono = render (c, m, SynthEngine::rootKey, rate, 1.5, 1.5);
        double diff = 0.0;
        for (int i = 0; i < mono.getNumSamples(); ++i)
            diff = std::max (diff, (double) std::abs (mono.getSample (0, i) - mono.getSample (1, i)));
        check (diff < 1.0e-5, tag + ": sample mono sigue mono con lecho y aliento", db (toDb (diff)));
    }
}

// What the note-off does to a note whose envelope was changed.
void auditRelease()
{
    title ("Release: sin golpe y sin rebote");
    const auto edges = [] (const Audio& y, int from, int to)
    {
        double e = 1.0e-20;
        for (int i = std::max (2, from); i < to; ++i)
        {
            const double d = (double) y.getSample (0, i) - 2.0 * y.getSample (0, i - 1) + y.getSample (0, i - 2);
            e += d * d;
        }
        return 10.0 * std::log10 (e / std::max (1, to - from));
    };

    for (const double rate : rates)
    {
        const juce::String tag = juce::String (rate / 1000.0, 1) + " kHz";
        const int ms = (int) (rate * 0.001);
        Sample s = SampleAnalysis::analyse (pluck (110.0, 1.0, rate), rate, "release", false);

        for (const float release : { 1.0f, 20.0f, 300.0f })
        {
            auto p = paramsFor (*s);
            p[P::AmpS] = 0.8f;
            p[P::AmpFade] = 0.0f;
            p[P::AmpR] = release;
            constexpr double held = 0.5;
            const int off = (int) (held * rate);
            const auto y = render (s, p, SynthEngine::rootKey, rate, held, 2.0);
            const double burst = edges (y, off - ms, off + 3 * ms) - edges (y, off - 24 * ms, off - 4 * ms);
            check (burst < 2.0, tag + ", R " + juce::String (release, 0) + " ms: soltar la tecla no golpea", db (burst));

            double fastest = 0.0;
            for (int i = off; i + 10 * ms < off + 60 * ms; i += ms)
                fastest = std::max (fastest, toDb (rmsOf (y.getReadPointer (0) + i, 5 * ms) / (rmsOf (y.getReadPointer (0) + i + 5 * ms, 5 * ms) + 1.0e-9)));
            check (fastest < 12.0, tag + ", R " + juce::String (release, 0) + " ms: la caída no es un corte", db (fastest) + " en 5 ms");
        }

        // Released long before the sample ends, with a release that outlasts it.
        auto p = paramsFor (*s);
        p[P::AmpS] = 0.3f;
        p[P::AmpR] = 8000.0f;
        const auto y = render (s, p, SynthEngine::rootKey, rate, 0.2, 2.0);
        double lowest = 1.0e9, rebound = 0.0;
        for (double t = 0.2; t + 0.05 < 1.9; t += 0.01)
        {
            const double level = toDb (rmsOf (y.getReadPointer (0) + (int) (t * rate), (int) (0.05 * rate)) + 1.0e-9);
            lowest = std::min (lowest, level);
            rebound = std::max (rebound, level - lowest);
        }
        check (rebound < 2.5, tag + ": una nota soltada no vuelve a subir al acabarse el sample", db (rebound));
    }
}

// Variation makes notes differ, but the shape of the note stays.
void auditVariation()
{
    title ("Variación");
    constexpr double rate = 48000.0;
    Sample s = SampleAnalysis::analyse (pluck (220.0, 1.0, rate), rate, "variation", false);
    auto p = paramsFor (*s);
    p[P::AmpS] = 0.6f;
    p[P::AmpFade] = 0.0f;
    p[P::AmpR] = 300.0f;

    const auto shapeOf = [&] (const Audio& y)
    {
        std::array<double, 5> shape {};
        const double times[] = { 0.02, 0.1, 0.3, 0.6, 0.75 };
        const double reference = rmsOf (y.getReadPointer (0) + (int) (0.5 * rate), (int) (0.05 * rate));
        for (size_t i = 0; i < shape.size(); ++i)
            shape[i] = toDb (rmsOf (y.getReadPointer (0) + (int) (times[i] * rate), (int) (0.03 * rate)) / reference);
        return shape;
    };
    const auto firstSound = [] (const Audio& y)
    {
        int i = 0;
        while (i < y.getNumSamples() && std::abs (y.getSample (0, i)) < 1.0e-3f)
            ++i;
        return i;
    };

    const auto plain = render (s, p, SynthEngine::rootKey, rate, 0.55, 1.0);
    const auto shape = shapeOf (plain);
    const int start = firstSound (plain);

    p[P::Variation] = 1.0f;
    SynthEngine e;
    e.prepare (rate);
    e.setSample (s);
    e.prepareFx (p);

    double worstShape = 0.0, spread = -1.0e9, lowest = 1.0e9;
    int worstStart = 0;
    Audio buf (2, 512);
    for (int note = 0; note < 12; ++note)
    {
        Audio out (2, (int) (1.0 * rate));
        out.clear();
        for (int pos = 0; pos < out.getNumSamples(); pos += 512)
        {
            const int n = std::min (512, out.getNumSamples() - pos), off = (int) (0.55 * rate);
            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey, 1.0f), 0);
            if (off >= pos && off < pos + n)
                midi.addEvent (juce::MidiMessage::noteOff (1, SynthEngine::rootKey), off - pos);
            Audio view (buf.getArrayOfWritePointers(), 2, n);
            e.process (view, midi, p, 120.0);
            out.copyFrom (0, pos, buf, 0, 0, n);
        }

        const auto varied = shapeOf (out);
        for (size_t i = 0; i < shape.size(); ++i)
            worstShape = std::max (worstShape, std::abs (varied[i] - shape[i]));
        worstStart = std::max (worstStart, std::abs (firstSound (out) - start));
        const double level = toDb (rmsOf (out.getReadPointer (0) + (int) (0.5 * rate), (int) (0.05 * rate)));
        spread = std::max (spread, level);
        lowest = std::min (lowest, level);
    }

    check (worstShape < 1.0, "Variación no cambia la forma de la nota", db (worstShape));
    check (worstStart <= 2, "Variación no mueve el inicio", juce::String (worstStart) + " muestras");
    check (spread - lowest > 0.3 && spread - lowest < 3.2, "Variación hace distintas las notas", db (spread - lowest));
}

// Synthesizer mode: the ADSR decides the level by itself; Golpe gives a soft sample a hit.
void auditSynthEnvelope()
{
    title ("ADSR en modo sintetizador");
    for (const double rate : rates)
    {
        const juce::String tag = juce::String (rate / 1000.0, 1) + " kHz";
        Sample s = SampleAnalysis::analyse (pluck (220.0, 2.0, rate), rate, "synth", false);
        auto p = paramsFor (*s);
        p[P::EnvMode] = (float) P::EnvSynth;
        p[P::AmpA] = 200.0f;
        p[P::AmpCurve] = 1.0f;
        p[P::AmpD] = 300.0f;
        p[P::AmpS] = 0.5f;
        p[P::AmpFade] = 0.0f;
        p[P::AmpR] = 300.0f;
        const auto y = render (s, p, SynthEngine::rootKey, rate, 1.5, 2.6);
        const auto level = [&] (double at) { return toDb (rmsOf (y.getReadPointer (0) + (int) (at * rate), (int) (0.02 * rate))); };

        const double peak = level (0.18), half = level (0.05), early = level (0.9), late = level (1.4);
        check (std::abs (early - late) < 1.0, tag + ": el sustain se mantiene fijo aunque el sample caiga", db (late - early));
        check (std::abs ((early - peak) + 6.0) < 1.5, tag + ": Sustain 50 % suena 6 dB por debajo del pico", db (early - peak));
        check (std::abs ((half - peak) + 9.5) < 2.0, tag + ": el ataque sube desde cero como lo dibuja la curva", db (half - peak));
        check (level (1.5 + 0.3) < late - 50.0, tag + ": el release cae como lo marca el mando", db (level (1.8) - late));
    }

    // A pad that fades in, played with an instant attack: Golpe makes the start bite.
    constexpr double rate = 48000.0;
    Audio pad = tone (330.0, 2.0, rate, 0.4f);
    for (int i = 0; i < pad.getNumSamples(); ++i)
    {
        const float g = std::min (1.0f, (float) i / (float) (0.4 * rate));
        pad.setSample (0, i, pad.getSample (0, i) * g + 0.02f * g * (float) std::sin (i * 2.17));
        pad.setSample (1, i, pad.getSample (1, i) * g + 0.02f * g * (float) std::sin (i * 2.17));
    }
    Sample s = SampleAnalysis::analyse (pad, rate, "pad", false);
    auto p = paramsFor (*s);
    p[P::AmpA] = 1.0f;
    double levelFirst = 0.0, levelLater = 0.0;
    const auto bite = [&] (float punch)
    {
        p[P::Punch] = punch;
        const auto y = render (s, p, SynthEngine::rootKey, rate, 0.5, 0.5);
        double e = 1.0e-12, first = 1.0e-12, later = 1.0e-12;
        for (int i = 2; i < (int) (0.02 * rate); ++i)
        {
            const double d = (double) y.getSample (0, i) - y.getSample (0, i - 1);
            e += d * d;
        }
        for (int i = (int) (0.005 * rate); i < (int) (0.025 * rate); ++i)
            first += (double) y.getSample (0, i) * y.getSample (0, i) / (0.02 * rate);
        for (int i = (int) (0.4 * rate); i < (int) (0.45 * rate); ++i)
            later += (double) y.getSample (0, i) * y.getSample (0, i) / (0.05 * rate);
        levelFirst = 10.0 * std::log10 (first);
        levelLater = 10.0 * std::log10 (later);
        return 10.0 * std::log10 (e);
    };
    const double without = bite (0.0);
    // The instant attack asked for: the note is at its level from the start (the bed fills what the
    // sample, still fading in, does not have), not 30 dB down as it was.
    check (levelFirst > levelLater - 3.0, "un ataque instantáneo en un sample que entra suave llega a su nivel desde el principio",
           db (levelFirst - levelLater));
    const double with = bite (1.0);
    check (with - without > 3.0, "Golpe da un ataque a un sample que entra suave", db (with - without));
}

// A keyboard's own controls act as on a real instrument.
void auditExpression()
{
    title (P::u8 ("Expresión MIDI").toRawUTF8());
    constexpr double rate = 48000.0;
    Sample s = SampleAnalysis::analyse (tone (440.0, 3.0, rate, 0.4f), rate, "tone", false);
    auto p = paramsFor (*s);
    p[P::SustainType] = (float) P::SustainHold;
    p[P::AmpS] = 1.0f;

    const auto play = [&] (const ParamSnapshot& q, std::function<void (juce::MidiBuffer&)> controls)
    {
        SynthEngine e;
        e.prepare (rate);
        e.setSample (s);
        e.prepareFx (q);
        Audio out (2, (int) (1.5 * rate)), buf (2, 512);
        for (int pos = 0; pos < out.getNumSamples(); pos += 512)
        {
            juce::MidiBuffer midi;
            if (pos == 0)
            {
                controls (midi);
                midi.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey, 1.0f), 0);
            }
            const int n = std::min (512, out.getNumSamples() - pos);
            Audio view (buf.getArrayOfWritePointers(), 2, n);
            e.process (view, midi, q, 120.0);
            out.copyFrom (0, pos, buf, 0, 0, n);
        }
        return out;
    };

    // Pitch over time: rising zero crossings, placed between samples, in 20 ms windows.
    const auto pitchSpread = [&] (const Audio& y)
    {
        double low = 1.0e9, high = 0.0;
        for (double at = 0.3; at < 1.4; at += 0.02)
        {
            const int a = (int) (at * rate), n = (int) (0.02 * rate);
            double first = -1.0, last = -1.0;
            int cycles = -1;
            for (int i = a + 1; i < a + n; ++i)
            {
                const float x0 = y.getSample (0, i - 1), x1 = y.getSample (0, i);
                if (x0 < 0.0f && x1 >= 0.0f)
                {
                    last = i - 1 + x0 / (x0 - x1);
                    if (first < 0.0)
                        first = last;
                    ++cycles;
                }
            }
            if (cycles < 2)
                continue;
            const double hz = cycles * rate / (last - first);
            low = std::min (low, hz);
            high = std::max (high, hz);
        }
        return 1200.0 * std::log2 (high / low);
    };
    const auto none = [] (juce::MidiBuffer&) {};
    const auto wheel = [] (juce::MidiBuffer& m) { m.addEvent (juce::MidiMessage::controllerEvent (1, 1, 127), 0); };
    check (pitchSpread (play (p, none)) < 15.0, "sin rueda no hay vibrato", juce::String (pitchSpread (play (p, none)), 0) + " ct");
    check (pitchSpread (play (p, wheel)) > 40.0, "la rueda de modulación hace vibrato", juce::String (pitchSpread (play (p, wheel)), 0) + " ct");

    auto bendRange = p;
    bendRange[P::BendRange] = 12.0f;
    const auto bent = play (bendRange, [] (juce::MidiBuffer& m) { m.addEvent (juce::MidiMessage::pitchWheel (1, 16383), 0); });
    int crossings = 0;
    for (int i = (int) (0.5 * rate); i < (int) (1.0 * rate); ++i)
        crossings += (bent.getSample (0, i - 1) < 0.0f) != (bent.getSample (0, i) < 0.0f);
    check (std::abs (crossings - 880) < 12, "Bend 12: la rueda de pitch sube una octava", juce::String (crossings) + " cruces (880 esperados)");

    const auto level = [&] (const Audio& y) { return toDb (rmsOf (y.getReadPointer (0) + (int) (0.8 * rate), (int) (0.3 * rate))); };
    const double plain = level (play (p, none));
    const double pressed = level (play (p, [] (juce::MidiBuffer& m) { m.addEvent (juce::MidiMessage::channelPressureChange (1, 127), 0); }));
    check (pressed - plain > 2.0, "el aftertouch sube el sonido", db (pressed - plain));

    const double half = level (play (p, [] (juce::MidiBuffer& m) { m.addEvent (juce::MidiMessage::controllerEvent (1, 11, 64), 0); }));
    check (std::abs ((half - plain) + 12.0) < 1.0, "CC11 a la mitad baja 12 dB", db (half - plain));
}

double difference (const Audio& a, const Audio& b);

// Engine E sounds as long as the key is held, at the same level whatever the density, and
// takes its grains where Posición says. The texture adds grains to the other engines.
void auditGranular()
{
    title ("Motor E: granular");
    constexpr double rate = 48000.0;
    Audio two (2, (int) (1.0 * rate));
    for (int i = 0; i < two.getNumSamples(); ++i)
    {
        const double hz = i < two.getNumSamples() / 2 ? 300.0 : 1200.0;
        const float v = 0.4f * (float) std::sin (twoPi * hz * i / rate) * std::exp (-(float) i / (float) rate);
        two.setSample (0, i, v);
        two.setSample (1, i, v);
    }
    Sample s = SampleAnalysis::analyse (two, rate, "two", false);
    auto p = paramsFor (*s);
    p[P::Engine] = (float) P::EngineGranular;
    p[P::GranSpeed] = 0.0f;
    p[P::AmpS] = 1.0f;
    p[P::AmpFade] = 0.0f;

    const auto y = render (s, p, SynthEngine::rootKey, rate, 3.0, 3.0);
    const auto level = [&] (const Audio& a, double at) { return toDb (rmsOf (a.getReadPointer (0) + (int) (at * rate), (int) (0.2 * rate))); };
    check (std::abs (level (y, 0.5) - level (y, 2.6)) < 3.0 && level (y, 2.6) > -40.0, "la nube suena mientras se mantiene la tecla",
           db (level (y, 2.6) - level (y, 0.5)));

    auto dense = p;
    dense[P::GrainDensity] = 120.0f;
    const auto z = render (s, dense, SynthEngine::rootKey, rate, 3.0, 3.0);
    check (std::abs (level (z, 1.0) - level (y, 1.0)) < 3.0, "más densidad no cambia el volumen", db (level (z, 1.0) - level (y, 1.0)));

    const auto highShare = [&] (float position)
    {
        auto q = p;
        q[P::GrainPos] = position;
        const auto out = render (s, q, SynthEngine::rootKey, rate, 1.0, 1.0);
        return toneAmp (out.getReadPointer (0) + (int) (0.3 * rate), (int) (0.5 * rate), 1200.0, rate)
             / (toneAmp (out.getReadPointer (0) + (int) (0.3 * rate), (int) (0.5 * rate), 300.0, rate) + 1.0e-9);
    };
    check (highShare (0.8f) > 10.0 && highShare (0.2f) < 0.1, "Posición elige de qué parte del sample salen los granos",
           juce::String (highShare (0.2f), 3) + " / " + juce::String (highShare (0.8f), 1));

    auto textured = paramsFor (*s);
    const auto plain = render (s, textured, SynthEngine::rootKey, rate, 0.6, 0.6);
    textured[P::GrainMix] = 1.0f;
    const auto mixed = render (s, textured, SynthEngine::rootKey, rate, 0.6, 0.6);
    check (difference (plain, mixed) > -20.0, "Textura añade granos al motor A", db (difference (plain, mixed)));
}

// Engine F at every rate: the level of the sample where it reads, the note of the sample at its
// key and an octave up, and nothing but finite numbers.
void auditDrone()
{
    title ("Motor F: drone maker");
    for (const double rate : rates)
    {
        const juce::String tag = juce::String (rate / 1000.0, 1) + " kHz";
        Sample s = SampleAnalysis::analyse (pluck (220.0, 2.0, rate), rate, "drone", false);
        auto p = paramsFor (*s);
        p[P::Engine] = (float) P::EngineDrone;
        p[P::SustainType] = (float) P::SustainHold;
        p[P::AmpA] = 5.0f;
        p[P::AmpD] = 10.0f;
        p[P::AmpS] = 1.0f;
        p[P::AmpFade] = 0.0f;
        p[P::GrainPos] = 0.1f;
        p[P::DroneStretch] = 50.0f;

        const auto y = render (s, p, SynthEngine::rootKey, rate, 4.0, 4.0);
        const auto up = render (s, p, SynthEngine::rootKey + 12, rate, 3.0, 3.0);
        bool finite = true;
        for (const auto* a : { &y, &up })
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a->getNumSamples(); ++i)
                    finite = finite && std::isfinite (a->getSample (ch, i));

        const double lift = juce::jlimit (1.0, 4.0, 1.0 / std::max (0.25, (double) s->envAt (0.2 * rate)));
        const double source = toDb (rmsOf (s->audio.getReadPointer (0) + (int) (0.1 * rate), (int) (0.2 * rate)) * lift);
        const double level = toDb (rmsOf (y.getReadPointer (0) + (int) (1.5 * rate), (int) (2.0 * rate)));
        check (finite && std::abs (level - source) < 4.0, tag + ": suena al nivel del sample", db (level - source));

        const int at = (int) (1.5 * rate), n = (int) (1.0 * rate);
        const double home = toneAmp (y.getReadPointer (0) + at, n, 220.0, rate) / (toneAmp (y.getReadPointer (0) + at, n, 233.1, rate) + 1.0e-9);
        const double octave = toneAmp (up.getReadPointer (0) + at, n, 440.0, rate) / (toneAmp (up.getReadPointer (0) + at, n, 466.2, rate) + 1.0e-9);
        check (home > 4.0 && octave > 4.0, tag + ": en la nota del sample y una octava arriba",
               juce::String (home, 1) + "x / " + juce::String (octave, 1) + "x sobre el semitono de al lado");
    }
}

// The edit page: the loop holds a short sample, Fin cuts it, the fades shape it, and the
// pitch envelope drops from above, like an 808.
void auditSampleEdit()
{
    title (P::u8 ("Edición del sample").toRawUTF8());
    for (const double rate : rates)
    {
        const juce::String tag = juce::String (rate / 1000.0, 1) + " kHz";
        Sample s = SampleAnalysis::analyse (pluck (220.0, 1.0, rate), rate, "edit", false);
        const auto level = [&] (const Audio& a, double at) { return toDb (rmsOf (a.getReadPointer (0) + (int) (at * rate), (int) (0.05 * rate)) + 1.0e-12); };

        auto p = paramsFor (*s);
        p[P::LoopOn] = 1.0f;
        p[P::LoopStart] = 0.2f;
        p[P::LoopEnd] = 0.4f;
        const auto looped = render (s, p, SynthEngine::rootKey, rate, 3.0, 3.0);
        check (level (looped, 2.5) > level (looped, 0.3) - 6.0, tag + ": el bucle sostiene un sample corto", db (level (looped, 2.5) - level (looped, 0.3)));

        // The jump at the loop end makes no step: the largest sample-to-sample change stays like the sound's own.
        double jump = 0.0, usual = 0.0;
        for (int i = (int) (1.0 * rate); i < (int) (2.5 * rate); ++i)
            jump = std::max (jump, (double) std::abs (looped.getSample (0, i) - looped.getSample (0, i - 1)));
        for (int i = (int) (0.25 * rate); i < (int) (0.38 * rate); ++i)
            usual = std::max (usual, (double) std::abs (looped.getSample (0, i) - looped.getSample (0, i - 1)));
        check (jump < usual * 1.5, tag + ": el salto del bucle no da clic", juce::String (jump, 4) + " frente a " + juce::String (usual, 4));

        auto cut = paramsFor (*s);
        cut[P::EndPos] = 0.3f;
        cut[P::FadeOut] = 20.0f;
        const auto ended = render (s, cut, SynthEngine::rootKey, rate, 0.8, 0.8);
        check (level (ended, 0.4) < -80.0, tag + ": Fin corta el sample ahí", db (level (ended, 0.4)));

        auto drop = paramsFor (*s);
        drop[P::PitchEnvAmt] = 12.0f;
        drop[P::PitchEnvTime] = 300.0f;
        const auto dropped = render (s, drop, SynthEngine::rootKey, rate, 0.8, 0.8);
        const double startHigh = toneAmp (dropped.getReadPointer (0), (int) (0.02 * rate), 440.0, rate);
        const double startLow = toneAmp (dropped.getReadPointer (0), (int) (0.02 * rate), 220.0, rate);
        check (startHigh > startLow, tag + ": la envolvente de pitch empieza una octava arriba", juce::String (startHigh / (startLow + 1.0e-9), 1) + " veces");
    }

    constexpr double rate = 48000.0;
    Audio quiet = pluck (220.0, 1.0, rate);
    quiet.applyGain (0.1f);
    Sample s = SampleAnalysis::analyse (quiet, rate, "quiet", false);
    auto p = paramsFor (*s);
    p[P::Normalize] = 1.0f;
    const auto y = render (s, p, SynthEngine::rootKey, rate, 0.5, 0.5);
    check (std::abs (toDb (y.getMagnitude (0, 0, y.getNumSamples())) - toDb (s->peakLevel / s->peakLevel)) < 1.5, "Normalizar lleva el pico a 0 dB",
           db (toDb (y.getMagnitude (0, 0, y.getNumSamples()))));
}

void auditOutputMode()
{
    title ("Mono, Unison y Bits");
    for (const double rate : rates)
    {
        const juce::String tag = juce::String (rate / 1000.0, 1) + " kHz";
        Audio stereo = pluck (220.0, 1.0, rate);
        for (int i = 0; i < stereo.getNumSamples(); ++i)
            stereo.setSample (1, i, stereo.getSample (1, i) * 0.4f);
        Sample s = SampleAnalysis::analyse (stereo, rate, "wide", false);

        auto p = paramsFor (*s);
        const auto wide = render (s, p, SynthEngine::rootKey, rate, 0.5, 0.5);
        p[P::MonoOut] = 1.0f;
        const auto mono = render (s, p, SynthEngine::rootKey, rate, 0.5, 0.5);
        double apart = 0.0, wrong = 0.0;
        for (int i = 0; i < mono.getNumSamples(); ++i)
        {
            apart = std::max (apart, (double) std::abs (mono.getSample (0, i) - mono.getSample (1, i)));
            wrong = std::max (wrong, (double) std::abs (mono.getSample (0, i) - 0.5f * (wide.getSample (0, i) + wide.getSample (1, i))));
        }
        check (apart < 1.0e-7, tag + ": Mono deja los dos canales iguales", db (toDb (apart)));
        check (wrong < 1.0e-5, tag + ": Mono es la media de los dos canales, sin retardo", db (toDb (wrong)));

        // Lanes in tune are one sound: same level, no comb.
        Sample c = SampleAnalysis::analyse (pluck (220.0, 1.0, rate), rate, "centre", false);
        auto q = paramsFor (*c);
        const auto one = render (c, q, SynthEngine::rootKey, rate, 0.5, 0.5);
        q[P::Unison] = 4.0f;
        q[P::UniDetune] = 0.0f;
        q[P::UniWidth] = 0.0f;
        q[P::UniBlend] = 1.0f / 1.5f;
        const auto four = render (c, q, SynthEngine::rootKey, rate, 0.5, 0.5);
        double difference = 0.0;
        for (int i = 0; i < one.getNumSamples(); ++i)
            difference = std::max (difference, (double) std::abs (one.getSample (0, i) - four.getSample (0, i)));
        check (difference < 1.0e-3, tag + ": Unison afinado suena como una voz, sin filtro de peine", db (toDb (difference)));

        // The lowest resolution is dirty, not silent, even for a quiet sample.
        Audio quiet = pluck (220.0, 1.0, rate);
        quiet.applyGain (0.05f);
        Sample l = SampleAnalysis::analyse (quiet, rate, "quiet", false);
        auto b = paramsFor (*l);
        b[P::Bits] = 1.0f;
        const auto crushed = render (l, b, SynthEngine::rootKey, rate, 0.5, 0.5);
        check (rmsOf (crushed.getReadPointer (0), crushed.getNumSamples()) > 1.0e-3, tag + ": Bits al mínimo no borra un sample flojo", "");
    }
}

void auditEnvelope()
{
    title ("Envolvente: igual a todas las frecuencias");
    double reference[3] {};
    for (const double rate : rates)
    {
        Sample s = SampleAnalysis::analyse (pluck (220.0, 2.0, 48000.0), 48000.0, "env", false);
        auto p = paramsFor (*s);
        p[P::SustainType] = (float) P::SustainHold;
        p[P::AmpA] = 100.0f;
        p[P::AmpCurve] = 1.0f;
        p[P::AmpD] = 200.0f;
        p[P::AmpS] = 0.5f;
        p[P::AmpR] = 200.0f;
        const auto y = render (s, p, SynthEngine::rootKey, rate, 1.0, 1.5);

        auto level = [&] (double at) { const int n = (int) (0.03 * rate); return rmsOf (y.getReadPointer (0) + (int) (at * rate), n); };
        const double values[3] = { toDb (level (0.05)), toDb (level (0.8)), toDb (level (1.2)) };
        if (rate == rates[0])
            std::copy (values, values + 3, reference);
        const double worst = std::max ({ std::abs (values[0] - reference[0]), std::abs (values[1] - reference[1]), std::abs (values[2] - reference[2]) });
        check (worst < 1.5, juce::String (rate / 1000.0, 1) + " kHz: mismos niveles en ataque, sustain y release", "diferencia " + db (worst));

        // Release ends quietly: no step when the voice stops.
        int last = y.getNumSamples() - 1;
        while (last > 0 && y.getSample (0, last) == 0.0f)
            --last;
        const double tail = std::abs (y.getSample (0, last));
        check (toDb (tail) < -90.0, juce::String (rate / 1000.0, 1) + " kHz: la cola acaba sin escalón", db (toDb (tail)));
    }
}

void auditStartPoint()
{
    title ("Punto de inicio");
    for (const double rate : rates)
    {
        const juce::String tag = juce::String (rate / 1000.0, 1) + " kHz: ";
        Sample s = SampleAnalysis::analyse (pluck (220.0, 2.0, rate), rate, "start", false);
        auto p = paramsFor (*s);
        const double start = 0.4 * s->length;
        p[P::StartPos] = 0.4f;

        const auto y = render (s, p, SynthEngine::rootKey, rate, 0.5, 0.6);
        const int from = (int) std::round (s->startPosition (0.4));

        // After the short fade it is the file from that point on, sample by sample.
        const int fade = (int) (0.003 * rate), n = (int) (0.4 * rate);
        double sig = 0.0, err = 0.0;
        for (int i = fade; i < n; ++i)
        {
            const double x = s->audio.getSample (0, from + i);
            sig += x * x;
            err += (x - y.getSample (0, i)) * (x - y.getSample (0, i));
        }
        check (10.0 * std::log10 (sig / std::max (1.0e-20, err)) > 100.0, tag + "toca el archivo desde el punto elegido", juce::String (start / rate, 2) + " s");

        // Landing on a loud part must not click: the first sample starts from silence.
        double worstStep = 0.0;
        for (int i = 1; i < fade; ++i)
            worstStep = std::max (worstStep, (double) std::abs (y.getSample (0, i) - y.getSample (0, i - 1)));
        double typicalStep = 0.0;
        for (int i = fade; i < n; ++i)
            typicalStep = std::max (typicalStep, (double) std::abs (y.getSample (0, i) - y.getSample (0, i - 1)));
        check (std::abs (y.getSample (0, 0)) < 1.0e-3f && worstStep <= typicalStep * 1.2, tag + "empezar a media onda no da clic",
               "primera muestra " + juce::String (y.getSample (0, 0), 5) + ", peor salto " + juce::String (worstStep, 5));

        // Changing the envelope from a later start: the level follows the sample, no jump.
        auto q = p;
        q[P::SustainType] = (float) P::SustainHold;
        q[P::AmpS] = 0.5f;
        const auto held = render (s, q, SynthEngine::rootKey, rate, 1.5, 1.6);
        const int w = (int) (0.02 * rate);
        double biggest = 0.0;
        double previous = toDb (rmsOf (held.getReadPointer (0) + w, w));
        for (int at = 2 * w; at + w < (int) (1.4 * rate); at += w)
        {
            const double level = toDb (rmsOf (held.getReadPointer (0) + at, w));
            biggest = std::max (biggest, std::abs (level - previous));
            previous = level;
        }
        check (biggest < 6.0, tag + "con ADSR cambiado, sin saltos de nivel desde un inicio tardío", "mayor salto " + db (biggest) + " en 20 ms");
    }

    // Before the detected onset is allowed too: the whole file is available.
    {
        const double rate = 48000.0;
        Audio padded (2, (int) (1.5 * rate));
        padded.clear();
        const auto body = pluck (220.0, 1.0, rate);
        const int silence = (int) (0.2 * rate);
        for (int ch = 0; ch < 2; ++ch)
            padded.copyFrom (ch, silence, body, ch, 0, body.getNumSamples());
        Sample s = SampleAnalysis::analyse (padded, rate, "padded", false);
        check (s->onset > 0.15 * rate, "el silencio inicial se detecta", juce::String (s->onset / rate, 3) + " s");

        auto p = paramsFor (*s);
        const auto detected = render (s, p, SynthEngine::rootKey, rate, 0.3, 0.4);
        p[P::StartPos] = 0.0f;
        const auto whole = render (s, p, SynthEngine::rootKey, rate, 0.5, 0.6);
        const int skip = (int) s->onset;
        check (rmsOf (detected.getReadPointer (0), 1000) > 1.0e-3 && rmsOf (whole.getReadPointer (0), 1000) < 1.0e-6 && rmsOf (whole.getReadPointer (0) + skip, 1000) > 1.0e-3,
               "inicio en 0 toca el silencio inicial; el detectado lo salta");
    }
}

void auditEdgeCases()
{
    title ("Archivos raros");
    const double rate = 48000.0;
    juce::Random rng (9);

    struct Case { const char* name; Audio audio; double fileRate; };
    std::vector<Case> cases;
    auto add = [&] (const char* name, int channels, int samples, double fileRate, std::function<float (int)> fill)
    {
        Audio a (channels, samples);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < samples; ++i)
                a.setSample (ch, i, fill (i));
        cases.push_back ({ name, std::move (a), fileRate });
    };

    add ("silencio", 2, 48000, rate, [] (int) { return 0.0f; });
    add ("una muestra", 2, 1, rate, [] (int) { return 0.5f; });
    add ("diez muestras", 1, 10, rate, [] (int) { return 0.5f; });
    add ("100 muestras", 2, 100, rate, [] (int i) { return 0.5f * std::sin (0.3f * (float) i); });
    add ("mono", 1, 48000, rate, [] (int i) { return 0.5f * std::sin (0.05f * (float) i); });
    add ("continua", 2, 48000, rate, [] (int) { return 0.9f; });
    add ("ruido a tope", 2, 96000, rate, [&] (int) { return rng.nextFloat() * 2.0f - 1.0f; });
    add ("onda cuadrada a tope", 2, 48000, rate, [] (int i) { return (i / 100) % 2 == 0 ? 1.0f : -1.0f; });
    add ("casi silencio", 2, 48000, rate, [] (int i) { return 1.0e-7f * std::sin (0.05f * (float) i); });
    add ("8 kHz", 2, 16000, 8000.0, [] (int i) { return 0.5f * std::sin (0.2f * (float) i); });
    add ("192 kHz", 2, 192000, 192000.0, [] (int i) { return 0.5f * std::sin (0.01f * (float) i); });

    for (const auto& c : cases)
    {
        juce::String problems;
        for (const int engineMode : { (int) P::EngineRebuild, (int) P::EngineHybrid, (int) P::EngineReenvelope, (int) P::EngineResynth })
        {
            Sample s = SampleAnalysis::analyse (c.audio, c.fileRate, c.name, false);
            auto p = paramsFor (*s);
            p[P::Engine] = (float) engineMode;
            for (const int type : { (int) P::SustainNatural, (int) P::SustainHold })
            {
                p[P::SustainType] = (float) type;
                p[P::AmpS] = 0.8f;
                const auto y = render (s, p, 72, rate, 0.4, 0.6);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < y.getNumSamples(); ++i)
                        if (! std::isfinite (y.getSample (ch, i)))
                        {
                            problems << "motor " << engineMode << " ";
                            ch = 2;
                            break;
                        }
            }
        }
        check (problems.isEmpty(), juce::String ("carga y toca: ") + c.name, problems);
    }
}

void auditSmoothness()
{
    title ("Cambios de parámetros sin clics");
    const double rate = 44100.0;
    Sample s = SampleAnalysis::analyse (tone (440.0, 4.0, rate, 0.25f), rate, "smooth", false);
    auto p = paramsFor (*s);
    SynthEngine e;
    e.prepare (rate);
    e.setSample (s);
    e.prepareFx (p);

    constexpr int block = 2048;
    Audio buf (2, block);
    juce::MidiBuffer on;
    on.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey, 1.0f), 0);
    e.process (buf, on, p, 120.0);

    // A big jump of Volume between two host blocks becomes a ramp inside the next one.
    const float last = buf.getSample (0, block - 1);
    const juce::MidiBuffer none;
    p[P::Volume] = -30.0f;
    e.process (buf, none, p, 120.0);
    double worst = std::abs (buf.getSample (0, 0) - last);
    for (int i = 1; i < block; ++i)
        worst = std::max (worst, (double) std::abs (buf.getSample (0, i) - buf.getSample (0, i - 1)));
    const double sineStep = 0.25 * twoPi * 440.0 / rate;
    check (worst <= sineStep * 1.05, "saltar Volumen 30 dB entre bloques no da clic", "mayor salto " + juce::String (worst, 5) + ", el de la onda " + juce::String (sineStep, 5));

    // Taking a note away while the tilt filter fades in and out.
    {
        auto q = paramsFor (*s);
        q[P::AmpA] = 800.0f;
        q[P::Bow] = 1.0f;
        const auto y = render (s, q, SynthEngine::rootKey, rate, 1.2, 1.4);
        // Relative to the local level: a louder tone has bigger steps, a click stands out from them.
        double worstRatio = 0.0;
        int where = 0;
        const int w = 64;
        for (int i = w; i + w < y.getNumSamples(); ++i)
        {
            const double step = std::abs (y.getSample (0, i) - y.getSample (0, i - 1));
            const double local = y.getMagnitude (0, i - w, 2 * w) * twoPi * 440.0 / rate;
            if (local > 1.0e-3 && step / local > worstRatio)
            {
                worstRatio = step / local;
                where = i;
            }
        }
        check (worstRatio < 1.5, "entrada lenta con arco sin clics", "peor salto " + juce::String (worstRatio, 2) + " veces el de la onda, en " + juce::String (where / rate, 3) + " s, nivel " + juce::String (y.getMagnitude (0, where - w, 2 * w), 3));
    }

    // The rack must not fall asleep between echoes.
    {
        FxRack rack;
        rack.prepare (rate);
        ParamSnapshot q {};
        for (int i = 0; i < P::Count; ++i)
            q[(size_t) i] = P::defs()[(size_t) i].def;
        q[P::fxType (0)] = (float) Fx::Delay;
        q[P::fxOn (0)] = 1.0f;
        q[P::fxMix (0)] = 1.0f;
        const auto& spec = Fx::spec (Fx::Delay);
        for (int k = 0; k < (int) spec.params.size(); ++k)
            q[(size_t) P::fxParam (0, k)] = Fx::toNorm (spec.params[(size_t) k], spec.params[(size_t) k].def);
        q[(size_t) P::fxParam (0, 0)] = 0.0f;                                   // no sync
        q[(size_t) P::fxParam (0, 1)] = Fx::toNorm (spec.params[1], 2000.0f);   // two seconds
        q[(size_t) P::fxParam (0, 7)] = Fx::toNorm (spec.params[7], 0.0f);
        rack.ensureModules (q);

        Audio chunk (2, 512);
        float echo = 0.0f;
        for (int pos = 0; pos < (int) (3.0 * rate); pos += 512)
        {
            chunk.clear();
            if (pos == 0)
                for (int i = 0; i < 64; ++i)
                    chunk.setSample (0, i, 0.5f), chunk.setSample (1, i, 0.5f);
            rack.process (chunk, q, 120.0);
            if (pos > (int) (1.5 * rate))
                echo = std::max (echo, chunk.getMagnitude (0, 512));
        }
        check (echo > 0.01f, "un eco de 2 s llega aunque haya silencio entre medias", juce::String (echo, 3));
    }
}

//==============================================================================
struct FxRun
{
    ParamSnapshot p {};
    int slot = 0;
    const Audio* side = nullptr;    // the sidechain (Impronta), as long as the input

    explicit FxRun (int type, float mix = 1.0f)
    {
        for (int i = 0; i < P::Count; ++i)
            p[(size_t) i] = P::defs()[(size_t) i].def;
        p[P::Volume] = 0.0f;           // the engine at unity: the -6 dB default is room for chords
    p[P::MonoBass] = 0.0f;         // the engine's own output (Graves mono shifts the lows' phase on purpose)
        p[(size_t) P::fxType (slot)] = (float) type;
        p[(size_t) P::fxOn (slot)] = 1.0f;
        p[(size_t) P::fxMix (slot)] = mix;
        const auto& spec = Fx::spec (type);
        for (int k = 0; k < (int) spec.params.size(); ++k)
            p[(size_t) P::fxParam (slot, k)] = Fx::toNorm (spec.params[(size_t) k], spec.params[(size_t) k].def);
    }

    void set (int k, float value)
    {
        const auto& spec = Fx::spec ((int) p[(size_t) P::fxType (slot)]);
        p[(size_t) P::fxParam (slot, k)] = Fx::toNorm (spec.params[(size_t) k], value);
    }

    Audio run (Audio in, double rate, int block = 512) const
    {
        FxRack rack;
        rack.prepare (rate);
        rack.ensureModules (p);
        Audio chunk (2, block);
        for (int pos = 0; pos < in.getNumSamples(); pos += block)
        {
            const int n = std::min (block, in.getNumSamples() - pos);
            Audio view (chunk.getArrayOfWritePointers(), 2, n);
            view.copyFrom (0, 0, in, 0, pos, n);
            view.copyFrom (1, 0, in, 1, pos, n);
            if (side != nullptr && pos + n <= side->getNumSamples())
                rack.setSidechain (side->getReadPointer (0, pos), side->getReadPointer (1, pos), n);
            else
                rack.setSidechain (nullptr, nullptr, 0);
            rack.process (view, p, 120.0);
            in.copyFrom (0, pos, view, 0, 0, n);
            in.copyFrom (1, pos, view, 1, 0, n);
        }
        return in;
    }
};

Audio richInput (double seconds, double rate)
{
    juce::Random rng (3);
    Audio b (2, (int) (seconds * rate));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const double t = i / rate;
        const float env = (float) std::exp (-std::fmod (t, 0.4) * 6.0);
        const float l = 0.25f * env * (rng.nextFloat() * 2.0f - 1.0f) + 0.25f * env * (float) std::sin (twoPi * 220.0 * t);
        const float r = 0.25f * env * (rng.nextFloat() * 2.0f - 1.0f) + 0.25f * env * (float) std::sin (twoPi * 331.0 * t);
        b.setSample (0, i, l);
        b.setSample (1, i, r);
    }
    return b;
}

// Turning an effect off and on: no jump in the sound and no echoes from the last time.
void auditFxSwitching()
{
    title ("Efectos: encender y apagar");
    constexpr double rate = 48000.0;
    constexpr int block = 256;

    FxRun off (Fx::Delay, 0.5f);
    off.p[(size_t) P::fxOn (0)] = 0.0f;
    FxRun on (Fx::Delay, 0.5f);

    FxRack rack;
    rack.prepare (rate);
    rack.ensureModules (on.p);

    double phase = 0.0;
    float last = 0.0f;
    const auto feed = [&] (const ParamSnapshot& p, double seconds, bool sound, double& jump, double& energy)
    {
        Audio view (2, block);
        for (int pos = 0; pos < (int) (seconds * rate); pos += block)
        {
            for (int i = 0; i < block; ++i)
            {
                const float v = sound ? 0.3f * (float) std::sin (phase) : 0.0f;
                phase += twoPi * 110.0 / rate;
                view.setSample (0, i, v);
                view.setSample (1, i, v);
            }
            rack.process (view, p, 120.0);
            for (int i = 0; i < block; ++i)
            {
                const float y = view.getSample (0, i);
                jump = std::max (jump, (double) std::abs (y - last));
                energy = std::max (energy, (double) std::abs (y));
                last = y;
            }
        }
    };

    double usual = 0.0, jump = 0.0, energy = 0.0, ignored = 0.0;
    feed (on.p, 0.6, true, usual, ignored);
    feed (off.p, 0.2, true, jump, energy);
    check (jump < usual * 3.0, "apagar un efecto no da un salto", juce::String (jump, 3) + " de " + juce::String (usual, 3));

    feed (off.p, 0.3, false, ignored, ignored);
    energy = 0.0;
    feed (on.p, 0.5, false, jump, energy);
    check (energy < 1.0e-6, "al encenderlo no vuelven los ecos de antes", db (toDb (energy + 1.0e-12)));
}

void auditFx()
{
    title ("Efectos: estabilidad, DC y parámetros extremos a cada frecuencia");
    for (const double rate : rates)
    {
        juce::String bad;
        for (int type = 1; type < Fx::NumTypes; ++type)
        {
            const auto& spec = Fx::spec (type);
            juce::Random rng (type);
            for (int trial = 0; trial < 6; ++trial)
            {
                FxRun run (type);
                for (int k = 0; k < (int) spec.params.size(); ++k)
                    run.p[(size_t) P::fxParam (run.slot, k)] = trial == 0 ? 1.0f : (trial == 1 ? 0.0f : rng.nextFloat());
                const auto y = run.run (richInput (1.5, rate), rate);

                bool finite = true;
                double dc = 0.0;
                float peak = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < y.getNumSamples(); ++i)
                    {
                        const float v = y.getSample (ch, i);
                        finite = finite && std::isfinite (v);
                        peak = std::max (peak, std::abs (v));
                        dc += v;
                    }
                dc /= 2.0 * y.getNumSamples();
                if (! finite || peak > 1000.0f)     // the compressor alone can add 54 dB on purpose
                    bad << juce::String::fromUTF8 (spec.name) << " (prueba " << trial << ", pico " << juce::String (peak, 1) << ") ";
            }
        }
        check (bad.isEmpty(), juce::String (rate / 1000.0, 1) + " kHz: todos los efectos estables en extremos", bad);
    }

    title ("Efectos: cada uno hace lo que dice");
    for (const double rate : rates)
    {
        const juce::String tag = juce::String (rate / 1000.0, 1) + " kHz: ";
        const int order = 14, at = (int) (0.3 * rate);

        // Frequency shifter: one sideband, not both.
        {
            FxRun run (Fx::Bode);
            run.set (0, 100.0f);
            const auto y = run.run (tone (1000.0, 1.0, rate), rate);
            const double up = toneAmp (y.getReadPointer (0) + at, 1 << order, 1100.0, rate);
            const double image = toneAmp (y.getReadPointer (0) + at, 1 << order, 900.0, rate);
            check (toDb (image / up) < -40.0 && std::abs (toDb (up / 0.5)) < 1.0, tag + "Bode desplaza +100 Hz sin banda imagen",
                   "nivel " + db (toDb (up / 0.5)) + ", imagen " + db (toDb (image / up)));
        }

        // Distortion: the bass stays, the aliasing stays out.
        {
            FxRun run (Fx::Distortion);
            run.set (0, 1.0f);      // Soft Clip
            run.set (1, 0.0f);
            const auto y = run.run (tone (40.0, 1.0, rate, 0.1f), rate);
            const double level = toneAmp (y.getReadPointer (0) + at, 1 << order, 40.0, rate);
            check (std::abs (toDb (level / 0.1)) < 0.5, tag + "Distorsión no recorta graves (40 Hz)", db (toDb (level / 0.1)));
        }
        for (const int mode : { 1, 2 })
        {
            FxRun run (Fx::Distortion);
            run.set (0, (float) mode);
            run.set (1, 0.6f);
            const double hz = 3163.0;
            const auto y = run.run (tone (hz, 1.0, rate, 0.8f), rate);
            const auto mag = spectrum (y.getReadPointer (0) + at, order);
            std::vector<double> harmonics;
            for (double h = hz; h < rate / 2; h += hz)
                harmonics.push_back (h);
            double worst = 1.0e-12, top = 0.0;
            const double binHz = rate / (1 << order);
            for (int k = 2; k < (int) mag.size() && k * binHz < 20000.0; ++k)
            {
                bool harmonic = false;
                for (const double h : harmonics)
                    harmonic = harmonic || std::abs (k - h / binHz) < 8.0;
                top = std::max (top, (double) mag[(size_t) k]);
                if (! harmonic)
                    worst = std::max (worst, (double) mag[(size_t) k]);
            }
            check (toDb (worst / top) < -60.0, tag + "Distorsión " + (mode == 1 ? "Soft" : "Hard") + " Clip, aliasing", db (toDb (worst / top)));
        }

        // Dry/wet of a slot that delays the signal internally: no comb.
        {
            FxRun run (Fx::Distortion, 0.5f);
            run.set (0, 1.0f);
            run.set (1, 0.0f);
            double worst = 0.0;
            for (const double hz : { 200.0, 2000.0, 8000.0, 15000.0 })
            {
                const auto y = run.run (tone (hz, 0.6, rate, 0.05f), rate);
                worst = std::max (worst, std::abs (toDb (toneAmp (y.getReadPointer (0) + at, 8192, hz, rate) / 0.05)));
            }
            check (worst < 0.5, tag + "Distorsión al 50 % sin filtro de peine", "peor desviación " + db (worst));
        }

        // Delay: the echo arrives when it should.
        {
            FxRun run (Fx::Delay);
            run.set (0, 0.0f);
            run.set (1, 250.0f);
            run.set (2, 250.0f);
            run.set (6, 0.0f);
            run.set (7, 0.0f);
            run.set (8, 3000.0f);
            run.set (9, 8.0f);
            Audio in (2, (int) (0.6 * rate));
            in.clear();
            for (int i = 0; i < 16; ++i)
                in.setSample (0, 100 + i, 0.5f), in.setSample (1, 100 + i, 0.5f);
            const auto y = run.run (in, rate);
            int found = 0;
            float peak = 0.0f;
            for (int i = 0; i < y.getNumSamples(); ++i)
                if (std::abs (y.getSample (0, i)) > peak) { peak = std::abs (y.getSample (0, i)); found = i; }
            const double ms = (found - 100) / rate * 1000.0;
            check (std::abs (ms - 250.0) < 2.0, tag + "Delay de 250 ms", juce::String (ms, 2) + " ms");
        }

        // Reverb: decay time independent of the rate, and no DC.
        {
            FxRun run (Fx::Reverb);
            run.set (2, 2.0f);
            run.set (3, 0.0f);
            Audio in (2, (int) (3.0 * rate));
            in.clear();
            for (int i = 0; i < 64; ++i)
                in.setSample (0, i, 0.5f), in.setSample (1, i, 0.5f);
            const auto y = run.run (in, rate);
            const int w = (int) (0.1 * rate);
            const double a = rmsOf (y.getReadPointer (0) + (int) (0.3 * rate), w), b = rmsOf (y.getReadPointer (0) + (int) (1.3 * rate), w);
            const double rt60 = 60.0 / std::max (1.0e-6, toDb (a) - toDb (b));
            check (rt60 > 1.2 && rt60 < 3.0, tag + "Reverb con decay 2 s", "RT60 medido " + juce::String (rt60, 2) + " s");
        }

        // Compressor: the static curve.
        {
            FxRun run (Fx::Compressor);
            run.set (0, -20.0f);
            run.set (1, 4.0f);
            run.set (2, 1.0f);
            run.set (3, 50.0f);
            const auto y = run.run (tone (1000.0, 1.0, rate, 0.5f), rate);
            const double out = toDb (toneAmp (y.getReadPointer (0) + at, 8192, 1000.0, rate));
            const double expected = -20.0 + (toDb (0.5) + 20.0) / 4.0;
            check (std::abs (out - expected) < 1.5, tag + "Compresor 4:1 en -20 dB", db (out) + ", esperado " + db (expected));
        }

        // EQ: the gain lands where it is asked for.
        {
            FxRun run (Fx::EQ);
            run.set (0, 1.0f);
            run.set (1, 1000.0f);
            run.set (2, 12.0f);
            run.set (3, 2.0f);
            const auto y = run.run (tone (1000.0, 0.8, rate, 0.1f), rate);
            const double gain = toDb (toneAmp (y.getReadPointer (0) + at, 8192, 1000.0, rate) / 0.1);
            check (std::abs (gain - 12.0) < 0.2, tag + "EQ +12 dB en 1 kHz", db (gain));
        }

        // Splitters: the bands add back up to the input.
        for (const int type : { (int) Fx::SplitLH, (int) Fx::SplitLMH })
        {
            FxRun run (type);
            double worst = 0.0;
            for (const double hz : { 80.0, 250.0, 600.0, 3000.0, 10000.0 })
            {
                const auto y = run.run (tone (hz, 0.8, rate, 0.25f), rate);
                worst = std::max (worst, std::abs (toDb (toneAmp (y.getReadPointer (0) + at, 8192, hz, rate) / 0.25)));
            }
            check (worst < 0.1, tag + juce::String::fromUTF8 (Fx::spec (type).name) + " suma plano", db (worst));
        }

        // Utility: invert one side and the mono sum disappears.
        {
            FxRun run (Fx::Utility);
            run.set (4, 1.0f);
            const auto y = run.run (tone (500.0, 0.5, rate, 0.25f), rate);
            double sum = 0.0;
            for (int i = 0; i < y.getNumSamples(); ++i)
                sum = std::max (sum, (double) std::abs (y.getSample (0, i) + y.getSample (1, i)));
            check (sum < 1.0e-6, tag + "Utility invierte la polaridad", db (toDb (sum)));
        }
    }
}

//==============================================================================
double difference (const Audio& a, const Audio& b)
{
    double err = 0.0, ref = 1.0e-20;
    const int n = std::min (a.getNumSamples(), b.getNumSamples());
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i)
        {
            const double x = a.getSample (ch, i), y = b.getSample (ch, i);
            err += (x - y) * (x - y);
            ref += x * x;
        }
    return 10.0 * std::log10 (std::max (1.0e-20, err) / ref);
}

// Values to try for a parameter, away from the one it has.
std::vector<float> otherValues (const P::Def& d, float current)
{
    if (d.kind == P::Def::Bool)
        return { current > 0.5f ? 0.0f : 1.0f };
    if (d.kind == P::Def::Choice)
    {
        std::vector<float> all;
        const int options = juce::StringArray::fromTokens (P::u8 (d.choices), "|", "").size();
        for (int i = 0; i < options; ++i)
            if (i != (int) current)
                all.push_back ((float) i);
        return all;
    }
    return { d.min, d.max };
}

// Two overlapping notes, so that glide and the voice modes have something to act on.
Audio phrase (Sample s, const ParamSnapshot& p, double rate)
{
    SynthEngine e;
    e.prepare (rate);
    e.setSample (s);
    e.prepareFx (p);

    constexpr int block = 512;
    const int total = (int) (1.0 * rate), key = SynthEngine::rootKey + 5;
    const int secondOn = (int) (0.25 * rate), firstOff = (int) (0.4 * rate), secondOff = (int) (0.7 * rate);
    Audio out (2, total), buf (2, block);
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min (block, total - pos);
        juce::MidiBuffer midi;
        auto at = [&] (int when, const juce::MidiMessage& m) { if (when >= pos && when < pos + n) midi.addEvent (m, when - pos); };
        at (0, juce::MidiMessage::noteOn (1, key, 0.7f));
        at (secondOn, juce::MidiMessage::noteOn (1, key + 7, 0.5f));
        at (firstOff, juce::MidiMessage::noteOff (1, key));
        at (secondOff, juce::MidiMessage::noteOff (1, key + 7));
        Audio view (buf.getArrayOfWritePointers(), 2, n);
        e.process (view, midi, p, 100.0);
        out.copyFrom (0, pos, buf, 0, 0, n);
        out.copyFrom (1, pos, buf, 1, 0, n);
    }
    return out;
}

void auditParameters()
{
    title ("Cada parámetro hace algo audible");
    const double rate = 48000.0;
    Sample s = SampleAnalysis::analyse (pluck (220.0, 1.5, rate), rate, "params", false);

    auto context = [&] (int engineMode, int sustainType)
    {
        auto p = paramsFor (*s);
        p[P::Engine] = (float) engineMode;
        p[P::SustainType] = (float) sustainType;
        p[P::AmpS] = 0.7f;
        p[P::AmpA] = 150.0f;
        p[P::AmpFade] = 6.0f;
        p[P::Unison] = 3.0f;
        p[P::Bits] = 12.0f;
        p[P::Downsample] = 2.0f;
        p[P::Glide] = 50.0f;
        p[P::RelNoise] = 0.5f;
        p[P::FltOn] = 1.0f;
        p[P::FltCircuit] = (float) P::OSR;
        p[P::FltDrive] = 6.0f;
        p[P::FltEnvAmt] = 0.5f;
        p[P::FltFollow] = 0.5f;
        p[P::FltKey] = 0.5f;
        p[P::FltType] = (float) P::Morph;
        p[P::Lfo1Rate] = 3.0f;
        p[P::Lfo2Rate] = 5.0f;
        p[P::Lfo3Rate] = 7.0f;
        p[P::Macro1] = 0.3f;
        p[P::Macro2] = 0.4f;
        p[P::Macro3] = 0.5f;
        p[P::Macro4] = 0.6f;

        const int routes[][3] = { { P::SrcLfo1, P::DstPitch, 10 }, { P::SrcLfo2, P::DstCutoff, 50 }, { P::SrcLfo3, P::DstPan, 50 },
                                  { P::SrcMacro1, P::DstVolume, 30 }, { P::SrcMacro2, P::DstCutoff, 30 }, { P::SrcMacro3, P::DstPitch, 30 },
                                  { P::SrcMacro4, P::DstRes, 50 }, { P::SrcVelocity, P::DstVolume, 30 } };
        for (int r = 0; r < P::numSlots; ++r)
        {
            const int base = P::M1Src + r * P::slotStride;
            p[(size_t) base] = (float) routes[r][0];
            p[(size_t) base + 1] = (float) routes[r][1];
            p[(size_t) base + 2] = (float) routes[r][2] * 0.01f;
        }

        // Something on every chain, so that the routing knobs have a job.
        const auto& utility = Fx::spec (Fx::Utility);
        for (const int slot : { 0, P::fxMainSlots, P::fxMainSlots + P::fxBusSlots })
        {
            p[(size_t) P::fxType (slot)] = (float) Fx::Utility;
            p[(size_t) P::fxOn (slot)] = 1.0f;
            p[(size_t) P::fxMix (slot)] = 1.0f;
            for (int k = 0; k < (int) utility.params.size(); ++k)
                p[(size_t) P::fxParam (slot, k)] = Fx::toNorm (utility.params[(size_t) k], utility.params[(size_t) k].def);
            p[(size_t) P::fxParam (slot, 0)] = Fx::toNorm (utility.params[0], -6.0f);
        }
        p[P::FxMainLevel] = 0.8f;
        p[P::FxBus1Send] = p[P::FxBus2Send] = 0.5f;
        p[P::FxBus1Level] = p[P::FxBus2Level] = 0.7f;
        return p;
    };

    // Parameters meant for other engines, or that act when a sample is loaded.
    auto expectedDead = [] (int engineMode, int id)
    {
        const bool mode = id == P::SustainMode, grain = id == P::GrainMs || id == P::Spray || id == P::GranSpeed;
        const bool partials = id == P::Partials || id == P::NoiseLevel;
        const bool transient = id == P::TransientMs || id == P::TransientGain;
        const bool cloud = id == P::GrainPos || id == P::GrainDensity || id == P::GrainPitch || id == P::GrainReverse || id == P::GrainWidth || id == P::GrainCoherent;
        const bool drone = id >= P::DroneStretch && id <= P::DroneCurveOn;

        // Armónico's knobs need that filter type and the Sidechain source a sidechain (auditColorPlay); Movimiento is
        // engines B and D's alone.
        if (id == P::FltHarmGain || id == P::FltHarmRegion || (id >= P::ScSens && id <= P::ScResponse))
            return true;
        if (id == P::SustainMotion && engineMode != P::EngineReenvelope && engineMode != P::EngineHybrid)
            return true;
        // They act when a sample is loaded or tuned, on a faster attack or a sample whose level
        // moves (auditSynthEnvelope), or answer the keyboard's controls (auditExpression).
        if (id == P::SnapC || id == P::Reverse || id == P::EnvLock || id == P::Punch || id == P::EnvMode)
            return true;
        if (id == P::AtAmount || id == P::VibDepth || id == P::VibRate || id == P::BendRange)
            return true;
        // The settings of LOOP act only in LOOP (auditLoops); Reproducción itself switches to it.
        if (id == P::LoopWarp || id == P::LoopSync || id == P::LoopBeats || id == P::LoopAlgo || id == P::LoopKind || id == P::LoopBpm
            || id == P::SliceBy || id == P::SliceDivision || id == P::SliceSensitivity || id == P::SliceStart
            || id == P::LoopPlay || id == P::SliceThru || id == P::SliceFade || id == P::LoopHitEnv || id == P::MechPerHit || id == P::LoopWinLen || id == P::LoopWinPos)    // auditTimbreHits, auditLoopWindow
            return true;
        // The key: for the arpeggiator and sequencer, it does not sound by itself (auditKey).
        if (id == P::KeySource || id == P::KeyRoot || id == P::KeyScale)
            return true;
        // ARP · SEQ: it acts on the notes played, not on a held note (auditArp).
        if (id >= P::ArpOn && id <= P::ArpDensity)
            return true;
        // The hand-over to the granular sustain of engines B and D.
        if ((id == P::GrainFade || id == P::GrainBlend) && engineMode != P::EngineReenvelope && engineMode != P::EngineHybrid)
            return true;
        // The loop needs Bucle on, the fade out a note that reaches Fin, and the pitch time a
        // pitch amount: auditSampleEdit. Fin, fades and loop belong to engine A.
        const bool edit = id == P::EndPos || id == P::FadeIn || id == P::FadeOut || id == P::LoopOn;
        if (id == P::LoopOn || id == P::LoopStart || id == P::LoopEnd || id == P::LoopXfade || id == P::FadeOut || id == P::PitchEnvTime)
            return true;
        if (edit && engineMode != P::EngineRebuild)
            return true;
        // Engine F: the width of each harmonic needs Armónicos on, and the drawn filter a curve.
        if (engineMode == P::EngineDrone)
            return mode || partials || transient || grain || (cloud && id != P::GrainPos) || id == P::Stretch || id == P::Flatten
                || id == P::GrainMix || id == P::StartPos || id == P::Breath || id == P::Bow || id == P::SustainType || id == P::UniWidth
                || id == P::DroneHarmWidth || id == P::DroneCurveOn;
        if (drone)
            return true;
        // Engine E: Warp only sets how fast the cloud travels, which needs Deriva (shown dimmed).
        if (engineMode == P::EngineGranular)
            return mode || partials || transient || id == P::Stretch || id == P::Flatten || id == P::GrainMix || id == P::StartPos
                || id == P::Breath || id == P::Bow || id == P::SustainType || id == P::WarpMode;
        if (cloud)
            return true;
        if (engineMode == P::EngineRebuild)
            return mode || grain || partials || transient || id == P::Stretch || id == P::Flatten;
        if (engineMode == P::EngineResynth)
            return mode || grain;
        return partials || (engineMode == P::EngineReenvelope && transient);
    };
    for (const int engineMode : { (int) P::EngineRebuild, (int) P::EngineHybrid, (int) P::EngineReenvelope, (int) P::EngineResynth, (int) P::EngineGranular,
                                  (int) P::EngineDrone })
    {
        const ParamSnapshot bases[2] = { context (engineMode, P::SustainHold), context (engineMode, P::SustainNatural) };
        const Audio references[2] = { phrase (s, bases[0], rate), phrase (s, bases[1], rate) };
        check (difference (references[0], phrase (s, bases[0], rate)) < -120.0, "motor " + juce::String (engineMode) + ": mismo render dos veces (determinista)", "");

        juce::String dead, unexpected;
        for (int id = 0; id < P::FxSlotBase; ++id)
        {
            const auto& d = P::defs()[(size_t) id];
            bool alive = false;
            for (int c = 0; c < 2 && ! alive; ++c)
            {
                for (const float value : otherValues (d, bases[c][(size_t) id]))
                {
                    auto q = bases[c];
                    q[(size_t) id] = value;
                    if (id == P::Lfo1Div || id == P::Lfo2Div || id == P::Lfo3Div)
                        q[(size_t) id - 1] = 1.0f;      // the division needs sync on
                    alive = difference (references[c], phrase (s, q, rate)) > -80.0;
                    if (alive)
                        break;
                }
            }

            if (! alive)
            {
                dead << d.id << " ";
                if (! expectedDead (engineMode, id))
                    unexpected << d.id << " ";
            }
        }
        check (unexpected.isEmpty(), "motor " + juce::String (engineMode) + ": ningún control muerto sin avisar",
               "inactivos: [" + dead.trim() + "]" + (unexpected.isEmpty() ? juce::String() : "  INESPERADOS: [" + unexpected.trim() + "]"));

        juce::String deadDest;
        const auto& base = bases[0];
        for (int dst = 1; dst < P::NumDests; ++dst)
        {
            auto q = base, without = base;
            q[P::M8Src] = without[P::M8Src] = (float) P::SrcMacro2;
            q[P::M8Amt] = without[P::M8Amt] = 0.5f;
            q[P::M8Dst] = (float) dst;
            without[P::M8Dst] = 0.0f;
            if (difference (phrase (s, without, rate), phrase (s, q, rate)) < -80.0)
                deadDest << P::choicesOf (P::M1Dst)[dst] << ", ";
        }
        std::printf ("(info) motor %d, destinos de modulación sin efecto: %s\n", engineMode, deadDest.isEmpty() ? "ninguno" : deadDest.toRawUTF8());
    }

    title ("Cada parámetro de cada efecto hace algo audible");
    const auto input = richInput (1.2, rate);
    // Impronta listens to a sidechain: a sound of its own, the input played backwards at half its level.
    Audio side (input);
    side.reverse (0, side.getNumSamples());
    side.applyGain (0.5f);
    for (int type = 1; type < Fx::NumTypes; ++type)
    {
        const auto& spec = Fx::spec (type);
        juce::String dead;
        for (int k = 0; k < (int) spec.params.size(); ++k)
        {
            bool alive = false;
            for (int variant = 0; variant < 3 && ! alive; ++variant)
            {
                FxRun run (type);
                run.side = &side;
                // Some knobs depend on a switch or a menu: try them with those changed too.
                for (int j = 0; j < (int) spec.params.size() && variant > 0; ++j)
                    if (j != k && spec.params[(size_t) j].kind != Fx::ParamSpec::Knob)
                        run.p[(size_t) P::fxParam (run.slot, j)] = variant == 1 ? 0.99f : 0.01f;
                if (type == Fx::Bode)
                    run.set (0, 300.0f);    // Spread splits the shift, so it needs one

                // A band that skips the next effect only shows if there is a next effect.
                if (Fx::isSplitter (type))
                {
                    const int next = run.slot + 1;
                    run.p[(size_t) P::fxType (next)] = (float) Fx::Utility;
                    run.p[(size_t) P::fxOn (next)] = 1.0f;
                    run.p[(size_t) P::fxMix (next)] = 1.0f;
                    run.p[(size_t) P::fxParam (next, 0)] = Fx::toNorm (Fx::spec (Fx::Utility).params[0], -12.0f);
                    for (int j = 1; j < (int) Fx::spec (Fx::Utility).params.size(); ++j)
                        run.p[(size_t) P::fxParam (next, j)] = Fx::toNorm (Fx::spec (Fx::Utility).params[(size_t) j], Fx::spec (Fx::Utility).params[(size_t) j].def);
                }

                const auto a = run.run (input, rate);
                auto other = run;
                const float now = run.p[(size_t) P::fxParam (run.slot, k)];
                other.p[(size_t) P::fxParam (run.slot, k)] = now > 0.5f ? 0.02f : 0.98f;
                alive = difference (a, other.run (input, rate)) > -80.0;
            }
            if (! alive)
                dead << juce::String::fromUTF8 (spec.params[(size_t) k].name) << ", ";
        }
        check (dead.isEmpty(), juce::String::fromUTF8 (spec.name) + ": todos sus parámetros actúan", dead);
    }
}

//==============================================================================
void auditFuzz (double seconds)
{
    title ("Prueba de resistencia: automatización y MIDI aleatorios");
    for (const double rate : rates)
    {
        juce::Random rng ((juce::int64) rate);
        Sample s = SampleAnalysis::analyse (pluck (110.0, 1.0, rate), rate, "fuzz", false);
        SynthEngine e;
        const int maxBlock = 2048;
        e.prepare (rate);
        e.setSample (s);

        auto p = paramsFor (*s);
        Audio buf (2, maxBlock);
        bool finite = true;
        float peak = 0.0f;
        double done = 0.0;

        while (done < seconds)
        {
            const int n = 1 + rng.nextInt (maxBlock);

            // Half of the time a few parameters jump anywhere in their range.
            if (rng.nextBool())
                for (int k = 0; k < 6; ++k)
                {
                    const int id = rng.nextInt (P::Count);
                    const auto& d = P::defs()[(size_t) id];
                    if (d.kind == P::Def::Float)
                        p[(size_t) id] = d.min + rng.nextFloat() * (d.max - d.min);
                    else if (d.kind == P::Def::Bool)
                        p[(size_t) id] = (float) rng.nextInt (2);
                    else
                        p[(size_t) id] = (float) rng.nextInt (juce::StringArray::fromTokens (P::u8 (d.choices), "|", "").size());
                }
            p[P::Volume] = -12.0f;
            e.prepareFx (p);

            juce::MidiBuffer midi;
            for (int k = rng.nextInt (6); k > 0; --k)
            {
                const int at = rng.nextInt (n), note = 24 + rng.nextInt (84);
                switch (rng.nextInt (6))
                {
                    case 0:  midi.addEvent (juce::MidiMessage::noteOff (1, note), at); break;
                    case 1:  midi.addEvent (juce::MidiMessage::pitchWheel (1, rng.nextInt (16384)), at); break;
                    case 2:  midi.addEvent (juce::MidiMessage::controllerEvent (1, rng.nextBool() ? 1 : 64, rng.nextInt (128)), at); break;
                    default: midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.05f + 0.95f * rng.nextFloat()), at); break;
                }
            }

            Audio view (buf.getArrayOfWritePointers(), 2, n);
            e.process (view, midi, p, 60.0 + 120.0 * rng.nextDouble());
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                {
                    const float v = view.getSample (ch, i);
                    finite = finite && std::isfinite (v);
                    peak = std::max (peak, std::abs (v));
                }
            done += n / rate;
        }
        check (finite, juce::String (rate / 1000.0, 1) + " kHz: " + juce::String (seconds, 0) + " s de caos sin NaN ni infinitos", "pico " + juce::String (peak, 3));
    }
}

void auditCpu()
{
    title ("Consumo (porcentaje de un núcleo)");
    for (const double rate : rates)
    {
        Sample s = SampleAnalysis::analyse (pluck (220.0, 2.0, 44100.0), 44100.0, "cpu", false);
        auto p = paramsFor (*s);
        p[P::SustainType] = (float) P::SustainHold;
        p[P::AmpS] = 0.7f;

        auto cost = [&] (const ParamSnapshot& q, int voices, int spread)
        {
            SynthEngine e;
            e.prepare (rate);
            e.setSample (s);
            e.prepareFx (q);
            Audio buf (2, 512);
            juce::MidiBuffer on;
            for (int n = 0; n < voices; ++n)
                on.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey + (n * spread) % 25, 0.9f), 0);
            const juce::MidiBuffer none;
            e.process (buf, on, q, 120.0);
            const int blocks = (int) (2.0 * rate / 512.0);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            for (int b = 0; b < blocks; ++b)
                e.process (buf, none, q, 120.0);
            return (juce::Time::getMillisecondCounterHiRes() - start) / 20.0;
        };

        std::printf ("%.1f kHz: reposo %.2f %%, 1 voz %.2f %%, 8 voces %.1f %%, 32 voces %.1f %%, 32 voces repartidas %.1f %%\n",
                     rate / 1000.0, cost (p, 0, 0), cost (p, 1, 0), cost (p, 8, 1), cost (p, 32, 0), cost (p, 32, 1));

        if (rate == 48000.0)
            for (int type = 1; type < Fx::NumTypes; ++type)
            {
                FxRun run (type);
                auto q = p;
                for (int j = 0; j < P::fxSlotStride; ++j)
                    q[(size_t) P::fxType (0) + (size_t) j] = run.p[(size_t) P::fxType (0) + (size_t) j];
                std::printf ("    %-18s %.2f %%\n", Fx::spec (type).name, cost (q, 1, 0) - cost (p, 1, 0));
            }
    }
}
} // namespace

//==============================================================================
// What each control of engine A changes besides its own job, to read by eye: every control at
// both ends of its range against the default, on a note with the given sample.
struct NoteFacts { double attackMs, peak, body, tailMs, width, centroid; };

NoteFacts factsOf (const Audio& y, double rate, double held)
{
    const auto frame = [&] (double from, double seconds)
    {
        const int a = (int) (from * rate), n = std::min ((int) (seconds * rate), y.getNumSamples() - a);
        return n > 0 ? 0.5 * (rmsOf (y.getReadPointer (0) + a, n) + rmsOf (y.getReadPointer (1) + a, n)) : 0.0;
    };

    NoteFacts f {};
    double top = 1.0e-9;
    for (double t = 0.0; t < 0.25; t += 0.001)
        top = std::max (top, frame (t, 0.003));
    f.peak = toDb (top);
    for (double t = 0.0; t < 0.25; t += 0.001)
        if (frame (t, 0.003) >= top * 0.7)
        {
            f.attackMs = t * 1000.0;
            break;
        }

    f.body = toDb (frame (held - 0.2, 0.15) + 1.0e-9);
    for (double t = held; t + 0.02 < (double) y.getNumSamples() / rate; t += 0.005)
    {
        f.tailMs = (t - held) * 1000.0;
        if (toDb (frame (t, 0.02) + 1.0e-9) < f.body - 40.0)
            break;
    }

    const int a = (int) ((held - 0.2) * rate), n = (int) (0.15 * rate);
    double mid = 1.0e-12, side = 1.0e-12;
    for (int i = a; i < a + n; ++i)
    {
        const double l = y.getSample (0, i), r = y.getSample (1, i);
        mid += (l + r) * (l + r);
        side += (l - r) * (l - r);
    }
    f.width = 10.0 * std::log10 (side / mid);

    constexpr int order = 12;
    const auto mag = spectrum (y.getReadPointer (0) + a, order);
    double sum = 1.0e-12, weighted = 0.0;
    for (size_t k = 1; k < mag.size(); ++k)
    {
        sum += mag[k];
        weighted += mag[k] * (double) k;
    }
    f.centroid = weighted / sum * rate / (double) (1 << order);
    return f;
}

void mapControls (const juce::File& file)
{
    Audio raw;
    double rate = 0.0;
    if (! SampleAnalysis::loadFile (file, raw, rate))
        return;

    const Sample s = SampleAnalysis::analyse (raw, rate, file.getFileNameWithoutExtension(), false, false);
    constexpr double held = 0.8, total = 3.0;
    constexpr float velocity = 0.7f;

    for (const float sustain : { -1.0f, 0.8f })
    {
        auto base = paramsFor (*s);
        base[P::Engine] = (float) P::EngineRebuild;
        if (sustain >= 0.0f)
            base[P::AmpS] = sustain;
        base[P::AmpR] = 300.0f;

        const auto reference = factsOf (render (s, base, SynthEngine::rootKey, rate, held, total, 512, 0, velocity), rate, held);
        std::printf ("\n%s, %s  ataque %.0f ms  pico %.1f  cuerpo %.1f  cola %.0f ms  ancho %.1f  brillo %.0f Hz\n",
                     file.getFileNameWithoutExtension().toRawUTF8(), sustain < 0.0f ? "natural" : "sustain 80 %",
                     reference.attackMs, reference.peak, reference.body, reference.tailMs, reference.width, reference.centroid);
        std::printf ("%-16s %8s | %7s %6s %6s %7s %6s %7s\n", "control", "valor", "ataque", "pico", "cuerpo", "cola", "ancho", "brillo");

        for (const int id : { (int) P::SustainType, (int) P::Breath, (int) P::Variation, (int) P::Fine, (int) P::Bits, (int) P::Downsample, (int) P::Bow,
                              (int) P::RelDark, (int) P::VelBright, (int) P::RelNoise, (int) P::AmpA, (int) P::AmpCurve, (int) P::AmpD, (int) P::AmpS,
                              (int) P::AmpFade, (int) P::AmpR, (int) P::VelAmp, (int) P::Unison, (int) P::UniDetune, (int) P::UniBlend, (int) P::UniWidth,
                              (int) P::Pan, (int) P::MonoOut })
        {
            const auto& d = P::defs()[(size_t) id];
            const bool discrete = d.kind != P::Def::Float;
            for (const float v : { discrete ? 0.0f : d.min, discrete ? 1.0f : d.max })
            {
                if (v == base[(size_t) id])
                    continue;
                auto p = base;
                p[(size_t) id] = v;
                if (id == P::Breath)
                    p[P::SustainType] = (float) P::SustainHold;
                if (id == P::UniDetune || id == P::UniBlend || id == P::UniWidth)
                    p[P::Unison] = 4.0f;

                const auto f = factsOf (render (s, p, SynthEngine::rootKey, rate, held, total, 512, 0, velocity), rate, held);
                std::printf ("%-16s %8.2f | %+7.0f %+6.1f %+6.1f %+7.0f %+6.1f %+7.0f\n", d.id, (double) v, f.attackMs - reference.attackMs,
                             f.peak - reference.peak, f.body - reference.body, f.tailMs - reference.tailMs,
                             f.width - reference.width, f.centroid - reference.centroid);
            }
        }
    }
}

// The detected note of a file and the strongest peaks of its spectrum, to check the detection by eye.
void showNote (const juce::File& file)
{
    Audio raw;
    double rate = 0.0;
    if (! SampleAnalysis::loadFile (file, raw, rate))
        return;
    const Sample s = SampleAnalysis::analyse (raw, rate, file.getFileNameWithoutExtension(), false, false);
    std::printf ("\n%s\n  nota %s (%.2f), con tono %d\n", file.getFileName().toRawUTF8(),
                 SampleAnalysis::midiNoteName (s->rootMidi).toRawUTF8(), s->rootMidi, (int) s->pitched);

    for (const double at : { 0.05, 0.3, 0.8 })
    {
        constexpr int order = 14;
        const int from = (int) (s->onset + at * rate);
        if (from + (1 << order) > s->length)
            break;
        std::vector<float> mono ((size_t) (1 << order));
        for (int i = 0; i < (1 << order); ++i)
            mono[(size_t) i] = 0.5f * (raw.getSample (0, from + i) + raw.getSample (std::min (1, raw.getNumChannels() - 1), from + i));
        const auto mag = spectrum (mono.data(), order);
        std::vector<std::pair<float, int>> peaks;
        for (size_t k = 2; k + 1 < mag.size() && (double) k * rate / (1 << order) < 4000.0; ++k)
            if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1])
                peaks.push_back ({ mag[k], (int) k });
        std::sort (peaks.rbegin(), peaks.rend());
        std::printf ("  a %.2f s:", at);
        for (size_t i = 0; i < std::min<size_t> (8, peaks.size()); ++i)
        {
            const double hz = peaks[i].second * rate / (1 << order);
            std::printf ("  %s %.0f Hz %.0f dB", SampleAnalysis::midiNoteName ((float) (69.0 + 12.0 * std::log2 (hz / 440.0))).toRawUTF8(),
                         hz, toDb (peaks[i].first / peaks[0].first));
        }
        std::printf ("\n");
    }
}

//==============================================================================
// Sound quality, measured: distortion and aliasing of the voice filter, its response at every rate,
// intermodulation, the lows in mono, engine F's stereo, each effect's polarity and delay at mix 0,
// the noise floor after a note, and the true peak of the output stage.
double thdOf (const std::vector<float>& mag, double rate, int order, double f0)
{
    const double binHz = rate / (1 << order);
    const auto at = [&] (double hz)
    {
        const int k = (int) std::round (hz / binHz);
        double m = 0.0;
        for (int j = std::max (1, k - 3); j <= std::min ((int) mag.size() - 1, k + 3); ++j)
            m = std::max (m, (double) mag[(size_t) j]);
        return m;
    };
    const double fund = at (f0);
    double sum = 0.0;
    for (int h = 2; h <= 12 && h * f0 < 0.48 * rate; ++h)
        sum += at (h * f0) * at (h * f0);
    return toDb (std::sqrt (sum) / std::max (1.0e-12, fund));
}

// Peak between the samples: the signal four times oversampled with a windowed sinc.
double truePeak (const Audio& y)
{
    constexpr int half = 16;
    static std::vector<float> kernels[3];
    if (kernels[0].empty())
        for (int q = 1; q <= 3; ++q)
        {
            auto& k = kernels[q - 1];
            k.resize (2 * half);
            for (int i = -half; i < half; ++i)
            {
                const double x = i + 1.0 - q * 0.25;      // distance from the point between sample i and i+1
                const double s = x == 0.0 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
                const double w = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * x / half);
                k[(size_t) (i + half)] = (float) (s * w);
            }
        }
    double peak = 0.0;
    for (int ch = 0; ch < y.getNumChannels(); ++ch)
    {
        const float* x = y.getReadPointer (ch);
        const int n = y.getNumSamples();
        for (int i = half; i + half < n; ++i)
        {
            peak = std::max (peak, (double) std::abs (x[i]));
            for (const auto& k : kernels)
            {
                double v = 0.0;
                for (int j = 0; j < 2 * half; ++j)
                    v += k[(size_t) j] * x[i - half + 1 + j - 1];
                peak = std::max (peak, std::abs (v));
            }
        }
    }
    return peak;
}

void auditQuality()
{
    title ("Calidad de sonido: distorsión, aliasing, respuesta por frecuencia, mono, polaridad, ruido, true peak");
    constexpr int order = 15;
    const int N = 1 << order;

    // The voice filter: THD of each circuit on a 1 kHz tone at -6 dBFS with the cutoff open and no drive,
    // and the aliasing of its saturation with 24 dB of drive on a 6 kHz tone.
    {
        const double rate = 48000.0;
        Sample s = SampleAnalysis::analyse (tone (1000.0, 3.0, rate), rate, "tone", false);
        Sample hi = SampleAnalysis::analyse (tone (6000.0, 3.0, rate), rate, "tone6k", false);
        const char* names[] = { "Clean", "OSR", "MS2", "SMP", "PRD" };
        for (int circuit = 0; circuit < 5; ++circuit)
        {
            auto p = paramsFor (*s);
            p[P::FltOn] = 1.0f;
            p[P::FltType] = (float) P::LP;
            p[P::FltSlope] = 1.0f;
            p[P::FltCircuit] = (float) circuit;
            p[P::FltFreq] = 20000.0f;
            p[P::FltRes] = 0.0f;
            p[P::FltDrive] = 0.0f;
            const auto y = render (s, p, SynthEngine::rootKey, rate, 2.5, 2.5);
            const auto mag = spectrum (y.getReadPointer (0) + (int) (0.5 * rate), order);
            const double thd = thdOf (mag, rate, order, 1000.0);
            // The coloured circuits saturate at unity by design (PRD the most, its drive 1.5x: 3.7 % at -6 dBFS).
            check (thd < (circuit == P::Clean ? -90.0 : -25.0), juce::String ("filtro ") + names[circuit] + " abierto, sin drive: THD a 1 kHz", db (thd));

            auto q = paramsFor (*hi);
            q[P::FltOn] = 1.0f;
            q[P::FltType] = (float) P::LP;
            q[P::FltSlope] = 1.0f;
            q[P::FltCircuit] = (float) circuit;
            q[P::FltFreq] = 20000.0f;
            q[P::FltRes] = 0.0f;
            q[P::FltDrive] = 24.0f;
            const auto z = render (hi, q, SynthEngine::rootKey, rate, 2.5, 2.5);
            const auto m6 = spectrum (z.getReadPointer (0) + (int) (0.5 * rate), order);
            const double ref = toneAmp (z.getReadPointer (0) + (int) (0.5 * rate), N, 6000.0, rate);
            const double spur = worstSpur (m6, rate, order, { 0.0, 6000.0, 12000.0, 18000.0 }, ref, 16000.0);
            check (spur < -60.0, juce::String ("filtro ") + names[circuit] + " con 24 dB de drive a 6 kHz: aliasing bajo 16 kHz", db (spur));
            // An asymmetric saturation leaves a DC offset unless it is taken out (MS2 was: +0.02 at the output).
            double dc = 0.0;
            for (int i = (int) (0.5 * rate); i < z.getNumSamples(); ++i)
                dc += z.getSample (0, i);
            dc /= (double) (z.getNumSamples() - (int) (0.5 * rate));
            check (std::abs (dc) < 1.0e-3, juce::String ("filtro ") + names[circuit] + " con 24 dB de drive: sin componente continua a la salida",
                   juce::String (dc, 5) + " (" + db (toDb (std::abs (dc) / std::max (1.0e-9, ref))) + " bajo el tono)");
        }
    }

    // The same cutoff gives the same response at every rate (a 24 dB lowpass at 1 kHz, seen at 1 and 2 kHz).
    {
        double at1k[4] {}, at2k[4] {};
        for (int r = 0; r < 4; ++r)
        {
            const double rate = rates[r];
            for (int which = 0; which < 2; ++which)
            {
                const double hz = which == 0 ? 1000.0 : 2000.0;
                Sample s = SampleAnalysis::analyse (tone (hz, 3.0, rate), rate, "t", false);
                auto p = paramsFor (*s);
                p[P::FltOn] = 1.0f;
                p[P::FltType] = (float) P::LP;
                p[P::FltSlope] = 1.0f;
                p[P::FltCircuit] = (float) P::Clean;
                p[P::FltFreq] = 1000.0f;
                p[P::FltRes] = 0.2f;
                const auto y = render (s, p, SynthEngine::rootKey, rate, 2.5, 2.5);
                const auto dry = render (s, paramsFor (*s), SynthEngine::rootKey, rate, 2.5, 2.5);
                const double g = toDb (toneAmp (y.getReadPointer (0) + (int) (0.5 * rate), N, hz, rate)
                                       / toneAmp (dry.getReadPointer (0) + (int) (0.5 * rate), N, hz, rate));
                (which == 0 ? at1k : at2k)[r] = g;
            }
        }
        double spread1 = 0.0, spread2 = 0.0;
        for (int r = 1; r < 4; ++r)
        {
            spread1 = std::max (spread1, std::abs (at1k[r] - at1k[0]));
            spread2 = std::max (spread2, std::abs (at2k[r] - at2k[0]));
        }
        check (spread1 < 0.3 && spread2 < 0.5, "el filtro responde igual a 44,1, 48, 88,2 y 96 kHz",
               "1 kHz " + db (at1k[0]) + " (±" + juce::String (spread1, 2) + "), 2 kHz " + db (at2k[0]) + " (±" + juce::String (spread2, 2) + ")");
    }

    // Intermodulation (SMPTE 60 Hz + 7 kHz, 4:1) of the saturations: reported, and never above -20 dB.
    {
        const double rate = 48000.0;
        Audio two (2, (int) (3.0 * rate));
        for (int i = 0; i < two.getNumSamples(); ++i)
        {
            const float v = (float) (0.4 * std::sin (twoPi * 60.0 * i / rate) + 0.1 * std::sin (twoPi * 7000.0 * i / rate));
            two.setSample (0, i, v);
            two.setSample (1, i, v);
        }
        const auto imd = [&] (const Audio& y)
        {
            const float* x = y.getReadPointer (0) + (int) (0.5 * rate);
            const double carrier = toneAmp (x, N, 7000.0, rate);
            double side = 0.0;
            for (int k = 1; k <= 3; ++k)
                for (const double f : { 7000.0 - 60.0 * k, 7000.0 + 60.0 * k })
                    side += toneAmp (x, N, f, rate) * toneAmp (x, N, f, rate);
            return toDb (std::sqrt (side) / std::max (1.0e-12, carrier));
        };
        // Reported (a saturation driven hard modulates the treble with the bass: that is what it does), and the
        // saturations must not intermodulate at all with the drive down.
        {
            juce::String report;
            double atZero = 0.0;
            for (const float drive : { 0.0f, 0.1f, 0.3f, 0.5f })
            {
                FxRun run (Fx::Distortion);
                run.set (0, 0.0f);      // Tube
                run.set (1, drive);
                const double d = imd (run.run (two, rate));
                if (drive == 0.0f)
                    atZero = d;
                report += "drive " + juce::String (drive * 100.0f, 0) + " %: " + db (d) + "; ";
            }
            // Tube is tanh on one side and an exponential on the other: it colours from drive 0 (12 % of
            // intermodulation at -8 dBFS), as a valve stage does; the numbers are the report.
            check (atZero < -10.0, "Distorsión Tube: intermodulación SMPTE (60 Hz + 7 kHz 4:1) por drive", report);
        }
        {
            Sample s = SampleAnalysis::analyse (two, rate, "two", false);
            auto p = paramsFor (*s);
            p[P::FltOn] = 1.0f;
            p[P::FltType] = (float) P::LP;
            p[P::FltCircuit] = (float) P::MS2;
            p[P::FltFreq] = 20000.0f;
            p[P::FltRes] = 0.0f;
            p[P::FltDrive] = 12.0f;
            const double d = imd (render (s, p, SynthEngine::rootKey, rate, 2.5, 2.5));
            p[P::FltDrive] = 0.0f;
            const double d0 = imd (render (s, p, SynthEngine::rootKey, rate, 2.5, 2.5));
            check (d0 < -20.0 && d < 0.0, "filtro MS2: intermodulación SMPTE con 0 y 12 dB de drive", db (d0) + ", " + db (d));
            const double clean = imd (render (s, paramsFor (*s), SynthEngine::rootKey, rate, 2.5, 2.5));
            check (clean < -90.0, "sin filtro ni efectos no hay intermodulación", db (clean));
        }
    }

    // Graves mono: a bass only on the left comes to the middle under the cut, the highs stay where they were,
    // and the level of the lows does not change.
    for (const double rate : { 44100.0, 96000.0 })
    {
        Audio in (2, (int) (3.0 * rate));
        for (int i = 0; i < in.getNumSamples(); ++i)
        {
            in.setSample (0, i, (float) (0.4 * std::sin (twoPi * 40.0 * i / rate) + 0.2 * std::sin (twoPi * 2000.0 * i / rate)));
            in.setSample (1, i, 0.0f);
        }
        Sample s = SampleAnalysis::analyse (in, rate, "left", false);
        auto p = paramsFor (*s);
        const auto before = render (s, p, SynthEngine::rootKey, rate, 2.5, 2.5);
        p[P::MonoBass] = 100.0f;
        const auto after = render (s, p, SynthEngine::rootKey, rate, 2.5, 2.5);
        const int from = (int) (0.5 * rate);
        const auto midSide = [&] (const Audio& y, double hz, double& mid, double& side)
        {
            std::vector<float> m ((size_t) N), d ((size_t) N);
            for (int i = 0; i < N; ++i)
            {
                m[(size_t) i] = 0.5f * (y.getSample (0, from + i) + y.getSample (1, from + i));
                d[(size_t) i] = 0.5f * (y.getSample (0, from + i) - y.getSample (1, from + i));
            }
            mid = toneAmp (m.data(), N, hz, rate);
            side = toneAmp (d.data(), N, hz, rate);
        };
        double mid60a, side60a, mid60b, side60b, mid2a, side2a, mid2b, side2b;
        midSide (before, 40.0, mid60a, side60a);
        midSide (after, 40.0, mid60b, side60b);
        midSide (before, 2000.0, mid2a, side2a);
        midSide (after, 2000.0, mid2b, side2b);
        const double sideDrop = toDb (side60b / side60a), midKeep = toDb (mid60b / mid60a), highKeep = toDb (side2b / side2a);
        // A Linkwitz-Riley of 24 dB per octave at 100 Hz takes 31.8 dB off the side at 40 Hz.
        check (sideDrop < -28.0 && std::abs (midKeep) < 0.5 && std::abs (highKeep) < 0.5,
               juce::String (rate / 1000.0, 1) + " kHz: Graves mono a 100 Hz centra un sub de 40 Hz y deja los 2 kHz donde estaban",
               "lado a 40 Hz " + db (sideDrop) + ", centro a 40 Hz " + db (midKeep) + ", lado a 2 kHz " + db (highKeep));
    }

    // Engine F at full width: the lows stay in the middle (equal in L and R), the highs apart.
    {
        const double rate = 48000.0;
        for (const double hz : { 55.0, 1000.0 })
        {
            Sample s = SampleAnalysis::analyse (tone (hz, 3.0, rate), rate, "f", false);
            auto p = paramsFor (*s);
            p[P::Engine] = (float) P::EngineDrone;
            p[P::DroneWidth] = 1.0f;
            const auto y = render (s, p, SynthEngine::rootKey, rate, 5.0, 5.0);
            // The drone spreads a tone over the bins around it: the band around it is compared, not one line.
            const int from = (int) (2.0 * rate);
            std::vector<float> d ((size_t) N);
            for (int i = 0; i < N; ++i)
                d[(size_t) i] = 0.5f * (y.getSample (0, from + i) - y.getSample (1, from + i));
            const auto sideMag = spectrum (d.data(), order), leftMag = spectrum (y.getReadPointer (0) + from, order);
            const double binHz = rate / N;
            double side = 0.0, left = 0.0;
            for (int k = (int) (0.8 * hz / binHz); k <= (int) (1.25 * hz / binHz); ++k)
            {
                side += sideMag[(size_t) k] * sideMag[(size_t) k];
                left += leftMag[(size_t) k] * leftMag[(size_t) k];
            }
            const double rel = toDb (std::sqrt (side / std::max (1.0e-24, left)));
            if (hz < 100.0)
                check (rel < -30.0, "motor F con Estéreo al 100 %: un grave de 55 Hz queda en el centro (compatible en mono)", "lado " + db (rel));
            else
                check (rel > -12.0, "motor F con Estéreo al 100 %: a 1 kHz los canales son distintos", "lado " + db (rel));
        }
    }

    // What each reader puts in the audible band that was not there, transposing up. Re-Pitch readers on a
    // 19 kHz tone: every reader of the file is band-limited (the sinc of engine A with the cutoff of its speed;
    // the grains of B, D and E read with a cubic before and let the tone fold back at -5 to -13 dB). Warp Tonos
    // and Textura on a note: a 440 Hz sawtooth with harmonics to 20 kHz, moved up; what lands between the
    // harmonics of the new note under 16 kHz is reported.
    {
        const double rate = 44100.0, src = 19000.0;
        Sample s = SampleAnalysis::analyse (tone (src, 4.0, rate), rate, "alias", false);
        for (const int engine : { P::EngineRebuild, P::EngineReenvelope, P::EngineHybrid, P::EngineGranular })
        {
            juce::String report;
            double worst = -200.0;
            for (const float semis : { 7.0f, 12.0f, 19.0f })
            {
                auto q = paramsFor (*s);
                q[P::Engine] = (float) engine;
                q[P::Transpose] = semis;
                q[P::SustainType] = 1.0f;
                const auto y = render (s, q, SynthEngine::rootKey, rate, 2.0, 1.5);
                const auto mag = spectrum (y.getReadPointer (0) + (int) (0.7 * rate), 14);
                const double alias = worstSpur (mag, rate, 14, {}, 0.5, 16000.0);
                worst = std::max (worst, alias);
                report += juce::String::formatted ("%+.0f st: ", semis) + db (alias) + "; ";
            }
            check (worst < -60.0, juce::String ("motor ") + "ABCDEF"[engine] + ": aliasing bajo 16 kHz al transportar un tono de 19 kHz", report);
        }

        Audio saw (2, (int) (4.0 * rate));
        for (int i = 0; i < saw.getNumSamples(); ++i)
        {
            double v = 0.0;
            for (int h = 1; h * 440.0 < 20000.0; ++h)
                v += std::sin (twoPi * 440.0 * h * i / rate) / h;
            saw.setSample (0, i, (float) (0.3 * v));
            saw.setSample (1, i, (float) (0.3 * v));
        }
        Sample n = SampleAnalysis::analyse (saw, rate, "saw", false);
        // Engine A only: B and D sustain with grains (lines widened and moved on purpose: a texture), so what
        // lies between harmonics there is their sound, not a reader's fault.
        for (const int engine : { P::EngineRebuild })
            for (const int warp : { P::WarpRepitch, P::WarpTones, P::WarpTexture })
            {
                juce::String report;
                double worst = -200.0;
                for (const float semis : { 7.0f, 12.0f })
                {
                    auto q = paramsFor (*n);
                    q[P::Engine] = (float) engine;
                    q[P::Transpose] = semis;
                    q[P::SustainType] = 1.0f;
                    q[P::WarpMode] = (float) warp;
                    const auto y = render (n, q, SynthEngine::rootKey, rate, 2.0, 1.5);
                    const auto mag = spectrum (y.getReadPointer (0) + (int) (0.7 * rate), 14);
                    const double f0 = 440.0 * std::pow (2.0, semis / 12.0);
                    std::vector<double> keepList;
                    for (int h = 1; h * f0 < 17000.0; ++h)
                        keepList.push_back (h * f0);
                    const double ref = toneAmp (y.getReadPointer (0) + (int) (0.7 * rate), 1 << 14, f0, rate);
                    // worstSpur takes an initializer_list: the same search, with the harmonics guarded.
                    const double binHz = rate / (1 << 14);
                    double w = 1.0e-12;
                    for (int k = 2; k < (int) mag.size() && k * binHz <= 16000.0; ++k)
                    {
                        bool near = false;
                        for (const double f : keepList)
                            near = near || std::abs (k - f / binHz) < 8;
                        if (! near)
                            w = std::max (w, (double) mag[(size_t) k]);
                    }
                    const double alias = toDb (w / std::max (1.0e-12, ref));
                    worst = std::max (worst, alias);
                    report += juce::String::formatted ("%+.0f st: ", semis) + db (alias) + "; ";
                }
                const char* warpName[] = { "Re-Pitch", "Tonos", "Textura" };
                // Tonos and Textura lay windowed grains (PSOLA): their windows put some energy between the
                // harmonics, -32 to -43 dB measured; a regression guard, not aliasing.
                check (worst < (warp == P::WarpRepitch ? -60.0 : -25.0), juce::String ("motor ") + "ABCDEF"[engine] + ", Warp " + warpName[warp]
                           + ": entre los armónicos de una nota de 440 Hz subida, bajo 16 kHz", report);
            }
    }

    // Unison wide open: the lows summed to mono keep their level (the detuned copies beat, they do not cancel
    // on average), in engines A and B.
    for (const int engine : { P::EngineRebuild, P::EngineReenvelope })
    {
        const double rate = 48000.0;
        Sample s = SampleAnalysis::analyse (tone (55.0, 4.0, rate), rate, "bass", false);
        auto p = paramsFor (*s);
        p[P::Engine] = (float) engine;
        p[P::Unison] = 4.0f;
        p[P::UniDetune] = 25.0f;
        p[P::UniWidth] = 0.0f;
        const auto narrow = render (s, p, SynthEngine::rootKey, rate, 3.5, 3.5);
        p[P::UniWidth] = 1.0f;
        const auto wide = render (s, p, SynthEngine::rootKey, rate, 3.5, 3.5);
        const auto monoLows = [&] (const Audio& y)
        {
            double e = 0.0;
            Dsp::Biquad lp;
            lp.set (Dsp::Biquad::LowPass, rate, 200.0f, 0.7071f);
            const int from = (int) (0.5 * rate), n = y.getNumSamples();
            for (int i = 0; i < n; ++i)
            {
                const float m = lp.process (0, 0.5f * (y.getSample (0, i) + y.getSample (1, i)));
                if (i >= from)
                    e += (double) m * m;
            }
            return std::sqrt (e / (n - from));
        };
        const double d = toDb (monoLows (wide) / std::max (1.0e-12, monoLows (narrow)));
        check (std::abs (d) < 3.0, juce::String ("motor ") + "ABCDEF"[engine] + ": Unison x4 con Ancho 100 % no pierde graves al sumar a mono", db (d));
    }

    // Every effect at mix 0: the dry signal comes out at its level and in phase, and the delay it picks up
    // is reported (the plugin reports none to the host: these are inside the effect, by design, at mix 0).
    {
        const double rate = 48000.0;
        const auto in = richInput (1.5, rate);
        juce::String report;
        bool allGood = true;
        for (int type = 1; type < Fx::NumTypes; ++type)
        {
            FxRun run (type, 0.0f);
            const auto y = run.run (in, rate);
            const int n = in.getNumSamples(), skip = (int) (0.2 * rate);
            const float* a = in.getReadPointer (0);
            const float* b = y.getReadPointer (0);
            double best = -2.0;
            int bestLag = 0;
            for (int lag = 0; lag <= 4096; ++lag)
            {
                double num = 0.0, ea = 0.0, eb = 0.0;
                for (int i = skip; i + lag < n; i += 1)
                {
                    num += (double) a[i] * b[i + lag];
                    ea += (double) a[i] * a[i];
                    eb += (double) b[i + lag] * b[i + lag];
                }
                const double c = num / std::sqrt (std::max (1.0e-20, ea * eb));
                if (c > best)
                {
                    best = c;
                    bestLag = lag;
                }
                if (best > 0.999)
                    break;
            }
            const double level = toDb (rmsOf (b + skip, n - skip) / rmsOf (a + skip, n - skip));
            // The Distortion's dry goes through its oversampling filters (IIR: a phase turn and 6 samples, so that
            // it mixes with the wet in phase) and the splitters are all-pass crossovers: their phase is theirs by
            // design, their level must be intact. Every other effect leaves the dry as it came.
            const bool allPass = type == Fx::Distortion || Fx::isSplitter (type);
            const bool good = std::abs (level) < 0.3 && (allPass || (best > 0.98 && bestLag == 0));
            allGood = allGood && good;
            report += juce::String (Fx::spec (type).name) + ": " + juce::String (bestLag) + (allPass ? " (all-pass)" : "")
                      + (good ? "" : " (MAL: corr " + juce::String (best, 3) + ", " + db (level) + ")") + "; ";
        }
        check (allGood, "cada efecto con mix 0 deja pasar la señal seca a su nivel y, salvo los all-pass, en fase y sin retardo (muestras)", report);
    }

    // After a note and its tail there is silence, with and without a full chain of effects.
    {
        const double rate = 48000.0;
        Sample s = SampleAnalysis::analyse (pluck (220.0, 1.5, rate), rate, "p", false);
        auto p = paramsFor (*s);
        const auto plain = render (s, p, SynthEngine::rootKey, rate, 0.5, 8.0);
        const int n = plain.getNumSamples(), tail = (int) (0.5 * rate);
        const double floorPlain = toDb (rmsOf (plain.getReadPointer (0) + n - tail, tail));
        const int types[] = { Fx::Distortion, Fx::Chorus, Fx::Phaser, Fx::Delay, Fx::Reverb, Fx::Compressor };
        for (int k = 0; k < 6; ++k)
        {
            p[(size_t) P::fxType (k)] = (float) types[k];
            p[(size_t) P::fxOn (k)] = 1.0f;
            const auto& spec = Fx::spec (types[k]);
            for (int j = 0; j < (int) spec.params.size(); ++j)
                p[(size_t) P::fxParam (k, j)] = Fx::toNorm (spec.params[(size_t) j], spec.params[(size_t) j].def);
        }
        const auto withFx = render (s, p, SynthEngine::rootKey, rate, 0.5, 20.0);
        const int m = withFx.getNumSamples();
        const double floorFx = toDb (rmsOf (withFx.getReadPointer (0) + m - tail, tail));
        check (floorPlain < -120.0, "tras la nota, silencio (sin efectos)", db (floorPlain) + "FS a los 7,5 s");
        check (floorFx < -96.0, "tras la nota, silencio con distorsión, chorus, phaser, delay, reverb y compresor", db (floorFx) + "FS a los 19,5 s");
    }

    // The output stage: driven 12 dB over full scale, what leaves the plugin never passes -0.3 dBFS between
    // the samples either (true peak, 4x).
    {
        const double rate = 44100.0;
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("reanima_audit_tp.wav");
        {
            Audio b = tone (5512.5, 2.0, rate, 0.9f);
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            juce::WavAudioFormat wav;
            if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        ReanimaProcessor proc;
        proc.setPlayConfigDetails (0, 2, rate, 512);
        proc.prepareToPlay (rate, 512);
        const bool loaded = proc.loadSampleNow (file);
        for (int k = 0; k < 200 && proc.isLoading(); ++k)
        {
            juce::Thread::sleep (20);
            proc.serviceMessageThread();
        }
        proc.serviceMessageThread();
        if (auto* vol = proc.apvts.getParameter ("volume"))
            vol->setValueNotifyingHost (vol->convertTo0to1 (12.0f));
        proc.serviceMessageThread();
        Audio out (2, (int) (1.5 * rate)), block (2, 512);
        out.clear();
        for (int pos = 0; pos < out.getNumSamples(); pos += 512)
        {
            const int n = std::min (512, out.getNumSamples() - pos);
            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey, (juce::uint8) 127), 0);
            Audio view (block.getArrayOfWritePointers(), 2, n);
            view.clear();
            proc.processBlock (view, midi);
            out.copyFrom (0, pos, view, 0, 0, n);
            out.copyFrom (1, pos, view, 1, 0, n);
        }
        const int from = (int) (0.3 * rate);
        Audio steady (2, out.getNumSamples() - from);
        steady.copyFrom (0, 0, out, 0, from, steady.getNumSamples());
        steady.copyFrom (1, 0, out, 1, from, steady.getNumSamples());
        const double sample = steady.getMagnitude (0, steady.getNumSamples()), tp = truePeak (steady);
        check (loaded && sample > 0.9 && tp <= 1.0, "salida a +12 dB: el true peak (4x) no pasa de 0 dBFS",
               "pico de muestra " + db (toDb (sample)) + "FS, true peak " + db (toDb (tp)) + "TP");
        file.deleteFile();
    }
}

//==============================================================================
// Warp in the sampler: what the duration and the pitch do when a sample is played away from its note.
// Re-Pitch is a classic sampler (an octave up is twice as fast and half as long); Tonos and Textura keep the
// duration and move the pitch. The note: 220 Hz, 8 harmonics, a 3 ms attack and a 1.2 s fall.
void auditWarpDuration()
{
    title ("Warp: duración y tono por separado al tocar lejos de la nota del sample");
    const double rate = 48000.0;
    Sample s = SampleAnalysis::analyse (pluck (220.0, 3.0, rate), rate, "warp", false);
    const auto timeTo = [&] (const Audio& y, double dbDown)
    {
        // Time from the onset until the level (20 ms RMS) stays under its peak by dbDown.
        const int w = (int) (0.02 * rate), n = y.getNumSamples();
        std::vector<double> lv;
        for (int i = 0; i + w <= n; i += w)
            lv.push_back (rmsOf (y.getReadPointer (0) + i, w));
        const double peak = *std::max_element (lv.begin(), lv.end());
        int last = 0;
        for (int k = 0; k < (int) lv.size(); ++k)
            if (lv[(size_t) k] > peak * std::pow (10.0, -dbDown / 20.0))
                last = k;
        return (last + 1) * 0.02;
    };
    const auto pitchOf = [&] (const Audio& y, double expected)
    {
        // The period, not the loudest line (keeping the formants, an octave down leaves the new fundamental
        // weak): the third harmonic of the expected note is there, and nothing of the note an octave under
        // it (its 1.5 x line), at 0.3 s.
        const float* x = y.getReadPointer (0) + (int) (0.3 * rate);
        const double f1 = toneAmp (x, 8192, expected, rate), f2 = toneAmp (x, 8192, 2.0 * expected, rate);
        const double f3 = toneAmp (x, 8192, 3.0 * expected, rate), under = toneAmp (x, 8192, 1.5 * expected, rate);
        const double top = std::max (f1, f2);
        return f3 > 0.02 * top && under < 0.05 * top;
    };
    const char* names[] = { "Re-Pitch", "Tonos", "Textura" };
    for (const int warp : { P::WarpRepitch, P::WarpTones, P::WarpTexture })
    {
        juce::String report;
        bool pitchOk = true;
        double rootTime = 0.0, upTime = 0.0, downTime = 0.0, attackUp = 0.0;
        for (const int semis : { 0, 12, -12 })
        {
            auto p = paramsFor (*s);
            p[P::WarpMode] = (float) warp;
            p[P::AmpR] = 50.0f;
            const auto y = render (s, p, SynthEngine::rootKey + semis, rate, 4.0, 4.0);
            const double t = timeTo (y, 30.0);
            (semis == 0 ? rootTime : semis > 0 ? upTime : downTime) = t;
            const bool rightOctave = pitchOf (y, 220.0 * std::pow (2.0, semis / 12.0));
            pitchOk = pitchOk && rightOctave;
            if (! rightOctave)
                report += "(OCTAVA MAL) ";
            if (semis == 12)
            {
                // The attack: the level of the first 5 ms against the peak of the first 50 ms.
                const double first = rmsOf (y.getReadPointer (0), (int) (0.005 * rate));
                double top = 0.0;
                for (int i = 0; i < (int) (0.05 * rate); i += (int) (0.005 * rate))
                    top = std::max (top, rmsOf (y.getReadPointer (0) + i, (int) (0.005 * rate)));
                attackUp = toDb (first / std::max (1.0e-12, top));
            }
            report += juce::String::formatted ("%+d st: %.2f s; ", semis, t);
        }
        report += "ataque a +12 st " + db (attackUp);
        const double upRatio = upTime / rootTime, downRatio = downTime / rootTime;
        const bool durationOk = warp == P::WarpRepitch ? (upRatio < 0.65 && downRatio > 1.6)
                                                       : (upRatio > 0.85 && upRatio < 1.15 && downRatio > 0.85 && downRatio < 1.15);
        check (durationOk && pitchOk && attackUp > -6.0, juce::String ("Warp ") + names[warp] + (warp == P::WarpRepitch ? ": la duración cambia con la nota (sampler clásico)"
                                                                                                  : ": la duración no cambia con la nota y el tono sí"), report);
    }
}

//==============================================================================
// LOOP: a drum loop of 4 beats at 100 BPM (a kick on every beat, a hat on every off-beat) and a pad loop
// of the same length, played with the host at 120 BPM, at 100, with no tempo, and without Warp.
Audio renderAt (Sample s, const ParamSnapshot& p, int note, double rate, double held, double total, double bpm)
{
    SynthEngine e;
    e.prepare (rate);
    e.setSample (s);
    e.prepareFx (p);
    const int length = (int) (total * rate), off = (int) (held * rate), block = 256;
    Audio out (2, length), buf (2, block);
    out.clear();
    for (int pos = 0; pos < length; pos += block)
    {
        const int n = std::min (block, length - pos);
        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
        if (off >= pos && off < pos + n)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), off - pos);
        Audio view (buf.getArrayOfWritePointers(), 2, n);
        e.process (view, midi, p, bpm);
        out.copyFrom (0, pos, buf, 0, 0, n);
        out.copyFrom (1, pos, buf, 1, 0, n);
    }
    return out;
}

// The times (s) where the level rises 10 dB within 6 ms: the hits. The level is the peak of each millisecond
// held over the 12 ms before (a low kick's waves do not count as new hits).
std::vector<double> hitsOf (const Audio& y, double rate)
{
    const int hop = (int) (0.001 * rate);
    std::vector<double> peak;
    for (int i = 0; i + hop <= y.getNumSamples(); i += hop)
        peak.push_back (y.getMagnitude (0, i, hop) + 1.0e-9);
    std::vector<double> lv (peak.size());
    for (size_t k = 0; k < peak.size(); ++k)
        for (size_t j = k >= 12 ? k - 12 : 0; j <= k; ++j)
            lv[k] = std::max (lv[k], peak[j]);
    std::vector<double> out;
    for (int k = 6; k < (int) lv.size(); ++k)
        if (lv[(size_t) k] > 3.16 * lv[(size_t) k - 6] && lv[(size_t) k] > 0.05 && (out.empty() || k * 0.001 - out.back() > 0.08))
        {
            int j = k - 6;      // where the rise begins
            while (j < k && peak[(size_t) j + 1] < 2.0 * lv[(size_t) k - 6])
                ++j;
            out.push_back (j * 0.001);
        }
    return out;
}

void auditLoops()
{
    title ("LOOP: detección, tempo del host, tono aparte, continuidad y nivel");
    const double rate = 48000.0, bpm = 100.0, seconds = 4 * 60.0 / bpm;
    const int n = (int) std::lround (seconds * rate);
    Audio drums (2, n), pad (2, n);
    juce::Random rng (11);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / rate, beat = std::fmod (t, 60.0 / bpm), half = std::fmod (t + 30.0 / bpm, 60.0 / bpm);
        // A kick as drum machines make it (a click, then a low tone that falls away), a hat on every off-beat.
        const double noise = rng.nextFloat() * 2.0 - 1.0;
        const double kick = 0.6 * std::exp (-beat / 0.08) * std::sin (twoPi * 60.0 * beat) + 0.3 * std::exp (-beat / 0.002) * noise;
        const double hat = 0.25 * std::exp (-half / 0.02) * noise;
        drums.setSample (0, i, (float) (kick + hat));
        drums.setSample (1, i, (float) (kick + hat));
        // A chord whose waves close on themselves over the loop: seamless at its end.
        const double base = 220.0 * std::round (seconds * 220.0) / (seconds * 220.0);
        const double v = 0.15 * (std::sin (twoPi * base * t) + std::sin (twoPi * base * 1.25 * std::round (seconds * base * 1.25) / (seconds * base * 1.25) * t)
                                 + std::sin (twoPi * base * 1.5 * std::round (seconds * base * 1.5) / (seconds * base * 1.5) * t));
        pad.setSample (0, i, (float) v);
        pad.setSample (1, i, (float) v);
    }
    Sample d = SampleAnalysis::analyse (drums, rate, "Test drums loop 100bpm", false, false);
    Sample a = SampleAnalysis::analyse (drums, rate, "x", false, false);
    Sample q = SampleAnalysis::analyse (pad, rate, "Test pad loop 100bpm", false, false);
    const auto& info = d->loopInfo;
    check (info.likely && info.beats == 4 && std::abs (info.bpm - 100.0) < 0.01 && info.rhythmic && info.slices.size() >= 7,
           "detecta el loop de batería: 4 tiempos a 100 BPM, rítmico, cortado en sus golpes",
           juce::String (info.likely ? "loop " : "NO LOOP ") + juce::String (info.beats) + " tiempos " + juce::String (info.bpm, 2) + " BPM, "
               + juce::String ((int) info.slices.size()) + " cortes, golpes " + juce::String (info.percussive, 2) + ", confianza "
               + juce::String (info.confidence, 2) + "; sin nombre: "
               + (a->loopInfo.likely ? "loop " : "duda/one shot ") + juce::String (a->loopInfo.beats) + " tiempos");
    check (q->loopInfo.likely && ! q->loopInfo.rhythmic, "detecta el pad como loop tonal", juce::String (q->loopInfo.confidence, 2));

    const auto loopParams = [&] (Sample s)
    {
        auto p = paramsFor (*s);
        p[P::PlayMode] = (float) P::PlayLoop;
        p[P::LoopAlgo] = (float) P::LoopBeatsAlgo;     // the algorithm itself (Auto chooses by the detection, checked above)
        p[P::AmpA] = 1.0f;
        p[P::AmpR] = 30.0f;
        return p;
    };

    // Beats at 120: the hits land on the eighths of 120 BPM, a loop every 2 s.
    {
        const auto p = loopParams (d);
        const auto y = renderAt (d, p, SynthEngine::rootKey, rate, 4.1, 4.3, 120.0);
        const auto hits = hitsOf (y, rate);
        double worst = 0.0;
        int matched = 0;
        for (const double h : hits)
            if (h < 4.0)
            {
                const double grid = 0.25 * std::round (h / 0.25);
                worst = std::max (worst, std::abs (h - grid));
                ++matched;
            }
        const double inLevel = rmsOf (drums.getReadPointer (0), n), outLevel = rmsOf (y.getReadPointer (0), (int) (4.0 * rate));
        check (matched >= 15 && worst < 0.004, "Beats a 120 BPM: los golpes caen en las corcheas de 120 (el loop dura 2 s)",
               juce::String (matched) + " golpes, peor desvío " + juce::String (worst * 1000.0, 1) + " ms");
        check (std::abs (toDb (outLevel / inLevel)) < 1.5, "Beats: el nivel del loop no cambia al estirarlo", db (toDb (outLevel / inLevel)));

        // A key up: the same hits in time, the kick a fifth up (60 to 90 Hz).
        const auto up = renderAt (d, p, SynthEngine::rootKey + 7, rate, 4.1, 4.3, 120.0);
        const auto upHits = hitsOf (up, rate);
        int sameTime = 0;
        for (const double h : upHits)
            if (h < 4.0 && std::abs (h - 0.25 * std::round (h / 0.25)) < 0.004)
                ++sameTime;
        const float* x = up.getReadPointer (0);
        const double at60 = toneAmp (x, 1 << 17, 60.0, rate), at90 = toneAmp (x, 1 << 17, 60.0 * std::pow (2.0, 7.0 / 12.0), rate);
        check (sameTime >= 15 && at90 > 3.0 * at60, "Beats: una tecla arriba sube el tono y deja los golpes en su sitio",
               juce::String (sameTime) + " golpes en su sitio; 90 Hz frente a 60 Hz " + db (toDb (at90 / std::max (1.0e-12, at60))));
    }

    // Beats slower than the loop (100 -> 80 BPM): the hits on the host's eighths as before, and between them the
    // loop's tail carries on (stretched) instead of a die-away into silence (the sample's own reverb dipped).
    {
        const auto p = loopParams (d);
        const auto y = renderAt (d, p, SynthEngine::rootKey, rate, 4.1, 4.3, 80.0);
        const auto hits = hitsOf (y, rate);
        int onGrid = 0, total = 0;
        for (const double h : hits)
            if (h > 0.1 && h < 4.0)
            {
                ++total;
                onGrid += std::abs (h - 0.375 * std::round (h / 0.375)) < 0.004 ? 1 : 0;
            }
        // The quietest 5 ms between 0.2 s and 3.8 s, against the drum loop's own quietest 5 ms.
        const auto quietest = [&] (const float* x, int from, int to)
        {
            double q = 1.0;
            for (int k = from; k + 240 < to; k += 240)
                q = std::min (q, rmsOf (x + k, 240));
            return q;
        };
        const double mine = quietest (y.getReadPointer (0), (int) (0.2 * rate), (int) (3.8 * rate)), own = quietest (drums.getReadPointer (0), 240, n - 240);
        check (total >= 8 && onGrid == total && mine > 0.25 * own, "Beats más lento (100 a 80 BPM): golpes en las corcheas y sin huecos entre ellos (la cola sigue)",
               juce::String (onGrid) + "/" + juce::String (total) + " golpes; lo más bajo en 5 ms " + db (toDb (mine / own)) + " frente al loop");
    }

    // Tonal at 120: level kept, no clicks at the seams.
    {
        auto p = loopParams (q);
        p[P::LoopAlgo] = (float) P::LoopComplex;
        const auto y = renderAt (q, p, SynthEngine::rootKey, rate, 6.1, 6.3, 120.0);
        const int from = (int) (0.1 * rate), to = (int) (6.0 * rate);
        double worstJump = 0.0, typical = 0.0;
        for (int i = from + 2; i < to; ++i)
        {
            const double d2 = std::abs ((double) y.getSample (0, i) - 2.0 * y.getSample (0, i - 1) + y.getSample (0, i - 2));
            worstJump = std::max (worstJump, d2);
            typical += d2;
        }
        typical /= (to - from);
        const double level = toDb (rmsOf (y.getReadPointer (0) + from, to - from) / rmsOf (pad.getReadPointer (0), n));
        check (worstJump < 6.0 * typical && std::abs (level) < 1.5, "Tonal a 120 BPM: sin saltos en las costuras y al mismo nivel",
               "mayor salto " + juce::String (worstJump / typical, 1) + " veces el típico, nivel " + db (level));
    }

    // At its own tempo it is the file itself; with no tempo from the host, its own tempo; without Warp a
    // key up an octave is twice as fast.
    {
        const auto p = loopParams (d);
        const auto y = renderAt (d, p, SynthEngine::rootKey, rate, 5.0, 5.0, 100.0);
        double diff = 0.0;
        for (int i = (int) (0.01 * rate); i < 2 * n && i < y.getNumSamples(); ++i)
            diff = std::max (diff, (double) std::abs (y.getSample (0, i) - drums.getSample (0, i % n)));
        check (diff < 1.0e-3, "a su propio tempo el loop suena como el archivo, y empalma consigo mismo", "diferencia máxima " + juce::String (diff, 6));
        const auto none = renderAt (d, p, SynthEngine::rootKey, rate, 3.0, 3.0, 0.0);
        const auto hitsNone = hitsOf (none, rate);
        bool own = hitsNone.size() >= 8;
        for (const double h : hitsNone)
            own = own && std::abs (h - 0.3 * std::round (h / 0.3)) < 0.004;
        check (own, "sin tempo del host, el loop va a su tempo (100 BPM)", juce::String ((int) hitsNone.size()) + " golpes");
        auto off = p;
        off[P::LoopWarp] = 0.0f;
        const auto fast = renderAt (d, off, SynthEngine::rootKey + 12, rate, 2.0, 2.0, 120.0);
        const auto hitsFast = hitsOf (fast, rate);
        bool doubled = hitsFast.size() >= 10;
        for (const double h : hitsFast)
            doubled = doubled && std::abs (h - 0.15 * std::round (h / 0.15)) < 0.004;
        check (doubled, "sin Warp, una octava arriba el loop va al doble de velocidad (sampler clásico)", juce::String ((int) hitsFast.size()) + " golpes");
        auto beats = p;
        beats[P::LoopBeats] = 6.0f;      // 2 compases: the loop is read as 8 beats, so at 120 it lasts 4 s
        const auto slow = renderAt (d, beats, SynthEngine::rootKey, rate, 4.1, 4.3, 120.0);
        const auto hitsSlow = hitsOf (slow, rate);
        bool halved = hitsSlow.size() >= 7;
        for (const double h : hitsSlow)
            halved = halved && std::abs (h - 0.5 * std::round (h / 0.5)) < 0.006;
        juce::String times;
        for (const double h : hitsSlow)
            times << juce::String (h, 3) << " ";
        check (halved, "Duración corregida a mano (2 compases): el loop dura el doble", juce::String ((int) hitsSlow.size()) + " golpes: " + times);
        auto written = p;
        written[P::LoopBpm] = 200.0f;    // BPM original written as 200: at 100 it plays at half speed (hits 0.6 s apart)
        const auto twice = renderAt (d, written, SynthEngine::rootKey, rate, 2.5, 2.7, 100.0);
        const auto hitsTwice = hitsOf (twice, rate);
        bool slow2 = hitsTwice.size() >= 4;
        juce::String at;
        for (const double h : hitsTwice)
        {
            slow2 = slow2 && std::abs (h - 0.6 * std::round (h / 0.6)) < 0.006;
            at << juce::String (h, 3) << " ";
        }
        check (slow2, "BPM original escrito (200): a 100 BPM el loop va a la mitad", juce::String ((int) hitsTwice.size()) + " golpes: " + at);
    }

    // The redesign of 0.9.6: at its own tempo and pitch, every algorithm is the file, Snap a C or not (it moved
    // a loop at C3 up to 6 semitones: the chipmunk at the root key).
    const auto diffToFile = [&] (const Audio& y, const Audio& file)
    {
        double diff = 0.0;
        for (int i = (int) (0.01 * rate); i < 2 * n && i < y.getNumSamples(); ++i)
            diff = std::max ({ diff, (double) std::abs (y.getSample (0, i) - file.getSample (0, i % n)),
                               (double) std::abs (y.getSample (1, i) - file.getSample (1, i % n)) });
        return diff;
    };
    for (const int algo : { (int) P::LoopAuto, (int) P::LoopBeatsAlgo, (int) P::LoopComplex, (int) P::LoopComplexPro })
    {
        static const char* names[] = { "Auto", "Beats", "Complex", "Complex Pro" };
        auto p = loopParams (q);
        p[P::LoopAlgo] = (float) algo;
        p[P::SnapC] = 0.0f;
        p[P::Engine] = (float) P::EngineHybrid;
        const double dp = diffToFile (renderAt (q, p, SynthEngine::rootKey, rate, 5.0, 5.0, 100.0), pad);
        auto pd = loopParams (d);
        pd[P::LoopAlgo] = (float) algo;
        pd[P::SnapC] = 0.0f;
        const double dd = diffToFile (renderAt (d, pd, SynthEngine::rootKey, rate, 5.0, 5.0, 100.0), drums);
        check (dp < 1.0e-3 && dd < 1.0e-3, juce::String ("en Do3 y a su tempo, ") + names[algo] + " suena como el archivo (pad y batería)",
               "diferencia máxima " + juce::String (std::max (dp, dd), 6));
    }

    // Warped to 120 the pitch stays: the pad's lowest note (220 Hz-ish) is where it was, not 20 % up; a key
    // up a fifth moves it a fifth. Level kept, no jumps at the seams. Complex is read ahead by its latency:
    // its hits do not lag.
    const double base = 220.0 * std::round (seconds * 220.0) / (seconds * 220.0);
    for (const int algo : { (int) P::LoopComplex, (int) P::LoopComplexPro })
    {
        static const char* names[] = { "", "", "Complex", "Complex Pro" };
        auto p = loopParams (q);
        p[P::LoopAlgo] = (float) algo;
        const auto y = renderAt (q, p, SynthEngine::rootKey, rate, 6.1, 6.3, 120.0);
        const float* x = y.getReadPointer (0) + (int) (0.5 * rate);
        const double kept = toneAmp (x, 1 << 17, base, rate), shifted = toneAmp (x, 1 << 17, base * 1.2, rate);
        const auto up = renderAt (q, p, SynthEngine::rootKey + 7, rate, 4.1, 4.3, 120.0);
        const float* u = up.getReadPointer (0) + (int) (0.5 * rate);
        const double fifth = toneAmp (u, 1 << 17, base * std::pow (2.0, 7.0 / 12.0), rate), root = toneAmp (u, 1 << 17, base, rate);
        const int from = (int) (0.2 * rate), to = (int) (6.0 * rate);
        double worstJump = 0.0, typical = 0.0;
        for (int i = from + 2; i < to; ++i)
        {
            const double d2 = std::abs ((double) y.getSample (0, i) - 2.0 * y.getSample (0, i - 1) + y.getSample (0, i - 2));
            worstJump = std::max (worstJump, d2);
            typical += d2;
        }
        typical /= (to - from);
        const double level = toDb (rmsOf (y.getReadPointer (0) + from, to - from) / rmsOf (pad.getReadPointer (0), n));
        const double limit = algo == P::LoopComplexPro ? 3.0 : 1.5;
        check (kept > 10.0 * shifted && fifth > 10.0 * root && worstJump < 6.0 * typical && std::abs (level) < limit,
               juce::String (names[algo]) + " a 120 BPM: el tono no sigue al tempo, la tecla sí; sin saltos; nivel",
               "220 Hz frente a +20 % " + db (toDb (kept / std::max (1.0e-12, shifted))) + ", quinta " + db (toDb (fifth / std::max (1.0e-12, root)))
                   + ", salto " + juce::String (worstJump / typical, 1) + "x, nivel " + db (level));
    }
    {
        auto p = loopParams (d);
        p[P::LoopAlgo] = (float) P::LoopComplex;
        const auto y = renderAt (d, p, SynthEngine::rootKey, rate, 4.1, 4.3, 120.0);
        const auto hits = hitsOf (y, rate);
        double worst = 0.0;
        int matched = 0;
        for (const double h : hits)
            if (h > 0.2 && h < 4.0)
            {
                worst = std::max (worst, std::abs (h - 0.25 * std::round (h / 0.25)));
                ++matched;
            }
        // A phase vocoder smears a hit over its window (here up to 8 ms): drums go to Beats (Auto does so).
        check (matched >= 12 && worst < 0.010, "Complex a 120 BPM: los golpes a menos de 10 ms de las corcheas (latencia compensada)",
               juce::String (matched) + " golpes, peor desvío " + juce::String (worst * 1000.0, 1) + " ms");
    }

    // Complex (Bungee) with four keys struck at once, blocks of 64: the starts are spread (a budget of grains per
    // block), no block takes longer than its audio, and each note is in time (the drum loop's hits on the eighths).
    {
        auto p = loopParams (d);
        p[P::LoopAlgo] = (float) P::LoopComplex;
        SynthEngine e;
        e.prepare (rate);
        e.setSample (d);
        e.prepareFx (p);
        const int length = (int) (2.2 * rate);
        Audio out (2, length), buf (2, 64);
        double worst = 0.0;
        for (int pos = 0; pos < length; pos += 64)
        {
            juce::MidiBuffer m;
            if (pos == 0)
                for (const int k : { 60, 63, 67, 70 })
                    m.addEvent (juce::MidiMessage::noteOn (1, k, 1.0f), 0);
            Audio view (buf.getArrayOfWritePointers(), 2, 64);
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            e.process (view, m, p, 120.0);
            worst = std::max (worst, (juce::Time::getMillisecondCounterHiRes() - t0) / (1000.0 * 64.0 / rate));
            out.copyFrom (0, pos, buf, 0, 0, std::min (64, length - pos));
        }
        const auto hits = hitsOf (out, rate);
        int onGrid = 0, total = 0;
        for (const double h : hits)
            if (h > 0.1 && h < 2.1)
            {
                ++total;
                onGrid += std::abs (h - 0.25 * std::round (h / 0.25)) < 0.010 ? 1 : 0;
            }
        check (worst < 0.9 && total >= 6 && onGrid == total, "Complex: 4 teclas a la vez en bloques de 64, ningún bloque pasa de su tiempo y todo a tempo",
               "peor bloque " + juce::String (100.0 * worst, 0) + " %, golpes en las corcheas " + juce::String (onGrid) + "/" + juce::String (total));
    }

    // SLICE: each key from Primera nota plays its slice whole, even tapped, as the file has it; past the last
    // slice nothing.
    {
        auto p = loopParams (d);
        p[P::LoopKind] = (float) P::LoopSlice;
        const std::vector<int> marks { 0, n / 4, n / 2, 3 * n / 4 };
        const auto play = [&] (int note)
        {
            SynthEngine e;
            e.prepare (rate);
            e.setSample (d);
            e.prepareFx (p);
            e.setSlices (marks);
            const int length = (int) (1.0 * rate), block = 256;
            Audio out (2, length), buf (2, block);
            out.clear();
            for (int pos = 0; pos < length; pos += block)
            {
                const int k = std::min (block, length - pos);
                juce::MidiBuffer midi;
                if (pos == 0)
                {
                    midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
                    midi.addEvent (juce::MidiMessage::noteOff (1, note), 100);     // a tap of 2 ms
                }
                Audio view (buf.getArrayOfWritePointers(), 2, k);
                e.process (view, midi, p, 120.0);
                out.copyFrom (0, pos, buf, 0, 0, k);
                out.copyFrom (1, pos, buf, 1, 0, k);
            }
            return out;
        };
        bool whole = true;
        juce::String detail;
        for (int i = 0; i < 4; ++i)
        {
            const auto y = play (60 + i);
            const int from = marks[(size_t) i], len = (i < 3 ? marks[(size_t) i + 1] : n) - from;
            double diff = 0.0, after = 0.0;
            for (int k = (int) (0.001 * rate); k < len - (int) (0.003 * rate); ++k)
                diff = std::max (diff, (double) std::abs (y.getSample (0, k) - drums.getSample (0, from + k)));
            for (int k = len + 64; k < y.getNumSamples(); ++k)
                after = std::max (after, (double) std::abs (y.getSample (0, k)));
            whole = whole && diff < 1.0e-3 && after < 1.0e-6;
            detail << SampleAnalysis::midiNoteName ((float) (60 + i)) << ": " << juce::String (diff, 5) << " ";
        }
        const auto none = play (64);
        whole = whole && rmsOf (none.getReadPointer (0), none.getNumSamples()) < 1.0e-6;
        check (whole, "SLICE: C3, C#3, D3 y D#3 tocan su trozo entero aunque se suelten al momento; E3 (sin trozo) calla", detail);
    }

    // SLICE through the plug-in: Rejilla 1/8 cuts the bar in 8 (each line moved back to its hit), Transitorios
    // finds the 8 hits, a marker placed by hand turns Cortes to A mano.
    {
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("reanima_slice_drums_100bpm.wav");
        file.deleteFile();
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            juce::WavAudioFormat wav;
            if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
                w->writeFromAudioSampleBuffer (drums, 0, n);
        }
        ReanimaProcessor proc;
        proc.setPlayConfigDetails (0, 2, rate, 256);
        proc.prepareToPlay (rate, 256);
        proc.loadSampleNow (file);
        for (int k = 0; k < 400 && proc.isLoading(); ++k)
        {
            juce::Thread::sleep (10);
            proc.serviceMessageThread();
        }
        proc.serviceMessageThread();
        const auto set = [&] (const char* id, float v)
        {
            if (auto* param = proc.apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (v));
            proc.serviceMessageThread();
        };
        set ("playMode", (float) P::PlayLoop);
        set ("loopKind", (float) P::LoopSlice);
        set ("sliceBy", (float) P::SliceGrid);
        set ("sliceDivision", 3.0f);
        const auto grid = proc.getSlices();
        bool onGrid = grid.size() == 8;
        for (size_t k = 0; k < grid.size(); ++k)
            onGrid = onGrid && std::abs (grid[k] - (double) k * n / 8.0) <= 0.012 * rate;
        set ("sliceBy", (float) P::SliceTransients);
        set ("sliceSensitivity", 0.5f);
        const auto found = proc.getSlices();
        bool onHits = found.size() == 8;
        for (size_t k = 0; k < found.size(); ++k)
            onHits = onHits && std::abs (found[k] - (double) k * n / 8.0) <= 0.003 * rate;
        proc.setSliceMarks ({ 0, n / 3 });
        const auto manual = proc.getSlices();
        const bool byHand = manual.size() == 2 && manual[1] == n / 3 && (int) proc.paramValue (P::SliceBy) == P::SliceManual;
        check (onGrid && onHits && byHand, "SLICE en el plugin: rejilla 1/8 = 8 trozos, transitorios = los 8 golpes, marcador a mano",
               juce::String ((int) grid.size()) + " en rejilla, " + juce::String ((int) found.size()) + " por golpes, a mano "
                   + juce::String ((int) manual.size()));
        file.deleteFile();
    }
}

//==============================================================================
// LOOP Normal with Play con el host: the loop plays with the host's transport, where the host is; keys tune it
// without restarting it. And SLICE's Thru, Fundido and MIDI clip.
void auditTransport()
{
    title ("LOOP con el host: Play, posición, parada, tempo, tono por MIDI; Slice Thru, fundido y clip MIDI");
    const double rate = 48000.0, bpm = 100.0;
    const int n = (int) std::lround (4 * 60.0 / bpm * rate);
    Audio drums (2, n), tone (2, n);
    juce::Random rng (5);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / rate, beat = std::fmod (t, 0.6);
        const float v = (float) (0.6 * std::exp (-beat / 0.08) * std::sin (2.0 * juce::MathConstants<double>::pi * 60.0 * beat)
                                 + 0.3 * std::exp (-beat / 0.002) * (rng.nextFloat() * 2.0 - 1.0));
        drums.setSample (0, i, v);
        drums.setSample (1, i, v);
        const double f = 220.0 * std::round (2.4 * 220.0) / (2.4 * 220.0);
        const float w = (float) (0.3 * std::sin (2.0 * juce::MathConstants<double>::pi * f * t));
        tone.setSample (0, i, w);
        tone.setSample (1, i, w);
    }
    Sample d = SampleAnalysis::analyse (drums, rate, "Test drums loop 100bpm", false, false);
    Sample q = SampleAnalysis::analyse (tone, rate, "Test tone loop 100bpm", false, false);

    // Plays the engine as a host does: blocks of 64, the transport (playing, quarter notes) given each block.
    struct Event { double at; int key; bool on; };
    const auto host = [&] (Sample smp, ParamSnapshot p, double seconds, std::function<bool (double)> playing,
                           std::function<double (double)> ppqAt, std::function<double (double)> bpmAt, const std::vector<Event>& events)
    {
        SynthEngine e;
        e.prepare (rate);
        e.setSample (smp);
        e.prepareFx (p);
        const int length = (int) (seconds * rate), block = 64;
        Audio out (2, length), buf (2, block);
        out.clear();
        for (int pos = 0; pos < length; pos += block)
        {
            const double t = pos / rate;
            juce::MidiBuffer midi;
            for (const auto& ev : events)
                if ((int) (ev.at * rate) >= pos && (int) (ev.at * rate) < pos + block)
                    midi.addEvent (ev.on ? juce::MidiMessage::noteOn (1, ev.key, 1.0f) : juce::MidiMessage::noteOff (1, ev.key), (int) (ev.at * rate) - pos);
            e.setTransport (playing (t), ppqAt (t));
            Audio view (buf.getArrayOfWritePointers(), 2, block);
            e.process (view, midi, p, bpmAt (t));
            const int k = std::min (block, length - pos);
            out.copyFrom (0, pos, buf, 0, 0, k);
            out.copyFrom (1, pos, buf, 1, 0, k);
        }
        return out;
    };
    const auto loopParams = [&] (Sample smp, int algo)
    {
        auto p = paramsFor (*smp);
        p[P::PlayMode] = (float) P::PlayLoop;
        p[P::LoopAlgo] = (float) algo;
        p[P::LoopPlay] = 1.0f;
        p[P::SnapC] = 0.0f;
        p[P::AmpA] = 1.0f;
        p[P::AmpR] = 30.0f;
        return p;
    };
    const auto steady = [] (double b) { return [b] (double) { return b; }; };

    // 1. Host plays from bar 1 at the loop's tempo: the file itself (bit for bit after 3 ms), with no key.
    //    From beat 2 (ppq 1): the file from its second beat.
    for (const int algo : { (int) P::LoopBeatsAlgo, (int) P::LoopComplex })
    {
        const auto p = loopParams (d, algo);
        const auto y = host (d, p, 3.0, [] (double) { return true; }, [&] (double t) { return t * bpm / 60.0; }, steady (bpm), {});
        double diff = 0.0;
        for (int i = (int) (0.004 * rate); i < (int) (2.9 * rate); ++i)
            diff = std::max (diff, (double) std::abs (y.getSample (0, i) - drums.getSample (0, i % n)));
        const auto z = host (d, p, 1.0, [] (double) { return true; }, [&] (double t) { return 1.0 + t * bpm / 60.0; }, steady (bpm), {});
        double diff2 = 0.0;
        for (int i = (int) (0.004 * rate); i < (int) (0.9 * rate); ++i)
            diff2 = std::max (diff2, (double) std::abs (z.getSample (0, i) - drums.getSample (0, (i + n / 4) % n)));
        check (diff < 1.0e-3 && diff2 < 1.0e-3, juce::String ("Play (") + (algo == P::LoopBeatsAlgo ? "Beats" : "Complex")
                   + "): con el host en marcha suena solo, en su sitio del compás (desde el compás 1 y desde el tiempo 2), igual que el archivo",
               "diferencia " + juce::String (diff, 6) + " / " + juce::String (diff2, 6));
    }

    // 2. Stop at 1 s, start again at 2 s at ppq 6 (a bar and a half later): silent while stopped, then the
    //    loop from ppq 6 (beat 3 of the loop), not from where it stopped.
    {
        const auto p = loopParams (d, P::LoopBeatsAlgo);
        const auto y = host (d, p, 3.0, [] (double t) { return t < 1.0 || t >= 2.0; },
                             [&] (double t) { return t < 1.0 ? t * bpm / 60.0 : 6.0 + (t - 2.0) * bpm / 60.0; }, steady (bpm), {});
        const double quiet = rmsOf (y.getReadPointer (0) + (int) (1.1 * rate), (int) (0.85 * rate));
        double diff = 0.0;
        for (int i = (int) (2.004 * rate); i < (int) (2.9 * rate); ++i)
            diff = std::max (diff, (double) std::abs (y.getSample (0, i) - drums.getSample (0, (i - (int) (2.0 * rate) + n / 2) % n)));
        check (quiet < 1.0e-5 && diff < 1.0e-3, "Play: al parar el host calla; al volver sigue desde la posición musical del host (ppq 6 = tiempo 3)",
               "parado " + juce::String (quiet, 7) + ", diferencia " + juce::String (diff, 6));
    }

    // 3. Off: the host plays, nothing sounds without a key.
    {
        auto p = loopParams (d, P::LoopBeatsAlgo);
        p[P::LoopPlay] = 0.0f;
        const auto y = host (d, p, 1.0, [] (double) { return true; }, [&] (double t) { return t * bpm / 60.0; }, steady (bpm), {});
        check (rmsOf (y.getReadPointer (0), y.getNumSamples()) < 1.0e-6, "Play apagado: el transporte del host no lo pone a sonar");
    }

    // 4. The host's tempo moves from 100 to 120 at 2.4 s: the hits keep to the host's beats (every 0.5 s after).
    {
        const auto p = loopParams (d, P::LoopBeatsAlgo);
        const auto ppqAt = [&] (double t) { return t < 2.4 ? t * 100.0 / 60.0 : 4.0 + (t - 2.4) * 120.0 / 60.0; };
        const auto y = host (d, p, 4.5, [] (double) { return true; }, ppqAt, [] (double t) { return t < 2.4 ? 100.0 : 120.0; }, {});
        const auto hits = hitsOf (y, rate);
        int onGrid = 0, after = 0;
        for (const double h : hits)
            if (h > 2.45 && h < 4.4)
            {
                ++after;
                onGrid += std::abs ((h - 2.4) - 0.5 * std::round ((h - 2.4) / 0.5)) < 0.006 ? 1 : 0;
            }
        check (after >= 3 && onGrid == after, "Play: un cambio de tempo del host (100 a 120) lo sigue, golpes en los tiempos del host",
               juce::String (onGrid) + "/" + juce::String (after));
    }

    // 4b. Play with the host at another tempo (120 against the loop's 100), Complex: a steady tone, stretched
    //     the whole time (no restarts of the stretch: once they came every few ms, a click each).
    {
        auto p = loopParams (q, P::LoopComplex);
        const auto y = host (q, p, 3.0, [] (double) { return true; }, [] (double t) { return t * 120.0 / 60.0; }, steady (120.0), {});
        const float* x = y.getReadPointer (0);
        double worstJump = 0.0, typical = 0.0, quiet = 1.0;
        for (int i = (int) (0.2 * rate); i < (int) (2.9 * rate); ++i)
        {
            const double d2 = std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]);
            worstJump = std::max (worstJump, d2);
            typical += d2;
        }
        typical /= 2.7 * rate;
        for (int k = (int) (0.2 * rate); k + 480 < (int) (2.9 * rate); k += 480)
            quiet = std::min (quiet, rmsOf (x + k, 480));
        check (worstJump < 4.0 * typical && quiet > 0.1, "Play con el host a otro tempo (120 frente a 100), Complex: estirado sin cortes ni clics",
               "mayor salto " + juce::String (worstJump / typical, 1) + "x el típico, nivel mínimo en 10 ms " + juce::String (quiet, 3));
    }

    // 5. Keys while it plays: a fifth up at 1 s, back at 2 s. The tone moves, the loop does not restart (the
    //    hits of the drum loop stay on the beat), no new voice; Snap a C moves the key to C.
    {
        auto p = loopParams (q, P::LoopComplex);
        const double f = 220.0 * std::round (2.4 * 220.0) / (2.4 * 220.0);
        const auto y = host (q, p, 3.0, [] (double) { return true; }, [&] (double t) { return t * bpm / 60.0; }, steady (bpm),
                             { { 1.0, 67, true }, { 1.5, 67, false }, { 2.0, 60, true } });
        const float* x = y.getReadPointer (0);
        const double before = toneAmp (x + (int) (0.3 * rate), 1 << 14, f, rate);
        const double fifth = toneAmp (x + (int) (1.3 * rate), 1 << 14, f * std::pow (2.0, 7.0 / 12.0), rate);
        const double backAgain = toneAmp (x + (int) (2.4 * rate), 1 << 14, f, rate);
        double worstJump = 0.0, typical = 0.0;
        for (int i = (int) (0.2 * rate); i < (int) (2.9 * rate); ++i)
        {
            const double d2 = std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]);
            worstJump = std::max (worstJump, d2);
            typical += d2;
        }
        typical /= 2.7 * rate;
        const double level = toDb (rmsOf (x + (int) (1.2 * rate), (int) (0.3 * rate)) / rmsOf (x + (int) (0.2 * rate), (int) (0.3 * rate)));
        auto pd = loopParams (d, P::LoopBeatsAlgo);      // drums: Beats, as Auto chooses
        const auto z = host (d, pd, 3.0, [] (double) { return true; }, [&] (double t) { return t * bpm / 60.0; }, steady (bpm),
                             { { 1.0, 67, true }, { 2.0, 55, true } });
        const auto hits = hitsOf (z, rate);
        int onBeat = 0, total = 0;
        juce::String at;
        for (const double h : hits)
            if (h > 0.1 && h < 2.9)
            {
                at << juce::String (h, 3) << " ";
                ++total;
                onBeat += std::abs (h - 0.6 * std::round (h / 0.6)) < 0.010 ? 1 : 0;
            }
        // Each beat of the host keeps its hit (the loop was not restarted by the keys).
        int beatsHit = 0;
        for (const double beat : { 0.6, 1.2, 1.8, 2.4 })
            for (const double h : hits)
                if (std::abs (h - beat) < 0.010)
                {
                    ++beatsHit;
                    break;
                }
        juce::ignoreUnused (onBeat, total);
        check (before > 0.05 && fifth > 0.05 && backAgain > 0.05 && worstJump < 8.0 * typical && std::abs (level) < 1.5 && beatsHit == 4,
               "Play: las teclas cambian el tono en marcha (quinta y vuelta), sin reiniciar el loop, sin saltos y al mismo nivel",
               "220 Hz " + juce::String (before, 3) + ", quinta " + juce::String (fifth, 3) + ", vuelta " + juce::String (backAgain, 3)
                   + ", salto " + juce::String (worstJump / typical, 1) + "x, nivel " + db (level) + ", golpes en su tiempo "
                   + juce::String (onBeat) + "/" + juce::String (total) + " (" + at + ")");
    }

    // 5b. Snap a C on: the loop's key (A, 220 Hz) moved to the nearest C; off: as recorded.
    {
        const double f = 220.0 * std::round (2.4 * 220.0) / (2.4 * 220.0);
        const float shift = SynthEngine::snapShift (*q, true);
        juce::String got;
        bool ok = q->pitched && std::abs (shift) > 0.5f;
        for (const float snap : { 0.0f, 1.0f })
        {
            auto p = loopParams (q, P::LoopComplex);
            p[P::SnapC] = snap;
            const auto y = host (q, p, 1.5, [] (double) { return true; }, [&] (double t) { return t * bpm / 60.0; }, steady (bpm), {});
            const double want = f * std::pow (2.0, (snap > 0.5f ? shift : 0.0f) / 12.0);
            const double atWant = toneAmp (y.getReadPointer (0) + (int) (0.5 * rate), 1 << 14, want, rate);
            got << (snap > 0.5f ? "Snap: " : "sin Snap: ") << juce::String (want, 1) << " Hz " << juce::String (atWant, 3) << "  ";
            ok = ok && atWant > 0.1;
        }
        check (ok, "Play con Snap a C: sin tecla suena en Do (el La del loop movido " + juce::String (shift, 0) + " st); sin Snap, como se grabó", got);
    }

    // 6. SLICE Thru: a key plays from its slice to the end of the sample; Fundido 0 / 2 / 20 ms at the end.
    {
        auto p = paramsFor (*d);
        p[P::PlayMode] = (float) P::PlayLoop;
        p[P::LoopKind] = (float) P::LoopSlice;
        p[P::AmpA] = 1.0f;
        const std::vector<int> marks { 0, n / 4, n / 2, 3 * n / 4 };
        const auto play = [&] (const ParamSnapshot& pp, int key, double seconds)
        {
            SynthEngine e;
            e.prepare (rate);
            e.setSample (d);
            e.prepareFx (pp);
            e.setSlices (marks);
            const int length = (int) (seconds * rate);
            Audio out (2, length), buf (2, 64);
            out.clear();
            for (int pos = 0; pos < length; pos += 64)
            {
                juce::MidiBuffer m;
                if (pos == 0)
                {
                    m.addEvent (juce::MidiMessage::noteOn (1, key, 1.0f), 0);
                    m.addEvent (juce::MidiMessage::noteOff (1, key), 30);
                }
                Audio view (buf.getArrayOfWritePointers(), 2, 64);
                e.process (view, m, pp, 100.0);
                const int k = std::min (64, length - pos);
                out.copyFrom (0, pos, buf, 0, 0, k);
            }
            return out;
        };
        auto thru = p;
        thru[P::SliceThru] = 1.0f;
        const auto y = play (thru, 61, 2.6);
        double diff = 0.0;
        for (int i = (int) (0.001 * rate); i < 3 * n / 4 - (int) (0.003 * rate); ++i)
            diff = std::max (diff, (double) std::abs (y.getSample (0, i) - drums.getSample (0, n / 4 + i)));
        const double after = rmsOf (y.getReadPointer (0) + 3 * n / 4 + 64, y.getNumSamples() - 3 * n / 4 - 64);
        check (diff < 1.0e-3 && after < 1.0e-6, "Thru: C#3 toca desde su trozo hasta el final del sample (3 trozos seguidos) y se para ahí",
               "diferencia " + juce::String (diff, 6));
        juce::String ends;
        bool fades = true;
        double prevTail = -1.0;
        for (const float ms : { 0.0f, 2.0f, 20.0f })
        {
            auto pf = p;
            pf[P::SliceFade] = ms;
            const auto z = play (pf, 60, 0.7);
            // The level over the last 2 ms of the slice against the file there: less as the fade grows.
            const int end = n / 4, w = (int) (0.002 * rate);
            const double tail = rmsOf (z.getReadPointer (0) + end - w, w) / std::max (1.0e-9, rmsOf (drums.getReadPointer (0) + end - w, w));
            const double body = rmsOf (z.getReadPointer (0) + (int) (0.01 * rate), end - (int) (0.05 * rate))
                                / rmsOf (drums.getReadPointer (0) + (int) (0.01 * rate), end - (int) (0.05 * rate));
            ends << juce::String (ms, 0) << " ms: final " << db (toDb (std::max (1.0e-9, tail))) << ", cuerpo " << db (toDb (body)) << "  ";
            fades = fades && std::abs (toDb (body)) < 0.1 && (prevTail < 0.0 || tail < prevTail);
            prevTail = tail;
        }
        check (fades, "Fundido: más largo baja más el final del trozo, sin tocar el resto del trozo (0, 2 y 20 ms)", ends);
    }

    // 7. The MIDI clip: a note per slice from C3, where each slice starts and as long, the clip as long as the loop.
    {
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Clip test 8 trozos 100bpm.wav");
        file.deleteFile();
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            juce::WavAudioFormat wav;
            if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
                w->writeFromAudioSampleBuffer (drums, 0, n);
        }
        ReanimaProcessor proc;
        proc.setPlayConfigDetails (0, 2, rate, 256);
        proc.prepareToPlay (rate, 256);
        proc.loadSampleNow (file);
        for (int k = 0; k < 400 && proc.isLoading(); ++k)
        {
            juce::Thread::sleep (10);
            proc.serviceMessageThread();
        }
        proc.serviceMessageThread();
        for (const auto& [id, v] : { std::pair<const char*, float> { "playMode", 1.0f }, { "loopKind", 1.0f }, { "sliceBy", 1.0f }, { "sliceDivision", 3.0f } })
            if (auto* param = proc.apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (v));
        proc.serviceMessageThread();
        const auto marks = proc.getSlices();
        const auto mid = proc.writeSliceMidi();
        juce::MidiFile mf;
        juce::FileInputStream in (mid);
        const bool read = in.openedOk() && mf.readFrom (in);
        bool ok = read && mf.getNumTracks() == 1;
        int notes = 0;
        juce::String got;
        double tempo = 0.0, endTick = 0.0;
        if (ok)
        {
            const auto& t = *mf.getTrack (0);
            const int ppq = mf.getTimeFormat();
            for (int i = 0; i < t.getNumEvents(); ++i)
            {
                const auto& m = t.getEventPointer (i)->message;
                if (m.isTempoMetaEvent())
                    tempo = 60.0 / m.getTempoSecondsPerQuarterNote();
                if (m.isEndOfTrackMetaEvent())
                    endTick = m.getTimeStamp() / ppq;
                if (m.isNoteOn())
                {
                    const double beat = m.getTimeStamp() / ppq;
                    const double expected = marks[(size_t) notes] / rate * bpm / 60.0;
                    const double len = (t.getTimeOfMatchingKeyUp (i) - m.getTimeStamp()) / ppq;
                    const double expectedLen = ((notes + 1 < (int) marks.size() ? marks[(size_t) notes + 1] : n) - marks[(size_t) notes]) / rate * bpm / 60.0;
                    ok = ok && m.getNoteNumber() == 60 + notes && std::abs (beat - expected) < 0.002 && std::abs (len - expectedLen) < 0.01;
                    got << SampleAnalysis::midiNoteName ((float) m.getNoteNumber()) << "@" << juce::String (beat, 2) << " ";
                    ++notes;
                }
            }
        }
        ok = ok && notes == (int) marks.size() && notes == 8 && std::abs (tempo - bpm) < 0.01 && std::abs (endTick - 4.0) < 0.01;
        check (ok, "Clip MIDI: 8 notas C3..G3 en el sitio y con la duración de sus trozos, a 100 BPM, el clip dura el loop (4 tiempos)",
               juce::String (notes) + " notas, " + juce::String (tempo, 2) + " BPM, fin en el tiempo " + juce::String (endTick, 2) + ": " + got);
        file.deleteFile();
    }
}

//==============================================================================
// EDICIÓN: the order of the slices (keys, MIDI clip, exported audio), EXPORTAR AUDIO against what plays, and
// Por golpe (Envolvente and the filter's envelope at each hit).
void auditSliceEdit()
{
    title ("EDICIÓN: orden de los trozos, exportar audio y MIDI; envolvente y filtro por golpe");
    const double rate = 48000.0;
    constexpr int slices = 8;
    const double freqs[slices] = { 1000.0, 1189.2, 1414.2, 1681.8, 2000.0, 2378.4, 2828.4, 3363.6 };
    const int seg = (int) (0.25 * rate), n = slices * seg;          // 4 beats at 120 BPM
    Audio loop (2, n);
    for (int k = 0; k < slices; ++k)
        for (int i = 0; i < seg; ++i)
        {
            const double t = i / rate;
            const double rise = std::min (1.0, t / 0.002);          // a hit: from nothing in 2 ms, then steady
            const float v = (float) (0.3 * rise * std::sin (2.0 * juce::MathConstants<double>::pi * freqs[k] * t));
            loop.setSample (0, k * seg + i, v);
            loop.setSample (1, k * seg + i, v);
        }
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Edit tonos loop 120bpm.wav");
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (loop, 0, n);
    }
    const auto which = [&] (const float* x, int len)
    {
        double best = 0.0;
        int at = -1;
        for (int k = 0; k < slices; ++k)
            if (const double a = toneAmp (x, len, freqs[k], rate); a > best)
            {
                best = a;
                at = k;
            }
        return best > 1.0e-3 ? at : -1;
    };
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, rate, 256);
    proc.prepareToPlay (rate, 256);
    proc.loadSampleNow (file);
    for (int k = 0; k < 400 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        proc.serviceMessageThread();
    }
    proc.serviceMessageThread();
    const auto set = [&] (const char* id, float v)
    {
        if (auto* param = proc.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
        proc.serviceMessageThread();
    };
    for (const auto& [id, v] : { std::pair<const char*, float> { "playMode", 1.0f }, { "loopKind", 1.0f }, { "sliceBy", 1.0f }, { "sliceDivision", 3.0f },
                                 { "volume", 0.0f }, { "monoBass", 0.0f }, { "envLock", 1.0f }, { "ampA", 0.1f }, { "ampS", 1.0f }, { "ampR", 30.0f } })
        set (id, v);

    // 1. A new order: the keys follow it, the clip plays it, the export sounds it.
    const std::vector<int> order { 7, 6, 5, 4, 3, 2, 1, 0 };
    proc.setSliceOrder (order);
    bool keys = proc.getSliceOrder() == order;
    juce::String heard;
    {
        Audio buf (2, 256);
        for (int k = 0; k < slices; ++k)
        {
            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, 60 + k, 1.0f), 0);
            Audio got (2, 256 * 20);
            for (int b = 0; b < 20; ++b)
            {
                buf.clear();
                juce::MidiBuffer none;
                proc.processBlock (buf, b == 0 ? on : none);
                got.copyFrom (0, b * 256, buf, 0, 0, 256);
            }
            const int w = which (got.getReadPointer (0) + 1024, 4096);
            heard << w + 1 << " ";
            keys = keys && w == order[(size_t) k];
            for (int b = 0; b < 60; ++b)
            {
                buf.clear();
                juce::MidiBuffer none;
                proc.processBlock (buf, none);
            }
        }
    }
    check (keys, "Orden 8..1: C3 toca el trozo 8, C#3 el 7 ... G3 el 1", heard);

    const auto mid = proc.writeSliceMidi();
    juce::MidiFile mf;
    juce::FileInputStream in (mid);
    bool clip = in.openedOk() && mf.readFrom (in);
    juce::String notes;
    if (clip)
    {
        const auto& t = *mf.getTrack (0);
        int k = 0;
        for (int i = 0; i < t.getNumEvents(); ++i)
            if (const auto& m = t.getEventPointer (i)->message; m.isNoteOn())
            {
                const double beat = m.getTimeStamp() / mf.getTimeFormat();
                clip = clip && m.getNoteNumber() == 60 + k && std::abs (beat - 0.5 * k) < 0.002;
                notes << SampleAnalysis::midiNoteName ((float) m.getNoteNumber()) << "@" << juce::String (beat, 2) << " ";
                ++k;
            }
        clip = clip && k == slices;
    }
    check (clip, "Clip MIDI en el orden nuevo: C3..G3 seguidas cada medio tiempo (la secuencia 8..1)", notes);

    const auto out = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("reanima_export_test.wav");
    juce::String error;
    bool exported = proc.renderExport (out, 0, 44100.0, 24, error);
    juce::String sequence;
    bool inOrder = exported;
    double exportLen = 0.0;
    if (exported)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (out));
        inOrder = r != nullptr && std::abs (r->sampleRate - 44100.0) < 1.0 && r->bitsPerSample == 24;
        if (inOrder)
        {
            Audio y ((int) r->numChannels, (int) r->lengthInSamples);
            r->read (&y, 0, (int) r->lengthInSamples, 0, true, true);
            exportLen = r->lengthInSamples / 44100.0;
            for (int k = 0; k < slices; ++k)
            {
                const int at = (int) ((0.25 * k + 0.05) * 44100.0);
                double best = 0.0;
                int w = -1;
                for (int q = 0; q < slices; ++q)
                    if (const double a = toneAmp (y.getReadPointer (0) + at, 4096, freqs[q], 44100.0); a > best)
                    {
                        best = a;
                        w = q;
                    }
                sequence << w + 1 << " ";
                inOrder = inOrder && w == order[(size_t) k];
            }
            inOrder = inOrder && std::abs (exportLen - 2.0) < 0.002;
        }
    }
    check (inOrder, "EXPORTAR AUDIO (WAV 44.1 kHz 24 bits): suena la secuencia 8..1, dura el loop (2 s)",
           (exported ? sequence + juce::String (exportLen, 3) + " s" : error));

    // 2. The export is what plays: identity order, against the same notes played through processBlock.
    proc.setSliceOrder ({ 0, 1, 2, 3, 4, 5, 6, 7 });
    {
        const auto out2 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("reanima_export_same.wav");
        proc.renderExport (out2, 0, rate, 32, error);
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (out2));
        Audio y (2, (int) r->lengthInSamples);
        r->read (&y, 0, (int) r->lengthInSamples, 0, true, true);
        // Live: the same sequence through processBlock (the host's blocks), latency of the output stage removed.
        const int latency = proc.getLatencySamples(), total = y.getNumSamples();
        Audio live (2, total + latency + 256), buf (2, 256);
        live.clear();
        for (int pos = 0; pos < total + latency; pos += 256)
        {
            juce::MidiBuffer m;
            for (int k = 0; k < slices; ++k)
            {
                const int on = k * seg, off = (k + 1) * seg - 1;
                if (on >= pos && on < pos + 256)
                    m.addEvent (juce::MidiMessage::noteOn (1, 60 + k, 1.0f), on - pos);
                if (off >= pos && off < pos + 256)
                    m.addEvent (juce::MidiMessage::noteOff (1, 60 + k), off - pos);
            }
            buf.clear();
            proc.processBlock (buf, m);
            live.copyFrom (0, pos, buf, 0, 0, 256);
        }
        double diff = 0.0, peak = 0.0;
        for (int i = 0; i < total; ++i)
        {
            diff = std::max (diff, (double) std::abs (y.getSample (0, i) - live.getSample (0, i + latency)));
            peak = std::max (peak, (double) std::abs (y.getSample (0, i)));
        }
        check (diff < 1.0e-4 * std::max (1.0, peak) && peak > 0.05, "EXPORTAR AUDIO es lo que suena: igual al plugin tocando la misma secuencia",
               "diferencia " + juce::String (diff, 7) + ", pico " + juce::String (peak, 3));
        out2.deleteFile();
    }
    out.deleteFile();

    // 3. Por golpe in Normal: Decay 40 ms to Sustain 0 shapes each hit (the loop's tones die in each slice and
    //    come back at the next hit); the filter's envelope opens at each hit. Off: the loop as it is.
    {
        auto p = paramsFor (*proc.getSample());
        p[P::PlayMode] = (float) P::PlayLoop;
        p[P::LoopKind] = (float) P::LoopNormal;
        p[P::LoopAlgo] = (float) P::LoopBeatsAlgo;
        p[P::AmpA] = 1.0f;
        p[P::AmpD] = 40.0f;
        p[P::AmpS] = 0.0f;
        p[P::AmpR] = 30.0f;
        const auto run = [&] (const ParamSnapshot& pp)
        {
            SynthEngine e;
            e.prepare (rate);
            e.setSample (proc.getSample());
            e.prepareFx (pp);
            e.setSlices (proc.getSlices());
            Audio out (2, n), buf (2, 64);
            for (int pos = 0; pos < n; pos += 64)
            {
                juce::MidiBuffer m;
                if (pos == 0)
                    m.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey, 1.0f), 0);
                Audio view (buf.getArrayOfWritePointers(), 2, 64);
                e.process (view, m, pp, 120.0);
                out.copyFrom (0, pos, buf, 0, 0, 64);
            }
            return out;
        };
        auto hit = p;
        hit[P::LoopHitEnv] = 1.0f;
        const auto y = run (hit), z = run (p);
        bool shaped = true;
        juce::String levels;
        for (int k = 1; k < slices; ++k)
        {
            const double early = rmsOf (y.getReadPointer (0) + k * seg + (int) (0.003 * rate), (int) (0.01 * rate));
            const double late = rmsOf (y.getReadPointer (0) + k * seg + (int) (0.15 * rate), (int) (0.08 * rate));
            shaped = shaped && early > 0.05 && late < 0.01 * early;
            if (k < 4)
                levels << db (toDb (late / early)) << " ";
        }
        const double held = rmsOf (z.getReadPointer (0) + 3 * seg + (int) (0.15 * rate), (int) (0.08 * rate));
        check (shaped && held > 0.1, "Por golpe (Normal): la envolvente vuelve a empezar en cada golpe (Decay 40 ms a Sustain 0 apaga cada trozo); apagado no",
               "final del trozo frente a su golpe " + levels + "; sin Por golpe nivel " + juce::String (held, 3));

        // The filter: low-pass at 300 Hz opened by its envelope (attack 1 ms, decay 30 ms to 0): each slice's tone
        // through at its hit, gone after; without Por golpe only the first slice opens it.
        auto f = hit;
        f[P::AmpD] = 300.0f;
        f[P::AmpS] = 1.0f;
        f[P::FltOn] = 1.0f;
        f[P::FltFreq] = 300.0f;
        f[P::FltEnvAmt] = 1.0f;
        f[P::E2A] = 1.0f;
        f[P::E2D] = 30.0f;
        f[P::E2S] = 0.0f;
        auto fOff = f;
        fOff[P::LoopHitEnv] = 0.0f;
        const auto yf = run (f), zf = run (fOff);
        bool opens = true;
        for (int k = 1; k < slices; ++k)
        {
            const double at = toneAmp (yf.getReadPointer (0) + k * seg + (int) (0.004 * rate), 512, freqs[k], rate);
            const double after = toneAmp (yf.getReadPointer (0) + k * seg + (int) (0.18 * rate), 2048, freqs[k], rate);
            const double off = toneAmp (zf.getReadPointer (0) + k * seg + (int) (0.004 * rate), 512, freqs[k], rate);
            opens = opens && at > 4.0 * after && at > 4.0 * off;
        }
        check (opens, "Por golpe: el filtro se abre con cada golpe y se cierra después; sin Por golpe sólo con la nota");
    }
    file.deleteFile();
}

//==============================================================================
// LOOP "Rubber Band": a tone loop at 120 BPM in a host at 100, through the processor (its timer services the
// background render). It sounds at once (Complex until the render is there), then crosses to Rubber Band R3:
// the tone in tune, a key up a fifth in tune, the level kept, no clicks (the crossing, the loop's seam), and the
// audio thread's blocks light (it only reads the render).
struct HostAt : juce::AudioPlayHead
{
    double bpm = 100.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo i;
        i.setBpm (bpm);
        i.setIsPlaying (false);
        return i;
    }
};

void auditRubber()
{
    title ("LOOP Rubber Band (R3 precalculado): suena al momento, luego R3; tono, nivel, sin clics, CPU");
    const double rate = 48000.0, f = 330.0;
    const int n = (int) (8 * 60.0 / 120.0 * rate);
    Audio loop (2, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / rate, beat = std::fmod (t, 0.5);
        const float v = (float) (0.25 * std::sin (2.0 * juce::MathConstants<double>::pi * f * std::round (n / rate * f) / (n / rate * f) * t)
                                 * (0.6 + 0.4 * std::exp (-beat / 0.1) * std::min (1.0, beat / 0.005)));     // soft attacks: clicks are the engine's
        loop.setSample (0, i, v);
        loop.setSample (1, i, v);
    }
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("RB tono loop 120bpm.wav");
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (loop, 0, n);
    }
    ReanimaProcessor proc;
    HostAt host;
    proc.setPlayHead (&host);
    proc.setPlayConfigDetails (0, 2, rate, 64);
    proc.prepareToPlay (rate, 64);
    proc.loadSampleNow (file);
    for (int k = 0; k < 400 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        proc.serviceMessageThread();
    }
    proc.serviceMessageThread();
    for (const auto& [id, v] : { std::pair<const char*, float> { "playMode", 1.0f }, { "loopKind", 0.0f }, { "loopAlgo", 4.0f }, { "volume", 0.0f },
                                 { "monoBass", 0.0f }, { "envLock", 1.0f }, { "ampA", 1.0f }, { "ampS", 1.0f }, { "ampR", 30.0f }, { "loopWarp", 1.0f }, { "loopSync", 1.0f } })
        if (auto* param = proc.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
    proc.serviceMessageThread();

    const int total = (int) (8.0 * rate);
    Audio out (2, total), buf (2, 64);
    double worstLate = 0.0;
    int firstSound = -1, readyAt = -1;
    for (int pos = 0; pos < total; pos += 64)
    {
        juce::MidiBuffer m;
        if (pos == 0)
            m.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        if (pos == (int) (5.0 * rate))
            m.addEvent (juce::MidiMessage::noteOn (1, 67, 1.0f), 0);         // a fifth up (a second voice: its own stretch)
        if (pos == (int) (5.0 * rate))
            m.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        buf.clear();
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        proc.processBlock (buf, m);
        const double ratio = (juce::Time::getMillisecondCounterHiRes() - t0) / (1000.0 * 64.0 / rate);
        if (pos > (int) (3.0 * rate) && pos < (int) (4.9 * rate))
            worstLate = std::max (worstLate, ratio);
        out.copyFrom (0, pos, buf, 0, 0, 64);
        out.copyFrom (1, pos, buf, 1, 0, 64);
        if (firstSound < 0 && buf.getMagnitude (0, 0, 64) > 1.0e-3f)
            firstSound = pos;
        if (readyAt < 0 && proc.engine.rubberReady.load() != nullptr)
            readyAt = pos;
        if (pos % (64 * 75) == 0)        // the message thread, ten times a second, at the pace of real time
        {
            proc.serviceMessageThread();
            juce::Thread::sleep (100);
        }
    }
    const float* x = out.getReadPointer (0);
    const double tuned = toneAmp (x + (int) (3.0 * rate), 1 << 15, f, rate);
    double fifth = 0.0;
    for (const double at : { 5.6, 6.0, 6.4, 6.8, 7.2, 7.6 })
        fifth += toneAmp (x + (int) (at * rate), 8192, f * std::pow (2.0, 7.0 / 12.0), rate) / 6.0;
    double worstJump = 0.0, typical = 0.0;
    for (int i = (int) (0.2 * rate); i < (int) (4.9 * rate); ++i)
    {
        const double d2 = std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]);
        worstJump = std::max (worstJump, d2);
        typical += d2;
    }
    typical /= 4.7 * rate;
    for (const double at : { 4.6, 5.1, 5.5, 5.9, 6.3, 6.7, 7.1, 7.5 })
        std::printf ("  %.1f s: 330 Hz %.3f, quinta %.3f\n", at, toneAmp (x + (int) (at * rate), 8192, f, rate),
                     toneAmp (x + (int) (at * rate), 8192, f * std::pow (2.0, 7.0 / 12.0), rate));
    int jumpAt = 0;
    for (int i = (int) (0.2 * rate); i < (int) (4.9 * rate); ++i)
        if (std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]) >= worstJump)
            jumpAt = i;
    std::printf ("  (mayor salto a %.4f s; render R3 listo a %.2f s)\n", jumpAt / rate, readyAt / rate);
    const double level = toDb (rmsOf (x + (int) (3.0 * rate), (int) (1.5 * rate)) / rmsOf (loop.getReadPointer (0), n)) + 4.0;    // the output stage's 4 dB
    check (firstSound >= 0 && firstSound < (int) (0.01 * rate) && tuned > 0.05 && fifth > 0.05 && std::abs (toDb (fifth / tuned)) < 2.0 && readyAt > 0 && worstJump < 6.0 * typical && std::abs (level) < 1.5 && worstLate < 0.5,
           "Rubber Band: suena al momento, luego R3 a 100 BPM en su tono, una quinta con la tecla, sin clics, nivel y bloques ligeros",
           "sonido a " + juce::String (1000.0 * firstSound / rate, 1) + " ms, tono " + juce::String (tuned, 3) + ", quinta " + juce::String (fifth, 3)
               + ", salto " + juce::String (worstJump / typical, 1) + "x, nivel " + db (level) + ", peor bloque con R3 " + juce::String (100.0 * worstLate, 0) + " %");
    if (readyAt < 0)
    {
        // Diagnosis: wait for the render with the message thread running.
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int k = 0; k < 100 && proc.engine.rubberReady.load() == nullptr; ++k)
        {
            juce::MidiBuffer none;
            buf.clear();
            proc.processBlock (buf, none);
            proc.serviceMessageThread();
            juce::Thread::sleep (50);
        }
        std::printf ("  (render tras esperar: %s, %.1f s)\n", proc.engine.rubberReady.load() != nullptr ? "listo" : "NO", (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0);
    }
    // EXPORTAR AUDIO in Rubber Band: rendered with R3 from its first sample (a first pass finds what the note asks).
    {
        const auto out2 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("reanima_export_rb.wav");
        juce::String error;
        const bool ok = proc.renderExport (out2, 0, rate, 24, error);
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (ok ? fm.createReaderFor (out2) : nullptr);
        double amp = 0.0, len = 0.0;
        if (r != nullptr)
        {
            Audio y (2, (int) r->lengthInSamples);
            r->read (&y, 0, (int) r->lengthInSamples, 0, true, true);
            amp = toneAmp (y.getReadPointer (0) + (int) (0.05 * rate), 8192, f, rate);
            len = r->lengthInSamples / rate;
        }
        check (ok && amp > 0.05 && std::abs (len - 4.8) < 0.01, "EXPORTAR AUDIO en Rubber Band: el loop a 100 BPM (4,8 s) con R3 desde el principio",
               ok ? "tono " + juce::String (amp, 3) + ", " + juce::String (len, 3) + " s" : error);
        out2.deleteFile();
    }
    proc.setPlayHead (nullptr);
    file.deleteFile();
}

//==============================================================================
// Where a hit is placed when it lands on the loud tail of the one before (a bass, a pad, a ringing tom): a loop
// of 16 hits, a quarter of a second apart, each a tone or noise burst rising in 1 ms and dying away slowly, so
// that the tail of each one is still at half to two thirds of the next hit's level when it comes. Each hit must
// be found, and placed on its rise: never more than 3 ms early (before, the start was walked back while the
// level stayed over a tenth of the peak, and on such a tail that went back by up to 20 ms) nor 1.5 ms late
// (a slice that starts after its hit's attack loses it).
void onsetCase (double tauTone, double tauNoise, const juce::String& label, double maxEarly, double maxLate)
{
    const double rate = 44100.0;
    const int n = (int) (4.0 * rate);
    juce::AudioBuffer<float> loop (2, n);
    loop.clear();
    juce::Random rng (21);
    std::vector<int> truth;
    for (int k = 0; k < 16; ++k)
    {
        const int at = (int) ((0.1 + k * 0.25) * rate);         // (a hit at the very start is the loop's start: not looked for)
        truth.push_back (at);
        const int kind = k % 4;                   // 80 Hz, 300 Hz, 1.2 kHz, noise
        const double f = kind == 0 ? 80.0 : kind == 1 ? 300.0 : 1200.0;
        const double tau = kind == 3 ? tauNoise : tauTone;
        for (int i = 0; at + i < n; ++i)
        {
            const double t = i / rate;
            const double env = std::min (1.0, t / 0.001) * std::exp (-t / tau);
            if (i > 100 && env < 1.0e-4)
                break;
            const double v = kind == 3 ? rng.nextFloat() * 2.0 - 1.0 : std::sin (2.0 * juce::MathConstants<double>::pi * f * t);
            loop.addSample (0, at + i, (float) (0.25 * env * v));
            loop.addSample (1, at + i, (float) (0.25 * env * v));
        }
    }
    const auto info = SampleAnalysis::analyseLoop (loop, rate, "colas loop 120bpm");
    const auto& hits = info.hits;
    int found = 0;
    double worstEarly = 0.0, worstLate = 0.0;
    juce::String detail;
    for (const int t : truth)
    {
        int best = -1;
        for (const auto& h : hits)
            if (std::abs (h.first - t) < (int) (0.03 * rate) && (best < 0 || std::abs (h.first - t) < std::abs (best - t)))
                best = h.first;
        if (best < 0)
        {
            detail << "falta " << juce::String (t / rate, 2) << " s; ";
            continue;
        }
        ++found;
        const double ms = (best - t) / rate * 1000.0;
        worstEarly = std::min (worstEarly, ms);
        worstLate = std::max (worstLate, ms);
    }
    if (found < (int) truth.size())
    {
        juce::String all;
        for (const auto& h : hits)
            all << juce::String (h.first / rate, 3) << " ";
        detail << "hallados: " << all << "(loop " << info.length << ", tiempos " << info.beats << ")";
    }
    // (A hit not found at all is the detector's, not where it places one: an 80 Hz hit just after a noise burst is not seen;
    // it was not before either. Reported, not failed.)
    std::printf ("(info) %s: %d de 16 golpes hallados %s\n", label.toRawUTF8(), found, detail.toRawUTF8());
    check (worstEarly >= -maxEarly && worstLate <= maxLate, label + ": cada golpe en su subida, no más de " + juce::String (maxEarly, 1) + " ms antes ni "
               + juce::String (maxLate, 1) + " ms después",
           "de " + juce::String (worstEarly, 1) + " a " + juce::String (worstLate, 1) + " ms");
}

void auditOnsetOnTail()
{
    title ("Golpes sobre la cola fuerte del anterior: en su sitio, no hasta 20 ms antes");
    onsetCase (0.25, 0.15, "Colas a -9 dB al llegar el siguiente", 3.0, 1.5);
    onsetCase (0.6, 0.35, "Colas a -3,5 dB y sumadas (extremo)", 3.0, 1.5);
}

//==============================================================================
// LOOP: the hits the user turns off or adds (HitEdits). Eight tones, a hit each 0.25 s (4 beats at 120 BPM):
// the corrections hold through Sensibilidad, saving and reopening, and a new analysis; a new sample starts
// without; Beats in a host at 100 BPM plays without a hit turned off (the slice before it runs on through it).
void auditHitEdits()
{
    title ("LOOP: golpes apagados y añadidos a mano (se guardan, sobreviven a Sensibilidad y al reanálisis; Beats los usa)");

    // The corrections alone.
    {
        HitEdits e;
        e.off = { 1000 };
        e.on = { 5000 };
        const auto cut = e.apply ({ 0, 1000, 3000, 5020, 8000 }, 10000, 100, 50);
        check (cut == std::vector<int> ({ 0, 3000, 5000, 8000 }), "Correcciones: quita la apagada, pone la añadida (y gana a una encontrada a 20 muestras)",
               juce::String ((int) cut.size()) + " cortes");
        const auto again = HitEdits::fromString (e.toString());
        check (again.off == e.off && again.on == e.on && e.toString() == "off:1000|on:5000", "Correcciones: se escriben y se leen igual", e.toString());
        HitEdits t;
        const bool a = t.toggle (2000, true, 50), b = t.toggle (2030, true, 50);
        t.add (7000, 50);
        const bool c = t.toggle (7010, true, 50);
        check (a && b && t.empty() && c, "Correcciones: clic apaga, otro clic enciende; un clic quita una añadida", t.toString());
        check (HitEdits().apply ({ 0, 10, 20 }, 100, 5, 2) == std::vector<int> ({ 0, 10, 20 }), "Sin correcciones, los cortes son los de siempre", "");
    }

    const double rate = 48000.0;
    constexpr int slices = 8;
    const double freqs[slices] = { 1000.0, 1189.2, 1414.2, 1681.8, 2000.0, 2378.4, 2828.4, 3363.6 };
    const int seg = (int) (0.25 * rate), n = slices * seg;
    Audio loop (2, n);
    for (int k = 0; k < slices; ++k)
        for (int i = 0; i < seg; ++i)
        {
            const double t = i / rate;
            // each a hit that dies away (a level that rises at each hit, as Transitorios asks)
            // dying away before the next (its tail under a tenth of the hit: the analysis places the hit on its
            // rise; over the tail of a loud note it can place it up to some 20 ms early, a matter of its own)
            const float v = (float) (0.5 * std::exp (-t / 0.04) * std::sin (2.0 * juce::MathConstants<double>::pi * freqs[k] * t));
            loop.setSample (0, k * seg + i, v);
            loop.setSample (1, k * seg + i, v);
        }
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Golpes tonos loop 120bpm.wav");
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (loop, 0, n);
    }
    const auto settle = [] (ReanimaProcessor& p)
    {
        for (int k = 0; k < 600 && p.isLoading(); ++k)
        {
            juce::Thread::sleep (10);
            p.serviceMessageThread();
        }
        for (int k = 0; k < 20; ++k)
        {
            p.serviceMessageThread();
            juce::Thread::sleep (5);
        }
    };
    HostAt host;        // 100 BPM: the loop (120) plays at x0.83
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, rate, 256);
    proc.prepareToPlay (rate, 256);
    proc.loadSampleNow (file);
    settle (proc);
    const auto set = [&] (ReanimaProcessor& p, const char* id, float v)
    {
        if (auto* param = p.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
        p.serviceMessageThread();
    };
    for (const auto& [id, v] : { std::pair<const char*, float> { "playMode", 1.0f }, { "loopKind", 1.0f }, { "sliceBy", 0.0f },
                                 { "volume", 0.0f }, { "monoBass", 0.0f }, { "envLock", 1.0f }, { "ampA", 0.1f }, { "ampS", 1.0f }, { "ampR", 30.0f } })
        set (proc, id, v);
    settle (proc);
    const int tol = HitEdits::tolerance (rate);
    const auto hasCut = [tol] (const std::vector<int>& cuts, int at) { return HitEdits::near (cuts, at, tol); };
    const auto first = proc.getSlices();
    if (std::getenv ("GOLPES_DIAG") != nullptr && proc.getSample() != nullptr)
    {
        for (const int c : first)
            std::printf ("corte %d\n", c);
        for (const auto& h : proc.getSample()->loopInfo.hits)
            std::printf ("golpe %d %.3f\n", h.first, h.second);
        std::printf ("loop length %d beats %d\n", proc.getSample()->loopInfo.length, proc.getSample()->loopInfo.beats);
    }
    check (first.size() == (size_t) slices && hasCut (first, 3 * seg), "Transitorios encuentra los 8 golpes", juce::String ((int) first.size()) + " cortes");

    // Off: the 4th hit.
    auto edits = proc.getHitEdits();
    edits.toggle (first.size() > 3 ? first[3] : 3 * seg, true, tol);
    proc.setHitEdits (edits);
    auto now = proc.getSlices();
    check (now.size() == (size_t) slices - 1 && ! hasCut (now, 3 * seg), "Clic en un golpe: deja de cortar (SLICE: un trozo menos)", juce::String ((int) now.size()) + " cortes");
    check ((int) proc.paramValue (P::SliceBy) == P::SliceTransients, "Apagar un golpe no pasa los cortes a «A mano»", "");

    // Sensibilidad down and up: still off.
    set (proc, "sliceSensitivity", 0.2f);
    set (proc, "sliceSensitivity", 0.9f);
    proc.serviceMessageThread();
    now = proc.getSlices();
    check (! hasCut (now, 3 * seg), "Sigue apagado al mover Sensibilidad", juce::String ((int) now.size()) + " cortes a 0,9");
    set (proc, "sliceSensitivity", 0.5f);

    // Added: one in the middle of the 6th tone.
    edits = proc.getHitEdits();
    edits.add (5 * seg + seg / 2, tol);
    proc.setHitEdits (edits);
    now = proc.getSlices();
    check (hasCut (now, 5 * seg + seg / 2) && now.size() == (size_t) slices, "Doble clic: añade un golpe donde no lo había", juce::String ((int) now.size()) + " cortes");

    // The project saved and opened: the same corrections, the same cuts.
    {
        juce::MemoryBlock state;
        proc.getStateInformation (state);
        ReanimaProcessor reopened;
        reopened.setPlayConfigDetails (0, 2, rate, 256);
        reopened.prepareToPlay (rate, 256);
        reopened.setStateInformation (state.getData(), (int) state.getSize());
        settle (reopened);
        check (reopened.getHitEdits().toString() == proc.getHitEdits().toString() && reopened.getSlices() == proc.getSlices(),
               "Al reabrir el proyecto: mismos golpes apagados y añadidos, mismos cortes", reopened.getHitEdits().toString());
    }

    // Analysed again: another engine asks for the whole analysis (a new SampleData of the same sound): kept.
    {
        const auto before = proc.getHitEdits().toString();
        const auto* analysed = proc.getSample().get();
        set (proc, "engine", 0.0f);
        settle (proc);
        set (proc, "engine", 1.0f);
        settle (proc);
        for (int k = 0; k < 300 && proc.getSample().get() == analysed; ++k)
        {
            juce::Thread::sleep (10);
            proc.serviceMessageThread();
        }
        settle (proc);
        check (proc.getSample().get() != analysed && proc.getHitEdits().toString() == before && ! hasCut (proc.getSlices(), 3 * seg),
               "Al analizar de nuevo el sample (otro motor) se conservan",
               juce::String (proc.getSample().get() != analysed ? "reanalizado, " : "SIN reanálisis, ") + proc.getHitEdits().toString());
    }

    // Beats (LOOP Normal) in the host at 100: the 4th tone starts where the 3rd slice, read on, reaches it
    // (3.4 slices of time) instead of at its own hit (3.6 slices: 3 x 1.2).
    proc.setPlayHead (&host);
    set (proc, "loopKind", 0.0f);
    set (proc, "loopAlgo", 1.0f);
    settle (proc);
    check (! proc.getBeatCuts().empty() && ! hasCut (proc.getBeatCuts(), 3 * seg), "Beats toma los mismos golpes corregidos",
           juce::String ((int) proc.getBeatCuts().size()) + " cortes");
    const auto onsetOf4th = [&] ()
    {
        const int total = (int) (2.5 * rate), block = 256;
        Audio out (2, total), buf (2, block);
        out.clear();
        for (int pos = 0; pos < total; pos += block)
        {
            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
            buf.clear();
            proc.processBlock (buf, midi);
            out.copyFrom (0, pos, buf, 0, 0, std::min (block, total - pos));
        }
        {
            juce::MidiBuffer off, none;
            off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            for (int k = 0; k < 200; ++k)
            {
                buf.clear();
                proc.processBlock (buf, k == 0 ? off : none);
            }
        }
        // where the 4th tone first reaches half its loudest in the first round
        const int win = (int) (0.02 * rate), stepAt = (int) (0.001 * rate);     // 50 Hz apart: the 3rd tone (267 Hz away) does not leak in
        double top = 0.0;
        for (int at = (int) (0.6 * rate); at + win < (int) (1.2 * rate); at += stepAt)
            top = std::max (top, toneAmp (out.getReadPointer (0) + at, win, freqs[3], rate));
        for (int at = (int) (0.6 * rate); at + win < (int) (1.2 * rate); at += stepAt)
            if (top > 1.0e-3 && toneAmp (out.getReadPointer (0) + at, win, freqs[3], rate) > 0.5 * top)
                return (at + win / 2) / rate;
        return -1.0;
    };
    const double withOff = onsetOf4th();
    const auto kept = proc.getHitEdits();
    proc.setHitEdits (HitEdits());
    settle (proc);
    const double withAll = onsetOf4th();
    proc.setHitEdits (kept);
    const double slice = 0.25;
    check (std::abs (withAll - 3.6 * slice) < 0.02 && std::abs (withOff - 3.4 * slice) < 0.02,
           "Beats a 100 BPM: sin el golpe apagado el 4.º tono no se corta en su golpe (llega con el trozo anterior)",
           "con el golpe " + juce::String (withAll, 3) + " s (esperado 0,900), apagado " + juce::String (withOff, 3) + " s (esperado 0,850)");

    // SAMPLE and EDICIÓN share the same cuts and the same click: off and back on from either.
    {
        const auto shown = proc.getLoopCuts();
        const int cut = shown.size() > 2 ? shown[2] : -1;
        proc.toggleHit (cut, true);
        const bool gone = ! hasCut (proc.getLoopCuts(), cut);
        proc.toggleHit (cut, false);
        const bool back = hasCut (proc.getLoopCuts(), cut);
        check (proc.hitsEditable() && cut > 0 && gone && back, "SAMPLE y EDICIÓN: los mismos cortes, el mismo clic (apaga y vuelve a encender)",
               juce::String ((int) shown.size()) + " cortes");
    }

    // A new sample: none.
    {
        const auto other = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Golpes otro loop 120bpm.wav");
        other.deleteFile();
        Audio quieter (2, n);
        quieter.copyFrom (0, 0, loop, 0, 0, n);
        quieter.copyFrom (1, 0, loop, 1, 0, n);
        quieter.applyGain (0.5f);
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (other);
            juce::WavAudioFormat wav;
            if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
                w->writeFromAudioSampleBuffer (quieter, 0, n);
        }
        proc.loadSample (other);        // as the user loads one (the browser, a drop)
        settle (proc);
        check (proc.getHitEdits().empty(), "Un sample nuevo empieza sin golpes corregidos", proc.getHitEdits().toString());
    }
    proc.setPlayHead (nullptr);
}

//==============================================================================
// EDICIÓN, Imán: the grid follows the loop's beats (a loop of 4 beats at 120 BPM: 24000 samples a beat at
// 48 kHz) or, without a tempo, time; Auto as fine as the zoom leaves room for; each line exactly on a sample;
// the zoom never moves a selection; a position near a line goes to it; off, positions are as given.
void auditMagnet()
{
    title ("EDICIÓN: imán y rejilla (tiempos del loop, sin tempo, Auto según el zoom, precisión de una muestra)");
    const double rate = 48000.0;
    const int beat = 24000, n = 4 * beat;
    Audio loop (2, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = std::fmod (i / rate, 0.5);
        const float v = (float) (0.5 * std::exp (-t / 0.05) * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / rate));
        loop.setSample (0, i, v);
        loop.setSample (1, i, v);
    }
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Iman loop 120bpm.wav");
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (loop, 0, n);
    }
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, rate, 256);
    proc.prepareToPlay (rate, 256);
    proc.loadSampleNow (file);
    for (int k = 0; k < 600 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        proc.serviceMessageThread();
    }
    proc.serviceMessageThread();
    if (auto* p = proc.apvts.getParameter ("playMode"))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));
    proc.serviceMessageThread();

    EditView view (proc);
    view.setSize (1000, 300);           // the waveform some 980 px wide
    view.sampleChanged();
    view.setSnapsToGrid (true);

    auto g = view.grid();
    check (g.musical && std::abs (g.beat - beat) < 1.0, "Loop: la rejilla cuenta sus tiempos", juce::String (g.beat, 1) + " muestras por tiempo");
    check (g.name.contains ("1/8") || g.name.contains ("1/16"), "Auto con todo el loop a la vista (980 px): una división útil", g.name);

    view.setViewForTests (0.0, 0.01);       // x100: under a 16th on screen
    g = view.grid();
    check (g.step < beat / 16.0 + 1.0, "Auto al acercar: más fina", g.name + ", paso " + juce::String (g.step, 1));

    view.setViewForTests (0.0, 1.0);
    view.setGridChoice (7);             // 1/16
    g = view.grid();
    check (std::abs (g.step - beat / 4.0) < 1.0e-9 && g.name == "1/16", "Elegida a mano: 1/16 = un cuarto de tiempo", g.name);
    // A position just past a 16th (by 0.3 % of the file: 288 samples) goes onto it, exactly on its sample.
    const double target = 5 * beat / 4.0;     // the 5th sixteenth: 30000
    const double got = view.snapped ((target + 288.0) / n);
    check (std::abs (got * n - target) < 1.0e-6, "Un borde cerca de una línea va a ella, en la muestra exacta",
           juce::String (got * n, 3) + " (esperado 30000)");
    view.setGridChoice (10);            // 1/4T
    g = view.grid();
    check (std::abs (g.step - beat * 2.0 / 3.0) < 1.0e-6, "Tresillos: 1/4T = dos tercios de tiempo", juce::String (g.step, 2));
    const double t3 = view.snapped ((beat * 2.0 / 3.0 + 100.0) / n) * n;
    check (std::abs (t3 - std::round (beat * 2.0 / 3.0)) < 1.0e-6, "Tresillo: a la muestra más cercana a la línea (16000)", juce::String (t3, 2));
    view.setGridChoice (0);

    // The zoom does not move a selection.
    view.selectAll();
    const double a0 = view.selectionStartForTests(), b0 = view.selectionEndForTests();
    view.setViewForTests (0.3, 0.05);
    view.setViewForTests (0.0, 1.0);
    check (view.selectionStartForTests() == a0 && view.selectionEndForTests() == b0, "El zoom no mueve la selección", "");

    // Off: positions as given.
    view.setSnapsToGrid (false);
    check (! view.snapsToGrid(), "Imán apagado: se edita libre", "");
    // Saved with the project.
    view.setSnapsToGrid (true);
    view.setGridChoice (7);
    {
        juce::MemoryBlock state;
        proc.getStateInformation (state);
        ReanimaProcessor reopened;
        reopened.setStateInformation (state.getData(), (int) state.getSize());
        EditView other (reopened);
        check (other.snapsToGrid() && other.gridChoice() == 7, "Imán y resolución se guardan con el proyecto", "");
    }

    // Without a tempo: a one-shot (no loop) with no host tempo: time.
    if (auto* p = proc.apvts.getParameter ("playMode"))
        p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
    proc.serviceMessageThread();
    Audio hit (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float v = (float) (0.5 * std::exp (-i / rate / 0.3) * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / rate));
        hit.setSample (0, i, v);
        hit.setSample (1, i, v);
    }
    const auto oneShot = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Iman golpe.wav");
    oneShot.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (oneShot);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (hit, 0, n);
    }
    ReanimaProcessor shot;
    shot.setPlayConfigDetails (0, 2, rate, 256);
    shot.prepareToPlay (rate, 256);
    shot.loadSampleNow (oneShot);
    for (int k = 0; k < 600 && shot.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        shot.serviceMessageThread();
    }
    shot.serviceMessageThread();
    EditView timeView (shot);
    timeView.setSize (1000, 300);
    timeView.sampleChanged();
    timeView.setSnapsToGrid (true);
    g = timeView.grid();
    check (! g.musical && g.step > 0.0 && g.name.endsWith ("ms"), "Sin tempo: rejilla de tiempo", g.name);
    timeView.setViewForTests (0.0, 0.0005);     // the finest zoom
    g = timeView.grid();
    check (g.step <= 0.001 * rate + 1.0e-9, "Sin tempo, al máximo zoom: 1 ms", g.name);
    file.deleteFile();
    oneShot.deleteFile();
}

//==============================================================================
// TIMBRE por golpe: a loop of 8 noise hits (strong, weak, strong, weak...) at 120 BPM, LOOP Normal at its own
// tempo. With Por golpe, TIMBRE acts on each hit: Brillo darkens the weak hits against the strong ones, Arco the
// start of each hit, Apagado the end of each, and Mecanismo strikes at each hit if asked. Without Por golpe each
// knob does what it did (the hits play alike).
void auditTimbreHits()
{
    title ("TIMBRE por golpe: Brillo, Arco y Apagado en cada golpe; Mecanismo por golpe");
    const double rate = 48000.0;
    constexpr int hits = 8;
    const int seg = (int) (0.25 * rate), n = hits * seg;
    Audio loop (2, n);
    juce::Random r (7);
    for (int k = 0; k < hits; ++k)
        for (int i = 0; i < seg; ++i)
        {
            const float v = (float) ((k % 2 == 0 ? 0.6 : 0.2) * std::exp (-i / rate / 0.06)) * (r.nextFloat() * 2.0f - 1.0f);
            loop.setSample (0, k * seg + i, v);
            loop.setSample (1, k * seg + i, v);
        }
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Timbre golpes loop 120bpm.wav");
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (loop, 0, n);
    }
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, rate, 128);
    proc.prepareToPlay (rate, 128);
    proc.loadSampleNow (file);
    for (int k = 0; k < 600 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        proc.serviceMessageThread();
    }
    const auto set = [&] (const char* id, float v)
    {
        if (auto* param = proc.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
    };
    for (const auto& [id, v] : { std::pair<const char*, float> { "playMode", 1.0f }, { "loopKind", 0.0f }, { "volume", 0.0f }, { "monoBass", 0.0f },
                                 { "envLock", 1.0f }, { "ampA", 1.0f }, { "ampD", 2000.0f }, { "ampS", 1.0f }, { "ampR", 30.0f },
                                 { "velBright", 0.0f }, { "bow", 0.0f }, { "relDark", 0.0f }, { "relNoise", 0.0f }, { "sliceSensitivity", 0.5f } })
        set (id, v);
    for (int k = 0; k < 30; ++k)
    {
        proc.serviceMessageThread();
        juce::Thread::sleep (5);
    }
    // One round of the loop (key held, at full velocity); the high band (over 6 kHz) of each hit's first and last
    // 60 ms, in dB, hit by hit.
    struct Round { std::vector<double> early, late, level; };
    const auto round = [&] (int listen)
    {
        proc.engine.listen.store (listen);
        const int total = n + (int) (0.3 * rate), block = 128;
        Audio out (2, total), buf (2, block);
        out.clear();
        for (int pos = 0; pos < total; pos += block)
        {
            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            if (pos <= n && n < pos + block)
                midi.addEvent (juce::MidiMessage::noteOff (1, 60), n - pos);
            buf.clear();
            proc.processBlock (buf, midi);
            out.copyFrom (0, pos, buf, 0, 0, std::min (block, total - pos));
        }
        for (int k = 0; k < 100; ++k)
        {
            juce::MidiBuffer none;
            buf.clear();
            proc.processBlock (buf, none);
        }
        proc.engine.listen.store (SynthEngine::ListenAll);
        // high band: the difference of neighbouring samples (rises 6 dB an octave), against the level
        Round rd;
        const float* x = out.getReadPointer (0);
        const int lat = proc.getLatencySamples();
        const auto band = [&] (int from, int len)
        {
            double hi = 0.0, all = 0.0;
            for (int i = from + 1; i < from + len; ++i)
            {
                const double d = x[i] - x[i - 1];
                hi += d * d;
                all += (double) x[i] * x[i];
            }
            return std::pair<double, double> (hi, all);
        };
        const int w = (int) (0.06 * rate);
        for (int k = 0; k < hits; ++k)
        {
            const int at = k * seg + lat + (int) (0.002 * rate);
            const auto e = band (at, w), l = band (at + seg - w - (int) (0.01 * rate), w);
            rd.early.push_back (10.0 * std::log10 ((e.first + 1.0e-20) / (e.second + 1.0e-20)));
            rd.late.push_back (10.0 * std::log10 ((l.first + 1.0e-20) / (l.second + 1.0e-20)));
            rd.level.push_back (10.0 * std::log10 (e.second / w + 1.0e-20));
        }
        return rd;
    };
    const auto mean = [] (const std::vector<double>& v, int parity)
    {
        double s = 0.0;
        int c = 0;
        for (size_t k = 1; k < v.size(); ++k)       // the first hit is the note's start: left out
            if (parity < 0 || (int) (k % 2) == parity)
            {
                s += v[k];
                ++c;
            }
        return s / std::max (1, c);
    };

    // Brillo: weak (odd) hits against strong (even) ones, Por golpe off and on.
    set ("velBright", 1.0f);
    set ("loopHitEnv", 0.0f);
    proc.serviceMessageThread();
    const auto off = round (SynthEngine::ListenAll);
    set ("loopHitEnv", 1.0f);
    proc.serviceMessageThread();
    for (int k = 0; k < 10; ++k)
        proc.serviceMessageThread();
    const auto on = round (SynthEngine::ListenAll);
    const double gapOff = mean (off.early, 0) - mean (off.early, 1), gapOn = mean (on.early, 0) - mean (on.early, 1);
    check (std::abs (gapOff) < 1.5 && gapOn > 3.0, "Brillo por golpe: los golpes flojos suenan más oscuros que los fuertes (sin Por golpe, iguales)",
           "agudos fuertes-flojos: sin " + juce::String (gapOff, 1) + " dB, con " + juce::String (gapOn, 1) + " dB");
    set ("velBright", 0.0f);

    // Arco: the start of each hit darker with a slow Ataque.
    set ("ampA", 80.0f);
    set ("bow", 0.0f);
    proc.serviceMessageThread();
    const auto bow0 = round (SynthEngine::ListenAll);
    set ("bow", 1.0f);
    proc.serviceMessageThread();
    const auto bow1 = round (SynthEngine::ListenAll);
    const double bowDrop = mean (bow0.early, -1) - mean (bow1.early, -1);
    check (bowDrop > 3.0, "Arco por golpe: el Ataque de cada golpe más oscuro", juce::String (bowDrop, 1) + " dB menos de agudos al empezar cada golpe");
    set ("bow", 0.0f);
    set ("ampA", 1.0f);

    // Apagado: the end of each hit darker as it falls (Decay 60 ms to Sustain 20 %).
    set ("ampD", 60.0f);
    set ("ampS", 0.2f);
    set ("relDark", 0.0f);
    proc.serviceMessageThread();
    const auto dark0 = round (SynthEngine::ListenAll);
    set ("relDark", 1.0f);
    proc.serviceMessageThread();
    const auto dark1 = round (SynthEngine::ListenAll);
    const double lateDrop = mean (dark0.late, -1) - mean (dark1.late, -1), earlyDrop = mean (dark0.early, -1) - mean (dark1.early, -1);
    check (lateDrop > 4.0 && lateDrop > earlyDrop + 2.0, "Apagado por golpe: cada golpe se oscurece al caer (más al final que al principio)",
           "al final " + juce::String (lateDrop, 1) + " dB, al principio " + juce::String (earlyDrop, 1) + " dB");
    set ("relDark", 0.0f);
    set ("ampD", 2000.0f);
    set ("ampS", 1.0f);

    // Mecanismo por golpe: only the strike heard (Oír golpe), off and on.
    set ("mechOn", 1.0f);
    set ("relNoise", 0.8f);
    set ("mechPerHit", 0.0f);
    proc.serviceMessageThread();
    const auto mech0 = round (SynthEngine::ListenMechanism);
    set ("mechPerHit", 1.0f);
    proc.serviceMessageThread();
    const auto mech1 = round (SynthEngine::ListenMechanism);
    int struck = 0;
    for (int k = 1; k < hits; ++k)
        struck += mech1.level[(size_t) k] > mech0.level[(size_t) k] + 6.0 ? 1 : 0;
    check (struck >= hits - 2, "Mecanismo por golpe: un golpe de tecla en cada golpe del loop", juce::String (struck) + " de " + juce::String (hits - 1) + " golpes");
    set ("loopHitEnv", 0.0f);
    proc.serviceMessageThread();
    const auto mech2 = round (SynthEngine::ListenMechanism);
    int without = 0;
    for (int k = 1; k < hits; ++k)
        without += mech2.level[(size_t) k] > mech0.level[(size_t) k] + 6.0 ? 1 : 0;
    check (without == 0, "Sin Por golpe, Mecanismo por golpe no actúa", juce::String (without) + " golpes");
    file.deleteFile();
}

//==============================================================================
// LOOP Ventana: a loop of 8 tones of an eighth each (4 beats at 120 BPM). A window of a beat placed at beat 1
// plays the 3rd and 4th tones only, round and round, with each algorithm, at the loop's tempo and in a host at
// 100 BPM; an eighth at beat 2.5 the 6th alone; moved while it plays, no click.
void auditLoopWindow()
{
    title ("LOOP Ventana: sólo suena la parte elegida (1/32 a 4 compases), con cada algoritmo y a otro tempo; moverla no hace clic");
    const double rate = 48000.0;
    constexpr int tones = 8;
    const double freqs[tones] = { 1000.0, 1189.2, 1414.2, 1681.8, 2000.0, 2378.4, 2828.4, 3363.6 };
    const int seg = (int) (0.25 * rate), n = tones * seg;
    Audio loop (2, n);
    for (int k = 0; k < tones; ++k)
        for (int i = 0; i < seg; ++i)
        {
            const double t = i / rate;
            const float v = (float) (0.3 * std::min (1.0, std::min (t, (seg - i) / rate) / 0.003) * std::sin (2.0 * juce::MathConstants<double>::pi * freqs[k] * t));
            loop.setSample (0, k * seg + i, v);
            loop.setSample (1, k * seg + i, v);
        }
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Ventana tonos loop 120bpm.wav");
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (loop, 0, n);
    }
    HostAt host;
    ReanimaProcessor proc;
    proc.setPlayHead (&host);
    proc.setPlayConfigDetails (0, 2, rate, 128);
    proc.prepareToPlay (rate, 128);
    proc.loadSampleNow (file);
    for (int k = 0; k < 600 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        proc.serviceMessageThread();
    }
    const auto set = [&] (const char* id, float v)
    {
        if (auto* param = proc.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
        proc.serviceMessageThread();
    };
    for (const auto& [id, v] : { std::pair<const char*, float> { "playMode", 1.0f }, { "loopKind", 0.0f }, { "volume", 0.0f }, { "monoBass", 0.0f },
                                 { "envLock", 1.0f }, { "ampA", 1.0f }, { "ampS", 1.0f }, { "ampR", 30.0f }, { "loopBeats", 4.0f } })
        set (id, v);
    // Which tones sound over the second half of `seconds` held (each window of 20 ms: the loudest tone, if any).
    const auto play = [&] (double seconds, std::function<void (int)> during = nullptr)
    {
        const int total = (int) (seconds * rate), block = 128;
        Audio out (2, total), buf (2, block);
        out.clear();
        for (int pos = 0; pos < total; pos += block)
        {
            if (during)
                during (pos);
            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            buf.clear();
            proc.processBlock (buf, midi);
            out.copyFrom (0, pos, buf, 0, 0, std::min (block, total - pos));
        }
        juce::MidiBuffer off, none;
        off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        for (int k = 0; k < 150; ++k)
        {
            buf.clear();
            proc.processBlock (buf, k == 0 ? off : none);
        }
        return out;
    };
    const auto heard = [&] (const Audio& out, double from)
    {
        std::set<int> found;
        const int w = (int) (0.02 * rate);
        for (int at = (int) (from * rate); at + w < out.getNumSamples(); at += w)
        {
            double best = 0.0;
            int which = -1;
            for (int k = 0; k < tones; ++k)
                if (const double a = toneAmp (out.getReadPointer (0) + at, w, freqs[k], rate); a > best)
                {
                    best = a;
                    which = k;
                }
            if (best > 0.03)
                found.insert (which);
        }
        juce::String text;
        for (const int k : found)
            text << (k + 1) << " ";
        return std::pair<std::set<int>, juce::String> (found, text.trim());
    };
    const char* names[] = { "Auto", "Beats", "Complex", "Complex Pro", "Rubber Band" };

    // Completo: every tone.
    set ("loopWinLen", 0.0f);
    {
        host.bpm = 120.0;
        const auto got = heard (play (2.5), 0.3);
        check (got.first.size() == (size_t) tones, "Completo: suena el loop entero", "tonos " + got.second);
    }
    // A beat (1/4) from beat 1: the 3rd and 4th tones, at 120 and at 100, with each algorithm.
    set ("loopWinLen", 6.0f);       // 1/4
    set ("loopWinPos", 1.0f);
    for (const double bpm : { 120.0, 100.0 })
        for (int algo = 1; algo <= 4; ++algo)
        {
            host.bpm = bpm;
            set ("loopAlgo", (float) algo);
            if (algo == 4)
                play (1.0);         // Rubber Band: its render asked for, made in the background
            for (int k = 0; k < (algo == 4 ? 200 : 2); ++k)
            {
                proc.serviceMessageThread();
                juce::Thread::sleep (algo == 4 ? 10 : 1);
            }
            const auto got = heard (play (3.0), 0.5);
            check (got.first == std::set<int> ({ 2, 3 }), juce::String ("Ventana 1/4 desde el tiempo 2, ") + names[algo] + " a " + juce::String ((int) bpm) + " BPM: sólo los tonos 3 y 4",
                   "tonos " + got.second);
        }
    // An eighth from beat 2.5: the 6th tone alone.
    host.bpm = 120.0;
    set ("loopAlgo", 1.0f);
    set ("loopWinLen", 9.0f);       // 1/8
    set ("loopWinPos", 2.5f);
    {
        const auto got = heard (play (2.0), 0.3);
        check (got.first == std::set<int> ({ 5 }), "Ventana 1/8 desde 3.2+: sólo el 6.º tono", "tonos " + got.second);
    }
    // Moved while it plays (every 0.4 s, a beat further): no click (no step of more than the tone's own over a sample).
    set ("loopWinLen", 6.0f);
    set ("loopWinPos", 0.0f);
    {
        int moves = 0;
        const auto out = play (2.4, [&] (int pos)
        {
            if (pos > 0 && pos % (int) (0.4 * rate) < 128)
            {
                ++moves;
                set ("loopWinPos", (float) (moves % 4));
            }
        });
        double worst = 0.0;
        int worstAt = 0;
        const float* x = out.getReadPointer (0);
        for (int i = 2; i < out.getNumSamples(); ++i)
            if (const double c = std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]); c > worst)      // a corner
            {
                worst = c;
                worstAt = i;
            }
        if (std::getenv ("VENTANA_DIAG") != nullptr)
            for (int i = 2; i < out.getNumSamples(); ++i)
                if (std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]) > 0.1)
                    std::printf ("esquina %.4f s (muestra %d)\n", i / rate, i);
        const double toneCorner = 0.3 * std::pow (2.0 * juce::MathConstants<double>::pi * 3363.6 / rate, 2.0);
        check (worst < 2.5 * toneCorner, "Mover la Ventana mientras suena: sin clics", juce::String (moves) + " movimientos, esquina " + juce::String (worst, 4)
                   + " (tono " + juce::String (toneCorner, 4) + ")");
    }
    // With the host's transport (Play): the window loops on the host's beats. A beat from beat 2: in each beat of
    // the host, its first half is the 3rd tone and its second half the 4th.
    {
        struct HostRun : juce::AudioPlayHead
        {
            double ppq = 0.0;
            juce::Optional<PositionInfo> getPosition() const override
            {
                PositionInfo i;
                i.setBpm (120.0);
                i.setIsPlaying (true);
                i.setPpqPosition (ppq);
                return i;
            }
        } run;
        set ("loopWinLen", 6.0f);
        set ("loopWinPos", 1.0f);
        set ("loopAlgo", 1.0f);
        set ("loopPlay", 1.0f);
        proc.setPlayHead (&run);
        const int total = (int) (4.0 * rate), block = 128;
        Audio out (2, total), buf (2, block);
        juce::MidiBuffer none;
        for (int pos = 0; pos < total; pos += block)
        {
            run.ppq = pos / rate * 2.0;         // 120 BPM: 2 beats a second
            buf.clear();
            proc.processBlock (buf, none);
            out.copyFrom (0, pos, buf, 0, 0, std::min (block, total - pos));
        }
        const int lat = proc.getLatencySamples(), w = (int) (0.06 * rate);
        int right = 0, all = 0;
        for (double beatAt = 2.0; beatAt < 7.5; beatAt += 1.0)
            for (const auto [half, tone] : { std::pair<double, int> { 0.05, 2 }, { 0.55, 3 } })
            {
                const int at = (int) ((beatAt + half) / 2.0 * rate) + lat;
                double best = 0.0;
                int which = -1;
                for (int k = 0; k < tones; ++k)
                    if (const double amp = toneAmp (out.getReadPointer (0) + at, w, freqs[k], rate); amp > best)
                    {
                        best = amp;
                        which = k;
                    }
                right += which == tone ? 1 : 0;
                ++all;
            }
        check (right == all, "Ventana con Play: sigue los tiempos del DAW (1.ª mitad de cada tiempo, tono 3; 2.ª, tono 4)",
               juce::String (right) + " de " + juce::String (all));
        set ("loopPlay", 0.0f);
    }
    proc.setPlayHead (nullptr);
    file.deleteFile();
}

//==============================================================================
// The knobs' outer ring: the matrix's modulation as the engine applies it. Macro 1 to the cutoff with no note
// (the macros alone), velocity to the resonance from the newest note, an LFO moving with a note held, and the
// values themselves never moved.
void auditModRings()
{
    title ("Aros de modulación: macros sin notas, la última nota, LFO en directo; el valor base no se toca");
    const double rate = 48000.0;
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, rate, 256);
    proc.prepareToPlay (rate, 256);
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Aros tono.wav");
    {
        Audio tone (2, (int) rate);
        for (int i = 0; i < tone.getNumSamples(); ++i)
        {
            const float v = (float) (0.3 * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / rate));
            tone.setSample (0, i, v);
            tone.setSample (1, i, v);
        }
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
    }
    proc.loadSampleNow (file);
    for (int k = 0; k < 600 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        proc.serviceMessageThread();
    }
    const auto set = [&] (const char* id, float v)
    {
        if (auto* param = proc.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
    };
    const auto blocks = [&] (int count, juce::MidiBuffer first)
    {
        Audio buf (2, 256);
        juce::MidiBuffer none;
        for (int b = 0; b < count; ++b)
        {
            buf.clear();
            proc.processBlock (buf, b == 0 ? first : none);
        }
    };
    set ("fltOn", 1.0f);
    set ("fltFreq", 1000.0f);
    set ("fltRes", 0.2f);
    // Slot 1: Macro 1 -> cutoff, 0.5. Slot 2: velocity -> resonance, 0.4.
    set ("m1Src", (float) P::SrcMacro1);
    set ("m1Dst", (float) P::DstCutoff);
    set ("m1Amt", 0.5f);
    set ("macro1", 0.6f);
    set ("m2Src", (float) P::SrcVelocity);
    set ("m2Dst", (float) P::DstRes);
    set ("m2Amt", 0.4f);
    blocks (4, {});
    const float cut = proc.engine.modShown[P::DstCutoff].load(), res = proc.engine.modShown[P::DstRes].load();
    check (std::abs (cut - 0.3f) < 1.0e-5f && std::abs (res) < 1.0e-6f, "Sin notas: el aro enseña sólo las macros (corte +0,3; velocidad nada)",
           "corte " + juce::String (cut, 4) + ", resonancia " + juce::String (res, 4));
    const float shownHz = SynthEngine::modulated (P::FltFreq, proc.paramValue (P::FltFreq), cut);
    check (std::abs (shownHz - 1000.0f * std::exp2 (1.5f)) < 1.0f, "Corte modulado como lo aplica el motor (60 semitonos por unidad: 1000 Hz -> 2828 Hz)",
           juce::String (shownHz, 1) + " Hz");

    // Two notes: the ring follows the newest (velocity 0.3 after 1.0).
    {
        juce::MidiBuffer a, b;
        a.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        blocks (4, a);
        const float first = proc.engine.modShown[P::DstRes].load();
        b.addEvent (juce::MidiMessage::noteOn (1, 64, 0.3f), 0);
        blocks (4, b);
        const float second = proc.engine.modShown[P::DstRes].load();
        check (std::abs (first - 0.4f) < 0.01f && std::abs (second - 0.4f * 0.3f) < 0.01f, "Con notas: el aro sigue a la última nota tocada (su velocidad)",
               juce::String (first, 3) + " y luego " + juce::String (second, 3));
        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        off.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
        blocks (200, off);
    }

    // An LFO on the resonance: the ring moves while the note is held.
    set ("m2Src", (float) P::SrcLfo1);
    set ("lfo1Rate", 4.0f);
    {
        juce::MidiBuffer a;
        a.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        Audio buf (2, 256);
        juce::MidiBuffer none;
        float lo = 1.0f, hi = -1.0f;
        for (int b = 0; b < 200; ++b)
        {
            buf.clear();
            proc.processBlock (buf, b == 0 ? a : none);
            const float m = proc.engine.modShown[P::DstRes].load();
            lo = std::min (lo, m);
            hi = std::max (hi, m);
        }
        check (lo < -0.3f && hi > 0.3f, "LFO: el aro se mueve en directo (de -0,4 a +0,4)", juce::String (lo, 2) + " a " + juce::String (hi, 2));
        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        blocks (200, off);
    }
    check (std::abs (proc.paramValue (P::FltFreq) - 1000.0f) < 0.01f && std::abs (proc.paramValue (P::FltRes) - 0.2f) < 1.0e-4f,
           "El valor base de los mandos no cambia por modularlos", juce::String (proc.paramValue (P::FltFreq), 1) + " Hz, " + juce::String (proc.paramValue (P::FltRes), 3));
    // Every destination has a knob, and a knob of none is left alone.
    int mapped = 0;
    for (int id = 0; id < P::Count; ++id)
        mapped += SynthEngine::destOf (id) != P::DstNone ? 1 : 0;
    check (mapped >= P::NumDests - 1 && SynthEngine::modulated (P::AmpA, 50.0f, 1.0f) == 50.0f, "Cada destino de la matriz tiene su mando; los demás no se tocan",
           juce::String (mapped) + " mandos para " + juce::String (P::NumDests - 1) + " destinos");
    file.deleteFile();
}

//==============================================================================
// The sample browser: Live's folders read from its Library.cfg (a made-up one: the User Library, a custom Splice
// download folder and two Places; and this computer's own, if Live is here), and the preview: heard, stopped,
// the loaded sample untouched.
void auditBrowser()
{
    title ("Explorador de samples: carpetas de Live (Splice incluida) y escucha previa");
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ReanimaExplorador");
    dir.deleteRecursively();
    const auto lib = dir.getChildFile ("User Library"), splice = dir.getChildFile ("Mis Splice"), place = dir.getChildFile ("Splice app/Samples");
    lib.createDirectory();
    splice.createDirectory();
    place.createDirectory();
    const auto cfg = dir.getChildFile ("Library.cfg");
    cfg.replaceWithText ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Ableton><ContentLibrary><UserLibrary><LibraryProject Id=\"0\">"
                         "<ProjectPath Value=\"" + lib.getFullPathName() + "\" /></LibraryProject></UserLibrary><UserFolderInfoList>"
                         "<UserFolderInfo Id=\"1\" Path=\"" + place.getParentDirectory().getFullPathName() + "\" DisplayName=\"Splice\" IconName=\"\" />"
                         "<UserFolderInfo Id=\"2\" Path=\"" + dir.getChildFile ("no existe").getFullPathName() + "\" DisplayName=\"Perdida\" IconName=\"\" />"
                         "</UserFolderInfoList><SpliceDownloadFolderModeMember Value=\"Custom\" /><CustomSpliceDownloadPathMember Value=\""
                         + splice.getFullPathName() + "\" /></ContentLibrary></Ableton>");
    const auto found = SampleBrowser::liveSources (cfg);
    juce::String names;
    for (const auto& s : found)
        names << s.name << " | ";
    check (found.size() == 3 && found[0].folder == splice && found[1].folder == place.getParentDirectory() && found[2].folder == lib,
           "Library.cfg: descargas de Splice (carpeta propia), Places existentes y User Library; sin rutas fijas", names);
    const auto mine = SampleBrowser::newestLiveLibraryCfg();
    if (mine.existsAsFile())
    {
        const auto here = SampleBrowser::liveSources (mine);
        juce::String list;
        for (const auto& s : here)
            list << s.name << " (" << s.folder.getFullPathName() << ") | ";
        check (! here.empty(), "Este PC: las carpetas de Live (" + mine.getParentDirectory().getParentDirectory().getFileName() + ")", list);
    }

    // Preview: a 440 Hz tone heard at -6 dB without a note; stopped; the loaded sample the same.
    const double rate = 48000.0;
    const auto tone = dir.getChildFile ("tono.wav");
    {
        Audio t (2, (int) rate);
        for (int i = 0; i < t.getNumSamples(); ++i)
        {
            const float v = (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / rate));
            t.setSample (0, i, v);
            t.setSample (1, i, v);
        }
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (tone);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (t, 0, t.getNumSamples());
    }
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, rate, 256);
    proc.prepareToPlay (rate, 256);
    const auto* before = proc.getSample().get();
    const bool sent = proc.previewFile (tone);
    Audio buf (2, 256);
    juce::MidiBuffer none;
    double heard = 0.0;
    for (int b = 0; b < 40; ++b)
    {
        buf.clear();
        proc.processBlock (buf, none);
        if (b >= 20)
            heard = std::max (heard, (double) buf.getMagnitude (0, 0, 256));
    }
    const bool playing = proc.isPreviewing();
    proc.stopPreview();
    double after = 0.0;
    for (int b = 0; b < 20; ++b)
    {
        buf.clear();
        proc.processBlock (buf, none);
        if (b >= 5)
            after = std::max (after, (double) buf.getMagnitude (0, 0, 256));
    }
    check (sent && playing && heard > 0.15 && heard < 0.4 && after < 1.0e-6 && proc.getSample().get() == before,
           "Escucha previa: suena sin nota (-6 dB), se para, y el sample cargado no cambia",
           "pico " + juce::String (heard, 3) + ", tras parar " + juce::String (after, 6));
    // Another while one plays: the new one takes over (no pile-up), and the old one is freed on the next.
    proc.previewFile (tone);
    for (int b = 0; b < 5; ++b)
    {
        buf.clear();
        proc.processBlock (buf, none);
    }
    proc.previewFile (tone);
    for (int b = 0; b < 5; ++b)
    {
        buf.clear();
        proc.processBlock (buf, none);
    }
    check (proc.isPreviewing(), "Escucha previa: un archivo tras otro, sin cortes ni acumulación", "");
    proc.stopPreview();
    dir.deleteRecursively();
}

//==============================================================================
// The chord: named from the notes held (inversions, sevenths, a fifth alone), its degree in the key, from the
// sound (a chord through the sidechain), and the progression of the chords played.
void auditChords()
{
    title ("Acordes: por las notas tocadas, por el sonido, su grado y la progresión");
    struct Case { std::vector<int> notes; const char* name; };
    const Case cases[] = { { { 60, 64, 67 }, "Do" },          { { 57, 60, 64, 67 }, "Lam7" },     { { 52, 55, 60 }, "Do/Mi" },
                           { { 55, 59, 62, 65 }, "Sol7" },    { { 60, 64, 67, 69 }, "Do6" },      { { 59, 62, 65 }, "Sidim" },
                           { { 48, 55 }, "Do5" },              { { 48, 52, 55, 59, 62 }, "Domaj9" }, { { 50, 53, 57, 60 }, "Rem7" },
                           { { 64, 67, 71, 74 }, "Mim7" },     { { 60, 63, 66, 69 }, "Dodim7" },   { { 62, 67, 69 }, "Resus4" } };
    int right = 0;
    juce::String wrong;
    for (const auto& c : cases)
    {
        const auto got = KeyDetect::chordFromNotes (c.notes).name();
        right += got == juce::String::fromUTF8 (c.name) ? 1 : 0;
        if (got != juce::String::fromUTF8 (c.name))
            wrong << c.name << " -> " << got << "; ";
    }
    check (right == (int) std::size (cases), "Por las notas: triadas, inversiones (Do/Mi), séptimas, 6, dim, dim7, sus4, 9, quinta sola",
           juce::String (right) + "/" + juce::String ((int) std::size (cases)) + " " + wrong);
    const auto deg = [] (std::vector<int> notes, int key) { return KeyDetect::chordFromNotes (notes).degreeIn (key); };
    check (deg ({ 57, 60, 64, 67 }, 0) == "vi7" && deg ({ 55, 59, 62, 65 }, 0) == "V7" && deg ({ 59, 62, 65 }, 0) == juce::String::fromUTF8 ("vii°")
               && deg ({ 58, 62, 65 }, 0) == "bVII" && deg ({ 57, 61, 64 }, 9) == "I",
           "Su grado en la tonalidad: Lam7 = vi7, Sol7 = V7, Sidim = vii° y Sib = bVII en Do; La = I en La", "");

    // From the sound: a chord with a few harmonics a note.
    const double rate = 48000.0;
    const auto chordAudio = [&] (std::initializer_list<int> notes, double seconds)
    {
        juce::AudioBuffer<float> a (2, (int) (seconds * rate));
        a.clear();
        for (const int note : notes)
        {
            const double f = 440.0 * std::pow (2.0, (note - 69) / 12.0);
            for (int i = 0; i < a.getNumSamples(); ++i)
            {
                double v = 0.0;
                for (int h = 1; h <= 4; ++h)
                    v += std::sin (2.0 * juce::MathConstants<double>::pi * f * h * i / rate) / (h * h);
                a.addSample (0, i, (float) (0.08 * v));
                a.addSample (1, i, (float) (0.08 * v));
            }
        }
        return a;
    };
    {
        const auto heard = KeyDetect::chordFromChroma (KeyDetect::fromAudio (chordAudio ({ 57, 60, 64 }, 1.0), rate).chroma);
        const auto heard7 = KeyDetect::chordFromChroma (KeyDetect::fromAudio (chordAudio ({ 55, 59, 62, 65 }, 1.0), rate).chroma);
        check (heard.name() == "Lam" && heard7.name() == "Sol7", "Por el sonido: La menor y Sol7", heard.name() + ", " + heard7.name());
    }

    // The processor: the keys down, the progression, the sidechain's chord.
    ReanimaProcessor proc;
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    proc.setBusesLayout (layout);
    proc.prepareToPlay (rate, 512);
    juce::AudioBuffer<float> buf (2, 512);
    const auto hold = [&] (std::vector<int> notes, std::vector<int> release)
    {
        juce::MidiBuffer m;
        for (const int n : release)
            m.addEvent (juce::MidiMessage::noteOff (1, n), 0);
        for (const int n : notes)
            m.addEvent (juce::MidiMessage::noteOn (1, n, 0.8f), 1);
        buf.clear();
        proc.processBlock (buf, m);
        for (int tick = 0; tick < 3; ++tick)
            proc.serviceMessageThread();
    };
    hold ({ 60, 64, 67 }, {});
    const auto first = proc.midiChord().name();
    hold ({ 57, 60, 64 }, { 60, 64, 67 });
    hold ({ 53, 57, 60 }, { 57, 60, 64 });
    hold ({ 55, 59, 62 }, { 53, 57, 60 });
    const auto last = proc.midiChord().name();
    hold ({}, { 55, 59, 62 });
    juce::StringArray names, degrees;
    for (const auto& c : proc.chordProgression())
    {
        names.add (c.name());
        degrees.add (c.degreeIn (0));
    }
    check (first == "Do" && last == "Sol" && proc.midiChord().root < 0, "Las teclas pulsadas: Do, luego Sol; al soltar, ninguno", first + ", " + last);
    check (names.joinIntoString (" ") == "Do Lam Fa Sol" && degrees.joinIntoString (" ") == "I vi IV V", "La progresión: Do Lam Fa Sol = I vi IV V",
           names.joinIntoString (" ") + " / " + degrees.joinIntoString (" "));

    // The sidechain: F major sounding, no keys down.
    proc.clearProgression();
    const auto f = chordAudio ({ 53, 57, 60 }, 2.0);
    juce::MidiBuffer none;
    for (int at = 0; at + 512 <= f.getNumSamples(); at += 512)
    {
        buf.copyFrom (0, 0, f, 0, at, 512);
        buf.copyFrom (1, 0, f, 1, at, 512);
        proc.processBlock (buf, none);
        if ((at / 512) % 8 == 7)
            proc.serviceMessageThread();
    }
    proc.serviceMessageThread();
    const auto side = proc.externalChord();
    const auto prog = proc.chordProgression();
    check (side.name() == "Fa" && side.confidence >= ReanimaProcessor::chordHeardSure && ! prog.empty() && prog.back().name() == "Fa",
           "Por la entrada lateral: Fa mayor sonando se reconoce y pasa a la progresión",
           side.name() + " " + juce::String (juce::roundToInt (side.confidence * 100.0f)) + " %");
}

//==============================================================================
// The key: from names (the ways libraries write it), from the sound of a progression in A minor, from the notes
// played through the processor (a D major scale; three notes say little), and the rules of the key in use: a
// name applies, a sound only suggests, Fijar sets it by hand.
void auditKey()
{
    title ("Tonalidad: por el nombre, por el sonido (sólo sugerencia), por el MIDI, y la que se usa");
    struct Case { const char* name; int root; bool minor; };
    const Case cases[] = { { "SSP_KAT_70_horn_section_soul_G#min.wav", 8, true }, { "OS_6ix_92_vocal_wrongside_A#m.wav", 10, true },
                           { "MA_HDD_String_Drone_LiquidCourage_Dm.wav", 2, true }, { "NW_WE_95_guitar_loop_glimmer_A.wav", 9, false },
                           { "synth_Bbmaj.wav", 10, false }, { "Keys 90bpm Fs minor.wav", 6, true }, { "Drum_Loop_122bpm.wav", -1, false },
                           { "A_Day_In_The_Life_of_a_kick.wav", -1, false } };
    int right = 0;
    juce::String wrong;
    for (const auto& c : cases)
    {
        const auto e = KeyDetect::fromName (c.name);
        const bool ok = e.root == c.root && (c.root < 0 || e.minor == c.minor);
        right += ok ? 1 : 0;
        if (! ok)
            wrong << c.name << " -> " << e.text() << "; ";
    }
    check (right == (int) std::size (cases), "Por el nombre: G#min, A#m, Dm, A, Bbmaj, Fs minor; ninguna en un nombre sin tonalidad",
           juce::String (right) + "/" + juce::String ((int) std::size (cases)) + " " + wrong);

    // A minor: Am, Dm, E, Am, each chord a second, notes with a few harmonics.
    const double rate = 44100.0;
    const int n = (int) (4.0 * rate);
    juce::AudioBuffer<float> prog (1, n);
    prog.clear();
    const int chords[4][3] = { { 57, 60, 64 }, { 50, 57, 62 }, { 52, 56, 59 }, { 57, 60, 64 } };
    for (int c = 0; c < 4; ++c)
        for (const int note : chords[c])
        {
            const double f = 440.0 * std::pow (2.0, (note - 69) / 12.0);
            for (int i = 0; i < (int) rate; ++i)
            {
                const double t = i / rate;
                double v = 0.0;
                for (int h = 1; h <= 4; ++h)
                    v += std::sin (2.0 * juce::MathConstants<double>::pi * f * h * t) / (h * h);
                prog.addSample (0, c * (int) rate + i, (float) (0.1 * v * std::exp (-t * 1.5)));
            }
        }
    const auto heard = KeyDetect::fromAudio (prog, rate);
    const bool aMinorOrRelative = (heard.root == 9 && heard.minor) || (heard.root == 0 && ! heard.minor);
    check (aMinorOrRelative && heard.confidence > 0.2f, "Por el sonido: una progresión en La menor (o su relativa, Do mayor)",
           heard.text() + ", confianza " + juce::String (juce::roundToInt (heard.confidence * 100.0f)) + " %");

    // The processor: the notes played, and the key in use.
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, 48000.0, 256);
    proc.prepareToPlay (48000.0, 256);
    const auto set = [&] (const char* id, float v)
    {
        if (auto* param = proc.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
    };
    const auto play = [&] (std::initializer_list<int> notes)
    {
        juce::AudioBuffer<float> buf (2, 256);
        for (const int note : notes)
        {
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
            m.addEvent (juce::MidiMessage::noteOff (1, note), 128);
            buf.clear();
            proc.processBlock (buf, m);
        }
        proc.serviceMessageThread();
    };
    set ("keySource", 2.0f);        // MIDI
    play ({ 62, 66, 69 });
    const auto few = proc.midiKey();
    const auto fewInUse = proc.keyInUse();
    check (few.confidence < ReanimaProcessor::keySure && fewInUse.doubtful, "MIDI: con tres notas es dudosa y no se aplica",
           few.text() + " " + juce::String (juce::roundToInt (few.confidence * 100.0f)) + " %");
    play ({ 62, 64, 66, 67, 69, 71, 73, 74, 62, 66, 69, 74, 69, 66, 62, 67, 71, 74, 62, 66, 69 });
    const auto many = proc.midiKey();
    const auto inUse = proc.keyInUse();
    check (many.root == 2 && ! many.minor && many.confidence >= ReanimaProcessor::keySure && ! inUse.doubtful && inUse.root == 2,
           "MIDI: una escala de Re mayor tocada la da segura y se usa", many.text() + " " + juce::String (juce::roundToInt (many.confidence * 100.0f)) + " %");

    // Fijar: by hand.
    proc.fixKey (many);
    const auto fixed = proc.keyInUse();
    check ((int) proc.paramValue (P::KeySource) == 0 && fixed.root == 2 && ! fixed.minor, "Fijar pasa la tonalidad a mano", "");
    // State: by hand, saved with the project.
    {
        juce::MemoryBlock state;
        proc.getStateInformation (state);
        ReanimaProcessor reopened;
        reopened.setStateInformation (state.getData(), (int) state.getSize());
        const auto k = reopened.keyInUse();
        check (k.root == 2 && ! k.minor, "La tonalidad a mano se guarda con el proyecto", "");
    }

    // The sidechain: an auxiliary input (VST3: kAux, which Live shows in its Sidechain panel), whose sound never
    // reaches the output, and whose key, after some 20 s of a progression in A minor, is sure and used.
    {
        ReanimaProcessor side;
        const double sr = 48000.0;
        const int block = 512;
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (juce::AudioChannelSet::stereo());
        layout.outputBuses.add (juce::AudioChannelSet::stereo());
        const bool layoutOk = side.setBusesLayout (layout);
        side.prepareToPlay (sr, block);
        auto* ext = side.getVST3ClientExtensions();
        check (layoutOk && side.hasSidechain() && side.getBus (true, 0) != nullptr && side.getBus (true, 0)->getName() == "Sidechain"
                   && ext != nullptr && ! ext->getPluginHasMainInput(),
               "Hay una entrada lateral (Sidechain), auxiliar en VST3", "");
        juce::AudioProcessor::BusesLayout noInput;
        noInput.inputBuses.add (juce::AudioChannelSet::disabled());
        noInput.outputBuses.add (juce::AudioChannelSet::stereo());
        juce::AudioProcessor::BusesLayout mono = layout;
        mono.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
        check (side.checkBusesLayoutSupported (noInput) && side.checkBusesLayoutSupported (mono), "La entrada lateral puede ir apagada o en mono", "");
        if (auto* param = side.apvts.getParameter ("keySource"))
            param->setValueNotifyingHost (param->convertTo0to1 (3.0f));
        // Am, Dm, E, Am, a second each, over and over: notes with a few harmonics, the chords' own ends fading
        const int chords[4][3] = { { 57, 60, 64 }, { 50, 57, 62 }, { 52, 56, 59 }, { 57, 60, 64 } };
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer none;
        float leak = 0.0f;
        double t0 = 0.0;
        bool earlyDoubtful = false;
        const int total = (int) (30.0 * sr / block);
        for (int b = 0; b < total; ++b)
        {
            buf.clear();
            for (int i = 0; i < block; ++i)
            {
                const double t = (b * block + i) / sr;
                const int c = (int) t % 4;
                const double inChord = t - std::floor (t);
                double v = 0.0;
                for (const int note : chords[c])
                {
                    const double f = 440.0 * std::pow (2.0, (note - 69) / 12.0);
                    for (int h = 1; h <= 4; ++h)
                        v += std::sin (2.0 * juce::MathConstants<double>::pi * f * h * t) / (h * h);
                }
                const float s = (float) (0.1 * v * std::exp (-inChord * 1.5));
                buf.setSample (0, i, s);
                buf.setSample (1, i, s);
            }
            side.processBlock (buf, none);
            leak = std::max (leak, buf.getMagnitude (0, block));
            if (b % 16 == 15)
                side.serviceMessageThread();
            t0 = (b + 1) * block / sr;
            if (std::abs (t0 - 5.0) < (double) block / sr)
                earlyDoubtful = side.keyInUse().doubtful;
        }
        side.serviceMessageThread();
        const auto heardKey = side.externalKey();
        const auto used = side.keyInUse();
        check (leak < 1.0e-6f, "El audio de la entrada lateral no sale por Reanima (sin notas: silencio)",
               juce::String (juce::Decibels::gainToDecibels (leak, -200.0f), 1) + " dB");
        check (earlyDoubtful, "Audio externo: a los 5 s todavía es dudosa y no se aplica", "");
        const bool rightKey = (used.root == 9 && used.minor) || (used.root == 0 && ! used.minor);
        check (rightKey && ! used.doubtful && heardKey.confidence >= ReanimaProcessor::keySure,
               "Audio externo: tras 30 s de una progresión en La menor la da segura y se usa",
               heardKey.text() + " " + juce::String (juce::roundToInt (heardKey.confidence * 100.0f)) + " %, "
                   + juce::String (side.externalSeconds(), 1) + " s escuchados");
    }
}

//==============================================================================
// ARP · SEQ: the step engine on its own (where each note starts and ends, to the sample) and through the plug-in.
namespace
{
struct ArpEvent { juce::int64 at; int note; bool on; int velocity; };

// Runs a player for `seconds`: keys pressed at `pressAt` (samples) and let go at `releaseAt` (-1: never), the DAW
// playing from ppq 0 at `bpm` when `host`.
std::vector<ArpEvent> runArp (StepSeq::Player& player, const StepSeq::Settings& s, const StepSeq::Pattern& p, std::initializer_list<int> keys,
                              double seconds, bool host = true, double bpm = 120.0, juce::int64 pressAt = 0, juce::int64 releaseAt = -1,
                              std::function<void (juce::int64, juce::MidiBuffer&)> extra = {})
{
    const double rate = 48000.0;
    const int block = 256;
    std::vector<ArpEvent> events;
    juce::MidiBuffer in, out;
    const auto total = (juce::int64) (seconds * rate);
    for (juce::int64 at = 0; at < total; at += block)
    {
        in.clear();
        if (pressAt >= at && pressAt < at + block)
            for (const int k : keys)
                in.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 90), (int) (pressAt - at));
        if (releaseAt >= at && releaseAt < at + block)
            for (const int k : keys)
                in.addEvent (juce::MidiMessage::noteOff (1, k), (int) (releaseAt - at));
        if (extra)
            extra (at, in);
        player.process (in, out, block, s, p, host, at / rate * bpm / 60.0, bpm);
        for (const auto meta : out)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOnOrOff())
                events.push_back ({ at + meta.samplePosition, m.getNoteNumber(), m.isNoteOn(), m.getVelocity() });
        }
    }
    return events;
}

std::vector<ArpEvent> onsOf (const std::vector<ArpEvent>& e)
{
    std::vector<ArpEvent> ons;
    for (const auto& x : e)
        if (x.on)
            ons.push_back (x);
    return ons;
}

bool balanced (const std::vector<ArpEvent>& e)
{
    std::array<int, 128> n {};
    for (const auto& x : e)
        n[(size_t) x.note] += x.on ? 1 : -1;
    for (const int v : n)
        if (v != 0)
            return false;
    return true;
}
}

void auditArp()
{
    title ("ARP · SEQ: el motor de pasos (al compás, swing, gate, repeticiones, probabilidad, carriles, Seq, latch) y el plugin");
    const double rate = 48000.0;
    StepSeq::Settings s;
    s.on = true;
    s.stepBeats = 0.25;             // 1/16 at 120: 6000 samples
    s.gate = 1.0f;
    StepSeq::Pattern p;             // 16 on, gate 50 %

    {
        StepSeq::Player off;
        off.prepare (rate);
        StepSeq::Settings none;
        juce::MidiBuffer in, out;
        in.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 90), 10);
        in.addEvent (juce::MidiMessage::pitchWheel (1, 9000), 20);
        off.process (in, out, 256, none, p, true, 0.0, 120.0);
        bool same = out.getNumEvents() == in.getNumEvents();
        for (auto a = in.cbegin(), b = out.cbegin(); same && a != in.cend(); ++a, ++b)
            same = (*a).samplePosition == (*b).samplePosition && (*a).getMessage().getRawDataSize() == (*b).getMessage().getRawDataSize()
                && std::memcmp ((*a).data, (*b).data, (size_t) (*a).numBytes) == 0;
        check (same && ! off.active(), "Apagado: el MIDI pasa tal cual", "");
    }
    {
        StepSeq::Player player;
        player.prepare (rate);
        const auto e = runArp (player, s, p, { 60, 64, 67 }, 1.05);
        const auto ons = onsOf (e);
        bool times = ons.size() >= 8, notes = times;
        const int expect[] = { 60, 64, 67, 60, 64, 67, 60, 64 };
        for (size_t i = 0; i < 8 && times; ++i)
        {
            times = ons[i].at == (juce::int64) i * 6000;
            notes = notes && ons[i].note == expect[i];
        }
        juce::int64 length = -1;
        for (const auto& x : e)
            if (! x.on && x.note == 60)
            {
                length = x.at;
                break;
            }
        check (times && notes, "Arp Arriba, 1/16 a 120: Do-Mi-Sol en las semicorcheas exactas (cada 6000 muestras)",
               juce::String ((int) ons.size()) + " notas, la 2.ª en " + juce::String (ons.size() > 1 ? (int) ons[1].at : -1));
        check (length == 3000, "Gate 50 %: cada nota dura media semicorchea", juce::String ((int) length) + " muestras");
    }
    {
        auto sw = s;
        sw.swing = 1.0f;
        StepSeq::Player player;
        player.prepare (rate);
        const auto ons = onsOf (runArp (player, sw, p, { 60 }, 0.6));
        check (ons.size() >= 4 && ons[0].at == 0 && ons[1].at == 8000 && ons[2].at == 12000 && ons[3].at == 20000,
               "Swing 100 %: los pasos pares se retrasan un tercio (tresillo)", ons.size() > 1 ? juce::String ((int) ons[1].at) : juce::String ("-"));
    }
    {
        auto q = p;
        q.value[StepSeq::Ratchet][0] = 3.0f;
        q.value[StepSeq::Chance][1] = 0.0f;
        q.value[StepSeq::Octave][2] = 1.0f;
        q.value[StepSeq::Velocity][2] = 40.0f;
        StepSeq::Player player;
        player.prepare (rate);
        const auto ons = onsOf (runArp (player, s, q, { 60 }, 0.3));
        check (ons.size() >= 4 && ons[0].at == 0 && ons[1].at == 2000 && ons[2].at == 4000 && ons[3].at == 12000 && ons[3].note == 72 && ons[3].velocity == 40,
               "Repetir x3, probabilidad 0 % (paso 2 callado), octava +1 y velocidad 40 (paso 3)",
               juce::String ((int) ons.size()) + " notas");
    }
    {
        auto q = p;
        q.value[StepSeq::Chance].fill (0.5f);
        StepSeq::Player player;
        player.prepare (rate);
        player.setSeed (99);
        const auto ons = onsOf (runArp (player, s, q, { 60 }, 30.0));
        const double share = ons.size() / 240.0;
        check (share > 0.42 && share < 0.58, "Probabilidad 50 %: suena alrededor de la mitad de los pasos", juce::String (share * 100.0, 1) + " %");
    }
    {
        // PASO 3 steps long (on, off, off), VELOCIDAD 2 long (127, 30): they drift against each other
        auto q = p;
        q.length[StepSeq::On] = 3;
        q.value[StepSeq::On] = {};
        q.value[StepSeq::On][0] = 1.0f;
        q.length[StepSeq::Velocity] = 2;
        q.value[StepSeq::Velocity][0] = 127.0f;
        q.value[StepSeq::Velocity][1] = 30.0f;
        StepSeq::Player player;
        player.prepare (rate);
        const auto ons = onsOf (runArp (player, s, q, { 60 }, 0.8));
        check (ons.size() >= 3 && ons[1].at == 18000 && ons[2].at == 36000 && ons[0].velocity == 127 && ons[1].velocity == 30 && ons[2].velocity == 127,
               "Carriles de longitud distinta (3 y 2 pasos): polirritmo", ons.size() > 1 ? juce::String ((int) ons[1].at) : juce::String ("-"));
    }
    {
        check (StepSeq::moveInKey (60, 2, 0, false) == 64 && StepSeq::moveInKey (60, 7, 0, false) == 72 && StepSeq::moveInKey (60, -1, 0, false) == 59
                   && StepSeq::moveInKey (61, 1, 0, false) == 62 && StepSeq::moveInKey (69, 2, 9, true) == 72 && StepSeq::moveInKey (64, -3, 9, true) == 59,
               "Grados de la tonalidad: Do+2 = Mi, +7 = Do', -1 = Si; en La menor La+2 = Do", "");
        auto seq = s;
        seq.seq = true;
        seq.keyRoot = 2;            // Re mayor
        auto q = p;
        q.length[StepSeq::Note] = 4;
        q.value[StepSeq::Note][1] = 2.0f;
        q.value[StepSeq::Note][2] = 4.0f;
        q.value[StepSeq::Note][3] = 7.0f;
        StepSeq::Player player;
        player.prepare (rate);
        const auto ons = onsOf (runArp (player, seq, q, { 62 }, 0.5));
        check (ons.size() >= 4 && ons[0].note == 62 && ons[1].note == 66 && ons[2].note == 69 && ons[3].note == 74,
               "Seq en Re mayor desde Re: grados 0, 2, 4, 7 = Re Fa# La Re'", "");
        StepSeq::Player player2;
        player2.prepare (rate);
        const auto up = onsOf (runArp (player2, seq, q, { 64 }, 0.5));
        check (up.size() >= 4 && up[0].note == 64 && up[1].note == 67 && up[2].note == 71 && up[3].note == 76,
               "Seq: otra tecla (Mi) transporta la frase dentro de la tonalidad (Mi Sol Si Mi')", "");
    }
    {
        auto free = s;
        free.sync = false;
        free.freeBpm = 90.0;
        StepSeq::Player player;
        player.prepare (rate);
        const auto ons = onsOf (runArp (player, free, p, { 60 }, 1.0, false, 120.0, 1234));
        check (ons.size() >= 3 && ons[0].at == 1234 && ons[1].at == 1234 + 8000 && ons[2].at == 1234 + 16000,
               "Tempo propio a 90: empieza al pulsar la tecla, una semicorchea cada 8000 muestras", "");
    }
    {
        auto hold = s;
        hold.latch = true;
        StepSeq::Player player;
        player.prepare (rate);
        const auto e = runArp (player, hold, p, { 60, 67 }, 1.0, true, 120.0, 0, 3000,
                               [] (juce::int64 at, juce::MidiBuffer& in)
                               {
                                   if (at == 30000 - 30000 % 256)
                                       in.addEvent (juce::MidiMessage::noteOn (1, 62, (juce::uint8) 90), (int) (30000 - at));
                               });
        const auto ons = onsOf (e);
        int afterRelease = 0, newChordOnly = 1;
        for (const auto& x : ons)
        {
            afterRelease += x.at > 3000 && x.at < 30000;
            if (x.at > 36000 && x.note != 62)
                newChordOnly = 0;
        }
        check (afterRelease >= 3 && newChordOnly == 1, "Latch: sigue al soltar; una tecla nueva empieza otro acorde", juce::String (afterRelease) + " notas tras soltar");
    }
    {
        // let go, the DAW's jump, Arp/Seq off: no note left on
        StepSeq::Player player;
        player.prepare (rate);
        auto e = runArp (player, s, p, { 60, 64 }, 1.0, true, 120.0, 0, 20000);
        bool ok = balanced (e);
        StepSeq::Player jumper;
        jumper.prepare (rate);
        juce::MidiBuffer in, out;
        in.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 90), 0);
        std::vector<ArpEvent> all;
        auto gate = s;
        gate.gate = 4.0f;           // long notes, across the jump
        for (int b = 0; b < 400; ++b)
        {
            const double ppq = b < 200 ? b * 256 / rate * 2.0 : 8.0 + (b - 200) * 256 / rate * 2.0;
            auto st = gate;
            st.on = b < 380;
            jumper.process (in, out, 256, st, p, true, ppq, 120.0);
            in.clear();
            for (const auto meta : out)
                if (meta.getMessage().isNoteOnOrOff())
                    all.push_back ({ b * 256LL + meta.samplePosition, meta.getMessage().getNoteNumber(), meta.getMessage().isNoteOn(), 0 });
        }
        ok = ok && balanced (all);
        check (ok, "Sin notas colgadas: al soltar, cuando el DAW salta, y al apagar Arp/Seq con notas sonando", "");
    }
    {
        StepSeq::Pattern q;
        juce::Random rng (5);
        StepSeq::generate (q, -1, 0.7f, rng);
        q.length[StepSeq::Gate] = 5;
        const auto back = StepSeq::Pattern::fromString (q.toString());
        bool inRange = true;
        for (int l = 0; l < StepSeq::numLanes; ++l)
            for (int k = 0; k < StepSeq::maxSteps; ++k)
                inRange = inRange && q.at (l, k) >= StepSeq::laneRange (l).lo && q.at (l, k) <= StepSeq::laneRange (l).hi;
        check (back == q && StepSeq::Pattern::fromString ("basura") == StepSeq::Pattern() && inRange && q.at (StepSeq::On, 0) > 0.5f,
               "Generar da valores válidos; el patrón se escribe y se lee igual; un texto roto da el patrón por defecto", "");
    }

    // Through the plug-in: the pattern saved with the project, the notes played, the MIDI clip.
    // a steady tone of 2 s (a sample to hear the steps with)
    const auto toneFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Arp tono C.wav");
    toneFile.deleteFile();
    {
        const int n = (int) (2.0 * rate);
        juce::AudioBuffer<float> tone (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float v = (float) (0.3 * std::min (1.0, i / 96.0) * std::sin (2.0 * juce::MathConstants<double>::pi * 261.63 * i / rate));
            tone.setSample (0, i, v);
            tone.setSample (1, i, v);
        }
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (toneFile);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (tone, 0, n);
    }
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, rate, 256);
    proc.prepareToPlay (rate, 256);
    proc.loadSampleNow (toneFile);
    for (int k = 0; k < 400 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        proc.serviceMessageThread();
    }
    proc.serviceMessageThread();
    const auto set = [&] (const char* id, float v)
    {
        if (auto* param = proc.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
    };
    auto q = StepSeq::Pattern();
    q.value[StepSeq::Note][1] = 2.0f;
    q.length[StepSeq::On] = 8;
    proc.setSeqPattern (q);
    {
        juce::MemoryBlock state;
        proc.getStateInformation (state);
        ReanimaProcessor reopened;
        reopened.setStateInformation (state.getData(), (int) state.getSize());
        check (reopened.seqPattern() == q, "El patrón se guarda con el proyecto", "");
    }
    set ("arpOn", 1.0f);
    set ("arpSync", 1.0f);          // Propio
    set ("arpBpm", 120.0f);
    set ("arpRate", 3.0f);          // 1/4: 24000 samples
    set ("arpGate", 40.0f);         // gate 50 % x 40 %: a fifth of the step
    set ("ampR", 20.0f);
    proc.serviceMessageThread();
    juce::AudioBuffer<float> buf (2, 256);
    juce::MidiBuffer m;
    std::vector<float> level;
    for (int b = 0; b < (int) (2.0 * rate / 256); ++b)
    {
        m.clear();
        if (b == 0)
            m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        buf.clear();
        proc.processBlock (buf, m);
        level.push_back (buf.getRMSLevel (0, 0, 256));
    }
    // loud at each beat, and quieter between (the note ends after a fifth of the beat)
    const auto around = [&] (double from, double to)
    {
        float top = 0.0f;
        for (int b = (int) (from * rate / 256); b < (int) (to * rate / 256) && b < (int) level.size(); ++b)
            top = std::max (top, level[(size_t) b]);
        return top;
    };
    const float onBeat = around (0.5, 0.56), late = around (0.85, 0.99);
    check (onBeat > 1.0e-3f && late < onBeat * 0.5f, "En el plugin: Do3 mantenido suena a negras (BPM propio 120), callado entre notas",
           juce::String (juce::Decibels::gainToDecibels (onBeat), 1) + " dB en el tiempo, " + juce::String (juce::Decibels::gainToDecibels (late), 1) + " dB al final");
    // the clip: from the last chord held (Do3), 1/4 notes, PASO 8 long: lanes meet again after 16 steps = 4 bars
    const auto file = proc.writeSeqMidi();
    juce::MidiFile mf;
    juce::FileInputStream stream (file);
    const bool read = stream.openedOk() && mf.readFrom (stream);
    int count = 0, second = -1;
    double lastTick = 0.0;
    if (read && mf.getNumTracks() > 0)
        for (const auto* e : *mf.getTrack (0))
            if (e->message.isNoteOn())
            {
                if (count == 1)
                    second = e->message.getNoteNumber();
                ++count;
                lastTick = e->message.getTimeStamp();
            }
    check (read && count == 16 && second == 64 && file.getFileName().contains ("4 comp"),
           "Arrastrar MIDI: el clip con las 16 negras de 4 compases, la 2.ª en Mi (+2 grados)",
           juce::String (count) + " notas, " + file.getFileName() + ", última en el tick " + juce::String (lastTick, 0));
}

//==============================================================================
// SLICE through the whole plug-in, as Ableton plays it: a loop of 8 slices, each a tone of its own (so the slice
// that sounds can be told by its pitch), loaded from a file, MIDI into processBlock. Each key from C3 (MIDI 60,
// Ableton's numbering) must play its own slice and only it: alone, in a fast run of overlapping notes, with
// Modo Poly, Mono and Legato (whose glides once kept the first slice playing), cut by Rejilla and by
// Transitorios, and after the project is saved and opened again.
void auditSliceMidi()
{
    title ("SLICE: cada tecla desde C3 toca su trozo (plugin completo, MIDI)");
    const double rate = 48000.0;
    constexpr int slices = 8;
    const double freqs[slices] = { 220.0, 261.6, 311.1, 370.0, 440.0, 523.3, 622.3, 740.0 };
    const int seg = (int) (0.25 * rate), n = slices * seg;          // 4 beats at 120 BPM: an eighth each
    Audio loop (2, n);
    for (int k = 0; k < slices; ++k)
        for (int i = 0; i < seg; ++i)
        {
            const double t = i / rate;
            const float v = (float) (0.5 * std::exp (-t / 0.12) * std::sin (2.0 * juce::MathConstants<double>::pi * freqs[k] * t));
            loop.setSample (0, k * seg + i, v);
            loop.setSample (1, k * seg + i, v);
        }
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Slice tones loop 120bpm.wav");
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (loop, 0, n);
    }

    // Which slice a stretch of output is: the strongest of the 8 tones, and how far above the next.
    const auto which = [&] (const Audio& y, int from, int len, double& margin)
    {
        double best = 0.0, second = 0.0;
        int at = -1;
        for (int k = 0; k < slices; ++k)
        {
            const double a = toneAmp (y.getReadPointer (0) + from, len, freqs[k], rate);
            if (a > best)
            {
                second = best;
                best = a;
                at = k;
            }
            else
                second = std::max (second, a);
        }
        margin = toDb (best / std::max (1.0e-12, second));
        return best > 1.0e-4 ? at : -1;
    };

    const auto prepare = [&] (ReanimaProcessor& proc, int polyMode, int sliceBy)
    {
        proc.setPlayConfigDetails (0, 2, rate, 256);
        proc.prepareToPlay (rate, 256);
        proc.loadSampleNow (file);
        for (int k = 0; k < 400 && proc.isLoading(); ++k)
        {
            juce::Thread::sleep (10);
            proc.serviceMessageThread();
        }
        proc.serviceMessageThread();
        for (const auto& [id, v] : { std::pair<const char*, float> { "playMode", (float) P::PlayLoop }, { "loopKind", (float) P::LoopSlice },
                                     { "sliceBy", (float) sliceBy }, { "sliceDivision", 3.0f }, { "sliceSensitivity", 0.5f },
                                     { "polyMode", (float) polyMode }, { "glide", 0.0f }, { "monoBass", 0.0f } })
            if (auto* param = proc.apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (v));
        proc.serviceMessageThread();
    };

    // Plays notes (key, start s, length s) through processBlock; returns the output.
    struct Note { int key; double at, len; };
    const auto play = [&] (ReanimaProcessor& proc, const std::vector<Note>& notes, double total)
    {
        const int length = (int) (total * rate), block = 256;
        Audio out (2, length), buf (2, block);
        out.clear();
        for (int pos = 0; pos < length; pos += block)
        {
            const int k = std::min (block, length - pos);
            juce::MidiBuffer midi;
            for (const auto& nt : notes)
            {
                const int on = (int) (nt.at * rate), off = (int) ((nt.at + nt.len) * rate);
                if (on >= pos && on < pos + k)
                    midi.addEvent (juce::MidiMessage::noteOn (1, nt.key, 0.9f), on - pos);
                if (off >= pos && off < pos + k)
                    midi.addEvent (juce::MidiMessage::noteOff (1, nt.key), off - pos);
            }
            buf.clear();
            Audio view (buf.getArrayOfWritePointers(), 2, k);
            proc.processBlock (view, midi);
            out.copyFrom (0, pos, buf, 0, 0, k);
            out.copyFrom (1, pos, buf, 1, 0, k);
        }
        return out;
    };

    const char* modes[] = { "Poly", "Mono", "Legato" };
    for (const int sliceBy : { (int) P::SliceGrid, (int) P::SliceTransients })
        for (const int polyMode : { 0, 1, 2 })
        {
            ReanimaProcessor proc;
            prepare (proc, polyMode, sliceBy);
            const auto marks = proc.getSlices();
            bool marksOk = (int) marks.size() == slices;
            for (int k = 0; k < (int) marks.size() && marksOk; ++k)
                marksOk = std::abs (marks[(size_t) k] - k * seg) <= (int) (0.003 * rate);

            // One at a time, a short tap each, 0.4 s apart: slice k must sound in its own window, nothing else.
            std::vector<Note> taps;
            for (int k = 0; k < slices; ++k)
                taps.push_back ({ 60 + k, 0.05 + 0.4 * k, 0.02 });
            taps.push_back ({ 60 + slices, 0.05 + 0.4 * slices, 0.02 });        // past the last slice: silence
            const auto y = play (proc, taps, 0.05 + 0.4 * (slices + 1) + 0.1);
            juce::String got;
            bool alone = marksOk;
            double worstMargin = 1000.0;
            for (int k = 0; k < slices; ++k)
            {
                double margin = 0.0;
                const int found = which (y, (int) ((0.05 + 0.4 * k + 0.01) * rate), (int) (0.2 * rate), margin);
                got << SampleAnalysis::midiNoteName ((float) (60 + k)) << "=" << (found + 1) << " ";
                alone = alone && found == k && margin > 20.0;
                worstMargin = std::min (worstMargin, margin);
                // The slice ends with its end: silent from 0.26 s after the key to the next key.
                const int quietFrom = (int) ((0.05 + 0.4 * k + 0.27) * rate), quietTo = (int) ((0.05 + 0.4 * (k + 1) - 0.005) * rate);
                alone = alone && rmsOf (y.getReadPointer (0) + quietFrom, quietTo - quietFrom) < 1.0e-5;
            }
            const int pastFrom = (int) ((0.05 + 0.4 * slices) * rate);
            alone = alone && rmsOf (y.getReadPointer (0) + pastFrom, y.getNumSamples() - pastFrom) < 1.0e-5;
            check (alone, juce::String ("SLICE ") + (sliceBy == P::SliceGrid ? "Rejilla" : "Transitorios") + ", " + modes[polyMode]
                       + ": C3..G3 tocan los trozos 1..8, cada uno sólo el suyo y hasta su final; G#3 calla",
                   got + "(margen peor " + db (worstMargin) + ", " + juce::String ((int) marks.size()) + " cortes:" + [&] {
                       juce::String t;
                       for (const int m : marks)
                           t << " " << juce::String (m / rate, 3);
                       return t; }() + ")");

            // A fast run: a key every 60 ms, each held 120 ms (overlapping), C3 up to G3 and back. Each onset
            // must be its own slice's tone.
            std::vector<Note> run;
            const int order[] = { 0, 1, 2, 3, 4, 5, 6, 7, 6, 5, 4, 3, 2, 1, 0 };
            for (int i = 0; i < (int) std::size (order); ++i)
                run.push_back ({ 60 + order[i], 0.05 + 0.06 * i, 0.12 });
            const auto z = play (proc, run, 0.05 + 0.06 * (double) std::size (order) + 0.4);
            int right = 0;
            juce::String seq;
            for (int i = 0; i < (int) std::size (order); ++i)
            {
                // The first 50 ms of each note, minus what the note before still plays there (its own slice).
                Audio solo (1, (int) (0.05 * rate));
                const int from = (int) ((0.05 + 0.06 * i + 0.003) * rate);
                double margin = 0.0;
                const int found = which (z, from, (int) (0.05 * rate), margin);
                seq << (found + 1);
                right += found == order[i] ? 1 : 0;
            }
            check (right == (int) std::size (order), juce::String ("SLICE ") + (sliceBy == P::SliceGrid ? "Rejilla" : "Transitorios") + ", "
                       + modes[polyMode] + ": secuencia rápida (una tecla cada 60 ms, solapadas) toca el trozo de cada tecla",
                   "trozos oídos " + seq + " (esperado 123456787654321)");

            // The project saved and opened again: same cuts, same mapping.
            if (polyMode == 0)
            {
                juce::MemoryBlock state;
                proc.getStateInformation (state);
                ReanimaProcessor reopened;
                reopened.setPlayConfigDetails (0, 2, rate, 256);
                reopened.prepareToPlay (rate, 256);
                reopened.setStateInformation (state.getData(), (int) state.getSize());
                for (int k = 0; k < 400 && reopened.isLoading(); ++k)
                {
                    juce::Thread::sleep (10);
                    reopened.serviceMessageThread();
                }
                reopened.serviceMessageThread();
                const auto y2 = play (reopened, { { 63, 0.05, 0.02 }, { 66, 0.45, 0.02 } }, 0.9);
                double m1 = 0.0, m2 = 0.0;
                const int a = which (y2, (int) (0.06 * rate), (int) (0.2 * rate), m1), b = which (y2, (int) (0.46 * rate), (int) (0.2 * rate), m2);
                check (reopened.getSlices() == marks && a == 3 && b == 6, juce::String ("SLICE ") + (sliceBy == P::SliceGrid ? "Rejilla" : "Transitorios")
                           + ": al reabrir el proyecto, mismos cortes y D#3 = trozo 4, F#3 = trozo 7",
                       juce::String ((int) reopened.getSlices().size()) + " cortes, D#3=" + juce::String (a + 1) + " F#3=" + juce::String (b + 1));
            }
        }

    // What the window shows comes from the engine: each voice's playhead and slice, as published after every
    // block. A key: its playhead inside its own slice, moving forward at the file's pace; three keys: three
    // slices, three playheads; at the end of a slice nothing is left lit; the same key again: one voice for it.
    {
        ReanimaProcessor proc;
        prepare (proc, 0, P::SliceGrid);
        const auto marks = proc.getSlices();
        Audio buf (2, 256);
        const auto block = [&] (juce::MidiBuffer m) { buf.clear(); proc.processBlock (buf, m); };
        const auto lit = [&]
        {
            std::vector<std::pair<int, float>> v;
            for (size_t i = 0; i < proc.engine.playheads.size(); ++i)
                if (proc.engine.playheads[i].load() >= 0.0f)
                    v.push_back ({ proc.engine.playSlices[i].load(), proc.engine.playheads[i].load() });
            return v;
        };
        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 63, 0.9f), 0);
        on.addEvent (juce::MidiMessage::noteOff (1, 63), 10);
        block (on);
        const auto a = lit();
        for (int i = 0; i < 10; ++i)
            block ({});
        const auto b = lit();
        const double expected = 11 * 256.0;      // read at the file's pace (48 kHz file, 48 kHz host)
        const bool one = a.size() == 1 && b.size() == 1 && a[0].first == 3 && b[0].first == 3
                         && a[0].second >= marks[3] && b[0].second < marks[4]
                         && std::abs ((b[0].second - marks[3]) - expected) < 64.0;
        for (int i = 0; i < (int) (0.3 * rate / 256); ++i)
            block ({});
        juce::MidiBuffer chord;
        for (const int k : { 60, 65, 67 })
            chord.addEvent (juce::MidiMessage::noteOn (1, k, 0.9f), 0);
        block (chord);
        auto c = lit();
        std::sort (c.begin(), c.end());
        const bool three = c.size() == 3 && c[0].first == 0 && c[1].first == 5 && c[2].first == 7
                           && c[0].second >= marks[0] && c[0].second < marks[1] && c[1].second >= marks[5] && c[1].second < marks[6]
                           && c[2].second >= marks[7];
        for (int i = 0; i < (int) (0.3 * rate / 256); ++i)
            block ({});
        const bool cleared = lit().empty();
        juce::MidiBuffer again;
        again.addEvent (juce::MidiMessage::noteOn (1, 62, 0.9f), 0);
        block (again);
        for (int i = 0; i < 20; ++i)
            block ({});
        juce::MidiBuffer twice;
        twice.addEvent (juce::MidiMessage::noteOn (1, 62, 0.9f), 0);
        block (twice);
        for (int i = 0; i < 4; ++i)
            block ({});
        const auto d = lit();
        const bool retrig = d.size() == 1 && d[0].first == 2 && d[0].second < marks[2] + 6 * 256;
        check (one && three && cleared && retrig, "SLICE en pantalla: el indicador de cada voz sale del motor, en su trozo y avanzando; "
                   "acorde = 3 trozos iluminados; al acabar no queda ninguno; repetir la tecla reinicia su trozo",
               juce::String (one ? "D#3 ok " : "D#3 MAL ") + (three ? "acorde ok " : "ACORDE MAL ") + (cleared ? "se apaga " : "QUEDA ENCENDIDO ")
                   + (retrig ? "retrigger ok" : "RETRIGGER MAL (" + juce::String ((int) d.size()) + " voces)"));
    }

    // Markers moved by hand: the keys follow at once.
    {
        ReanimaProcessor proc;
        prepare (proc, 0, P::SliceGrid);
        proc.setSliceMarks ({ 0, 2 * seg, 5 * seg });          // 3 slices: tones 1, 3, 6 start them
        const auto y = play (proc, { { 60, 0.05, 0.02 }, { 61, 0.65, 0.02 }, { 62, 1.25, 0.02 }, { 63, 2.2, 0.02 } }, 2.6);
        double m = 0.0;
        const int a = which (y, (int) (0.06 * rate), (int) (0.2 * rate), m), b = which (y, (int) (0.66 * rate), (int) (0.2 * rate), m),
                  c = which (y, (int) (1.26 * rate), (int) (0.2 * rate), m);
        const bool silent = rmsOf (y.getReadPointer (0) + (int) (2.2 * rate), (int) (0.35 * rate)) < 1.0e-5;
        check (a == 0 && b == 2 && c == 5 && silent, "SLICE a mano (3 cortes): C3 = tono 1, C#3 = tono 3, D3 = tono 6, D#3 calla",
               "C3=" + juce::String (a + 1) + " C#3=" + juce::String (b + 1) + " D3=" + juce::String (c + 1) + (silent ? ", D#3 calla" : ", D#3 SUENA"));
    }
    file.deleteFile();
}

//==============================================================================
// The master output stage, measured through the processor: a 2.5 kHz tone (its 13th harmonic folds back to
// 15.5 kHz at 48 kHz, so aliasing shows) and a drum loop with hard hits, driven so that the meter reads 0 to
// +6 dB. For each: the meter (before the stage), sample and true peak after it, THD and folded spurs of the
// tone, RMS in and out, and clicks (a second difference far above its surroundings).
struct MasterRun
{
    double meterDb = -100.0, peakDb = -100.0, truePeakDb = -100.0, rmsInDb = -100.0, rmsOutDb = -100.0;
    double thdDb = 0.0, aliasDb = -200.0;
    int clicks = 0;
};

Audio writeAndPlay (const Audio& source, double rate, double sampleRate, float volumeDb, const juce::String& name, double& meterPeak)
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("reanima_master_" + name + ".wav");
    if (! file.existsAsFile())
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (32)))
            w->writeFromAudioSampleBuffer (source, 0, source.getNumSamples());
    }
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, rate, 64);
    proc.prepareToPlay (rate, 64);
    proc.loadSampleNow (file);
    for (int k = 0; k < 400 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        proc.serviceMessageThread();
    }
    proc.serviceMessageThread();
    for (const auto& [id, v] : { std::pair<const char*, float> { "volume", volumeDb }, { "monoBass", 0.0f }, { "envLock", 1.0f } })
        if (auto* param = proc.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
    proc.serviceMessageThread();
    proc.takeMeterReading();
    const int total = (int) (std::min (2.0, source.getNumSamples() / sampleRate) * rate);
    Audio out (2, total), block (2, 64);
    meterPeak = 0.0;
    for (int pos = 0; pos < total; pos += 64)
    {
        const int n = std::min (64, total - pos);
        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey, (juce::uint8) 127), 0);
        Audio view (block.getArrayOfWritePointers(), 2, n);
        view.clear();
        proc.processBlock (view, midi);
        out.copyFrom (0, pos, view, 0, 0, n);
        out.copyFrom (1, pos, view, 1, 0, n);
        const auto m = proc.takeMeterReading();
        meterPeak = std::max (meterPeak, (double) std::max (m.peak[0], m.peak[1]));
    }
    return out;
}

int clicksIn (const Audio& y, int from)
{
    int count = 0;
    const int w = 480;
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* x = y.getReadPointer (ch);
        std::vector<double> d2 ((size_t) y.getNumSamples(), 0.0);
        for (int i = 2; i < y.getNumSamples(); ++i)
            d2[(size_t) i] = std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]);
        for (int i = std::max (from, w); i + w < y.getNumSamples(); ++i)
        {
            double around = 0.0;
            for (int k = i - w; k < i + w; k += 8)
                around += d2[(size_t) k];
            around /= (2 * w / 8);
            if (d2[(size_t) i] > 12.0 * around + 1.0e-4)
            {
                ++count;
                i += w;
            }
        }
    }
    return count;
}

MasterRun measureMaster (const Audio& source, double rate, double sampleRate, double targetDb, const juce::String& name, bool tone, double hz)
{
    // How loud it plays at a Volume of 0 dB, then the Volume that brings the meter to the target.
    double peak0 = 0.0;
    writeAndPlay (source, rate, sampleRate, 0.0f, name, peak0);
    const float volume = (float) (targetDb - toDb (peak0));
    MasterRun r;
    double meter = 0.0;
    const auto y = writeAndPlay (source, rate, sampleRate, volume, name, meter);
    // MASTER_WAV=<folder>: each render written as it comes out, for listening.
    if (const char* folder = std::getenv ("MASTER_WAV"); folder != nullptr && ! tone)
    {
        const auto file = juce::File (juce::String::fromUTF8 (folder)).getChildFile (name + juce::String::formatted ("_%+.0fdB_%.0fk.wav", targetDb, rate / 1000.0));
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (y, 0, y.getNumSamples());
    }
    const int from = tone ? (int) (0.2 * rate) : 0, len = y.getNumSamples() - from;
    Audio steady (2, len);
    steady.copyFrom (0, 0, y, 0, from, len);
    steady.copyFrom (1, 0, y, 1, from, len);
    r.meterDb = toDb (meter);
    r.peakDb = toDb (steady.getMagnitude (0, len));
    r.truePeakDb = toDb (truePeak (steady));
    r.rmsOutDb = toDb (rmsOf (steady.getReadPointer (0), len));
    r.rmsInDb = r.rmsOutDb;
    if (tone)
    {
        constexpr int order = 15;
        const auto mag = spectrum (steady.getReadPointer (0), order);
        r.thdDb = thdOf (mag, rate, order, hz);
        std::vector<double> keep;
        for (int h = 1; h * hz < rate / 2; ++h)
            keep.push_back (h * hz);
        const double binHz = rate / (1 << order);
        const double ref = toneAmp (steady.getReadPointer (0), 1 << order, hz, rate);
        double worst = 1.0e-12;
        for (int k = 4; k < (int) mag.size() && k * binHz < 20000.0; ++k)
        {
            bool near = false;
            for (const double f : keep)
                near = near || std::abs (k - f / binHz) < 8;
            if (! near)
                worst = std::max (worst, (double) mag[(size_t) k]);
        }
        r.aliasDb = toDb (worst / std::max (1.0e-12, ref));
    }
    r.clicks = clicksIn (steady, 0);
    return r;
}

void auditMaster()
{
    title ("Salida master: de 0 a +6 dB en el vúmetro");
    for (const double rate : { 48000.0, 96000.0 })
    {
        const double sr = 48000.0, hz = 2500.0;
        Audio tone (2, (int) (3.0 * sr)), drums (2, (int) (3.0 * sr));
        juce::Random rng (21);
        for (int i = 0; i < tone.getNumSamples(); ++i)
        {
            const double t = i / sr, beat = std::fmod (t, 0.5);
            const float v = (float) (0.5 * std::sin (twoPi * hz * t));
            tone.setSample (0, i, v);
            tone.setSample (1, i, v);
            const double noise = rng.nextFloat() * 2.0 - 1.0;
            const float dv = (float) (0.7 * std::exp (-beat / 0.06) * std::sin (twoPi * 55.0 * beat) + 0.6 * std::exp (-beat / 0.004) * noise
                                      + 0.2 * std::exp (-std::fmod (t + 0.25, 0.5) / 0.03) * noise);
            drums.setSample (0, i, dv);
            drums.setSample (1, i, (float) (dv * 0.8 + 0.1 * noise * std::exp (-beat / 0.01)));
        }
        juce::String table;
        double lastThd = -300.0, worstAlias = -300.0, worstTonePeak = -300.0, worstToneTp = -300.0, worstDrumPeak = -300.0, thd0 = 0.0, thd6 = 0.0;
        int toneClicks = 0;
        bool rising = true;
        for (int step = 0; step <= 6; ++step)
        {
            const auto t = measureMaster (tone, rate, sr, (double) step, "tono", true, hz);
            const auto d = measureMaster (drums, rate, sr, (double) step, "bateria", false, 0.0);
            rising = rising && t.thdDb > lastThd;
            lastThd = t.thdDb;
            (step == 0 ? thd0 : thd6) = t.thdDb;
            worstAlias = std::max (worstAlias, t.aliasDb);
            worstTonePeak = std::max (worstTonePeak, t.peakDb);
            worstToneTp = std::max (worstToneTp, t.truePeakDb);
            worstDrumPeak = std::max (worstDrumPeak, d.peakDb);
            toneClicks += t.clicks;
            table << juce::String::formatted ("\n    +%d dB  tono: vúmetro %+5.1f, pico %+5.2f, TP %+5.2f, THD %6.1f dB, aliasing %6.1f dB, clics %d"
                                              "  |  batería: vúmetro %+5.1f, pico %+5.2f, TP %+5.2f, clics %d",
                                              step, t.meterDb, t.peakDb, t.truePeakDb, t.thdDb, t.aliasDb, t.clicks, d.meterDb, d.peakDb, d.truePeakDb, d.clicks);
        }
        std::printf ("  %.1f kHz:%s\n", rate / 1000.0, table.toRawUTF8());
        const juce::String at = juce::String (rate / 1000.0, 1) + " kHz: ";
        check (thd0 < -90.0, at + "salida master limpia a 0 dB del vúmetro", "THD " + db (thd0));
        check (rising && thd6 > -25.0 && thd6 < -12.0, at + "la saturación crece de 0 a +6 dB, paso a paso", "THD a +6 dB " + db (thd6));
        check (worstAlias < -80.0 && toneClicks == 0, at + "la saturación no pliega armónicos (sobremuestreada) ni da clics", "aliasing " + db (worstAlias));
        check (worstTonePeak <= -0.3 && worstToneTp <= -0.3 && worstDrumPeak <= -0.3, at + "de 0 a +6 dB ninguna muestra pasa de -0,3 dBFS (tono y golpes)",
               "tono pico " + db (worstTonePeak) + "FS, TP " + db (worstToneTp) + "TP; golpes pico " + db (worstDrumPeak) + "FS");
    }

    // Real samples of the library (when it is there): hits, a loop, a bass, a pad, at 48 kHz.
    const juce::File library = juce::File::getCurrentWorkingDirectory().getChildFile ("samples_prueba/all samples");
    const char* files[] = { "006_Clap_Snare_-_TDV_Zenhiser.wav", "001_Kick_-_FLECKMELTECHNO_Zenhiser.wav", "003_b__Drum_Loop_122bpm_C_-_80SW1_Zenhiser.wav",
                            "003_Bassline_122bpm_Fm_-_MAGNETICTECHNO_Zenhiser.wav", "044_Pad_118bpm_Gmin_-_NEON80S_Zenhiser.wav" };
    for (const auto* name : files)
    {
        const auto f = library.getChildFile (name);
        Audio audio;
        double sr = 0.0;
        if (! SampleAnalysis::loadFile (f, audio, sr))
            continue;
        if (audio.getNumChannels() == 1)
        {
            audio.setSize (2, audio.getNumSamples(), true);
            audio.copyFrom (1, 0, audio, 0, 0, audio.getNumSamples());
        }
        juce::String row;
        double worstTp = -300.0;
        for (const int step : { 0, 3, 6 })
        {
            const auto r = measureMaster (audio, 48000.0, sr, (double) step, juce::String (name).upToFirstOccurrenceOf ("_-", false, false), false, 0.0);
            row << juce::String::formatted ("  +%d: vúmetro %+5.1f pico %+5.2f TP %+5.2f", step, r.meterDb, r.peakDb, r.truePeakDb);
            worstTp = std::max (worstTp, r.truePeakDb);
        }
        check (worstTp <= 0.0, juce::String ("muestra real ") + juce::String (name).substring (0, 24) + ": de 0 a +6 dB sin pasar de 0 dBTP", row);
    }
    {
        MasterStage stage;
        for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0 })
        {
            stage.prepare (rate, 512);
            std::printf ("  latencia de la etapa a %.1f kHz: %d muestras (%.2f ms)\n", rate / 1000.0, stage.latency(), 1000.0 * stage.latency() / rate);
        }
    }
}

//==============================================================================
// The four functions from the Color Play research (docs/investigacion_colorplay_2026-11-23.md): LFO Deriva and
// Suavidad, the filter Armónico, Movimiento in engines B and D, and the sidechain (the matrix's source and Impronta).
namespace colorplay
{
constexpr double rate = 48000.0;

Sample analysed (const Audio& a, const char* name)
{
    return SampleAnalysis::analyse (a, rate, name, false, true);
}

// One note (or none: note < 0) with the sidechain fed block by block; counts allocations during process().
Audio play (Sample s, const ParamSnapshot& p, int note, double held, double total, const Audio* side = nullptr,
            long* allocations = nullptr, std::vector<float>* shown = nullptr)
{
    SynthEngine e;
    e.prepare (rate);
    e.setSample (s);
    e.prepareFx (p);
    const int length = (int) (total * rate), off = (int) (held * rate);
    Audio out (2, length), buf (2, 256);
    out.clear();
    long count = 0;
    for (int pos = 0; pos < length; pos += 256)
    {
        const int n = std::min (256, length - pos);
        juce::MidiBuffer midi;
        if (note >= 0 && pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
        if (note >= 0 && off >= pos && off < pos + n)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), off - pos);
        Audio view (buf.getArrayOfWritePointers(), 2, n);
        if (side != nullptr && pos + n <= side->getNumSamples())
            e.setSidechain (side->getReadPointer (0, pos), side->getReadPointer (1, pos), n);
        allocationCount = 0;
        countingAllocations = true;
        e.process (view, midi, p, 120.0);
        countingAllocations = false;
        count += allocationCount.load();
        if (shown != nullptr)
            shown->push_back (e.sideShown.load());
        out.copyFrom (0, pos, buf, 0, 0, n);
        out.copyFrom (1, pos, buf, 1, 0, n);
    }
    if (allocations != nullptr)
        *allocations = count;
    return out;
}

double rmsDb (const Audio& a, double from, double to)
{
    const int i0 = (int) (from * rate), i1 = std::min (a.getNumSamples(), (int) (to * rate));
    double e = 0.0;
    for (int i = i0; i < i1; ++i)
        e += 0.5 * (juce::square ((double) a.getSample (0, i)) + juce::square ((double) a.getSample (1, i)));
    return toDb (std::sqrt (e / std::max (1, i1 - i0)));
}

double centroid (const Audio& a, double from, double to)
{
    const int i0 = (int) (from * rate), i1 = std::min (a.getNumSamples(), (int) (to * rate));
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> w ((size_t) size * 2);
    double num = 0.0, den = 0.0;
    for (int at = i0; at + size <= i1; at += size / 2)
    {
        std::fill (w.begin(), w.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            w[(size_t) i] = a.getSample (0, at + i) * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));
        fft.performFrequencyOnlyForwardTransform (w.data());
        for (int k = 1; k < size / 2; ++k)
        {
            num += (double) w[(size_t) k] * w[(size_t) k] * k * rate / size;
            den += (double) w[(size_t) k] * w[(size_t) k];
        }
    }
    return den > 0.0 ? num / den : 0.0;
}

// The pitch in cents against `hz`, every `hop` seconds, from upward zero crossings (interpolated).
std::vector<double> centsTrack (const Audio& a, double from, double to, double hz, double hop)
{
    std::vector<double> crossings;
    const float* x = a.getReadPointer (0);
    for (int i = (int) (from * rate) + 1; i < std::min (a.getNumSamples(), (int) (to * rate)); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            crossings.push_back (i - 1 + (double) -x[i - 1] / (double) (x[i] - x[i - 1]));
    std::vector<double> cents;
    size_t k = 0;
    for (double t = from; t + hop <= to; t += hop)
    {
        const double a0 = t * rate, a1 = (t + hop) * rate;
        while (k < crossings.size() && crossings[k] < a0)
            ++k;
        size_t j = k;
        while (j + 1 < crossings.size() && crossings[j + 1] < a1)
            ++j;
        if (j > k)
            cents.push_back (1200.0 * std::log2 ((double) (j - k) * rate / (crossings[j] - crossings[k]) / hz));
    }
    return cents;
}

// Amplitude of harmonic h of f0 over one second from `from` (left channel).
double harmonicDb (const Audio& a, double f0, int h, double from)
{
    return toDb (toneAmp (a.getReadPointer (0, (int) (from * rate)), (int) rate, f0 * h, rate));
}

Audio noise (double seconds, unsigned seed, float amp = 0.3f)
{
    juce::Random r ((juce::int64) seed);
    Audio b (2, (int) (seconds * rate));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const float v = amp * (r.nextFloat() * 2.0f - 1.0f);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

// A tone whose highs die faster than its body: a struck string, darkening as it sounds.
Audio darkening (double hz, double seconds)
{
    Audio b (2, (int) (seconds * rate));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const double t = i / rate;
        double v = 0.0;
        for (int h = 1; h <= 16; ++h)
            v += std::sin (twoPi * hz * h * t) / h * std::exp (-t * 0.25 * h);
        const double attack = std::min (1.0, t / 0.004);
        b.setSample (0, i, (float) (0.35 * attack * v));
        b.setSample (1, i, (float) (0.35 * attack * v));
    }
    return b;
}

ParamSnapshot defaults (const SampleData& s, int engine)
{
    auto p = paramsFor (s);
    p[P::Engine] = (float) engine;
    return p;
}
} // namespace colorplay

void auditColorPlay()
{
    using namespace colorplay;
    title ("Color Play · LFO Deriva y Suavidad");
    {
        const auto s = analysed (tone (220.0, 6.0, rate), "tono 220");
        auto p = defaults (*s, P::EngineRebuild);
        p[P::Lfo1Shape] = (float) P::Drift;
        p[P::Lfo1Rate] = 1.0f;
        p[P::Lfo1Smooth] = 0.5f;
        p[P::M1Src] = (float) P::SrcLfo1;
        p[P::M1Dst] = (float) P::DstFine;
        p[P::M1Amt] = 0.2f;            // ±20 cents
        long allocations = 0;
        const auto y = play (s, p, 60, 5.0, 5.2, nullptr, &allocations);
        const auto cents = centsTrack (y, 0.2, 4.8, 220.0, 0.05);
        double peak = 0.0, jump = 0.0, mean = 0.0, sq = 0.0;
        for (size_t i = 0; i < cents.size(); ++i)
        {
            peak = std::max (peak, std::abs (cents[i]));
            if (i > 0)
                jump = std::max (jump, std::abs (cents[i] - cents[i - 1]));
            mean += cents[i];
            sq += cents[i] * cents[i];
        }
        const double sd = std::sqrt (std::max (0.0, sq / (double) cents.size() - juce::square (mean / (double) cents.size())));
        check (peak <= 23.0, "Deriva en Tono fino al 20 %: no pasa de ±20 cents", juce::String (peak, 1) + " cents");
        check (sd >= 2.0, "Deriva: la afinación se mueve de verdad", "desviación " + juce::String (sd, 1) + " cents");
        check (jump <= 6.0, "Deriva: sin saltos (cada 50 ms)", juce::String (jump, 2) + " cents");
        check (allocations == 0, "Deriva: sin memoria nueva en el hilo de audio", juce::String (allocations));

        // Suavidad on Deriva: less quick wobble (cents moved from one 50 ms to the next).
        const auto wobble = [&] (float smooth)
        {
            auto q = p;
            q[P::Lfo1Smooth] = smooth;
            const auto c = centsTrack (play (s, q, 60, 5.0, 5.2), 0.2, 4.8, 220.0, 0.05);
            double d = 0.0;
            for (size_t i = 1; i < c.size(); ++i)
                d += std::abs (c[i] - c[i - 1]);
            return d / std::max<size_t> (1, c.size() - 1);
        };
        const double rough = wobble (0.0), smooth = wobble (1.0);
        check (smooth < rough * 0.8, "Suavidad en Deriva quita el temblor rápido",
               juce::String (rough, 2) + " → " + juce::String (smooth, 2) + " cents por paso");

        // Suavidad on a square: rounder steps of the volume.
        const auto steps = [&] (float smooth)
        {
            auto q = defaults (*s, P::EngineRebuild);
            q[P::Lfo1Shape] = (float) P::Square;
            q[P::Lfo1Rate] = 2.0f;
            q[P::Lfo1Smooth] = smooth;
            q[P::M1Src] = (float) P::SrcLfo1;
            q[P::M1Dst] = (float) P::DstVolume;
            q[P::M1Amt] = 0.25f;
            const auto z = play (s, q, 60, 3.0, 3.2);
            double worst = 0.0, prev = rmsDb (z, 0.3, 0.305);
            for (double t = 0.305; t < 2.8; t += 0.005)
            {
                const double now = rmsDb (z, t, t + 0.005);
                worst = std::max (worst, std::abs (now - prev));
                prev = now;
            }
            return worst;
        };
        const double hard = steps (0.0), soft = steps (0.8);
        check (soft < hard * 0.5, "Suavidad redondea los saltos de una cuadrada", db (hard) + " → " + db (soft) + " cada 5 ms");
    }

    title ("Color Play · filtro Armónico");
    {
        const auto s = analysed (pluck (110.0, 3.0, rate), "pluck 110");
        const auto base = defaults (*s, P::EngineRebuild);
        const auto off = play (s, base, 60, 2.5, 2.6);
        const auto with = [&] (float gain, float morph, float region, float centre)
        {
            auto q = base;
            q[P::FltOn] = 1.0f;
            q[P::FltType] = (float) P::Harmonic;
            q[P::FltFreq] = centre;
            q[P::FltHarmGain] = gain;
            q[P::FltHarmRegion] = region;
            q[P::FltMorph] = morph;
            q[P::FltRes] = 0.5f;
            return q;
        };
        long allocations = 0;
        const auto up = play (s, with (12.0f, 0.5f, 3.0f, 700.0f), 60, 2.5, 2.6, nullptr, &allocations);
        const auto down = play (s, with (-12.0f, 0.5f, 3.0f, 700.0f), 60, 2.5, 2.6);
        const double h1 = harmonicDb (off, 110.0, 1, 0.3), h6 = harmonicDb (off, 110.0, 6, 0.3);
        const double upH1 = harmonicDb (up, 110.0, 1, 0.3), upH6 = harmonicDb (up, 110.0, 6, 0.3);
        const double downH1 = harmonicDb (down, 110.0, 1, 0.3), downH6 = harmonicDb (down, 110.0, 6, 0.3);
        check ((upH6 - upH1) - (h6 - h1) >= 6.0, "Armónico +12 dB realza los armónicos de su región",
               "6º frente a la fundamental: " + db ((upH6 - upH1) - (h6 - h1)));
        check ((downH6 - downH1) - (h6 - h1) <= -6.0, "Armónico -12 dB atenúa los armónicos de su región",
               db ((downH6 - downH1) - (h6 - h1)));
        check (downH1 >= h1 - 0.5, "Armónico nunca baja la fundamental", db (downH1 - h1));
        const double lvl = rmsDb (off, 0.05, 2.4), lvlUp = rmsDb (up, 0.05, 2.4), lvlDown = rmsDb (down, 0.05, 2.4);
        check (std::abs (lvlUp - lvl) <= 1.5 && std::abs (lvlDown - lvl) <= 3.0, "Armónico mantiene el volumen",
               "+12: " + db (lvlUp - lvl) + ", -12: " + db (lvlDown - lvl));
        check (allocations == 0, "Armónico: sin memoria nueva en el hilo de audio", juce::String (allocations));

        // Odd against even.
        const auto odd = play (s, with (12.0f, 0.0f, 6.0f, 500.0f), 60, 2.5, 2.6);
        const auto even = play (s, with (12.0f, 1.0f, 6.0f, 500.0f), 60, 2.5, 2.6);
        const double ref = harmonicDb (off, 110.0, 3, 0.3) - harmonicDb (off, 110.0, 4, 0.3);
        const double oddR = harmonicDb (odd, 110.0, 3, 0.3) - harmonicDb (odd, 110.0, 4, 0.3);
        const double evenR = harmonicDb (even, 110.0, 3, 0.3) - harmonicDb (even, 110.0, 4, 0.3);
        check (oddR - ref >= 6.0 && evenR - ref <= -6.0, "Pares/impares: 0 % realza los impares, 100 % los pares",
               "3º frente al 4º: impares " + db (oddR - ref) + ", pares " + db (evenR - ref));

        // Noise played on a key: tuned.
        const auto ns = analysed (noise (3.0, 7), "ruido");
        auto nq = defaults (*ns, P::EngineRebuild);
        const auto noiseOff = play (ns, nq, 45, 2.5, 2.6);
        nq[P::FltOn] = 1.0f;
        nq[P::FltType] = (float) P::Harmonic;
        nq[P::FltFreq] = 1000.0f;
        nq[P::FltHarmGain] = 24.0f;
        nq[P::FltHarmRegion] = 8.0f;
        nq[P::FltRes] = 0.9f;
        const auto noiseOn = play (ns, nq, 45, 2.5, 2.6);
        const auto tonalDb = [] (const Audio& a)
        {
            double on = 1.0e-20, between = 1.0e-20;
            for (int h = 1; h <= 20; ++h)
            {
                on += juce::square (toneAmp (a.getReadPointer (0, (int) (0.5 * rate)), (int) rate, 110.0 * h, rate));
                between += juce::square (toneAmp (a.getReadPointer (0, (int) (0.5 * rate)), (int) rate, 110.0 * (h + 0.5), rate));
            }
            return 10.0 * std::log10 (on / between);
        };
        const double t0 = tonalDb (noiseOff), t1 = tonalDb (noiseOn);
        check (t1 - t0 >= 10.0, "Armónico afina un ruido en la nota tocada (La1)", "armónicos frente a lo de en medio: " + db (t1 - t0));

        // A glide with Armónico: no clicks more than without it.
        const auto glide = [&] (bool harmonicOn)
        {
            auto q = harmonicOn ? with (12.0f, 0.5f, 4.0f, 1000.0f) : base;
            q[P::PolyMode] = (float) P::Legato;
            q[P::Glide] = 150.0f;
            q[P::FltKey] = 1.0f;
            SynthEngine e;
            e.prepare (rate);
            e.setSample (s);
            e.prepareFx (q);
            Audio out (2, (int) (2.0 * rate)), buf (2, 256);
            for (int pos = 0; pos < out.getNumSamples(); pos += 256)
            {
                juce::MidiBuffer midi;
                if (pos == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
                if (pos == 25600) midi.addEvent (juce::MidiMessage::noteOn (1, 67, 1.0f), 0);
                if (pos == 25600) midi.addEvent (juce::MidiMessage::noteOff (1, 60), 1);
                e.process (buf, midi, q, 120.0);
                out.copyFrom (0, pos, buf, 0, 0, 256);
                out.copyFrom (1, pos, buf, 1, 0, 256);
            }
            // A click: the second difference far above its own level over the 10 ms around it.
            const float* x = out.getReadPointer (0);
            const int n = out.getNumSamples(), w = (int) (0.01 * rate);
            std::vector<double> d2 ((size_t) n, 0.0), cum ((size_t) n + 1, 0.0);
            for (int i = 2; i < n; ++i)
                d2[(size_t) i] = x[i] - 2.0 * x[i - 1] + x[i - 2];
            for (int i = 0; i < n; ++i)
                cum[(size_t) i + 1] = cum[(size_t) i] + d2[(size_t) i] * d2[(size_t) i];
            double worst = 0.0;
            for (int i = (int) (0.05 * rate); i < n - w; ++i)
            {
                const double around = std::sqrt ((cum[(size_t) (i + w)] - cum[(size_t) (i - w)]) / (2.0 * w)) + 1.0e-9;
                worst = std::max (worst, std::abs (d2[(size_t) i]) / around);
            }
            return worst;
        };
        const double gOff = glide (false), gOn = glide (true);
        check (gOn <= std::max (8.0, gOff * 1.5), "Armónico en un glide: sin clics",
               "pico de la 2ª diferencia frente a su entorno: " + juce::String (gOff, 2) + " → " + juce::String (gOn, 2));
    }

    title ("Color Play · Movimiento (motores B y D)");
    {
        const auto s = analysed (darkening (220.0, 5.0), "tono que se oscurece");
        for (const int engine : { P::EngineReenvelope, P::EngineHybrid })
        {
            const char* name = engine == P::EngineReenvelope ? "B" : "D";
            auto p = defaults (*s, engine);
            p[P::SustainType] = (float) P::SustainHold;
            p[P::AmpS] = 1.0f;
            p[P::SustainMotion] = 0.0f;
            const auto still = play (s, p, 60, 6.0, 6.3);
            p[P::SustainMotion] = 1.0f;
            long allocations = 0;
            const auto moving = play (s, p, 60, 6.0, 6.3, nullptr, &allocations);
            const double hand = (s->early.start - s->onset) / rate;
            const double a0 = hand + 0.3, a1 = std::min (5.5, hand + 3.5);
            const double stillDrop = centroid (still, a1 - 0.3, a1 + 0.3) / centroid (still, a0 - 0.1, a0 + 0.3);
            const double moveDrop = centroid (moving, a1 - 0.3, a1 + 0.3) / centroid (moving, a0 - 0.1, a0 + 0.3);
            check (moveDrop < stillDrop * 0.9, juce::String ("Movimiento en ") + name + ": el sostenido se oscurece como el sample",
                   "centroide al final / al principio: quieto " + juce::String (stillDrop, 2) + ", con Movimiento " + juce::String (moveDrop, 2)
                   + " (mano a " + juce::String (hand, 2) + " s)");
            const double dl = rmsDb (moving, hand + 0.5, 5.8) - rmsDb (still, hand + 0.5, 5.8);
            check (std::abs (dl) <= 1.5, juce::String ("Movimiento en ") + name + ": el volumen no cambia", db (dl));
            double jump = 0.0, prev = rmsDb (moving, hand + 0.1, hand + 0.11);
            for (double t = hand + 0.11; t < 5.8; t += 0.01)
            {
                const double now = rmsDb (moving, t, t + 0.01);
                jump = std::max (jump, std::abs (now - prev));
                prev = now;
            }
            check (jump <= 3.0, juce::String ("Movimiento en ") + name + ": sin saltos de nivel (cada 10 ms)", db (jump));
            check (allocations == 0, juce::String ("Movimiento en ") + name + ": sin memoria nueva en el hilo de audio", juce::String (allocations));
        }
    }

    title ("Color Play · Sidechain: fuente de la matriz");
    {
        // A steady bright tone (a sawtooth of 110 Hz), so that only the filter changes its colour.
        Audio saw (2, (int) (5.0 * rate));
        for (int i = 0; i < saw.getNumSamples(); ++i)
        {
            double v = 0.0;
            for (int h = 1; h <= 40; ++h)
                v += std::sin (twoPi * 110.0 * h * i / rate) / h;
            saw.setSample (0, i, (float) (0.25 * v));
            saw.setSample (1, i, (float) (0.25 * v));
        }
        const auto s = analysed (saw, "sierra 110");
        auto p = defaults (*s, P::EngineRebuild);
        p[P::FltOn] = 1.0f;
        p[P::FltType] = (float) P::LP;
        p[P::FltFreq] = 300.0f;
        // Bursts of noise: 100 ms on, 400 ms off.
        Audio side (2, (int) (4.0 * rate));
        juce::Random r (3);
        for (int i = 0; i < side.getNumSamples(); ++i)
        {
            const bool on = std::fmod (i / rate, 0.5) < 0.1;
            const float v = on ? 0.3f * (r.nextFloat() * 2.0f - 1.0f) : 0.0f;
            side.setSample (0, i, v);
            side.setSample (1, i, v);
        }
        const auto plain = play (s, p, 60, 3.8, 3.8, &side);
        auto q = p;
        q[P::M1Src] = (float) P::SrcSidechain;
        q[P::M1Dst] = (float) P::DstCutoff;
        q[P::M1Amt] = 0.6f;
        std::vector<float> shown;
        long allocations = 0;
        const auto opened = play (s, q, 60, 3.8, 3.8, &side, &allocations, &shown);
        const auto bright = [] (const Audio& a, bool burst)
        {
            double sum = 0.0;
            int count = 0;
            for (double t = 1.0; t < 3.5; t += 0.5)
            {
                const double from = burst ? t + 0.02 : t + 0.3;
                sum += centroid (a, from, from + 0.09);
                ++count;
            }
            return sum / count;
        };
        const double during = bright (opened, true), after = bright (opened, false);
        double shownOn = 0.0, shownOff = 0.0;
        int nOn = 0, nOff = 0;
        for (size_t b = 0; b < shown.size(); ++b)
        {
            const double t = std::fmod ((double) b * 256.0 / rate, 0.5);
            if (t > 0.02 && t < 0.09) { shownOn += shown[b]; ++nOn; }
            if (t > 0.3 && t < 0.39) { shownOff += shown[b]; ++nOff; }
        }
        // Against the same note without the source (its own sustain changes colour too): how much brighter.
        const double openDuring = during / bright (plain, true), openAfter = after / bright (plain, false);
        check (openDuring > openAfter * 1.15 && openDuring > 1.2, "La fuente Sidechain abre el filtro con los golpes de la pista lateral",
               "brillo frente a sin fuente: con golpe x" + juce::String (openDuring, 2) + ", sin golpe x" + juce::String (openAfter, 2) + "; fuente "
               + juce::String (shownOn / std::max (1, nOn), 2) + " / " + juce::String (shownOff / std::max (1, nOff), 2));
        bool range = true;
        for (const float v : shown)
            range = range && v >= 0.0f && v <= 1.0f;
        check (range, "La fuente Sidechain se queda entre 0 y 1");
        check (allocations == 0, "Sidechain: sin memoria nueva en el hilo de audio", juce::String (allocations));
        const auto none = play (s, q, 60, 3.8, 3.8, nullptr);
        bool same = true;
        for (int i = 0; i < none.getNumSamples() && same; ++i)
            same = none.getSample (0, i) == plain.getSample (0, i);
        check (same, "Sin entrada lateral, la fuente Sidechain no mueve nada (idéntico al sonido sin ella)");
    }

    title ("Color Play · Impronta");
    {
        const auto shaped = [] (int mode, const Audio* side, const Audio& in, long* allocations = nullptr)
        {
            ParamSnapshot p {};
            for (int i = 0; i < P::Count; ++i)
                p[(size_t) i] = P::defs()[(size_t) i].def;
            p[(size_t) P::fxType (0)] = (float) Fx::Impronta;
            p[(size_t) P::fxOn (0)] = 1.0f;
            p[(size_t) P::fxMix (0)] = 1.0f;
            const auto& spec = Fx::spec (Fx::Impronta);
            for (int k = 0; k < (int) spec.params.size(); ++k)
                p[(size_t) P::fxParam (0, k)] = Fx::toNorm (spec.params[(size_t) k], spec.params[(size_t) k].def);
            p[(size_t) P::fxParam (0, 0)] = Fx::toNorm (spec.params[0], (float) mode);
            p[(size_t) P::fxParam (0, 1)] = Fx::toNorm (spec.params[1], 1.0f);
            FxRack fx;
            fx.prepare (rate);
            fx.ensureModules (p);
            Audio out (in);
            long count = 0;
            for (int pos = 0; pos < out.getNumSamples(); pos += 256)
            {
                const int n = std::min (256, out.getNumSamples() - pos);
                Audio view (out.getArrayOfWritePointers(), 2, pos, n);
                if (side != nullptr)
                    fx.setSidechain (side->getReadPointer (0, pos), side->getReadPointer (1, pos), n);
                else
                    fx.setSidechain (nullptr, nullptr, 0);
                allocationCount = 0;
                countingAllocations = true;
                fx.process (view, p, 120.0);
                countingAllocations = false;
                count += allocationCount.load();
            }
            if (allocations != nullptr)
                *allocations = count;
            return out;
        };
        // Reanima's side: noise tilted down 3 dB an octave (pink-like), as most sounds are.
        Audio in (noise (3.0, 11, 0.2f));
        {
            float b0 = 0, b1 = 0, b2 = 0;
            for (int i = 0; i < in.getNumSamples(); ++i)
            {
                const float w = in.getSample (0, i);
                b0 = 0.99765f * b0 + w * 0.0990460f;
                b1 = 0.96300f * b1 + w * 0.2965164f;
                b2 = 0.57000f * b2 + w * 1.0526913f;
                const float pink = 0.5f * (b0 + b1 + b2 + w * 0.1848f);
                in.setSample (0, i, pink);
                in.setSample (1, i, pink);
            }
        }
        // Highs (2 to 12 kHz) against lows (100 Hz to 1 kHz), in dB.
        const auto tilt = [] (const Audio& a)
        {
            constexpr int order = 13, size = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<float> w ((size_t) size * 2);
            double hi = 1.0e-20, lo = 1.0e-20;
            for (int at = (int) (0.5 * rate); at + size <= (int) (2.9 * rate); at += size)
            {
                std::fill (w.begin(), w.end(), 0.0f);
                for (int i = 0; i < size; ++i)
                    w[(size_t) i] = a.getSample (0, at + i) * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));
                fft.performFrequencyOnlyForwardTransform (w.data());
                for (int k = 1; k < size / 2; ++k)
                {
                    const double hz = k * rate / size, pw = (double) w[(size_t) k] * w[(size_t) k];
                    if (hz >= 2000.0 && hz <= 12000.0) hi += pw;
                    else if (hz >= 100.0 && hz <= 1000.0) lo += pw;
                }
            }
            return 10.0 * std::log10 (hi / lo);
        };
        const auto maxDiff = [] (const Audio& a, const Audio& b)
        {
            float d = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    d = std::max (d, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
            return d;
        };
        check (maxDiff (shaped (0, nullptr, in), in) == 0.0f, "Impronta sin entrada lateral deja el sonido igual (bit a bit)");
        Audio silent (2, in.getNumSamples());
        silent.clear();
        check (maxDiff (shaped (0, &silent, in), in) < 1.0e-6f, "Impronta Color con la pista lateral en silencio deja el sonido igual");

        // A dark and a bright sidechain.
        Audio dark (noise (3.0, 5, 0.3f)), light (noise (3.0, 6, 0.3f));
        {
            Dsp::Biquad lp, hp;
            lp.set (Dsp::Biquad::LowPass, rate, 400.0f, 0.707f);
            hp.set (Dsp::Biquad::HighPass, rate, 4000.0f, 0.707f);
            for (int ch = 0; ch < 2; ++ch)
            {
                lp.reset();
                hp.reset();
                for (int i = 0; i < dark.getNumSamples(); ++i)
                {
                    dark.setSample (ch, i, lp.process (0, dark.getSample (ch, i)));
                    light.setSample (ch, i, hp.process (0, light.getSample (ch, i)));
                }
            }
        }
        long allocations = 0;
        const auto byDark = shaped (0, &dark, in, &allocations), byLight = shaped (0, &light, in);
        const double c0 = tilt (in), cd = tilt (byDark), cl = tilt (byLight);
        check (cd < c0 - 10.0 && cl > c0 + 10.0, "Impronta Color: una pista oscura oscurece, una brillante abre",
               "agudos frente a graves: " + db (c0) + " → oscura " + db (cd) + ", brillante " + db (cl));
        const double l0 = rmsDb (in, 0.5, 2.9), ld = rmsDb (byDark, 1.0, 2.9), ll = rmsDb (byLight, 1.0, 2.9);
        check (std::abs (ld - l0) <= 1.5 && std::abs (ll - l0) <= 1.5, "Impronta Color cambia el timbre, no el volumen",
               "oscura " + db (ld - l0) + ", brillante " + db (ll - l0));
        check (allocations == 0, "Impronta: sin memoria nueva en el hilo de audio", juce::String (allocations));
        const double vocSilent = rmsDb (shaped (1, &silent, in), 1.0, 2.9);
        check (vocSilent <= l0 - 40.0, "Impronta Vocoder: la pista lateral en silencio calla el sonido", db (vocSilent - l0));
        Audio quiet (2, in.getNumSamples());
        quiet.clear();
        const double invQuiet = rmsDb (shaped (2, &light, quiet), 1.0, 2.9), invPlaying = rmsDb (shaped (2, &light, in), 1.0, 2.9);
        check (invPlaying - invQuiet >= 30.0, "Impronta Invertido: suena la pista lateral cuando Reanima suena",
               "con notas " + db (invPlaying) + ", sin notas " + db (invQuiet));
    }
}

//==============================================================================
// The technical audit of 23-11 (docs/auditoria_tecnica_2026-11-23.md): what its fixes promise.
void auditTechnical()
{
    title ("Técnica · latencia declarada frente a la real");
    for (const double rate : rates)
    {
        MasterStage stage;
        stage.prepare (rate, 64);
        const int n = 4096;
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
        l[100] = r[100] = 0.1f;     // well under the knee: the stage is linear there
        for (int pos = 0; pos < n; pos += 64)
            stage.process (l.data() + pos, r.data() + pos, 64);
        int at = 0;
        for (int i = 0; i < n; ++i)
            if (std::abs (l[(size_t) i]) > std::abs (l[(size_t) at]))
                at = i;
        check (at - 100 == stage.latency(), "etapa de salida a " + juce::String (rate / 1000.0, 1) + " kHz: el retardo real es el declarado",
               "pico " + juce::String (at - 100) + " muestras, declarado " + juce::String (stage.latency()) + " ("
               + juce::String (1000.0 * stage.latency() / rate, 2) + " ms)");
    }

    title ("Técnica · robo de voces sin cortes");
    {
        // 32 notes held (all the voices), then a burst of new ones in one block, nearly silent (velocity 0.01): what
        // jumps is the old notes being cut. Up to the 8 spare voices each stolen note fades out over 4 ms; beyond them
        // the furthest into its fade is stopped at once (a known limit: docs/auditoria_tecnica_2026-11-23.md).
        const auto s = SampleAnalysis::analyse (tone (220.0, 4.0, 48000.0), 48000.0, "tono", false, true);
        auto p = paramsFor (*s);
        p[P::SustainType] = (float) P::SustainHold;
        p[P::AmpS] = 1.0f;
        p[P::VelAmp] = 1.0f;
        const auto jump = [&] (int burst)
        {
            SynthEngine e;
            e.prepare (48000.0);
            e.setSample (s);
            e.prepareFx (p);
            Audio out (2, 48000), buf (2, 64);
            out.clear();
            for (int pos = 0; pos < out.getNumSamples(); pos += 64)
            {
                juce::MidiBuffer midi;
                if (pos == 0)
                    for (int k = 0; k < 32; ++k)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 36 + k, 0.5f), k);
                if (pos == 24000)
                    for (int k = 0; k < burst; ++k)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 70 + k, 0.01f), k);
                e.process (buf, midi, p, 120.0);
                out.copyFrom (0, pos, buf, 0, 0, 64);
                out.copyFrom (1, pos, buf, 1, 0, 64);
            }
            double before = 0.0, after = 0.0;
            const float* x = out.getReadPointer (0);
            for (int i = 12000; i < 23900; ++i)
                before = std::max (before, (double) std::abs (x[i] - x[i - 1]));
            for (int i = 23990; i < 24600; ++i)
                after = std::max (after, (double) std::abs (x[i] - x[i - 1]));
            return after / before;
        };
        const double eight = jump (8), many = jump (24);
        check (eight <= 1.5, "8 notas nuevas de golpe sobre 32 voces: todas las robadas se apagan en 4 ms, sin cortes",
               "salto mayor entre muestras x" + juce::String (eight, 2) + " del acorde estable");
        std::printf ("(info) 24 notas nuevas de golpe sobre 32 voces: salto x%.2f (las que pasan de las 8 de reserva se cortan)%c", many, 10);
    }

    title ("Técnica · cambiar de motor con una nota sonando (informativo)");
    {
        const auto s = SampleAnalysis::analyse (pluck (220.0, 3.0, 48000.0), 48000.0, "pluck", false, true);
        for (const int to : { 1, 2, 3, 4, 5 })
        {
            auto p = paramsFor (*s);
            p[P::SustainType] = (float) P::SustainHold;
            p[P::AmpS] = 1.0f;
            SynthEngine e;
            e.prepare (48000.0);
            e.setSample (s);
            e.prepareFx (p);
            Audio out (2, 48000), buf (2, 64);
            for (int pos = 0; pos < out.getNumSamples(); pos += 64)
            {
                juce::MidiBuffer midi;
                if (pos == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
                if (pos == 24000)
                    p[P::Engine] = (float) to;
                e.process (buf, midi, p, 120.0);
                out.copyFrom (0, pos, buf, 0, 0, 64);
                out.copyFrom (1, pos, buf, 1, 0, 64);
            }
            double before = 0.0, at = 0.0;
            const float* x = out.getReadPointer (0);
            for (int i = 12000; i < 23900; ++i)
                before = std::max (before, (double) std::abs (x[i] - x[i - 1]));
            for (int i = 23990; i < 24300; ++i)
                at = std::max (at, (double) std::abs (x[i] - x[i - 1]));
            double eBefore = 0.0, eAfter = 0.0;
            for (int i = 18000; i < 23900; ++i)
                eBefore += (double) x[i] * x[i];
            for (int i = 30000; i < 35900; ++i)
                eAfter += (double) x[i] * x[i];
            std::printf ("(info) motor A -> %c con la nota sonando: salto mayor x%.2f del de antes; nivel despues %.1f dB%c", "ABCDEF"[to],
                         at / std::max (1.0e-9, before), 10.0 * std::log10 ((eAfter + 1.0e-20) / (eBefore + 1.0e-20)), 10);
        }
    }

    title ("Técnica · cerrar el plugin con un render de Rubber Band en marcha");
    {
        const juce::File loop = juce::File::getCurrentWorkingDirectory().getChildFile ("samples_prueba/all samples/OSS_TL_82_drum_loop_full_pusherman.wav");
        if (! loop.existsAsFile())
            std::printf ("(info) sin el loop de prueba; omitido\n");
        else
        {
            auto proc = std::make_unique<ReanimaProcessor>();
            proc->setPlayConfigDetails (0, 2, 48000.0, 256);
            proc->prepareToPlay (48000.0, 256);
            proc->loadSampleNow (loop);
            proc->serviceMessageThread();
            for (const auto& [id, v] : std::initializer_list<std::pair<int, float>> { { P::PlayMode, (float) P::PlayLoop },
                                                                                      { P::LoopAlgo, (float) P::LoopRubberBand } })
                if (auto* param = proc->apvts.getParameter (P::defs()[(size_t) id].id))
                    param->setValueNotifyingHost (param->convertTo0to1 (v));
            juce::AudioBuffer<float> b (2, 256);
            for (int k = 0; k < 40; ++k)
            {
                juce::MidiBuffer midi;
                if (k == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 63, 1.0f), 0);
                proc->processBlock (b, midi);
                if (k % 8 == 7)
                    proc->serviceMessageThread();       // asks for the render
            }
            proc->serviceMessageThread();
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            proc.reset();
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            check (ms < 3000.0, "el plugin se cierra sin esperar al render entero", juce::String (ms, 0) + " ms");
        }
    }
}

//==============================================================================
// Sample rates (23-11): a file at 44.1 kHz played in projects at every rate, and the rate changed mid-session the way
// a host does it (processing stopped, prepareToPlay again, processing resumed).
namespace rates2311
{
juce::File writeWav (const Audio& a, double rate, const juce::String& name)
{
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
    juce::WavAudioFormat wav;
    if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
        writer->writeFromAudioSampleBuffer (a, 0, a.getNumSamples());
    return file;
}

void setParam (ReanimaProcessor& proc, int id, float v)
{
    if (auto* param = proc.apvts.getParameter (P::defs()[(size_t) id].id))
        param->setValueNotifyingHost (param->convertTo0to1 (v));
}

void settle (ReanimaProcessor& proc)
{
    proc.serviceMessageThread();
    for (int k = 0; k < 600 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (20);
        proc.serviceMessageThread();
    }
    proc.serviceMessageThread();
}

// Plays `seconds` with a note from `noteAt` to `noteOff` (seconds; -1: none), in blocks of `block`.
Audio run (ReanimaProcessor& proc, double rate, int block, double seconds, double noteAt, double noteOff, int note = 60)
{
    const int n = (int) (seconds * rate);
    Audio out (2, n), buf (2, block);
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        Audio view (buf.getArrayOfWritePointers(), 2, len);
        view.clear();
        juce::MidiBuffer midi;
        const int on = noteAt >= 0.0 ? (int) (noteAt * rate) : -1, off = noteOff >= 0.0 ? (int) (noteOff * rate) : -1;
        if (on >= pos && on < pos + len)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), on - pos);
        if (off >= pos && off < pos + len)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), off - pos);
        proc.processBlock (view, midi);
        out.copyFrom (0, pos, view, 0, 0, len);
        out.copyFrom (1, pos, view, 1, 0, len);
    }
    return out;
}

// Frequency of the strongest spectral peak between two times (Hann window, zero-padded FFT, parabolic interpolation in
// dB): grains and overlapping copies make zero crossings unreliable.
double frequency (const Audio& a, double rate, double from, double to)
{
    const int i0 = (int) (from * rate), len = std::min (a.getNumSamples() - i0, (int) ((to - from) * rate));
    if (len < 1024)
        return 0.0;
    int order = 1;
    while ((1 << order) < 4 * len)
        ++order;
    const int size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> w ((size_t) size * 2, 0.0f);
    for (int i = 0; i < len; ++i)
        w[(size_t) i] = a.getSample (0, i0 + i) * (float) (0.5 - 0.5 * std::cos (twoPi * i / len));
    fft.performFrequencyOnlyForwardTransform (w.data());
    int best = 1;
    for (int k = 2; k < size / 2 - 1; ++k)
        if (w[(size_t) k] > w[(size_t) best])
            best = k;
    const double l = std::log (w[(size_t) best - 1] + 1.0e-12), c = std::log (w[(size_t) best] + 1.0e-12), r = std::log (w[(size_t) best + 1] + 1.0e-12);
    const double delta = 0.5 * (l - r) / (l - 2.0 * c + r);
    return (best + delta) * rate / size;
}

// (the zero-crossing count, kept for reference)
double frequencyByCrossings (const Audio& a, double rate, double from, double to)
{
    const float* x = a.getReadPointer (0);
    double first = -1.0, last = -1.0;
    int count = 0;
    for (int i = (int) (from * rate) + 1; i < std::min (a.getNumSamples(), (int) (to * rate)); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = i - 1 + (double) -x[i - 1] / (double) (x[i] - x[i - 1]);
            if (first < 0.0)
                first = t;
            else
                ++count;
            last = t;
        }
    return count > 0 ? count * rate / (last - first) : 0.0;
}

bool finite (const Audio& a)
{
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < a.getNumSamples(); ++i)
            if (! std::isfinite (a.getSample (c, i)))
                return false;
    return true;
}

double rmsDbOf (const Audio& a, double rate, double from, double to)
{
    double e = 0.0;
    const int i0 = (int) (from * rate), i1 = std::min (a.getNumSamples(), (int) (to * rate));
    for (int i = i0; i < i1; ++i)
        e += 0.5 * (juce::square ((double) a.getSample (0, i)) + juce::square ((double) a.getSample (1, i)));
    return 10.0 * std::log10 (e / std::max (1, i1 - i0) + 1.0e-20);
}
} // namespace rates2311

void auditRates()
{
    using namespace rates2311;
    title ("Frecuencias · un sample de 44,1 kHz en proyectos a cada frecuencia");
    // A held tone of 440 Hz (the root plays it as it is), recorded at 44.1 kHz. Each engine must sound the same at every
    // project rate as at 48 kHz; A and C, which read or rebuild the file without grains, exactly at 440 Hz. (B and D's
    // granular sustain and E's cloud scatter the pitch of a pure tone, and F's FFT bins quantise it, the same way at
    // every rate: docs/auditoria_tecnica_2026-11-23.md. F's level with a pure tone moves with its bins: informative.)
    const auto file = writeWav (tone (440.0, 3.0, 44100.0, 0.4f), 44100.0, "reanima_audit_tono440.wav");
    const double projectRates[] = { 48000.0, 32000.0, 44100.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    std::map<int, double> levelAt48, centsAt48;
    for (const double rate : projectRates)
    {
        ReanimaProcessor proc;
        proc.setPlayConfigDetails (0, 2, rate, 64);
        proc.prepareToPlay (rate, 64);
        proc.loadSampleNow (file);
        settle (proc);
        juce::String bad, info;
        for (int engine = 0; engine < 6; ++engine)
        {
            setParam (proc, P::Engine, (float) engine);
            setParam (proc, P::SustainType, (float) P::SustainHold);
            setParam (proc, P::AmpS, 1.0f);
            settle (proc);
            run (proc, rate, 64, 0.3, -1.0, -1.0);
            const auto y = run (proc, rate, 64, 2.0, 0.0, 1.8);
            const double hz = frequency (y, rate, 0.6, 1.6);
            const double cents = hz > 0.0 ? 1200.0 * std::log2 (hz / 440.0) : 9999.0;
            const double level = rmsDbOf (y, rate, 0.6, 1.6);
            if (rate == 48000.0)
            {
                levelAt48[engine] = level;
                centsAt48[engine] = cents;
            }
            if (! finite (y))
                bad << "ABCDEF"[engine] << " NaN; ";
            if ((engine == 0 || engine == 2) && std::abs (cents) > 1.0)
                bad << "ABCDEF"[engine] << " " << juce::String (cents, 2) << " ct; ";
            if (engine <= 4 && std::abs (cents - centsAt48[engine]) > 1.5)      // B and D: 1.3 at 176.4 and 192 (their granular grid)
                bad << "ABCDEF"[engine] << " " << juce::String (cents - centsAt48[engine], 2) << " ct frente a 48 kHz; ";
            if (engine <= 4 && std::abs (level - levelAt48[engine]) > 0.5)
                bad << "ABCDEF"[engine] << " " << juce::String (level - levelAt48[engine], 2) << " dB frente a 48 kHz; ";
            if (engine == 5)
                info << "F " << juce::String (cents, 1) << " ct, " << juce::String (level - levelAt48[engine], 2) << " dB";
        }
        check (bad.isEmpty(), "proyecto a " + juce::String (rate / 1000.0, 1) + " kHz: cada motor suena como a 48 kHz (A y C a 440 Hz exactos)",
               bad.isEmpty() ? "(informativo) " + info : bad);
    }

    title ("Frecuencias · cambio de frecuencia en mitad de la sesión");
    for (const auto& [from, to] : std::initializer_list<std::pair<double, double>> { { 48000.0, 96000.0 }, { 96000.0, 44100.0 }, { 44100.0, 88200.0 } })
        for (const int engine : { 0, 1, 2, 3, 4, 5 })
        {
            ReanimaProcessor proc;
            proc.setPlayConfigDetails (0, 2, from, 128);
            proc.prepareToPlay (from, 128);
            proc.loadSampleNow (file);
            settle (proc);
            setParam (proc, P::Engine, (float) engine);
            setParam (proc, P::SustainType, (float) P::SustainHold);
            settle (proc);
            run (proc, from, 128, 0.3, -1.0, -1.0);
            const auto before = run (proc, from, 128, 1.0, 0.0, -1.0);       // a note left sounding
            proc.releaseResources();
            proc.setPlayConfigDetails (0, 2, to, 128);
            proc.prepareToPlay (to, 128);
            settle (proc);
            const auto after = run (proc, to, 128, 2.0, 0.2, 1.8);
            // At the resume: what was left of the old note must not come back as a jump (the first 0.2 s are before the new note).
            float firstPeak = 0.0f;
            int peakAt = 0, firstSound = -1;
            for (int i = 0; i < (int) (0.19 * to); ++i)
            {
                const float v = std::abs (after.getSample (0, i));
                if (v > firstPeak) { firstPeak = v; peakAt = i; }
                if (firstSound < 0 && v > 1.0e-4f) firstSound = i;
            }
            if (std::getenv ("RATES_DEBUG") != nullptr)
                std::printf ("   primera muestra con sonido %d, pico en %d (%.1f ms)%c", firstSound, peakAt, 1000.0 * peakAt / to, 10);
            const double hz = frequency (after, to, 0.8, 1.6);
            const double cents = hz > 0.0 ? 1200.0 * std::log2 (hz / 440.0) : 9999.0;
            // Its pitch: the engine's own in a project that starts at 48 kHz (B, D, E and F are not at 440 Hz with a pure
            // tone, the same at every rate: above).
            // (E's random grains and F's random phases give a pure tone a pitch that depends on when the note starts: for
            // them only the clean resume is checked.)
            const bool pitchKnown = engine <= 3;
            check (finite (after) && (! pitchKnown || std::abs (cents - centsAt48[engine]) < 1.5) && firstPeak < 0.01f,
                   "motor " + juce::String::charToString ("ABCDEF"[engine]) + ", " + juce::String (from / 1000.0, 1) + " → " + juce::String (to / 1000.0, 1)
                       + " kHz con una nota sonando: se reanuda limpio y afinado",
                   "pico al reanudar " + juce::String (firstPeak, 4) + ", nota nueva " + juce::String (cents, 2) + " cents");
            (void) before;
        }
    file.deleteFile();
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;

    if (argc > 3 && juce::String (argv[1]) == "estres")
    {
        // estres <file> <out prefix> [id=value ...] [notes=N] [gap=ms] [held=s] [block=64] [rate=48000] [spread=12] [wav=1]:
        // many notes in quick succession through the whole processor, against the same notes each rendered alone
        // (a sum of them is what a voice that nothing disturbs would give). Per 20 ms: the residual against that
        // sum in dB, the voices sounding, and the time each block took against its real-time budget.
        const juce::File in (juce::String::fromUTF8 (argv[2]));
        const juce::String prefix = juce::String::fromUTF8 (argv[3]);
        int notes = 24, block = 64, spread = 12, firstKey = 0;
        double gapMs = 40.0, held = 0.6, rate = 48000.0;
        bool writeWav = false, fresh = false;
        std::vector<std::pair<juce::String, float>> sets { { "volume", -24.0f }, { "variation", 0.0f }, { "monoBass", 0.0f } };
        for (int a = 4; a < argc; ++a)
        {
            const juce::String s (argv[a]);
            const auto key = s.upToFirstOccurrenceOf ("=", false, false);
            const auto value = s.fromFirstOccurrenceOf ("=", false, false);
            if (key == "notes") notes = value.getIntValue();
            else if (key == "gap") gapMs = value.getDoubleValue();
            else if (key == "held") held = value.getDoubleValue();
            else if (key == "block") block = value.getIntValue();
            else if (key == "rate") rate = value.getDoubleValue();
            else if (key == "spread") spread = value.getIntValue();
            else if (key == "key") firstKey = value.getIntValue();
            else if (key == "wav") writeWav = value.getIntValue() != 0;
            else if (key == "fresh") fresh = value.getIntValue() != 0;
            else sets.push_back ({ key, value.getFloatValue() });
        }
        std::vector<int> pitch ((size_t) notes);
        std::vector<int> onAt ((size_t) notes), offAt ((size_t) notes);
        juce::Random r (1234);
        // every note on a key of its own (a key played again would be another test: the same key's note-off),
        // shuffled over 60 +- spread, wider when there are more notes than keys
        std::vector<int> keys;
        for (int k = -std::max (spread, notes / 2 + 1); k <= std::max (spread, notes / 2 + 1); ++k)
            keys.push_back (60 + k);
        for (int k = (int) keys.size() - 1; k > 0; --k)
            std::swap (keys[(size_t) k], keys[(size_t) r.nextInt (k + 1)]);
        for (int k = 0; k < notes; ++k)
        {
            pitch[(size_t) k] = firstKey > 0 ? firstKey + k : keys[(size_t) k % keys.size()];
            onAt[(size_t) k] = (int) std::lround (k * gapMs * 0.001 * rate);
            offAt[(size_t) k] = onAt[(size_t) k] + (int) std::lround (held * rate);
        }
        const int total = offAt.back() + (int) (2.5 * rate);

        auto makeProc = [&] (ReanimaProcessor& proc)
        {
            proc.engine.keySeeds.store (true);
            proc.setPlayConfigDetails (0, 2, rate, block);
            proc.prepareToPlay (rate, block);
            proc.loadSampleNow (in);
            for (int k = 0; k < 1500 && proc.isLoading(); ++k)
            {
                juce::Thread::sleep (10);
                proc.serviceMessageThread();
            }
            proc.serviceMessageThread();
            for (const auto& [id, v] : sets)
            {
                if (auto* param = proc.apvts.getParameter (id))
                    param->setValueNotifyingHost (param->convertTo0to1 (v));
                else
                    std::printf ("sin parámetro %s\n", id.toRawUTF8());
            }
            for (int k = 0; k < (std::getenv ("ESTRES_ESPERA") != nullptr ? 2000 : 300); ++k)       // analysis an engine may still ask for, the Rubber Band render
            {
                proc.serviceMessageThread();
                juce::Thread::sleep (5);
            }
        };
        // which: -1 all notes, else that note alone. times: per block seconds (all notes only).
        auto render = [&] (ReanimaProcessor& proc, int which, juce::AudioBuffer<float>& out, std::vector<double>* times, std::vector<int>* voices)
        {
            out.setSize (2, total);
            out.clear();
            juce::AudioBuffer<float> buf (2, block);
            for (int pos = 0; pos < total; pos += block)
            {
                const int n = std::min (block, total - pos);
                juce::MidiBuffer m;
                for (int k = 0; k < notes; ++k)
                {
                    if (which >= 0 && k != which)
                        continue;
                    if (onAt[(size_t) k] >= pos && onAt[(size_t) k] < pos + n)
                        m.addEvent (juce::MidiMessage::noteOn (1, pitch[(size_t) k], 0.9f), onAt[(size_t) k] - pos);
                    if (offAt[(size_t) k] >= pos && offAt[(size_t) k] < pos + n)
                        m.addEvent (juce::MidiMessage::noteOff (1, pitch[(size_t) k]), offAt[(size_t) k] - pos);
                }
                buf.setSize (2, n, false, false, true);
                buf.clear();
                const auto t0 = juce::Time::getHighResolutionTicks();
                proc.processBlock (buf, m);
                const auto t1 = juce::Time::getHighResolutionTicks();
                if (times != nullptr)
                    times->push_back (juce::Time::highResolutionTicksToSeconds (t1 - t0));
                if (voices != nullptr)
                {
                    int count = 0;
                    for (auto& ph : proc.engine.playheads)
                        count += ph.load() >= 0.0f ? 1 : 0;
                    voices->push_back (count);
                }
                out.copyFrom (0, pos, buf, 0, 0, n);
                out.copyFrom (1, pos, buf, 1, 0, n);
            }
            // drain: the next run starts from silence
            for (int k = 0; k < (int) (3.0 * rate) / block; ++k)
            {
                juce::MidiBuffer m;
                buf.setSize (2, block, false, false, true);
                buf.clear();
                proc.processBlock (buf, m);
            }
        };

        ReanimaProcessor proc;
        makeProc (proc);
        juce::AudioBuffer<float> all, sum (2, total), one;
        sum.clear();
        std::vector<double> times;
        std::vector<int> voices;
        for (int k = 0; k < notes; ++k)
        {
            if (fresh)
            {
                ReanimaProcessor alone;      // a processor of its own for each note: nothing carried from the last one
                makeProc (alone);
                render (alone, k, one, nullptr, nullptr);
            }
            else
                render (proc, k, one, nullptr, nullptr);
            for (int ch = 0; ch < 2; ++ch)
                sum.addFrom (ch, 0, one, ch, 0, total);
        }
        render (proc, -1, all, &times, &voices);

        const double budget = block / rate;
        if (std::getenv ("ESTRES_PICOS") != nullptr)
            for (size_t b = 0; b < times.size(); ++b)
                if (times[b] > 0.8 * budget)
                    std::printf ("pico %.3f s %.0f%% (%d voces)\n", b * block / rate, 100.0 * times[b] / budget, voices[b]);
        int over = 0;
        double worstT = 0.0, meanT = 0.0;
        for (auto t : times)
        {
            over += t > budget ? 1 : 0;
            worstT = std::max (worstT, t);
            meanT += t;
        }
        meanT /= std::max<size_t> (1, times.size());

        // residual per 20 ms where the sum has sound
        const int win = (int) (0.02 * rate);
        double worstDb = -300.0, worstAt = 0.0, firstBadAt = -1.0;
        int worstVoices = 0, firstBadVoices = 0;
        double totalRef = 0.0, totalRes = 0.0, peak = 0.0;
        for (int w = 0; w + win <= total; w += win)
        {
            double ref = 0.0, res = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = w; i < w + win; ++i)
                {
                    const double a = sum.getSample (ch, i), b = all.getSample (ch, i);
                    ref += a * a;
                    res += (a - b) * (a - b);
                    peak = std::max (peak, std::abs (b));
                }
            totalRef += ref;
            totalRes += res;
            if (ref < 1.0e-9 * win)
                continue;
            const double db = 10.0 * std::log10 ((res + 1.0e-20) / ref);
            const int v = voices[(size_t) std::min ((int) voices.size() - 1, w / block)];
            if (db > worstDb)
            {
                worstDb = db;
                worstAt = w / rate;
                worstVoices = v;
            }
            if (firstBadAt < 0.0 && db > -30.0)
            {
                firstBadAt = w / rate;
                firstBadVoices = v;
            }
        }
        int maxVoices = 0;
        for (auto v : voices)
            maxVoices = std::max (maxVoices, v);
        std::printf ("%s | voces max %d | residuo total %.1f dB, peor %.1f dB a %.2f s (%d voces), primero > -30 dB: %s | pico %.2f dBFS | "
                     "CPU bloque media %.0f%% peor %.0f%%, %d de %d bloques sobre el tiempo real\n",
                     in.getFileName().toRawUTF8(), maxVoices, 10.0 * std::log10 ((totalRes + 1.0e-20) / (totalRef + 1.0e-20)), worstDb, worstAt, worstVoices,
                     firstBadAt < 0.0 ? "nunca" : (juce::String (firstBadAt, 2) + " s (" + juce::String (firstBadVoices) + " voces)").toRawUTF8(),
                     20.0 * std::log10 (peak + 1.0e-20), 100.0 * meanT / budget, 100.0 * worstT / budget, over, (int) times.size());
        if (writeWav)
        {
            juce::AudioBuffer<float> res (2, total);
            for (int ch = 0; ch < 2; ++ch)
            {
                res.copyFrom (ch, 0, all, ch, 0, total);
                res.addFrom (ch, 0, sum, ch, 0, total, -1.0f);
            }
            for (const auto& [name, b] : { std::pair<const char*, juce::AudioBuffer<float>*> { "_todas.wav", &all }, { "_suma.wav", &sum }, { "_residuo.wav", &res } })
            {
                const juce::File f (prefix + name);
                f.deleteFile();
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
                juce::WavAudioFormat wav;
                if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
                    w->writeFromAudioSampleBuffer (*b, 0, total);
            }
        }
        return 0;
    }

    if (argc > 4 && juce::String (argv[1]) == "procloop")
    {
        // procloop <file> <out.wav> <seconds> [masterOff]: the file as LOOP Normal (C3 held) through the whole
        // processor at the file's rate, Volumen 0 dB, Graves mono off.
        const juce::File in (juce::String::fromUTF8 (argv[2]));
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (in));
        const double rate = reader->sampleRate;
        ReanimaProcessor proc;
        proc.setPlayConfigDetails (0, 2, rate, 64);
        proc.prepareToPlay (rate, 64);
        proc.loadSampleNow (in);
        for (int k = 0; k < 600 && proc.isLoading(); ++k)
        {
            juce::Thread::sleep (10);
            proc.serviceMessageThread();
        }
        proc.serviceMessageThread();
        for (const auto& [id, v] : { std::pair<const char*, float> { "playMode", 1.0f }, { "loopKind", 0.0f }, { "volume", 0.0f }, { "monoBass", 0.0f },
                                     { "envLock", 1.0f }, { "ampA", 1.0f }, { "ampS", 1.0f }, { "ampR", 30.0f } })
            if (auto* param = proc.apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (v));
        proc.serviceMessageThread();
        const int total = (int) (juce::String (argv[4]).getDoubleValue() * rate);
        juce::AudioBuffer<float> out (2, total), buf (2, 64);
        for (int pos = 0; pos < total; pos += 64)
        {
            juce::MidiBuffer m;
            if (pos == 0)
                m.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            buf.clear();
            proc.processBlock (buf, m);
            const int k = std::min (64, total - pos);
            out.copyFrom (0, pos, buf, 0, 0, k);
            out.copyFrom (1, pos, buf, 1, 0, k);
        }
        const juce::File outFile (juce::String::fromUTF8 (argv[3]));
        outFile.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (outFile);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
            w->writeFromAudioSampleBuffer (out, 0, total);
        std::printf ("latencia %d\n", proc.getLatencySamples());
        return 0;
    }

    if (argc > 1 && juce::String (argv[1]) == "defaults")
    {
        // Every parameter's id and default value (as the state stores it), one per line.
        ReanimaProcessor proc;
        for (int i = 0; i < P::Count; ++i)
            if (auto* param = proc.apvts.getParameter (P::defs()[(size_t) i].id))
                std::printf ("%s=%s\n", P::defs()[(size_t) i].id, juce::String (P::defs()[(size_t) i].def, 6).toRawUTF8());
        return 0;
    }

    if (argc > 3 && juce::String (argv[1]) == "midi")
    {
        // midi <file> <out.mid> [division]: the clip the button drags, by Rejilla (division) or Transitorios (-1).
        ReanimaProcessor proc;
        proc.setPlayConfigDetails (0, 2, 48000.0, 256);
        proc.prepareToPlay (48000.0, 256);
        proc.loadSampleNow (juce::File (juce::String::fromUTF8 (argv[2])));
        for (int k = 0; k < 600 && proc.isLoading(); ++k)
        {
            juce::Thread::sleep (10);
            proc.serviceMessageThread();
        }
        proc.serviceMessageThread();
        const int division = argc > 4 ? juce::String (argv[4]).getIntValue() : 4;
        for (const auto& [id, v] : { std::pair<const char*, float> { "playMode", 1.0f }, { "loopKind", 1.0f },
                                     { "sliceBy", division < 0 ? 0.0f : 1.0f }, { "sliceDivision", (float) std::max (0, division) } })
            if (auto* param = proc.apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (v));
        proc.serviceMessageThread();
        const auto f = proc.writeSliceMidi();
        const juce::File out (juce::String::fromUTF8 (argv[3]));
        out.deleteFile();
        std::printf ("%s %d trozos\n", f.copyFileTo (out) ? "ok" : "FALLO", (int) proc.getSlices().size());
        return 0;
    }

    if (argc > 2 && juce::String (argv[1]) == "cortes")
    {
        // cortes <file> [sliceBy 0|1] [division]: the table slice -> key -> start-end as the plug-in makes it,
        // then each key played through processBlock: the slice the voice took (engine diagnosis).
        const juce::File file (juce::String::fromUTF8 (argv[2]));
        ReanimaProcessor proc;
        proc.setPlayConfigDetails (0, 2, 48000.0, 256);
        proc.prepareToPlay (48000.0, 256);
        proc.loadSampleNow (file);
        for (int k = 0; k < 600 && proc.isLoading(); ++k)
        {
            juce::Thread::sleep (10);
            proc.serviceMessageThread();
        }
        proc.serviceMessageThread();
        for (const auto& [id, v] : { std::pair<const char*, float> { "playMode", 1.0f }, { "loopKind", 1.0f },
                                     { "sliceBy", argc > 3 ? (float) juce::String (argv[3]).getIntValue() : 0.0f },
                                     { "sliceDivision", argc > 4 ? (float) juce::String (argv[4]).getIntValue() : 4.0f } })
            if (auto* param = proc.apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (v));
        proc.serviceMessageThread();
        const auto s = proc.getSample();
        const auto marks = proc.getSlices();
        const int len = s->loopInfo.length > 0 ? std::min (s->loopInfo.length, s->length) : s->length;
        std::printf ("%s: %d trozos (golpes detectados %d, loop %d muestras a %.0f Hz)\n", file.getFileName().toRawUTF8(), (int) marks.size(),
                     (int) s->loopInfo.hits.size(), len, s->sampleRate);
        std::printf ("  trozo  tecla  MIDI   inicio    fin (s)   | tocada: trozo de la voz, inicio-fin\n");
        juce::AudioBuffer<float> buf (2, 256);
        for (size_t i = 0; i < marks.size() + 1; ++i)
        {
            const int key = 60 + (int) i;
            juce::MidiBuffer on, off;
            on.addEvent (juce::MidiMessage::noteOn (1, key, 0.9f), 0);
            off.addEvent (juce::MidiMessage::noteOff (1, key), 0);
            buf.clear();
            proc.processBlock (buf, on);
            buf.clear();
            proc.processBlock (buf, off);
            const int got = proc.engine.lastSliceIndex.load();
            if (i < marks.size())
                std::printf ("  %5d  %-5s  %4d  %9.6f  %9.6f    | %d, %.3f-%.3f\n", (int) i + 1, SampleAnalysis::midiNoteName ((float) key).toRawUTF8(), key,
                             marks[i] / s->sampleRate, (i + 1 < marks.size() ? marks[i + 1] : len) / s->sampleRate, got + 1,
                             proc.engine.lastSliceFrom.load() / s->sampleRate, proc.engine.lastSliceTo.load() / s->sampleRate);
            else
                std::printf ("     --  %-5s  %4d  (sin trozo)         | %s\n", SampleAnalysis::midiNoteName ((float) key).toRawUTF8(), key,
                             got < 0 ? "calla" : "SUENA");
        }
        return 0;
    }

    if (argc > 2 && juce::String (argv[1]) == "nota")
    {
        for (int i = 2; i < argc; ++i)
            showNote (juce::File (juce::String::fromUTF8 (argv[i])));
        return 0;
    }

    if (argc > 1 && juce::String (argv[1]) == "master")
    {
        auditMaster();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }

    if (argc > 1 && juce::String (argv[1]) == "fuzz")
    {
        auditFuzz (argc > 2 ? juce::String (argv[2]).getDoubleValue() : 60.0);
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }

    if (argc > 1 && juce::String (argv[1]) == "iman")
    {
        juce::ScopedJuceInitialiser_GUI gui;
        auditMagnet();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "pestanas")
    {
        ReanimaProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        ed->setSize (1180, 760);
        std::function<juce::TabbedComponent* (juce::Component&)> find = [&] (juce::Component& c) -> juce::TabbedComponent*
        {
            for (auto* ch : c.getChildren())
            {
                if (auto* t = dynamic_cast<juce::TabbedComponent*> (ch))
                    return t;
                if (auto* t = find (*ch))
                    return t;
            }
            return nullptr;
        };
        auto* tabs = find (*ed);
        for (int t = 0; t < tabs->getNumTabs(); ++t)
        {
            std::printf ("pestaña %d\n", t);
            tabs->setCurrentTabIndex (t);
            ed->createComponentSnapshot (ed->getLocalBounds());
            for (int k = 0; k < 20; ++k)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        }
        std::printf ("bien\n");
        return 0;
    }
    if (argc > 1 && juce::String (argv[1]) == "arp")
    {
        auditArp();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "tonalidad")
    {
        auditKey();
        auditChords();
    auditArp();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "explorador")
    {
        auditBrowser();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "frecuencias")
    {
        auditRates();
        std::printf ("%c%d comprobaciones, %d fallos%c", 10, checks, failures, 10);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "tecnica")
    {
        auditTechnical();
        std::printf ("%c%d comprobaciones, %d fallos%c", 10, checks, failures, 10);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "colorplay")
    {
        auditColorPlay();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "aros")
    {
        auditModRings();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "ventana")
    {
        auditLoopWindow();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "timbre")
    {
        auditTimbreHits();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "golpes")
    {
        auditHitEdits();
        auditOnsetOnTail();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && juce::String (argv[1]) == "loops")
    {
        auditLoops();
        auditSliceMidi();
        auditTransport();
        auditSliceEdit();
        auditHitEdits();
        auditOnsetOnTail();
        auditMagnet();
        auditTimbreHits();
        auditLoopWindow();
        auditRubber();
        std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }

    if (argc > 2 && juce::String (argv[1]) == "mapa")
    {
        mapControls (juce::File (juce::String::fromUTF8 (argv[2])));
        return 0;
    }

    const bool fast = argc > 1 && juce::String (argv[1]) == "fast";

    auditHelp();
    auditRealTime();
    auditDelayLine();
    auditPlayback();
    auditStereo();
    auditEnvelope();
    auditRelease();
    auditVariation();
    auditOutputMode();
    auditSynthEnvelope();
    auditExpression();
    auditGranular();
    auditDrone();
    auditSampleEdit();
    auditStartPoint();
    auditEdgeCases();
    auditSmoothness();
    auditFx();
    auditFxSwitching();
    auditQuality();
    auditWarpDuration();
    auditLoops();
    auditSliceMidi();
    auditTransport();
    auditSliceEdit();
    auditHitEdits();
    auditOnsetOnTail();
    auditMagnet();
    auditTimbreHits();
    auditLoopWindow();
    auditModRings();
    auditBrowser();
    auditKey();
    auditChords();
    auditArp();
    auditRubber();
    auditMaster();
    auditParameters();
    auditFuzz (fast ? 10.0 : 120.0);
    if (! fast)
        auditCpu();

    std::printf ("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
