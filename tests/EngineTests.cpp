// Offline checks of the analysis and the reconstruction engine on synthetic samples.
// Run: build\ReanimaTests_artefacts\Release\ReanimaTests.exe

#include "Params.h"
#include "SampleAnalysis.h"
#include "SynthEngine.h"
#include "FxRack.h"
#include "Mechanism.h"
#include "Filters.h"
#include <juce_dsp/juce_dsp.h>
#include <cstdio>

namespace
{
constexpr double sr = 48000.0;
int failures = 0;

void check (bool ok, const char* what, const juce::String& detail)
{
    std::printf ("[%s] %s  %s\n", ok ? "PASS" : "FAIL", what, detail.toRawUTF8());
    if (! ok)
        ++failures;
}

juce::AudioBuffer<float> makePluck (float hz, double seconds)
{
    juce::AudioBuffer<float> b (2, (int) (seconds * sr));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const double t = i / sr;
        const double env = t < 0.003 ? t / 0.003 : 0.25 * std::exp (-(t - 0.003) * 1.2) + 0.75 * std::exp (-(t - 0.003) / 0.08);
        double v = 0.0;
        for (int h = 1; h <= 8; ++h)
            v += std::sin (2.0 * juce::MathConstants<double>::pi * hz * h * t) / h;
        b.setSample (0, i, (float) (0.4 * env * v));
        b.setSample (1, i, (float) (0.4 * env * v));
    }
    return b;
}

// A soft, sustained, slightly breathy tone: what a bow or a breath suits.
juce::AudioBuffer<float> makePad (float hz, double seconds)
{
    juce::Random rng (11);
    juce::AudioBuffer<float> b (2, (int) (seconds * sr));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const double t = i / sr;
        double v = 0.0;
        for (int k = 1; k <= 8; ++k)
            v += std::sin (2.0 * juce::MathConstants<double>::pi * hz * k * t) / (k * k);
        const double env = std::min (1.0, t / 0.15) * std::min (1.0, (seconds - t) / 0.05);
        const float s = (float) (0.4 * env * (v + 0.02 * (rng.nextDouble() * 2.0 - 1.0)));
        b.setSample (0, i, s);
        b.setSample (1, i, s);
    }
    return b;
}

juce::AudioBuffer<float> makeDrum (double seconds)
{
    juce::Random rng (7);
    juce::AudioBuffer<float> b (2, (int) (seconds * sr));
    double phase = 0.0;
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const double t = i / sr;
        const double hz = 55.0 + 120.0 * std::exp (-t / 0.03);
        phase += hz / sr;
        const double body = std::sin (2.0 * juce::MathConstants<double>::pi * phase) * std::exp (-t / 0.25);
        const double click = (rng.nextDouble() * 2.0 - 1.0) * std::exp (-t / 0.008);
        const float v = (float) (0.6 * body + 0.4 * click);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

void clampToRange (ParamSnapshot& p, int index, float v)
{
    const auto& d = P::defs()[(size_t) index];
    p[(size_t) index] = juce::jlimit (d.min, d.max, v);
}

ParamSnapshot defaultsFor (const SampleData& s)
{
    ParamSnapshot p {};
    for (int i = 0; i < P::Count; ++i)
        p[(size_t) i] = P::defs()[(size_t) i].def;
    p[P::Volume] = 0.0f;           // the engine at unity: the -6 dB default is room for chords
    p[P::MonoBass] = 0.0f;         // the engine's own output (Graves mono shifts the lows' phase on purpose)

    clampToRange (p, P::AmpA, s.detected.attack * 1000.0f);
    clampToRange (p, P::AmpCurve, s.detected.curve);
    clampToRange (p, P::AmpD, s.detected.decay * 1000.0f);
    clampToRange (p, P::AmpS, s.detected.sustain);
    clampToRange (p, P::AmpFade, s.detected.fadeDbPerSec);
    clampToRange (p, P::AmpR, s.detected.release * 1000.0f);
    clampToRange (p, P::TransientMs, s.transientMs);
    p[P::StartPos] = s.length > 0 ? (float) (s.onset / s.length) : 0.0f;
    return p;
}

// Plays one note held for `held` seconds and returns the left channel.
std::vector<float> play (std::shared_ptr<const SampleData> s, const ParamSnapshot& p, int note, double held, double total,
                         bool original = false)
{
    SynthEngine e;
    e.prepare (sr);
    e.listenOriginal.store (original);
    e.prepareFx (p);
    e.setSample (s);

    std::vector<float> out;
    juce::AudioBuffer<float> buf (2, 512);
    const int heldSamples = (int) (held * sr);

    for (int pos = 0; pos < (int) (total * sr); pos += 512)
    {
        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
        if (pos <= heldSamples && heldSamples < pos + 512)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), heldSamples - pos);
        e.process (buf, midi, p, 120.0);
        out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + 512);
    }
    return out;
}

double rms (const std::vector<float>& x, double from, double to)
{
    const int a = (int) (from * sr), b = std::min ((int) x.size(), (int) (to * sr));
    double s = 0.0;
    for (int i = a; i < b; ++i)
        s += (double) x[(size_t) i] * x[(size_t) i];
    return std::sqrt (s / std::max (1, b - a));
}

double snrAgainstOriginal (const std::vector<float>& y, const SampleData& s, double seconds)
{
    const int n = std::min ((int) (seconds * sr), (int) y.size());
    const float* x = s.audio.getReadPointer (0);
    double sig = 0.0, err = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const int src = (int) s.onset + i;
        const float ref = src < s.length ? x[src] : 0.0f;
        sig += (double) ref * ref;
        err += (double) (ref - y[(size_t) i]) * (ref - y[(size_t) i]);
    }
    return 10.0 * std::log10 (sig / std::max (1.0e-20, err));
}

// Frequency of the strongest bin over 16384 samples from `at`.
float peakHz (const std::vector<float>& y, double at)
{
    constexpr int order = 14, n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> w ((size_t) 2 * n, 0.0f);
    for (int i = 0; i < n; ++i)
        w[(size_t) i] = y[(size_t) ((int) (at * sr) + i)] * (0.5f - 0.5f * std::cos (6.2831853f * (float) i / n));
    fft.performFrequencyOnlyForwardTransform (w.data(), true);
    int best = 1;
    for (int k = 2; k < n / 2; ++k)
        if (w[(size_t) k] > w[(size_t) best])
            best = k;
    return (float) (best * sr / n);
}

// The usual peak over several moments: engine F's random phases make one partial or another the loudest now
// and then (the pluck's second harmonic is close to its first), so one moment alone is a draw.
float steadyPeakHz (const std::vector<float>& y, double from, double to)
{
    std::vector<float> peaks;
    for (double at = from; at <= to + 1.0e-9; at += 0.25)
        peaks.push_back (peakHz (y, at));
    std::sort (peaks.begin(), peaks.end());
    return peaks[peaks.size() / 2];
}

