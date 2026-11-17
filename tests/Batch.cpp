// Runs engine A over a whole folder of samples and writes one CSV row per file, so that
// failures show up by numbers across many kinds of sound. Never modifies the samples.
// Usage: ReanimaBatch.exe <samples folder> <out.csv> [shard] [shards]
//        ReanimaBatch.exe motores <samples folder | list.txt> <out.csv> [every N]: every engine, one row per file and engine
//        ReanimaBatch.exe info <samples folder | list.txt> <out.csv>: what the analysis finds in each file

#include "Params.h"
#include "SampleAnalysis.h"
#include "SynthEngine.h"
#include "Mechanism.h"
#include "Filters.h"
#include "Stretch.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdio>

namespace
{
ParamSnapshot defaultsFor (const SampleData& s)
{
    ParamSnapshot p {};
    for (int i = 0; i < P::Count; ++i)
        p[(size_t) i] = P::defs()[(size_t) i].def;
    p[P::Volume] = 0.0f;           // the engine at unity: the -12 dB default is room for chords

    auto set = [&p] (int index, float v)
    {
        const auto& d = P::defs()[(size_t) index];
        p[(size_t) index] = juce::jlimit (d.min, d.max, v);
    };

    set (P::AmpA, s.detected.attack * 1000.0f);
    set (P::AmpCurve, s.detected.curve);
    set (P::AmpD, s.detected.decay * 1000.0f);
    set (P::AmpS, s.detected.sustain);
    set (P::AmpFade, s.detected.fadeDbPerSec);
    set (P::AmpR, s.detected.release * 1000.0f);
    set (P::TransientMs, s.transientMs);
    p[P::Engine] = (float) P::EngineRebuild;
    p[P::StartPos] = s.length > 0 ? (float) (s.onset / s.length) : 0.0f;
    return p;
}

int listenMode = SynthEngine::ListenAll;     // nota ... listen=1|2: hear the note alone or Mecanismo alone

juce::AudioBuffer<float> play (std::shared_ptr<const SampleData> s, const ParamSnapshot& p, int note, double held, double total, double sr)
{
    SynthEngine e;
    e.listen.store (listenMode);
    e.prepare (sr);
    e.setSample (s);
    e.prepareFx (p);

    const int totalSamples = (int) (total * sr), heldSamples = (int) (held * sr);
    juce::AudioBuffer<float> out (2, totalSamples);
    out.clear();
    juce::AudioBuffer<float> buf (2, 512);

    for (int pos = 0; pos < totalSamples; pos += 512)
    {
        const int n = std::min (512, totalSamples - pos);
        buf.setSize (2, n, false, false, true);
        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
        if (pos <= heldSamples && heldSamples < pos + n)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), heldSamples - pos);
        e.process (buf, midi, p, 120.0);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, buf, ch, 0, n);
    }
    return out;
}

double rmsDb (const juce::AudioBuffer<float>& a, double from, double to, double sr)
{
    const int i0 = (int) (from * sr), i1 = std::min (a.getNumSamples(), (int) (to * sr));
    double e = 0.0;
    for (int i = i0; i < i1; ++i)
    {
        const double m = 0.5 * ((double) a.getSample (0, i) + a.getSample (1, i));
        e += m * m;
    }
    return 10.0 * std::log10 (e / std::max (1, i1 - i0) + 1.0e-12);
}

double swingDb (const juce::AudioBuffer<float>& a, double from, double to, double sr)
{
    double s1 = 0.0, s2 = 0.0;
    int count = 0;
    for (double t = from; t + 0.05 < to; t += 0.05)
    {
        const double db = rmsDb (a, t, t + 0.05, sr);
        s1 += db;
        s2 += db * db;
        ++count;
    }
    const double mean = s1 / std::max (1, count);
    return std::sqrt (std::max (0.0, s2 / std::max (1, count) - mean * mean));
}

double widthDb (const juce::AudioBuffer<float>& a, int from, int to)
{
    double mid = 0.0, side = 0.0;
    for (int i = from; i < std::min (to, a.getNumSamples()); ++i)
    {
        const double l = a.getSample (0, i), r = a.getSample (1, i);
        mid += (l + r) * (l + r) * 0.25;
        side += (l - r) * (l - r) * 0.25;
    }
    return 10.0 * std::log10 ((side + 1.0e-12) / (mid + 1.0e-12));
}

// Between the channels: 1 is mono, 0 unrelated, -1 out of phase.
double correlation (const juce::AudioBuffer<float>& a, int from, int to)
{
    double lr = 0.0, ll = 1.0e-12, rr = 1.0e-12;
    for (int i = from; i < std::min (to, a.getNumSamples()); ++i)
    {
        const double l = a.getSample (0, i), r = a.getSample (1, i);
        lr += l * r;
        ll += l * l;
        rr += r * r;
    }
    return lr / std::sqrt (ll * rr);
}

// Energy of the sample-to-sample difference (a click is mostly that) in a window.
double edgeDb (const juce::AudioBuffer<float>& a, int from, int to)
{
    double e = 0.0;
    from = std::max (1, from);
    to = std::min (to, a.getNumSamples());
    for (int ch = 0; ch < 2; ++ch)
        for (int i = from; i < to; ++i)
        {
            const double d = (double) a.getSample (ch, i) - a.getSample (ch, i - 1);
            e += d * d;
        }
    return 10.0 * std::log10 (e / std::max (1, 2 * (to - from)) + 1.0e-20);
}

bool allFinite (const juce::AudioBuffer<float>& a)
{
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            if (! std::isfinite (a.getSample (ch, i)))
                return false;
    return true;
}

double snrUntouched (const juce::AudioBuffer<float>& y, const SampleData& s, double heldSeconds)
{
    double sig = 0.0, err = 0.0;
    const int start = (int) s.onset;
    const int n = std::min ((int) (heldSeconds * s.sampleRate), (int) (s.usableEnd - s.onset));
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i)
        {
            const double ref = s.audio.getSample (ch, start + i), o = y.getSample (ch, i);
            sig += ref * ref;
            err += (ref - o) * (ref - o);
        }
    return 10.0 * std::log10 (sig / std::max (1.0e-20, err) + 1.0e-12);
}
}