float detectHz (const std::vector<float>& y, double at)
{
    juce::AudioBuffer<float> b (1, (int) (0.5 * sr));
    for (int i = 0; i < b.getNumSamples(); ++i)
        b.setSample (0, i, y[(size_t) ((int) (at * sr) + i)]);
    const auto d = SampleAnalysis::analyse (b, sr, "probe", false);
    return d->pitched ? 440.0f * std::pow (2.0f, (d->rootMidi - 69.0f) / 12.0f) : 0.0f;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    // --- Pluck (A3 = 220 Hz = MIDI 57) -------------------------------------
    const auto pluckAudio = makePluck (220.0f, 2.0);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    std::shared_ptr<const SampleData> pluck = SampleAnalysis::analyse (pluckAudio, sr, "pluck", false);
    const auto analysisMs = juce::Time::getMillisecondCounterHiRes() - t0;
    const auto& d = pluck->detected;

    std::printf ("Pluck: root %.2f  A %.1f ms  curve %.2f  D %.0f ms  S %.2f  fade %.1f dB/s  R %.0f ms\n"
                 "       onset %.0f  peak %.0f  knee %.0f  sustainPos %.0f  (analysis %.0f ms)\n",
                 pluck->rootMidi, d.attack * 1000, d.curve, d.decay * 1000, d.sustain, d.fadeDbPerSec, d.release * 1000,
                 pluck->onset, pluck->peak, pluck->knee, pluck->sustainPos, analysisMs);

    check (pluck->pitched && std::abs (pluck->rootMidi - 57.0f) < 0.1f, "pluck root note A3 (57)", juce::String (pluck->rootMidi, 2));
    check (pluck->nearestC() == 60, "nearest C of A3 is C3 (60)", juce::String (pluck->nearestC()));
    check (d.attack < 0.01f, "pluck attack detected under 10 ms", juce::String (d.attack * 1000.0f, 1) + " ms");

    for (const auto* src : { &pluck->early, &pluck->late })
    {
        auto bufRms = [] (const juce::AudioBuffer<float>& b)
        {
            double s = 0.0;
            for (int i = 0; i < b.getNumSamples(); ++i)
                s += (double) b.getSample (0, i) * b.getSample (0, i);
            return std::sqrt (s / std::max (1, b.getNumSamples()));
        };
        double raw = 0.0;
        for (int i = (int) src->start; i < (int) src->end; ++i)
            raw += (double) pluck->audio.getSample (0, i) * pluck->audio.getSample (0, i);
        raw = std::sqrt (raw / std::max (1.0, src->end - src->start));
        std::printf ("  source %.3f-%.3f s: raw %.4f loop %.4f freeze %.4f model %.3f env %.3f (peakRms %.4f)\n",
                     src->start / sr, src->end / sr, raw, bufRms (src->loop), bufRms (src->freeze), src->model, src->env, pluck->peakRms);
    }

    auto p = defaultsFor (*pluck);
    p[P::Engine] = (float) P::EngineReenvelope;
    for (const int mode : { P::SustainLoop, P::SustainGranular, P::SustainFreeze })
    {
        p[P::SustainMode] = (float) mode;
        const auto y = play (pluck, p, SynthEngine::rootKey, 2.0, 2.0);
        const double until = pluck->sustainPos / sr - pluck->onset / sr;
        const double snr = snrAgainstOriginal (y, *pluck, until);
        check (snr > 25.0, "untouched knobs = original (before sustain zone)",
               "mode " + juce::String (mode) + ": SNR " + juce::String (snr, 1) + " dB over " + juce::String (until, 2) + " s");
    }

    // Engine A: what it must guarantee.
    {
        auto q = p;
        q[P::Engine] = (float) P::EngineRebuild;

        const auto same = play (pluck, q, SynthEngine::rootKey, 2.0, 2.0);
        const double snr = snrAgainstOriginal (same, *pluck, pluck->sustainPos / sr - pluck->onset / sr);
        check (snr > 60.0, "engine A untouched = the sample itself", "SNR " + juce::String (snr, 1) + " dB");

        q[P::SustainType] = (float) P::SustainHold;
        q[P::AmpS] = 0.7f;
        q[P::AmpR] = 200.0f;
        const auto held = play (pluck, q, SynthEngine::rootKey, 4.0, 4.6);
        const double a = rms (held, 1.0, 1.5), b = rms (held, 3.4, 3.9);
        const double drift = 20.0 * std::log10 (b / a);
        check (std::abs (drift) < 2.0, "engine A holds the sustain level", juce::String (drift, 2) + " dB after 2.4 s");
        float loudest = 0.0f;
        for (const float v : held)
            loudest = std::max (loudest, std::abs (v));
        check (loudest <= 1.0f, "output never goes over full scale", juce::String (loudest, 3));
        check (rms (held, 4.4, 4.6) < 1.0e-3, "engine A release dies away", juce::String (rms (held, 4.4, 4.6), 6));

        // A sample that stops at full level must not leave a held note in silence.
        {
            juce::AudioBuffer<float> cut (2, (int) (0.5 * sr));
            for (int i = 0; i < cut.getNumSamples(); ++i)
            {
                const float v = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) sr);
                cut.setSample (0, i, v);
                cut.setSample (1, i, v);
            }
            const auto abrupt = SampleAnalysis::analyse (cut, sr, "cut", false);
            auto c = q;
            c[P::AmpA] = abrupt->detected.attack * 1000.0f;
            c[P::AmpS] = abrupt->detected.sustain;
            c[P::AmpD] = abrupt->detected.decay * 1000.0f;
            const auto keep = play (abrupt, c, SynthEngine::rootKey, 3.0, 3.2);
            const double before = rms (keep, 0.2, 0.35), after = rms (keep, 1.5, 2.8);
            check (after > 0.5 * before, "held note survives the end of an abruptly cut sample",
                   juce::String (20.0 * std::log10 (after / std::max (1.0e-9, before)), 1) + " dB");
        }

        // Channels detuned by 1 Hz: the bed must keep turning between in and out of phase,
        // not freeze at one moment of that cycle.
        {
            juce::AudioBuffer<float> wide (2, (int) (1.5 * sr));
            for (int i = 0; i < wide.getNumSamples(); ++i)
            {
                const float t = (float) i / (float) sr;
                const float env = std::exp (-1.0f * t);
                wide.setSample (0, i, 0.5f * env * std::sin (juce::MathConstants<float>::twoPi * 220.0f * t));
                wide.setSample (1, i, 0.5f * env * std::sin (juce::MathConstants<float>::twoPi * 221.0f * t));
            }
            const auto detuned = SampleAnalysis::analyse (wide, sr, "detuned", false);
            const auto& wideBed = detuned->bedBright;
            double mid = 0.0, side = 0.0;
            for (int i = 0; i < wideBed.length; ++i)
            {
                const double l = wideBed.audio.getSample (0, i), r = wideBed.audio.getSample (1, i);
                mid += (l + r) * (l + r);
                side += (l - r) * (l - r);
            }
            const double balance = 10.0 * std::log10 (side / std::max (1.0e-12, mid));
            check (std::abs (balance) < 3.0, "detuned channels stay balanced in the bed", juce::String (balance, 1) + " dB side/mid");
        }

        // The beds must keep the stereo image of the sample and loop without a seam.
        const auto& bed = pluck->bedBright;
        const int pad = SampleData::Bed::pad;
        double jump = 0.0, typical = 0.0;
        for (int i = pad + 1; i < pad + bed.length; ++i)
        {
            typical += std::abs (bed.audio.getSample (0, i) - bed.audio.getSample (0, i - 1));
            if (i == pad + 1)
                jump = std::abs (bed.audio.getSample (0, pad) - bed.audio.getSample (0, pad + bed.length - 1));
        }
        typical /= bed.length;
        check (jump < 8.0 * typical + 1.0e-6, "bed loops without a seam", "seam " + juce::String (jump, 5) + ", typical step " + juce::String (typical, 5));
    }

    // Hybrid engine, untouched knobs.
    {
        auto q = p;
        q[P::Engine] = (float) P::EngineHybrid;
        q[P::SustainMode] = (float) P::SustainLoop;
        const auto y = play (pluck, q, SynthEngine::rootKey, 2.0, 2.0);
        const double snr = snrAgainstOriginal (y, *pluck, pluck->sustainPos / sr - pluck->onset / sr);
        check (snr > 25.0, "hybrid engine untouched = original", "SNR " + juce::String (snr, 1) + " dB");
    }

    // Synth-style sustain: S = 100 %, no fade, held 3 s. Level must hold in every mode.
    for (const int mode : { P::SustainLoop, P::SustainGranular, P::SustainFreeze })
    {
        auto q = p;
        q[P::SustainMode] = (float) mode;
        q[P::AmpS] = 1.0f;
        q[P::AmpFade] = 0.0f;
        q[P::AmpD] = 50.0f;
        const auto y = play (pluck, q, SynthEngine::rootKey, 3.0, 3.2);
        const double early = rms (y, 0.3, 0.6), late = rms (y, 2.5, 2.9);
        const double ratioDb = 20.0 * std::log10 (late / early);
        check (std::abs (ratioDb) < 3.0, "sustain holds the level while the key is down",
               "mode " + juce::String (mode) + ": late/early " + juce::String (ratioDb, 2) + " dB");
    }

    // Long linear attack: the level follows the new envelope, with and without stretch.
    for (const bool stretch : { true, false })
    {
        auto q = p;
        q[P::AmpA] = 600.0f;
        q[P::AmpCurve] = 1.0f;
        q[P::Stretch] = stretch ? 1.0f : 0.0f;
        const auto y = play (pluck, q, SynthEngine::rootKey, 2.0, 2.0);
        const double early = rms (y, 0.05, 0.1), mid = rms (y, 0.25, 0.35), atPeak = rms (y, 0.5, 0.6);
        const double expectedEarly = 20.0 * std::log10 (0.075 / 0.55), expectedMid = 20.0 * std::log10 (0.3 / 0.55);
        const double gotEarly = 20.0 * std::log10 (early / atPeak), gotMid = 20.0 * std::log10 (mid / atPeak);
        check (std::abs (gotEarly - expectedEarly) < 4.0 && std::abs (gotMid - expectedMid) < 3.0,
               stretch ? "600 ms linear attack follows the envelope (stretch on)" : "600 ms linear attack follows the envelope (stretch off)",
               "75 ms: " + juce::String (gotEarly, 1) + " dB (expected " + juce::String (expectedEarly, 1) + "), 300 ms: "
                   + juce::String (gotMid, 1) + " dB (expected " + juce::String (expectedMid, 1) + ")");
    }

    // Release: fast cut after note-off.
    {
        auto q = p;
        q[P::AmpS] = 1.0f; q[P::AmpFade] = 0.0f; q[P::AmpR] = 50.0f;
        const auto y = play (pluck, q, SynthEngine::rootKey, 0.5, 1.0);
        check (rms (y, 0.7, 1.0) < 1.0e-4, "50 ms release goes silent", juce::String (rms (y, 0.7, 1.0), 6));
    }

    // Snap a C: off, Do3 plays the A sample as it is; on, Do3 plays it moved up to the nearest C.
    {
        const auto plain = play (pluck, p, SynthEngine::rootKey, 1.0, 1.0);
        check (std::abs (detectHz (plain, 0.1) - 220.0f) < 2.0f, "without snap Do3 plays the sample as it is",
               juce::String (detectHz (plain, 0.1), 1) + " Hz");

        auto q = p;
        q[P::SnapC] = 1.0f;
        const auto snapped = play (pluck, q, SynthEngine::rootKey, 1.0, 1.0);
        check (std::abs (detectHz (snapped, 0.1) - 261.63f) < 2.0f, "Snap a C: Do3 plays the A sample as C",
               juce::String (detectHz (snapped, 0.1), 1) + " Hz");

        const auto above = play (pluck, q, SynthEngine::rootKey + 2, 1.0, 1.0);
        check (std::abs (detectHz (above, 0.1) - 293.66f) < 2.5f, "Snap a C: the keyboard is in tune (Re3 plays D)",
               juce::String (detectHz (above, 0.1), 1) + " Hz");
    }

    // A sample already on C is left alone.
    {
        const auto cAudio = makePluck (261.63f, 1.0);
        const auto c = SampleAnalysis::analyse (cAudio, sr, "c", false);
        check (std::abs (SynthEngine::snapShift (*c, true)) < 0.05f, "sample already in C is not changed by snap", juce::String (c->rootMidi, 2));
    }

    // --- Drum hit ------------------------------------------------------------
    {
        const auto drumAudio = makeDrum (1.0);
        std::shared_ptr<const SampleData> drum = SampleAnalysis::analyse (drumAudio, sr, "drum", false);
        const auto& dd = drum->detected;
        std::printf ("Drum: pitched %d root %.2f  A %.1f ms  D %.0f ms  S %.2f  fade %.1f  R %.0f ms  transient %.1f ms\n",
                     (int) drum->pitched, drum->rootMidi, dd.attack * 1000, dd.decay * 1000, dd.sustain, dd.fadeDbPerSec,
                     dd.release * 1000, drum->transientMs);

        auto q = defaultsFor (*drum);
        q[P::Engine] = (float) P::EngineHybrid;
        const auto y = play (drum, q, SynthEngine::rootKey, 1.0, 1.0);
        const double snr = snrAgainstOriginal (y, *drum, drum->sustainPos / sr - drum->onset / sr);
        check (snr > 20.0, "drum, hybrid, untouched = original", "SNR " + juce::String (snr, 1) + " dB");

        q[P::AmpS] = 1.0f; q[P::AmpFade] = 0.0f;
        const auto held = play (drum, q, SynthEngine::rootKey, 2.0, 2.0);
        check (rms (held, 1.5, 1.9) > 0.01, "drum can be sustained", juce::String (rms (held, 1.5, 1.9), 4));

        // Engine A, a long decay to a low sustain, held: once the note falls it never climbs back
        // (the end of the sample, the two beds cancelling each other, a hole in a bed).
        for (const auto& [name, sample] : { std::pair<const char*, std::shared_ptr<const SampleData>> { "drum", drum }, { "pluck", pluck } })
        {
            auto a = defaultsFor (*sample);
            a[P::Engine] = (float) P::EngineRebuild;
            a[P::SustainType] = (float) P::SustainHold;
            a[P::AmpA] = 37.0f;
            a[P::AmpCurve] = 2.56f;
            a[P::AmpD] = 5280.0f;
            a[P::AmpS] = 0.14f;
            a[P::AmpFade] = 0.0f;
            const auto note = play (sample, a, SynthEngine::rootKey, 4.0, 4.0);
            const int w = (int) (0.02 * sr);
            std::vector<double> e;
            for (int i = 0; i + w <= (int) note.size(); i += w)
                e.push_back (20.0 * std::log10 (rms (note, (double) i / sr, (double) (i + w) / sr) + 1.0e-9));
            const int top = (int) (std::max_element (e.begin(), e.end()) - e.begin());
            double worst = 0.0, at = 0.0;
            for (int k = top + 3; k + 8 < (int) e.size() && k < 195; ++k)
            {
                const double low = *std::min_element (e.begin() + std::max (top, k - 15), e.begin() + k + 1);
                if (e[(size_t) k + 8] - low > worst)
                {
                    worst = e[(size_t) k + 8] - low;
                    at = k * 0.02;
                }
            }
            check (worst < 6.0, "engine A, long decay held: the note never climbs back", juce::String (name) + ": " + juce::String (worst, 1) + " dB at " + juce::String (at, 2) + " s");
        }
    }

    // --- Engine C (resynthesis) ----------------------------------------------
    {
        std::printf ("Engine C: %d partials, %d frames, ref %.1f Hz\n", pluck->partials.numPartials,
                     pluck->partials.numFrames, pluck->partials.refFreq);
        check (pluck->partials.valid(), "pluck has a partial model", juce::String (pluck->partials.numPartials) + " partials");

        auto q = p;
        q[P::Engine] = (float) P::EngineResynth;

        const auto y = play (pluck, q, SynthEngine::rootKey, 2.0, 2.0);
        const double early = rms (y, 0.05, 0.15), original = [&]
        {
            std::vector<float> x (pluck->audio.getReadPointer (0), pluck->audio.getReadPointer (0) + pluck->length);
            return rms (x, 0.05, 0.15);
        }();
        check (std::abs (20.0 * std::log10 (early / original)) < 2.0, "engine C level matches the original",
               juce::String (20.0 * std::log10 (early / original), 2) + " dB");
        check (std::abs (detectHz (y, 0.1) - 220.0f) < 2.0f, "engine C plays the right pitch", juce::String (detectHz (y, 0.1), 1) + " Hz");

        // Natural: held past the end of the 2 s sample, it keeps decaying instead of stopping.
        q[P::SustainType] = (float) P::SustainNatural;
        const auto nat = play (pluck, q, SynthEngine::rootKey, 3.5, 3.5);
        const double atEnd = rms (nat, 1.6, 1.9), beyond = rms (nat, 2.6, 2.9);
        check (beyond > 1.0e-4 && beyond < atEnd, "engine C natural extends the note past the sample end",
               juce::String (20.0 * std::log10 (std::max (1.0e-9, beyond) / atEnd), 1) + " dB after 1 s more");

        // Sostenido: level holds and breathes slightly.
        q[P::SustainType] = (float) P::SustainHold;
        q[P::AmpS] = 0.6f;
        const auto held = play (pluck, q, SynthEngine::rootKey, 4.0, 4.0);
        const double h1 = rms (held, 1.0, 1.5), h2 = rms (held, 3.0, 3.5);
        check (std::abs (20.0 * std::log10 (h2 / h1)) < 2.0, "engine C sostenido holds the level",
               juce::String (20.0 * std::log10 (h2 / h1), 2) + " dB");

        // Slow bowed attack: the start must be darker than the sustained tone.
        q[P::AmpA] = 500.0f;
        q[P::AmpCurve] = 1.0f;
        q[P::Bow] = 1.0f;
        const auto bowed = play (pluck, q, SynthEngine::rootKey, 2.0, 2.0);
        auto brightness = [&] (const std::vector<float>& x, double at)
        {
            double hf = 0.0, all = 0.0, prev = 0.0;
            for (int i = (int) (at * sr); i < (int) ((at + 0.08) * sr); ++i)
            {
                const double d = x[(size_t) i] - prev;
                prev = x[(size_t) i];
                hf += d * d;
                all += (double) x[(size_t) i] * x[(size_t) i];
            }
            return hf / std::max (1.0e-12, all);
        };
        check (brightness (bowed, 0.1) < brightness (bowed, 1.0) * 0.8, "bowed attack starts darker",
               juce::String (brightness (bowed, 0.1) / brightness (bowed, 1.0), 2) + " brightness ratio");
    }

    // --- FX rack: every module and splitter --------------------------------
    {
        ParamSnapshot q {};
        for (int i = 0; i < P::Count; ++i)
            q[(size_t) i] = P::defs()[(size_t) i].def;

        for (int type = 1; type < Fx::NumTypes; ++type)
        {
            const auto& spec = Fx::spec (type);
            q[P::fxType (0)] = (float) type;
            q[P::fxOn (0)] = 1.0f;
            q[P::fxMix (0)] = 1.0f;
            for (int k = 0; k < P::fxParamsPerSlot; ++k)
                q[(size_t) P::fxParam (0, k)] = k < (int) spec.params.size() ? Fx::toNorm (spec.params[(size_t) k], spec.params[(size_t) k].def) : 0.5f;

            FxRack rack;
            rack.prepare (sr);
            rack.ensureModules (q);
            juce::AudioBuffer<float> buf (2, 512);
            double inSq = 0.0, outSq = 0.0;
            bool finite = true;
            double phase = 0.0;
            for (int block = 0; block < 200; ++block)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.3f * (float) std::sin (phase);
                    phase += 2.0 * juce::MathConstants<double>::pi * 330.0 / sr;
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v);
                    if (block >= 100) inSq += (double) v * v;
                }
                rack.process (buf, q, 120.0);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                    {
                        const float v = buf.getSample (ch, i);
                        finite = finite && std::isfinite (v);
                        if (block >= 100 && ch == 0) outSq += (double) v * v;
                    }
            }
            const double gainDb = 10.0 * std::log10 (std::max (1.0e-20, outSq) / inSq);
            const bool ok = finite && gainDb < 12.0 && (Fx::isSplitter (type) ? std::abs (gainDb) < 0.5 : gainDb > -40.0);
            check (ok, "FX module stable and sane", juce::String::fromUTF8 (spec.name) + ": " + juce::String (gainDb, 1) + " dB");
        }
    }

    // --- Engine F: the drone maker ------------------------------------------
    {
        auto q = defaultsFor (*pluck);
        q[P::Engine] = (float) P::EngineDrone;
        q[P::SustainType] = (float) P::SustainHold;
        q[P::AmpA] = 5.0f;
        q[P::AmpD] = 10.0f;
        q[P::AmpS] = 1.0f;
        q[P::AmpR] = 300.0f;
        q[P::DroneStretch] = 50.0f;
        q[P::GrainPos] = 0.1f;

        const auto y = play (pluck, q, 60, 6.0, 6.2);
        bool finite = true;
        for (const float v : y)
            finite = finite && std::isfinite (v);

        // The level of the sample where the drone reads it (0.2 s, the window around it).
        double src = 0.0;
        const int from = (int) (0.1 * sr), to = (int) (0.3 * sr);
        for (int i = from; i < to; ++i)
            src += (double) pluck->audio.getSample (0, i) * pluck->audio.getSample (0, i);
        src = std::sqrt (src / (to - from));
        // Where it reads, raised towards the level of the sample's loudest part (up to +12 dB).
        const double raise = juce::jlimit (1.0, 4.0, 1.0 / std::max (0.25, (double) pluck->envAt (0.2 * sr)));
        const double level = 20.0 * std::log10 (rms (y, 1.5, 2.5) / (src * raise));
        check (finite && std::abs (level) < 4.0, "engine F sounds at the level of the sample", juce::String (level, 1) + " dB");

        // Random phases make a drone shimmer by a dB or two from one second to the next.
        const double steady = 20.0 * std::log10 (rms (y, 1.0, 3.0) / rms (y, 3.5, 5.5));
        check (std::abs (steady) < 3.0, "engine F drones on while the key is held", juce::String (steady, 1) + " dB over 2.5 s");

        const float f60 = steadyPeakHz (y, 1.0, 4.0);
        const float f72 = steadyPeakHz (play (pluck, q, 72, 3.0, 3.2), 1.0, 2.5);
        check (std::abs (f60 - 220.0f) < 6.0f && std::abs (f72 - 440.0f) < 6.0f, "engine F follows the keyboard",
               juce::String (f60, 0) + " Hz and " + juce::String (f72, 0) + " Hz an octave up");

        auto sub = q;
        sub[P::DroneSub1] = 1.0f;
        sub[P::DroneHarm] = 1.0f;
        const auto ys = play (pluck, sub, 60, 3.0, 3.2);
        const float fs = steadyPeakHz (ys, 1.0, 2.5);
        check (std::abs (fs - 110.0f) < 6.0f || std::abs (fs - 220.0f) < 6.0f, "engine F octave below and harmonics", juce::String (fs, 0) + " Hz");

        // An octave copy must actually be there: energy around 110 Hz rises.
        auto band = [] (const std::vector<float>& x, double at, float lo, float hi)
        {
            constexpr int order = 14, n = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<float> w ((size_t) 2 * n, 0.0f);
            for (int i = 0; i < n; ++i)
                w[(size_t) i] = x[(size_t) ((int) (at * sr) + i)];
            fft.performFrequencyOnlyForwardTransform (w.data(), true);
            double e = 0.0;
            for (int k = (int) (lo * n / sr); k <= (int) (hi * n / sr); ++k)
                e += (double) w[(size_t) k] * w[(size_t) k];
            return e;
        };
        const double lift = 10.0 * std::log10 (band (ys, 2.0, 100.0f, 120.0f) / std::max (1.0e-20, band (y, 2.0, 100.0f, 120.0f)));
        check (lift > 10.0, "engine F -1 oct adds the octave below", juce::String (lift, 1) + " dB at 110 Hz");

        auto frozen = q;
        frozen[P::DroneFreeze] = 1.0f;
        const auto yf = play (pluck, frozen, 60, 6.0, 6.2);
        const double still = 20.0 * std::log10 (rms (yf, 1.0, 3.0) / rms (yf, 3.5, 5.5));
        check (std::abs (still) < 2.0, "engine F Congelar holds one moment", juce::String (still, 2) + " dB");

        // No long FFT may land in a single block: eight notes at the largest size.
        auto big = q;
        big[P::DroneSize] = 5.0f;
        SynthEngine e;
        e.prepare (sr);
        e.prepareFx (big);
        e.setSample (pluck);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer chord;
        for (int n = 0; n < 8; ++n)
            chord.addEvent (juce::MidiMessage::noteOn (1, 48 + n * 3, 1.0f), 0);
        double worst = 0.0, total = 0.0;
        int worstAt = 0, overHalf = 0;
        const int blocks = (int) (4.0 * sr / 256.0);
        for (int b = 0; b < blocks; ++b)
        {
            const auto t0b = juce::Time::getMillisecondCounterHiRes();
            e.process (buf, b == 0 ? chord : juce::MidiBuffer(), big, 120.0);
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0b;
            if (ms > worst) worstAt = b;
            worst = std::max (worst, ms);
            if (ms > 0.5 * 256.0 / sr * 1000.0) { ++overHalf; std::printf ("   bloque %d: %.2f ms\n", b, ms); }
            total += ms;
        }
        const double blockMs = 256.0 / sr * 1000.0;
        check (worst < 0.5 * blockMs, "engine F 8 notes at 1.4 s FFT: no block over half its time",
               juce::String (worst, 2) + " ms worst (block " + juce::String (worstAt) + ", " + juce::String (overHalf) + " over half) of " + juce::String (blockMs, 2) + " ms, average "
               + juce::String (100.0 * total / (blocks * blockMs), 1) + " % of one core");
    }

    // --- Mechanism ------------------------------------------------------------
    {
        // Every type (the "Real" ones came later: with 6 names here the loop read past the list, found by ASan).
        const char* names[Mechanism::NumTypes] = { "Piano", "Pick", "Bow", "Key", "Breath", "From the sample", "Real piano", "Real pick",
                                                   "Real bow", "Real breath", "Real guitar" };
        auto q = defaultsFor (*pluck);
        q[P::AmpR] = 40.0f;
        const auto dry = play (pluck, q, 60, 0.6, 1.2);
        for (int type = 0; type < Mechanism::NumTypes; ++type)
        {
            auto m = q;
            m[P::RelNoise] = 0.6f;
            m[P::MechType] = (float) type;
            const auto wet = play (pluck, m, 60, 0.6, 1.2);
            std::vector<float> diff (dry.size());
            for (size_t i = 0; i < diff.size(); ++i)
                diff[i] = wet[i] - dry[i];
            const double down = 20.0 * std::log10 (std::max (1.0e-9, rms (diff, 0.0, 0.05)) / rms (dry, 0.0, 0.05));
            const double up = 20.0 * std::log10 (std::max (1.0e-9, rms (diff, 0.6, 0.7)) / rms (dry, 0.0, 0.05));
            check (down > -30.0 && up > -45.0 && down < -3.0 && up < -3.0, "Mecanismo is heard at the key and at the release, under the note",
                   juce::String (names[type]) + ": " + juce::String (down, 1) + " dB / " + juce::String (up, 1) + " dB");
        }

        // Entrada and Salida set each strike apart; On/Off silences both without touching them.
        {
            const auto measure = [&] (float downDb, float upDb, bool on)
            {
                auto m = q;
                m[P::RelNoise] = 0.6f;
                m[P::MechType] = (float) Mechanism::Pick;
                m[P::MechDown] = downDb;
                m[P::MechUp] = upDb;
                m[P::MechOn] = on ? 1.0f : 0.0f;
                const auto wet = play (pluck, m, 60, 0.6, 1.2);
                std::vector<float> d (dry.size());
                for (size_t i = 0; i < d.size(); ++i)
                    d[i] = wet[i] - dry[i];
                return std::pair<double, double> { 20.0 * std::log10 (std::max (1.0e-9, rms (d, 0.0, 0.05)) / rms (dry, 0.0, 0.05)),
                                                   20.0 * std::log10 (std::max (1.0e-9, rms (d, 0.6, 0.7)) / rms (dry, 0.0, 0.05)) };
            };
            const auto both = measure (0.0f, 0.0f, true), noDown = measure (-60.0f, 0.0f, true), noUp = measure (0.0f, -60.0f, true),
                       louder = measure (6.0f, 0.0f, true), off = measure (0.0f, 0.0f, false);
            check (noDown.first < both.first - 40.0 && std::abs (noDown.second - both.second) < 1.0
                       && noUp.second < both.second - 40.0 && std::abs (noUp.first - both.first) < 1.0
                       && std::abs (louder.first - both.first - 6.0) < 1.0 && off.first < -150.0 && off.second < -150.0,
                   "Mecanismo: Entrada and Salida set each strike apart, On/Off silences both",
                   "down " + juce::String (both.first, 1) + " / " + juce::String (noDown.first, 1) + " / +6: " + juce::String (louder.first, 1)
                       + " dB; up " + juce::String (both.second, 1) + " / " + juce::String (noUp.second, 1) + " dB; off " + juce::String (off.first, 0));
        }

        // Which gestures suit which sound: a pluck takes a hammer, a pick or a key; a soft held
        // tone, a bow or a breath.
        {
            const auto padAudio = makePad (220.0f, 2.0);
            const auto pad = SampleAnalysis::analyse (padAudio, sr, "pad", false);
            const auto onPluck = Mechanism::fit (*pluck, pluck->detected.attack * 1000.0f, pluck->detected.sustain, false);
            const auto onPad = Mechanism::fit (*pad, pad->detected.attack * 1000.0f, 0.8f, true);
            const auto pc = [] (float v) { return juce::String (juce::roundToInt (100.0f * v)); };
            check ((onPluck.best == Mechanism::Piano || onPluck.best == Mechanism::Pick || onPluck.best == Mechanism::Key)
                       && onPluck.score[Mechanism::Breath] < 0.5f
                       && (onPad.best == Mechanism::Bow || onPad.best == Mechanism::Breath) && onPad.score[Mechanism::Pick] < 0.45f,
                   "Mecanismo recommends a strike for a pluck and a bow or a breath for a soft held tone",
                   "pluck: best " + juce::String (onPluck.best) + ", breath " + pc (onPluck.score[Mechanism::Breath])
                       + " %; pad: best " + juce::String (onPad.best) + ", pick " + pc (onPad.score[Mechanism::Pick]) + " %");

            // The breath goes on for as long as the note: heard in the middle of a held pad.
            auto b = defaultsFor (*pad);
            b[P::AmpR] = 40.0f;
            b[P::SustainType] = (float) P::SustainHold;
            const auto padDry = play (pad, b, 57, 1.2, 1.4);
            b[P::RelNoise] = 0.6f;
            b[P::MechType] = (float) Mechanism::Breath;
            const auto padWet = play (pad, b, 57, 1.2, 1.4);
            std::vector<float> air (padDry.size());
            for (size_t i = 0; i < air.size(); ++i)
                air[i] = padWet[i] - padDry[i];
            const double breath = 20.0 * std::log10 (std::max (1.0e-9, rms (air, 0.5, 1.0)) / rms (padDry, 0.5, 1.0));
            check (breath > -40.0 && breath < -10.0, "Soplo: the air is heard while the note is held", juce::String (breath, 1) + " dB under the note");
        }

        // The pick joins the pluck: its strike on the pluck's (one attack, not two), and
        // nothing an octave under the note (no bass of its own).
        {
            auto k = q;
            k[P::RelNoise] = 0.6f;
            k[P::MechType] = (float) Mechanism::Pick;
            const auto wet = play (pluck, k, 60, 0.6, 1.2);         // as long as dry
            std::vector<float> pick (dry.size());
            for (size_t i = 0; i < pick.size(); ++i)
                pick[i] = wet[i] - dry[i];
            const auto strike = [] (const std::vector<float>& x)
            {
                const int w = (int) (0.001 * sr);
                std::vector<double> e;
                for (int i = 0; i + w <= (int) (0.04 * sr); i += w / 4)
                {
                    double s = 0.0;
                    for (int j = i; j < i + w; ++j)
                        s += (double) x[(size_t) j] * x[(size_t) j];
                    e.push_back (s);
                }
                const double top = *std::max_element (e.begin(), e.end());
                size_t k = 0;
                while (k < e.size() && e[k] < 0.25 * top)
                    ++k;
                while (k + 1 < e.size() && e[k + 1] >= e[k])
                    ++k;
                return (double) k * 0.25;
            };
            const double apart = strike (pick) - strike (dry);
            const int n = 1 << 14;
            juce::dsp::FFT fft (14);
            std::vector<float> buf ((size_t) n * 2, 0.0f);
            for (int i = 0; i < (int) (0.15 * sr) && i < n; ++i)
                buf[(size_t) i] = pick[(size_t) i] * (0.5f - 0.5f * std::cos (6.2831853f * (float) i / (float) (0.15 * sr)));
            fft.performFrequencyOnlyForwardTransform (buf.data());
            double below = 0.0, all = 1.0e-30;
            for (int i = 1; i < n / 2; ++i)
            {
                const double f = i * sr / n, power = (double) buf[(size_t) i] * buf[(size_t) i];
                all += power;
                if (f < 110.0)                  // the pluck plays at 220 Hz: an octave under it
                    below += power;
            }
            const double bass = 10.0 * std::log10 (below / all + 1.0e-12);
            check (std::abs (apart) <= 1.5 && bass < -25.0, "Púa: one attack with the pluck's, and no bass of its own",
                   juce::String (apart, 2) + " ms from the pluck's strike, " + juce::String (bass, 1) + " dB an octave under the note");
        }

        // The sustain pedal of the piano: heard when it goes down and up, only with Piano.
        auto pedal = [&] (int type, float amount)
        {
            auto m = q;
            m[P::RelNoise] = amount;
            m[P::MechType] = (float) type;
            SynthEngine e;
            e.prepare (sr);
            e.prepareFx (m);
            e.setSample (pluck);
            juce::AudioBuffer<float> buf (2, 512);
            double energy = 0.0;
            for (int pos = 0; pos < (int) (1.5 * sr); pos += 512)
            {
                juce::MidiBuffer midi;
                if (pos == 0)
                    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
                if (pos == 512 * 70)
                    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
                e.process (buf, midi, m, 120.0);
                for (int i = 0; i < 512; ++i)
                    energy += (double) buf.getSample (0, i) * buf.getSample (0, i);
            }
            return 10.0 * std::log10 (energy / (1.5 * sr) + 1.0e-20) - 20.0 * std::log10 (rms (dry, 0.0, 0.05));
        };
        const double pianoPedal = pedal (Mechanism::Piano, 0.6f), pickPedal = pedal (Mechanism::Pick, 0.6f), off = pedal (Mechanism::Piano, 0.0f);
        check (pianoPedal > -40.0 && pianoPedal < -6.0 && pickPedal < -120.0 && off < -120.0, "Mecanismo: the piano's sustain pedal is heard, only with Piano",
               juce::String (pianoPedal, 1) + " dB, with Púa " + juce::String (pickPedal, 1) + " dB, at 0 " + juce::String (off, 1) + " dB");
    }

    // --- ADSR mode Sintetizador ------------------------------------------------
    for (const int engineMode : { (int) P::EngineRebuild, (int) P::EngineReenvelope, (int) P::EngineResynth, (int) P::EngineHybrid })
    {
        auto q = defaultsFor (*pluck);
        q[P::Engine] = (float) engineMode;
        q[P::EnvMode] = (float) P::EnvSynth;
        q[P::AmpA] = 5.0f;
        q[P::AmpD] = 300.0f;
        q[P::AmpS] = 0.6f;
        q[P::AmpFade] = 0.0f;
        const auto y = play (pluck, q, 60, 3.5, 3.6);
        const double held = 20.0 * std::log10 (rms (y, 1.0, 1.5) / rms (y, 2.8, 3.3));
        const double drop = 20.0 * std::log10 (rms (y, 0.0, 0.03) / rms (y, 1.0, 1.5));
        check (std::abs (held) < 2.0 && drop > 2.0, "Sintetizador: decay to a steady sustain",
               "engine " + juce::String (engineMode) + ": " + juce::String (drop, 1) + " dB decay, " + juce::String (held, 1) + " dB drift");
    }

    // --- Filter: the circuits that saturate do not fold their harmonics back into the band ----
    {
        double worst = -200.0;
        const int n = 1 << 15;
        juce::dsp::FFT fft (15);
        for (const int circuit : { (int) P::OSR, (int) P::MS2, (int) P::SMP, (int) P::PRD })
        {
            VoiceFilter f;
            f.prepare (sr);
            f.set (P::LP, 1, circuit, 18000.0f, 0.3f, 24.0f, 0.5f, 1.0f);
            const double hz = std::round (6211.1 * n / sr) * sr / n;
            std::vector<float> out ((size_t) n * 2, 0.0f);
            for (int i = 0; i < n + 4096; ++i)
            {
                float l = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / sr), r = l;
                f.process (l, r);
                if (i >= 4096)
                    out[(size_t) (i - 4096)] = l * (0.5f - 0.5f * std::cos (6.2831853f * (float) (i - 4096) / (float) n));
            }
            fft.performFrequencyOnlyForwardTransform (out.data());
            const int bin0 = (int) std::round (hz * n / sr);
            double tone = 0.0, folded = 0.0;
            for (int k = 2; k < n / 2; ++k)
            {
                const double power = (double) out[(size_t) k] * out[(size_t) k];
                const int h = (int) std::round ((double) k / bin0);
                const bool harmonic = h >= 1 && std::abs (k - h * bin0) <= 3;
                if (harmonic && h == 1)
                    tone += power;
                else if (! harmonic)
                    folded += power;           // the true harmonics of the tone are not counted
            }
            worst = std::max (worst, 10.0 * std::log10 (folded / std::max (1.0e-30, tone) + 1.0e-20));
        }
        check (worst < -35.0, "filter: 24 dB of drive on a 6.2 kHz tone folds almost nothing back", juce::String (worst, 1) + " dB in the worst circuit");
    }

    // --- Engines B, C, D: a dry hit held at 70 % stays near the sample's own peak --------------
    {
        const auto drumAudio = makeDrum (1.0);
        const auto drum = SampleAnalysis::analyse (drumAudio, sr, "drum", false);
        double worst = -200.0;
        for (const int engineMode : { (int) P::EngineReenvelope, (int) P::EngineResynth, (int) P::EngineHybrid })
        {
            auto q = defaultsFor (*drum);
            q[P::Engine] = (float) engineMode;
            q[P::AmpS] = 0.7f;
            q[P::AmpD] = 300.0f;
            q[P::SustainType] = (float) P::SustainHold;
            const auto y = play (drum, q, 67, 2.0, 2.5);
            float peak = 0.0f;
            for (const float v : y)
                peak = std::max (peak, std::abs (v));
            worst = std::max (worst, 20.0 * std::log10 (peak / drum->peakLevel));
        }
        check (worst < 2.6, "engines B, C, D: a dry hit held at 70 % stays within 2.5 dB of the sample's peak", juce::String (worst, 1) + " dB over it at most");
    }

    // --- Room: as it comes, a chord of four notes stays under the sample's own peak ------------
    for (const auto& [name, sample] : { std::pair<const char*, std::shared_ptr<const SampleData>> { "pluck", pluck } })
    {
        auto q = defaultsFor (*sample);
        q[P::Volume] = P::defs()[P::Volume].def;
        SynthEngine e;
        e.prepare (sr);
        e.prepareFx (q);
        e.setSample (sample);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer chord;
        for (const int n : { 60, 64, 67, 72 })
            chord.addEvent (juce::MidiMessage::noteOn (1, n, 1.0f), 0);
        float peak = 0.0f;
        for (int b = 0; b < (int) (1.5 * sr / 512); ++b)
        {
            e.process (buf, b == 0 ? chord : juce::MidiBuffer(), q, 120.0);
            peak = std::max (peak, buf.getMagnitude (0, 512));
        }
        const double room = 20.0 * std::log10 (sample->peakLevel / std::max (1.0e-9f, peak));
        // Volume -6 dB (asked for, 2026-11-14): a full-velocity chord may pass the sample's peak by some dB; the
        // processor's soft saturation keeps the output under -0.6 dBFS.
        check (room > -9.0, "as it comes, a chord of four notes stays within 9 dB of the sample's own peak",
               juce::String (name) + ": " + juce::String (room, 1) + " dB to spare (Volume " + juce::String (P::defs()[P::Volume].def, 0) + " dB)");
    }

    // --- Warp: the pitch apart from the speed ------------------------------------
    {
        // Time for the note to fall 20 dB under its loudest 20 ms.
        const auto lengthOf = [] (const std::vector<float>& y)
        {
            const int w = (int) (0.02 * sr), n = (int) y.size() / w;
            std::vector<double> e ((size_t) n);
            for (int k = 0; k < n; ++k)
                e[(size_t) k] = rms (y, k * 0.02, (k + 1) * 0.02);
            const int top = (int) (std::max_element (e.begin(), e.end()) - e.begin());
            for (int k = top; k < n; ++k)
                if (e[(size_t) k] < e[(size_t) top] * 0.1)
                    return k * 0.02;
            return n * 0.02;
        };
        // Spectral centre of gravity of the body: where the formants sit, roughly.
        const auto centroid = [] (const std::vector<float>& y, double at)
        {
            constexpr int order = 14, n = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<float> w ((size_t) 2 * n, 0.0f);
            for (int i = 0; i < n; ++i)
                w[(size_t) i] = y[(size_t) ((int) (at * sr) + i)] * (0.5f - 0.5f * std::cos (6.2831853f * (float) i / n));
            fft.performFrequencyOnlyForwardTransform (w.data(), true);
            double sum = 0.0, weighted = 0.0;
            for (int k = 1; k < n / 2; ++k)
            {
                sum += w[(size_t) k];
                weighted += w[(size_t) k] * k * sr / n;
            }
            return weighted / std::max (1.0e-9, sum);
        };
        float samplePeak = 0.0f;
        for (int i = 0; i < pluck->length; ++i)
            samplePeak = std::max (samplePeak, std::abs (pluck->audio.getSample (0, i)));

        const char* engineNames[] { "A", "B", "C", "D" };
        for (int engineMode = P::EngineRebuild; engineMode <= P::EngineHybrid; ++engineMode)
        {
            auto q = p;
            q[P::Engine] = (float) engineMode;
            const int root = pluck->nearestC();   // the key where the sample sounds as recorded
            const auto own = play (pluck, q, root, 1.5, 2.0);
            const double ownLength = lengthOf (own), ownLevel = rms (own, 0.1, 0.6), ownCentre = centroid (own, 0.05);
            const float ownHz = detectHz (own, 0.15);
            q[P::WarpMode] = (float) P::WarpRepitch;
            const auto up = play (pluck, q, root + 12, 1.5, 2.0);
            const double repitchCentre = centroid (up, 0.05);

            for (int mode : { (int) P::WarpTones, (int) P::WarpTexture })
            {
                q[P::WarpMode] = (float) mode;
                const juce::String what = juce::String ("Warp ") + (mode == P::WarpTones ? "Tonos" : "Textura") + ", engine " + engineNames[engineMode];
                float worst = 0.0f;
                for (int semis : { -12, 12 })
                {
                    const auto y = play (pluck, q, root + semis, 1.5, 2.0);
                    const float hz = detectHz (y, 0.15);
                    const float off = 12.0f * std::log2 (std::max (1.0f, hz) / (ownHz * std::pow (2.0f, semis / 12.0f)));
                    const double lengthRatio = lengthOf (y) / ownLength;
                    const double levelDb = 20.0 * std::log10 (rms (y, 0.1, 0.6) / ownLevel);
                    for (float v : y)
                        worst = std::max (worst, std::abs (v));
                    check (std::abs (off) < 0.3f, (what + ", " + juce::String (semis) + " st: in tune").toRawUTF8(), juce::String (off, 2) + " st");
                    check (lengthRatio > 0.75 && lengthRatio < 1.3, (what + ", " + juce::String (semis) + " st: lasts as long as the note at its own pitch").toRawUTF8(),
                           "x" + juce::String (lengthRatio, 2));
                    check (std::abs (levelDb) < 3.0, (what + ", " + juce::String (semis) + " st: as loud as the note at its own pitch").toRawUTF8(),
                           juce::String (levelDb, 1) + " dB");
                    if (mode == P::WarpTones && semis == 12)
                    {
                        const double centre = centroid (y, 0.05);
                        check (std::abs (std::log2 (centre / ownCentre)) < 0.6 * std::abs (std::log2 (repitchCentre / ownCentre)),
                               (what + ", +12 st: the formants stay nearer than Re-Pitch's").toRawUTF8(),
                               juce::String (centre, 0) + " Hz (own pitch " + juce::String (ownCentre, 0) + ", Re-Pitch " + juce::String (repitchCentre, 0) + ")");
                    }
                }
                check (worst <= samplePeak * 1.34f, (what + ": stays under the ceiling").toRawUTF8(),
                       juce::String (worst / samplePeak, 2) + " x the sample's peak");
            }
        }
        // Re-Pitch is the old reading, sample for sample.
        auto q = p;
        q[P::WarpMode] = (float) P::WarpRepitch;
        check (play (pluck, q, 69, 1.0, 1.2) == play (pluck, p, 69, 1.0, 1.2), "Warp Re-Pitch plays as before", "");

        // Tonos is tried on loading: a note with harmonics keeps it, a chord plays it as Textura in
        // A, B and D (Tonos would melt its notes into one) and the menu says so; C keeps its own.
        check (pluck->tonesHold, "Tonos: a plucked note passes its test on loading", "share " + juce::String (pluck->tonesGrid, 2));
        juce::AudioBuffer<float> chordAudio (2, (int) (2.0 * sr));
        {
            const auto a = makePluck (220.0f, 2.0), c = makePluck (277.18f, 2.0), e = makePluck (329.63f, 2.0);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < chordAudio.getNumSamples(); ++i)
                    chordAudio.setSample (ch, i, 0.5f * (a.getSample (ch, i) + c.getSample (ch, i) + e.getSample (ch, i)));
        }
        {
            // CP9: the note of a file name (pitch class), and none where there is none.
            const std::pair<const char*, int> names[] = { { "Kick_F", 5 }, { "Piano_C4", 0 }, { "bass_F#2", 6 }, { "808_Bb", 10 },
                                                          { "Sub_Hit_G_-_SCIF", 7 }, { "pad_Am", 9 }, { "vox_Ebmin", 3 }, { "Gs_stab", 8 },
                                                          { "A_Day_In_The_Life_loop", -1 }, { "Clap_Snare_TDV", -1 }, { "kick_dull", -1 },
                                                          { "Drum_Beat_122bpm", -1 }, { "Csus_chord", -1 } };
            juce::String wrong;
            for (const auto& n : names)
                if (const int got = SampleAnalysis::noteClassFromName (n.first); got != n.second)
                    wrong << n.first << " -> " << got << " (" << n.second << ")  ";
            check (wrong.isEmpty(), "CP9: the note of a file name, and none where there is none", wrong);
        }
        {
            // CP9 Original: a note started under it sounds as the sample with an instance's defaults (engine A, its
            // detected envelope), whatever the engine, filter and effects set.
            auto s = SampleAnalysis::analyse (chordAudio, sr, "orig", false, true);
            auto heavy = defaultsFor (*s);
            heavy[P::Engine] = (float) P::EngineGranular;
            heavy[P::FltOn] = 1.0f;
            heavy[P::FltCircuit] = 2.0f;
            heavy[P::FltDrive] = 12.0f;
            heavy[(size_t) P::fxType (0)] = 8.0f;          // Reverb
            const auto asIs = play (s, heavy, 60, 1.0, 1.5, true), reference = play (s, defaultsFor (*s), 60, 1.0, 1.5);
            const auto changed = play (s, heavy, 60, 1.0, 1.5);
            double diff = 0.0, peak = 0.0, apart = 0.0;
            for (size_t i = 0; i < reference.size(); ++i)
            {
                diff = std::max (diff, (double) std::abs (asIs[i] - reference[i]));
                apart = std::max (apart, (double) std::abs (changed[i] - reference[i]));
                peak = std::max (peak, (double) std::abs (reference[i]));
            }
            check (diff <= 1.0e-4 * peak && apart > 0.1 * peak, "CP9: Original plays the sample as it is, whatever is set",
                   juce::String ("difference with A as is ") + juce::String (juce::Decibels::gainToDecibels (diff / peak + 1.0e-12), 1)
                       + " dB; the settings themselves " + juce::String (juce::Decibels::gainToDecibels (apart / peak + 1.0e-12), 1) + " dB away");
        }
        std::shared_ptr<const SampleData> chord = SampleAnalysis::analyse (chordAudio, sr, "chord", false);
        check (chord->pitched && ! chord->tonesHold, "Tonos: a chord fails its test on loading",
               juce::String ("pitched ") + (chord->pitched ? "yes" : "no") + ", share " + juce::String (chord->tonesGrid, 2));
        const int chordRoot = chord->nearestC();
        for (int engineMode : { (int) P::EngineRebuild, (int) P::EngineReenvelope, (int) P::EngineHybrid })
        {
            auto t = p;
            t[P::Engine] = (float) engineMode;
            t[P::WarpMode] = (float) P::WarpTones;
            const auto tones = play (chord, t, chordRoot + 12, 1.0, 1.4);
            t[P::WarpMode] = (float) P::WarpTexture;
            const auto texture = play (chord, t, chordRoot + 12, 1.0, 1.4);
            check (tones == texture && SynthEngine::tonesFallBack (*chord, engineMode),
                   (juce::String ("Tonos on a chord plays as Textura, engine ") + engineNames[engineMode]).toRawUTF8(), "");
        }
        check (! SynthEngine::tonesFallBack (*chord, P::EngineResynth) && ! SynthEngine::tonesFallBack (*pluck, P::EngineRebuild),
               "Tonos -> Textura is shown only where it happens", "");
    }

    // --- CPU -----------------------------------------------------------------
    auto cpuPercent = [&] (int voices, int unison, int engineMode, int mechanism = -1, int warp = 0)
    {
        auto q = p;
        q[P::WarpMode] = (float) warp;
        if (mechanism >= 0)
        {
            q[P::RelNoise] = 0.6f;
            q[P::MechType] = (float) mechanism;
        }
        q[P::Unison] = (float) unison;
        q[P::Engine] = (float) engineMode;
        q[P::FltOn] = 1.0f;
        q[P::FltCircuit] = (float) P::OSR;
        SynthEngine e;
        e.prepare (sr);
        e.setSample (pluck);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer on;
        for (int n = 0; n < voices; ++n)
            on.addEvent (juce::MidiMessage::noteOn (1, 40 + n, 1.0f), 0);
        const auto start = juce::Time::getMillisecondCounterHiRes();
        const int blocks = (int) (2.0 * sr / 512.0);
        for (int b = 0; b < blocks; ++b)
            e.process (buf, b == 0 ? on : juce::MidiBuffer(), q, 120.0);
        return (juce::Time::getMillisecondCounterHiRes() - start) / 20.0;   // % of one core
    };

    // 64 lanes of grains, each read band-limited (the sinc of engine A since 2026-11-15: a 19 kHz tone moved an
    // octave folded back at -9 dB with the cubic before, -107 dB now) at some 20 % more than the cubic cost.
    const double heavy = cpuPercent (16, 4, P::EngineHybrid);
    check (heavy < 60.0, "16 voices x 4 unison (hybrid, granular) under 60 % of a core", juce::String (heavy, 1) + " % of one core");
    const double resynth = cpuPercent (16, 1, P::EngineResynth);
    check (resynth < 50.0, "16 voices engine C under half a core", juce::String (resynth, 1) + " % of one core");
    const double rebuildCpu = cpuPercent (32, 1, P::EngineRebuild);
    check (rebuildCpu < 50.0, "32 voices engine A under half a core", juce::String (rebuildCpu, 1) + " % of one core");
    const double breathCpu = cpuPercent (32, 1, P::EngineRebuild, Mechanism::Breath) - rebuildCpu;
    const double bowCpu = cpuPercent (32, 1, P::EngineRebuild, Mechanism::Bow) - rebuildCpu;
    check (std::max (breathCpu, bowCpu) < 15.0, "Mecanismo on 32 voices (breath or bow all along the note) costs little",
           "breath " + juce::String (breathCpu, 1) + " %, bow " + juce::String (bowCpu, 1) + " % of one core more");
    double warpWorst = 0.0;
    juce::String warpDetail;
    for (int engineMode : { (int) P::EngineRebuild, (int) P::EngineResynth, (int) P::EngineHybrid })
        for (int warp : { (int) P::WarpTones, (int) P::WarpTexture })
        {
            const double extra = cpuPercent (16, 1, engineMode, -1, warp) - cpuPercent (16, 1, engineMode);
            warpWorst = std::max (warpWorst, extra);
            warpDetail << "ACD"[engineMode == P::EngineRebuild ? 0 : (engineMode == P::EngineResynth ? 1 : 2)]
                       << (warp == P::WarpTones ? " Tonos " : " Textura ") << juce::String (extra, 1) << "  ";
        }
    check (warpWorst < 20.0, "Warp on 16 voices costs little more", warpDetail + "(% of one core more)");
    std::printf ("(info) engine A 16 voices x 4 unison: %.0f %% of one core\n", cpuPercent (16, 4, P::EngineRebuild));
    std::printf ("(info) extreme case 32 voices x 8 unison: %.0f %% of one core\n", cpuPercent (32, 8, P::EngineHybrid));

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "SOME CHECKS FAILED", failures);
    return failures == 0 ? 0 : 1;
}