// Clicks: |second difference| above 8 times its own RMS over the surrounding 10 ms, above -48 dB under
// the render's peak and 5 % of the signal around it (as ReanimaClickCheck).
int countClicks (const juce::AudioBuffer<float>& a, double rate)
{
    std::vector<double> at;
    const int n = a.getNumSamples(), w = (int) (0.01 * rate);
    const float floor = 0.004f * std::max (1.0e-6f, a.getMagnitude (0, n));
    for (int c = 0; c < 2; ++c)
    {
        const float* x = a.getReadPointer (c);
        std::vector<float> d2 ((size_t) n, 0.0f);
        for (int i = 2; i < n; ++i)
            d2[(size_t) i] = x[i] - 2.0f * x[i - 1] + x[i - 2];
        std::vector<double> cum ((size_t) n + 1, 0.0);
        for (int i = 0; i < n; ++i)
            cum[(size_t) i + 1] = cum[(size_t) i] + (double) d2[(size_t) i] * d2[(size_t) i];
        for (int i = w; i < n - w; ++i)
        {
            const double around = std::sqrt ((cum[(size_t) (i + w)] - cum[(size_t) (i - w)] - (double) d2[(size_t) i] * d2[(size_t) i]) / (2.0 * w));
            float local = 0.0f;
            for (int k = i - 64; k <= i + 64; k += 4)
                local = std::max (local, std::abs (x[juce::jlimit (0, n - 1, k)]));
            if (std::abs (d2[(size_t) i]) > std::max (0.001f, floor) && std::abs (d2[(size_t) i]) > 8.0 * around + 1.0e-6
                && std::abs (d2[(size_t) i]) > 0.05f * local)
            {
                const double t = i / rate;
                bool near = false;
                for (const double b : at)
                    near = near || std::abs (b - t) < 0.02;
                if (! near)
                    at.push_back (t);
            }
        }
    }
    return (int) at.size();
}

// ataque <lista.txt> <out.csv>: engine A under Remodelar with the attack and the sustain moved, against the
// sample as it is. For each: when the note gets within 3 dB of its loudest (reachMs), how loud it is just
// after the attack asked for against the untouched note's loudest (lvlDb), the held level from 1 to 2.5 s
// against the untouched one (holdDb), and the clicks.
int attackLab (const juce::Array<juce::File>& files, const char* out)
{
    std::FILE* csv = nullptr;
    if (fopen_s (&csv, out, "w") != 0)
        return 1;
    std::fprintf (csv, "file,detA,detS,seconds,case,attackMs,sustain,hold,reachMs,lvlDb,earlyDb,holdDb,clicks,finite\n");
    for (const auto& file : files)
    {
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (file, raw, rate))
            continue;
        const auto s = SampleAnalysis::analyse (raw, rate, file.getFileNameWithoutExtension(), false, true);
        const double sr = 48000.0, held = 3.0, total = 3.6;
        const auto base = defaultsFor (*s);
        const auto name = file.getFileName().replaceCharacter (',', '_');
        const float detA = s->detected.attack * 1000.0f, detS = s->detected.sustain;

        juce::AudioBuffer<float> ref;
        double refTop = -200.0;
        struct Case { const char* name; float attack, sustain; bool hold; };
        std::vector<Case> cases { { "det", detA, detS, false } };
        if (detA >= 20.0f)
            cases.push_back ({ "corto", std::max (1.0f, detA / 8.0f), detS, false });
        cases.push_back ({ "largo", std::min (1500.0f, std::max (200.0f, detA * 4.0f)), detS, false });
        for (const bool h : { false, true })
        {
            cases.push_back ({ h ? "sus0h" : "sus0", detA, 0.0f, h });
            cases.push_back ({ h ? "sus50h" : "sus50", detA, 0.5f, h });
            cases.push_back ({ h ? "sus100h" : "sus100", detA, 1.0f, h });
        }
        for (const auto& c : cases)
        {
            auto p = base;
            p[P::AmpA] = juce::jlimit (P::defs()[P::AmpA].min, P::defs()[P::AmpA].max, c.attack);
            p[P::AmpS] = c.sustain;
            p[P::SustainType] = c.hold ? (float) P::SustainHold : (float) P::SustainNatural;
            const auto y = play (s, p, SynthEngine::rootKey, held, total, sr);
            // 5 ms levels
            std::vector<double> lv;
            for (double t = 0.0; t + 0.005 <= total; t += 0.005)
                lv.push_back (rmsDb (y, t, t + 0.005, sr));
            const auto upTo = (size_t) std::min ((double) lv.size(), std::max (1.0, 2.0 * c.attack * 0.001 + 0.3) / 0.005);
            double top = -200.0;
            for (size_t i = 0; i < upTo; ++i)
                top = std::max (top, lv[i]);
            double reach = -1.0;
            for (size_t i = 0; i < upTo; ++i)
                if (lv[i] >= top - 3.0)
                {
                    reach = i * 5.0;
                    break;
                }
            const double after = rmsDb (y, c.attack * 0.001 + 0.005, c.attack * 0.001 + 0.025, sr);
            // LABDUMP=<folder>: the renders as WAV, to listen to and to find where a click is
            if (const char* dump = std::getenv ("LABDUMP"))
            {
                auto f = juce::File (juce::String::fromUTF8 (dump)).getChildFile (file.getFileNameWithoutExtension() + "__" + c.name + ".wav");
                f.deleteFile();
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
                juce::WavAudioFormat wav;
                if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)))
                    writer->writeFromAudioSampleBuffer (y, 0, y.getNumSamples());
            }
            const double early = rmsDb (y, 0.4, 0.9, sr), hold = rmsDb (y, 1.0, 2.5, sr);
            if (std::string (c.name) == "det")
                refTop = top;
            std::fprintf (csv, "%s,%.1f,%.3f,%.2f,%s,%.1f,%.2f,%d,%.0f,%.1f,%.1f,%.1f,%d,%d\n", name.toRawUTF8(), detA, detS,
                          s->length / s->sampleRate, c.name, c.attack, c.sustain, c.hold ? 1 : 0, reach, after - refTop, early - refTop,
                          hold - refTop, countClicks (y, sr), allFinite (y) ? 1 : 0);
        }
        std::fflush (csv);
    }
    std::fclose (csv);
    return 0;
}

// A folder of samples, or a text file with one path per line.
juce::Array<juce::File> filesIn (const juce::File& where)
{
    juce::Array<juce::File> files;
    if (where.isDirectory())
        files = where.findChildFiles (juce::File::findFiles, false, "*.wav");
    else
        for (const auto& line : juce::StringArray::fromLines (where.loadFileAsString()))
            if (line.trim().isNotEmpty())
                files.add (juce::File (line.trim()));
    files.sort();
    return files;
}

// What the analysis finds in each file, and how the file itself is made.
int info (const juce::Array<juce::File>& files, const char* out)
{
    std::FILE* csv = nullptr;
    if (fopen_s (&csv, out, "w") != 0)
        return 1;
    std::fprintf (csv, "file,seconds,rate,channels,bits,peakDb,peakRmsDb,clipped,dcDb,widthDb,corr,floorDb,pitched,root,onsetMs,attackMs,curve,decayMs,"
                       "sustain,fadeDbS,releaseMs,transientMs,centroidHz,loadMs,"
                       "exTransientDb,exSustain,exNoisiness,exTonal,exLowEdgeHz,mechBest,fitPiano,fitPick,fitBow,fitKey,fitBreath\n");
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    for (const auto& file : files)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (reader == nullptr || ! SampleAnalysis::loadFile (file, raw, rate))
        {
            std::fprintf (csv, "%s,UNREADABLE\n", file.getFileName().replaceCharacter (',', '_').toRawUTF8());
            continue;
        }
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const auto s = SampleAnalysis::analyse (raw, rate, file.getFileNameWithoutExtension(), false, true);
        const double loadMs = juce::Time::getMillisecondCounterHiRes() - t0;
        const int n = s->length;

        int clipped = 0;
        double dc = 0.0, sq = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const float v = s->audio.getSample (ch, i);
                clipped += std::abs (v) >= 0.999f ? 1 : 0;
                dc += v;
                sq += (double) v * v;
            }
        dc /= 2.0 * n;
        const double rmsAll = std::sqrt (sq / (2.0 * n));

        // Brightness: spectral centroid over 4096 samples from the peak.
        constexpr int order = 12, size = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> w ((size_t) 2 * size, 0.0f);
        const int from = std::min ((int) s->peak, std::max (0, n - size));
        for (int i = 0; i < size && from + i < n; ++i)
            w[(size_t) i] = 0.5f * (s->audio.getSample (0, from + i) + s->audio.getSample (1, from + i))
                          * (0.5f - 0.5f * std::cos (6.2831853f * (float) i / size));
        fft.performFrequencyOnlyForwardTransform (w.data(), true);
        double num = 0.0, den = 1.0e-12;
        for (int k = 1; k < size / 2; ++k)
        {
            num += (double) k * rate / size * w[(size_t) k];
            den += w[(size_t) k];
        }

        const int tail = std::max (1, std::min (n, (int) (0.05 * rate)));
        const double floorRms = rmsDb (s->audio, (n - tail) / rate, n / rate, rate);
        const auto& d = s->detected;
        std::fprintf (csv, "%s,%.3f,%.0f,%d,%d,%.2f,%.2f,%d,%.1f,%.1f,%.3f,%.1f,%d,%.2f,%.1f,%.1f,%.2f,%.0f,%.3f,%.1f,%.0f,%.1f,%.0f,%.0f",
                      file.getFileName().replaceCharacter (',', '_').toRawUTF8(), n / rate, rate, (int) reader->numChannels, (int) reader->bitsPerSample,
                      juce::Decibels::gainToDecibels (s->peakLevel), juce::Decibels::gainToDecibels (s->peakRms), clipped,
                      20.0 * std::log10 (std::abs (dc) + 1.0e-9) - 20.0 * std::log10 (rmsAll + 1.0e-12),
                      widthDb (s->audio, 0, n), correlation (s->audio, 0, n), floorRms - 20.0 * std::log10 (s->peakRms),
                      s->pitched ? 1 : 0, s->rootMidi, s->onset / rate * 1000.0, d.attack * 1000.0f, d.curve, d.decay * 1000.0f, d.sustain,
                      d.fadeDbPerSec, d.release * 1000.0f, s->transientMs, num / den, loadMs);
        // The articulations: how the sample's excitation reads, and how well each Mecanismo suits it,
        // played with its own attack and not held (what the panel shows on loading).
        const auto& e = s->excitation;
        const auto fit = Mechanism::fit (*s, d.attack * 1000.0f, d.sustain, false);
        std::fprintf (csv, ",%.1f,%.3f,%.3f,%.3f,%.0f,%d,%.3f,%.3f,%.3f,%.3f,%.3f\n", e.transientDb, e.sustain, e.noisiness, e.tonal,
                      e.lowEdgeHz, fit.best, fit.score[0], fit.score[1], fit.score[2], fit.score[3], fit.score[4]);
        std::fflush (csv);
    }
    std::fclose (csv);
    return 0;
}

// Every engine on every file, held with a sustain and released: what goes wrong where.
int engines (const juce::Array<juce::File>& files, const char* out, int every)
{
    std::FILE* csv = nullptr;
    if (fopen_s (&csv, out, "w") != 0)
        return 1;
    std::fprintf (csv, "file,engine,finite,peakDb,holdErrorDb,holdSwingDb,dcDb,releaseClickDb,tailDb,onsetClickDb,renderMs,seconds,peakAt,srcPeakDb\n");

    for (int index = 0; index < files.size(); index += every)
    {
        const auto& file = files.getReference (index);
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (file, raw, rate))
            continue;
        const auto s = SampleAnalysis::analyse (raw, rate, file.getFileNameWithoutExtension(), false, true);
        const auto name = file.getFileName().replaceCharacter (',', '_');

        for (int engine = 0; engine <= P::EngineDrone; ++engine)
        {
            auto q = defaultsFor (*s);
            q[P::Engine] = (float) engine;
            q[P::AmpS] = 0.7f;
            q[P::AmpR] = 200.0f;
            q[P::AmpD] = std::min (q[P::AmpD], 500.0f);
            q[P::AmpA] = std::min (q[P::AmpA], 300.0f);
            q[P::AmpFade] = 0.0f;
            q[P::SustainType] = (float) P::SustainHold;

            const double sr = rate, held = 2.5, total = 3.2;
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            const auto y = play (s, q, SynthEngine::rootKey + 7, held, total, sr);
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;

            double dc = 0.0;
            const int h0 = (int) (1.0 * sr), h1 = (int) (2.4 * sr);
            for (int i = h0; i < h1; ++i)
                dc += 0.5 * ((double) y.getSample (0, i) + y.getSample (1, i));
            dc /= (h1 - h0);
            const double holdDb = rmsDb (y, 1.0, 2.4, sr);
            const int off = (int) (held * sr);
            const double expected = 20.0 * std::log10 (0.7 * s->peakRms);

            int peakAt = 0;
            for (int i = 0; i < y.getNumSamples(); ++i)
                if (std::abs (y.getSample (0, i)) > std::abs (y.getSample (0, peakAt)))
                    peakAt = i;
            std::fprintf (csv, "%s,%d,%d,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.0f,%.2f,%.3f,%.2f\n", name.toRawUTF8(), engine, allFinite (y) ? 1 : 0,
                          juce::Decibels::gainToDecibels (y.getMagnitude (0, y.getNumSamples())), holdDb - expected, swingDb (y, 1.0, 2.4, sr),
                          20.0 * std::log10 (std::abs (dc) + 1.0e-9) - holdDb,
                          edgeDb (y, off, off + (int) (0.005 * sr)) - edgeDb (y, off - (int) (0.05 * sr), off),
                          rmsDb (y, 3.0, 3.2, sr) - holdDb,
                          edgeDb (y, 0, (int) (0.003 * sr)) - edgeDb (y, (int) (0.003 * sr), (int) (0.05 * sr)),
                          ms, s->length / rate, peakAt / sr, juce::Decibels::gainToDecibels (s->peakLevel));
        }
        std::fflush (csv);
    }
    std::fclose (csv);
    return 0;
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 3)
        return 1;

    if (juce::String (argv[1]) == "pico" && argc > 3)
    {
        // pico <wav> <engine>: where the loudest moment of a held note comes from.
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (juce::File (juce::String::fromUTF8 (argv[2])), raw, rate))
            return 1;
        const auto s = SampleAnalysis::analyse (raw, rate, "pico", false, true);
        std::printf ("len %.3f s  onset %.0f peak %.0f knee %.0f sustainPos %.0f usableEnd %.0f  A %.1f D %.0f S %.2f  early %.0f-%.0f model %.3f env %.3f\n",
                     s->length / rate, s->onset, s->peak, s->knee, s->sustainPos, s->usableEnd, s->detected.attack * 1000, s->detected.decay * 1000,
                     s->detected.sustain, s->early.start, s->early.end, s->early.model, s->early.env);
        for (const int hold : { 0, 1 })
            for (const float sus : { 0.0f, 0.7f })
            {
                auto q = defaultsFor (*s);
                q[P::Engine] = (float) juce::String (argv[3]).getIntValue();
                q[P::AmpS] = sus;
                q[P::AmpR] = 200.0f;
                q[P::AmpD] = std::min (q[P::AmpD], 500.0f);
                q[P::AmpA] = std::min (q[P::AmpA], 300.0f);
                q[P::AmpFade] = 0.0f;
                q[P::SustainType] = (float) hold;
                const auto y = play (s, q, SynthEngine::rootKey + 7, 2.5, 3.2, rate);
                int at = 0;
                for (int i = 0; i < y.getNumSamples(); ++i)
                    if (std::abs (y.getSample (0, i)) > std::abs (y.getSample (0, at)))
                        at = i;
                std::printf ("hold %d S %.1f: peak %.1f dB at %.3f s\n", hold, sus, juce::Decibels::gainToDecibels (std::abs (y.getSample (0, at))), at / rate);
            }
        return 0;
    }

    if (juce::String (argv[1]) == "render" && argc > 4)
    {
        // render <wav> <engine> <out.wav>: the held note of the "motores" test, to listen to or inspect.
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (juce::File (juce::String::fromUTF8 (argv[2])), raw, rate))
            return 1;
        const auto s = SampleAnalysis::analyse (raw, rate, "render", false, true);
        auto q = defaultsFor (*s);
        q[P::Engine] = (float) juce::String (argv[3]).getIntValue();
        q[P::AmpS] = 0.7f;
        q[P::AmpR] = 200.0f;
        q[P::AmpD] = std::min (q[P::AmpD], 500.0f);
        q[P::AmpA] = std::min (q[P::AmpA], 300.0f);
        q[P::AmpFade] = 0.0f;
        q[P::SustainType] = (float) P::SustainHold;
        for (int a = 5; a < argc; ++a)      // extra settings: id=value, in the units of the knob
        {
            const juce::String setting (argv[a]);
            for (int i = 0; i < P::Count; ++i)
                if (setting.upToFirstOccurrenceOf ("=", false, false) == P::defs()[(size_t) i].id)
                    q[(size_t) i] = setting.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
        }
        double held = 2.5;
        for (int a = 5; a < argc; ++a)
            if (juce::String (argv[a]).startsWith ("held="))
                held = juce::String (argv[a]).fromFirstOccurrenceOf ("=", false, false).getDoubleValue();
        const auto y = play (s, q, SynthEngine::rootKey + 7, held, held + 0.7, rate);
        juce::File out (juce::String::fromUTF8 (argv[4]));
        out.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (out);
        juce::WavAudioFormat wav;
        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
            writer->writeFromAudioSampleBuffer (y, 0, y.getNumSamples());
        return 0;
    }

    if (juce::String (argv[1]) == "nota" && argc > 5)
    {
        // nota <wav> <engine> <semitones> <out.wav> [held=s] [id=value ...]: one note, held, at
        // that distance from the sample's own note, with the detected envelope.
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (juce::File (juce::String::fromUTF8 (argv[2])), raw, rate))
            return 1;
        const auto s = SampleAnalysis::analyse (raw, rate, "nota", false, true);
        auto q = defaultsFor (*s);
        q[P::Engine] = (float) juce::String (argv[3]).getIntValue();
        double held = 1.5;
        for (int a = 6; a < argc; ++a)
        {
            const juce::String setting (argv[a]);
            if (setting.startsWith ("held="))
                held = setting.fromFirstOccurrenceOf ("=", false, false).getDoubleValue();
            if (setting.startsWith ("listen="))
                listenMode = setting.fromFirstOccurrenceOf ("=", false, false).getIntValue();
            for (int i = 0; i < P::Count; ++i)
                if (setting.upToFirstOccurrenceOf ("=", false, false) == P::defs()[(size_t) i].id)
                    q[(size_t) i] = setting.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
        }
        const int note = SynthEngine::rootKey + juce::String (argv[4]).getIntValue();
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto y = play (s, q, note, held, held + 1.0, rate);
        std::printf ("%.1f ms\n", juce::Time::getMillisecondCounterHiRes() - t0);
        juce::File out (juce::String::fromUTF8 (argv[5]));
        out.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (out);
        juce::WavAudioFormat wav;
        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
            writer->writeFromAudioSampleBuffer (y, 0, y.getNumSamples());
        return 0;
    }

    if (juce::String (argv[1]) == "warp" && argc > 3)
    {
        // warp <wav> <out prefix>: the sample moved -12, -7, +7 and +12 semitones keeping its length
        // (Signalsmith Stretch, exact()), with and without its formants; time of each render.
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (juce::File (juce::String::fromUTF8 (argv[2])), raw, rate))
            return 1;
        const auto s = SampleAnalysis::analyse (raw, rate, "warp", false, false);
        const int n = s->length;
        std::printf ("%.2f s, root %.2f (%s)\n", n / rate, s->rootMidi, s->pitched ? "con nota" : "sin nota");
        for (const bool formants : { false, true })
            for (const float semis : { -12.0f, -7.0f, 7.0f, 12.0f })
            {
                signalsmith::stretch::SignalsmithStretch<float> stretch;
                stretch.presetDefault (2, (float) rate);
                stretch.setTransposeSemitones (semis);
                if (formants)
                {
                    stretch.setFormantSemitones (0.0f, true);
                    const double f0 = s->pitched ? 440.0 * std::exp2 ((s->rootMidi - 69.0) / 12.0) : 200.0;
                    stretch.setFormantBase ((float) (f0 / rate));
                }
                juce::AudioBuffer<float> out (2, n);
                const float* in[2] = { s->audio.getReadPointer (0), s->audio.getReadPointer (1) };
                float* o[2] = { out.getWritePointer (0), out.getWritePointer (1) };
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                stretch.exact (in, n, o, n);
                const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                std::printf ("%+3.0f st %s: %.0f ms (%.0f x tiempo real), latencia de entrada %d, de salida %d muestras\n", semis,
                             formants ? "con formantes" : "sin formantes", ms, n / rate * 1000.0 / ms, stretch.inputLatency(), stretch.outputLatency());
                juce::File file (juce::String::fromUTF8 (argv[3]) + (formants ? "_formantes_" : "_tonos_") + juce::String ((int) semis) + ".wav");
                file.deleteFile();
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
                juce::WavAudioFormat wav;
                if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
                    writer->writeFromAudioSampleBuffer (out, 0, n);
            }
        return 0;
    }

    if (juce::String (argv[1]) == "aliasing")
    {
        // aliasing [rate]: a sine through the voice filter with drive; how much of what comes out lies
        // off the sine's harmonics (folded back from above Nyquist), against the sine, in dB.
        const double rate = argc > 2 ? juce::String (argv[2]).getDoubleValue() : 48000.0;
        const char* circuits[] = { "Clean", "OSR", "MS2", "SMP", "PRD" };
        const int n = 1 << 16;
        juce::dsp::FFT fft (16);
        std::printf ("circuit,driveDb,hz,aliasDb,toneDb\n");
        for (int circuit = 0; circuit < 5; ++circuit)
            for (const float driveDb : { 0.0f, 12.0f, 24.0f })
                for (const double hz : { 1234.5, 2987.6, 6211.1 })
                {
                    VoiceFilter f;
                    f.prepare (rate);
                    f.set (P::LP, 1, circuit, 18000.0f, 0.3f, driveDb, 0.5f, 1.0f);
                    std::vector<float> out ((size_t) n * 2, 0.0f);
                    const double f0 = std::round (hz * n / rate) * rate / n;       // on a bin
                    for (int i = 0; i < n + 4096; ++i)
                    {
                        float l = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * f0 * i / rate), r = l;
                        f.process (l, r);
                        if (i >= 4096)
                            out[(size_t) (i - 4096)] = l * (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) (i - 4096) / (float) n));
                    }
                    fft.performFrequencyOnlyForwardTransform (out.data());
                    const int bin0 = (int) std::round (f0 * n / rate);
                    double fundamental = 0.0, alias = 0.0;
                    for (int k = 2; k < n / 2; ++k)
                    {
                        const double p = (double) out[(size_t) k] * out[(size_t) k];
                        const int h = (int) std::round ((double) k / bin0);
                        const bool onHarmonic = h >= 1 && std::abs (k - h * bin0) <= 3;
                        if (h == 1 && onHarmonic)
                            fundamental += p;
                        else if (! onHarmonic)
                            alias += p;
                    }
                    std::printf ("%s,%.0f,%.0f,%.1f,%.2f\n", circuits[circuit], driveDb, hz, 10.0 * std::log10 (alias / std::max (1.0e-30, fundamental) + 1.0e-20),
                                 10.0 * std::log10 (fundamental + 1.0e-30));
                }
        return 0;
    }

    if (juce::String (argv[1]) == "memoria" && argc > 2)
    {
        // memoria <folder>: how much memory the analysis of each sample keeps, and where.
        auto files = juce::File (juce::String::fromUTF8 (argv[2])).findChildFiles (juce::File::findFiles, false, "*.wav");
        files.sort();
        std::printf ("file,seconds,audioMB,bedsMB,partialsMB,sourcesMB,totalMB\n");
        for (const auto& f : files)
        {
            juce::AudioBuffer<float> raw;
            double rate = 0.0;
            if (! SampleAnalysis::loadFile (f, raw, rate))
                continue;
            const auto s = SampleAnalysis::analyse (raw, rate, f.getFileName(), false, true);
            const auto mb = [] (const juce::AudioBuffer<float>& b) { return (double) b.getNumChannels() * b.getNumSamples() * 4.0 / 1048576.0; };
            const auto& pm = s->partials;
            const double partials = (pm.amp.size() + pm.freq.size() + pm.phase.size() + pm.noise.size()) * 4.0 / 1048576.0;
            const double audio = mb (s->audio), beds = mb (s->bedBright.audio) + mb (s->bedDark.audio);
            const double sources = mb (s->early.loop) + mb (s->early.freeze) + mb (s->late.loop) + mb (s->late.freeze);
            std::printf ("%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", f.getFileName().toRawUTF8(), raw.getNumSamples() / rate, audio, beds, partials, sources,
                         audio + beds + partials + sources);
        }
        return 0;
    }

    if (juce::String (argv[1]) == "lecho" && argc > 3)
    {
        // lecho <wav> <out prefix>: the two beds of engine A as they loop, to inspect.
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (juce::File (juce::String::fromUTF8 (argv[2])), raw, rate))
            return 1;
        const auto s = SampleAnalysis::analyse (raw, rate, "lecho", false, true);
        int which = 0;
        for (const auto* bed : { &s->bedBright, &s->bedDark })
        {
            juce::File file (juce::String::fromUTF8 (argv[3]) + (which++ == 0 ? "_brillante.wav" : "_oscuro.wav"));
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            juce::WavAudioFormat wav;
            if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
                writer->writeFromAudioSampleBuffer (bed->audio, SampleData::Bed::pad, bed->length);
        }
        std::printf ("bed %d samples (%.2f s), region %.0f..%.0f ms, usable end %.0f ms\n", s->bedBright.length, s->bedBright.length / rate,
                     s->bedStart / rate * 1000.0, s->bedEnd / rate * 1000.0, s->usableEnd / rate * 1000.0);
        return 0;
    }

    if (juce::String (argv[1]) == "mecanismo" && argc > 3)
    {
        // mecanismo <wav> <out.wav> [amount] [held s]: a short phrase without Mecanismo, then with each type
        // (the piano with its sustain pedal), to listen to.
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (juce::File (juce::String::fromUTF8 (argv[2])), raw, rate))
            return 1;
        const auto s = SampleAnalysis::analyse (raw, rate, "mecanismo", false, true);
        const float amount = argc > 4 ? juce::String (argv[4]).getFloatValue() : 0.8f;
        const int notes[] = { 0, 4, 7, 12, 7, 4 };
        const float velocities[] = { 0.5f, 0.7f, 0.9f, 1.0f, 0.6f, 0.4f };
        const double heldSeconds = argc > 5 ? juce::String (argv[5]).getDoubleValue() : 0.4;
        const int step = (int) (0.55 * rate), held = (int) (juce::jlimit (0.02, 0.5, heldSeconds) * rate), gap = (int) (0.9 * rate);
        juce::AudioBuffer<float> out (2, (Mechanism::NumTypes + 1) * (6 * step + gap));
        out.clear();
        int at = 0;
        for (int type = -1; type < Mechanism::NumTypes; ++type)
        {
            auto q = defaultsFor (*s);
            q[P::AmpR] = 250.0f;
            q[P::RelNoise] = type < 0 ? 0.0f : amount;
            q[P::MechType] = (float) std::max (0, type);
            SynthEngine e;
            e.prepare (rate);
            e.prepareFx (q);
            e.setSample (s);
            const int length = 6 * step + gap;
            juce::AudioBuffer<float> buf (2, 256);
            for (int pos = 0; pos < length; pos += 256)
            {
                juce::MidiBuffer midi;
                for (int i = pos; i < pos + 256; ++i)
                {
                    const int n = i / step, t = i % step;
                    if (i == 0 && type == Mechanism::Piano)
                        midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), i - pos);
                    if (i == 6 * step && type == Mechanism::Piano)
                        midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), i - pos);
                    if (n < 6 && t == 0)
                        midi.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey + notes[n], velocities[n]), i - pos);
                    if (n < 6 && t == held)
                        midi.addEvent (juce::MidiMessage::noteOff (1, SynthEngine::rootKey + notes[n]), i - pos);
                }
                e.process (buf, midi, q, 120.0);
                const int count = std::min (256, length - pos);
                for (int c = 0; c < 2; ++c)
                    out.copyFrom (c, at + pos, buf, c, 0, count);
            }
            at += length;
        }
        const float peak = out.getMagnitude (0, at);
        std::printf ("peak %.1f dB, written at -1 dB\n", juce::Decibels::gainToDecibels (peak));
        out.applyGain (0.89f / std::max (1.0e-6f, peak));
        juce::File file (juce::String::fromUTF8 (argv[3]));
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            writer->writeFromAudioSampleBuffer (out, 0, at);
        return 0;
    }

    if (juce::String (argv[1]) == "motores")
        return argc > 3 ? engines (filesIn (juce::File (juce::String::fromUTF8 (argv[2]))), argv[3], argc > 4 ? juce::String (argv[4]).getIntValue() : 1) : 1;
    if (juce::String (argv[1]) == "loops" && argc > 3)
    {
        // loops <lista.txt> <out.csv>: the loop detection of each file, with its name and without it (the
        // audio alone).
        std::FILE* csv = nullptr;
        if (fopen_s (&csv, argv[3], "w") != 0)
            return 1;
        std::fprintf (csv, "file,seconds,likely,confidence,beats,bpm,nameBpm,fromName,tail,rhythmic,slices,a_likely,a_confidence,a_beats,a_bpm,perc\n");
        for (const auto& f : filesIn (juce::File (juce::String::fromUTF8 (argv[2]))))
        {
            juce::AudioBuffer<float> audio;
            double rate = 0.0;
            if (! SampleAnalysis::loadFile (f, audio, rate))
                continue;
            const auto a = SampleAnalysis::analyseLoop (audio, rate, f.getFileNameWithoutExtension());
            const auto b = SampleAnalysis::analyseLoop (audio, rate, "x");
            std::fprintf (csv, "\"%s\",%.3f,%d,%.2f,%d,%.2f,%.1f,%d,%d,%d,%d,%d,%.2f,%d,%.2f,%.3f\n", f.getFileName().replace ("\"", "'").toRawUTF8(),
                          audio.getNumSamples() / rate, (int) a.likely, a.confidence, a.beats, a.bpm, a.nameBpm, (int) a.fromName, (int) a.tailAlive,
                          (int) a.rhythmic, (int) a.slices.size(), (int) b.likely, b.confidence, b.beats, b.bpm, a.percussive);
            std::fflush (csv);
        }
        std::fclose (csv);
        return 0;
    }
    if (juce::String (argv[1]) == "loopss" && argc > 4)
    {
        // loopss <file> <out.wav> <host bpm> [semitones] [seconds]: the loop repeated and stretched to the host's
        // tempo by Signalsmith Stretch (exact, offline), to compare with LOOP's own Tonal.
        juce::AudioBuffer<float> audio;
        double rate = 0.0;
        const juce::File in (juce::String::fromUTF8 (argv[2]));
        if (! SampleAnalysis::loadFile (in, audio, rate))
            return 1;
        const double hostBpm = juce::String (argv[4]).getDoubleValue();
        const float semis = argc > 5 ? juce::String (argv[5]).getFloatValue() : 0.0f;
        const double seconds = argc > 6 ? juce::String (argv[6]).getDoubleValue() : 12.0;
        auto data = SampleAnalysis::analyse (audio, rate, in.getFileNameWithoutExtension(), false, false);
        const auto& info = data->loopInfo;
        const int length = info.length > 0 ? info.length : data->length;
        const double ratio = info.bpm > 0.0 ? hostBpm / info.bpm : 1.0;         // output faster than the file by this
        const int repeats = (int) std::ceil (seconds * ratio * rate / length) + 2;
        juce::AudioBuffer<float> tiled (2, repeats * length);
        for (int r = 0; r < repeats; ++r)
            for (int c = 0; c < 2; ++c)
                tiled.copyFrom (c, r * length, audio, std::min (c, audio.getNumChannels() - 1), 0, length);
        const int outLen = (int) std::lround (tiled.getNumSamples() / ratio);
        signalsmith::stretch::SignalsmithStretch<float> stretch;
        stretch.presetDefault (2, (float) rate);
        stretch.setTransposeSemitones (semis);
        juce::AudioBuffer<float> out (2, outLen);
        const float* inp[2] = { tiled.getReadPointer (0), tiled.getReadPointer (1) };
        float* o[2] = { out.getWritePointer (0), out.getWritePointer (1) };
        stretch.exact (inp, tiled.getNumSamples(), o, outLen);
        const int keep = std::min (outLen, (int) (seconds * rate));
        const juce::File outFile (juce::String::fromUTF8 (argv[3]));
        outFile.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (outFile);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (out, 0, keep);
        return 0;
    }
    if (juce::String (argv[1]) == "loopdemo" && argc > 4)
    {
        // loopdemo <file> <out.wav> <host bpm> [semitones] [algo 0 auto 1 beats 2 tonal] [seconds]: the sample as
        // LOOP (Warp on, following the host), held for the seconds asked; prints what was detected.
        juce::AudioBuffer<float> audio;
        double rate = 0.0;
        const juce::File in (juce::String::fromUTF8 (argv[2]));
        if (! SampleAnalysis::loadFile (in, audio, rate))
            return 1;
        const double outRate = 48000.0, hostBpm = juce::String (argv[4]).getDoubleValue();
        const int semis = argc > 5 ? juce::String (argv[5]).getIntValue() : 0, algo = argc > 6 ? juce::String (argv[6]).getIntValue() : 0;
        const double seconds = argc > 7 ? juce::String (argv[7]).getDoubleValue() : 12.0;
        auto data = SampleAnalysis::analyse (audio, rate, in.getFileNameWithoutExtension(), false, false);
        const auto& info = data->loopInfo;
        std::printf ("%s: %s, confianza %.2f, %d tiempos, %.2f BPM, %s, %d cortes\n", in.getFileName().toRawUTF8(), info.likely ? "loop" : "no seguro",
                     info.confidence, info.beats, info.bpm, info.rhythmic ? "rítmico" : "tonal", (int) info.slices.size());
        ParamSnapshot p {};
        for (int i = 0; i < P::Count; ++i)
            p[(size_t) i] = P::defs()[(size_t) i].def;
        p[P::Volume] = 0.0f;
        p[P::MonoBass] = 0.0f;
        p[P::PlayMode] = (float) P::PlayLoop;
        p[P::LoopAlgo] = (float) algo;
        p[P::AmpA] = 1.0f;
        p[P::AmpR] = 30.0f;
        SynthEngine e;
        e.prepare (outRate);
        e.setSample (data);
        e.prepareFx (p);
        const int total = (int) ((seconds + 0.2) * outRate), block = 64;
        double worstBlock = 0.0, worstStart = 0.0, sumRatio = 0.0;
        int blocks = 0;
        juce::AudioBuffer<float> out (2, total), buf (2, block);
        out.clear();
        for (int pos = 0; pos < total; pos += block)
        {
            const int n = std::min (block, total - pos);
            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey + semis, 1.0f), 0);
            if (pos <= (int) (seconds * outRate) && pos + n > (int) (seconds * outRate))
                midi.addEvent (juce::MidiMessage::noteOff (1, SynthEngine::rootKey + semis), (int) (seconds * outRate) - pos);
            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, n);
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            e.process (view, midi, p, hostBpm);
            const double ratio = (juce::Time::getMillisecondCounterHiRes() - t0) / (1000.0 * n / outRate);
            (pos < (int) (0.1 * outRate) ? worstStart : worstBlock) = std::max (pos < (int) (0.1 * outRate) ? worstStart : worstBlock, ratio);
            sumRatio += ratio;
            ++blocks;
            out.copyFrom (0, pos, buf, 0, 0, n);
            out.copyFrom (1, pos, buf, 1, 0, n);
        }
        const juce::File outFile (juce::String::fromUTF8 (argv[3]));
        outFile.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (outFile);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (outRate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (out, 0, total);
        std::printf ("bloques de 64: media %.1f %%, peor al arrancar %.1f %%, peor despues %.1f %% de su tiempo\n",
                     100.0 * sumRatio / std::max (1, blocks), 100.0 * worstStart, 100.0 * worstBlock);
        return 0;
    }
    if (juce::String (argv[1]) == "ataque")
        return argc > 3 ? attackLab (filesIn (juce::File (juce::String::fromUTF8 (argv[2]))), argv[3]) : 1;
    if (juce::String (argv[1]) == "info")
        return argc > 3 ? info (filesIn (juce::File (juce::String::fromUTF8 (argv[2]))), argv[3]) : 1;

    if (juce::String (argv[1]) == "tonos" && argc > 3)
    {
        // tonos <lista.txt> <out.csv>: the test Tonos is put to on loading (SynthEngine::checkTones):
        // the note, the cycle it laid out against the note's period, the share on the right
        // harmonics and the verdict, with the time the whole analysis took.
        std::FILE* csv = nullptr;
        if (fopen_s (&csv, argv[3], "w") != 0)
            return 1;
        std::fprintf (csv, "file,pitched,root,period,cycle,grid,hold,loadMs\n");
        for (const auto& file : filesIn (juce::File (juce::String::fromUTF8 (argv[2]))))
        {
            juce::AudioBuffer<float> raw;
            double rate = 0.0;
            if (! SampleAnalysis::loadFile (file, raw, rate))
                continue;
            std::fprintf (stderr, "FILE %s\n", file.getFileName().toRawUTF8());
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            const auto s = SampleAnalysis::analyse (raw, rate, file.getFileNameWithoutExtension(), false, true);
            std::fprintf (csv, "%s,%d,%.2f,%.2f,%.2f,%.3f,%d,%.0f\n", file.getFileName().replaceCharacter (',', '_').toRawUTF8(),
                          s->pitched ? 1 : 0, s->rootMidi, s->period, s->cycle, s->tonesGrid, s->tonesHold ? 1 : 0,
                          juce::Time::getMillisecondCounterHiRes() - t0);
            std::fflush (csv);
        }
        std::fclose (csv);
        return 0;
    }

    const juce::File folder (juce::String::fromUTF8 (argv[1]));
    const int shard = argc > 3 ? juce::String (argv[3]).getIntValue() : 0;
    const int shards = argc > 4 ? juce::String (argv[4]).getIntValue() : 1;
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.wav");
    files.sort();

    std::FILE* csv = nullptr;
    if (fopen_s (&csv, argv[2], "w") != 0)
        return 1;
    std::fprintf (csv, "file,seconds,rate,pitched,root,attackMs,decayMs,sustain,loadMs,renderMs,finite,snrUntouched,holdErrorDb,holdSwingDb,lateDriftDb,bedWidth,srcWidth,peak,tailDb,srcCorr,bedCorr,bedSwing\n");

    for (int index = shard; index < files.size(); index += shards)
    {
        const auto& file = files.getReference (index);
        const auto name = file.getFileName().replaceCharacter (',', '_');
        std::fprintf (csv, "%s,", name.toRawUTF8());
        std::fflush (csv);      // a crash leaves the guilty file on the last line

        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (file, raw, rate))
        {
            std::fprintf (csv, "unreadable\n");
            continue;
        }

        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const auto s = SampleAnalysis::analyse (raw, rate, file.getFileNameWithoutExtension(), false, false);
        const double loadMs = juce::Time::getMillisecondCounterHiRes() - t0;

        const auto base = defaultsFor (*s);
        const int key = SynthEngine::rootKey;
        const double sr = rate;

        const double t1 = juce::Time::getMillisecondCounterHiRes();
        const double span = std::min (30.0, s->length / rate);
        const auto same = play (s, base, key, span, span + 0.1, sr);

        auto q = base;
        q[P::AmpS] = 0.7f;
        q[P::AmpR] = 200.0f;
        q[P::AmpD] = std::min (q[P::AmpD], 500.0f);
        q[P::AmpA] = std::min (q[P::AmpA], 300.0f);
        q[P::SustainType] = (float) P::SustainHold;
        const auto held = play (s, q, key, 4.0, 4.6, sr);
        const double renderMs = juce::Time::getMillisecondCounterHiRes() - t1;

        const double expected = 20.0 * std::log10 (0.7 * s->peakRms) - 20.0 * std::log10 (1.0);
        const double holdLevel = rmsDb (held, 1.5, 3.9, sr);

        std::fprintf (csv, "%.2f,%.0f,%d,%.2f,%.1f,%.0f,%.3f,%.0f,%.0f,%d,%.1f,%.2f,%.2f,%.2f,%.1f,%.1f,%.3f,%.1f,%.3f,%.3f,%.2f\n",
                      s->length / rate, rate, s->pitched ? 1 : 0, s->rootMidi, s->detected.attack * 1000.0f, s->detected.decay * 1000.0f, s->detected.sustain,
                      loadMs, renderMs, allFinite (same) && allFinite (held) ? 1 : 0, snrUntouched (same, *s, span), holdLevel - expected,
                      swingDb (held, 1.5, 3.9, sr), rmsDb (held, 3.2, 3.9, sr) - rmsDb (held, 1.5, 2.2, sr),
                      widthDb (s->bedBright.audio, 0, s->bedBright.audio.getNumSamples()), widthDb (s->audio, (int) s->bedStart, (int) s->bedEnd),
                      held.getMagnitude (0, held.getNumSamples()), rmsDb (held, 4.45, 4.6, sr),
                      correlation (s->audio, (int) s->bedStart, (int) s->bedEnd),
                      correlation (s->bedBright.audio, 0, s->bedBright.audio.getNumSamples()),
                      swingDb (s->bedBright.audio, 0.0, s->bedBright.audio.getNumSamples() / rate, rate));
        std::fflush (csv);
    }

    std::fclose (csv);
    return 0;
}
