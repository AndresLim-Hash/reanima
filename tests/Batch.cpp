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
#include "KeyDetect.h"
#include "FxRack.h"
#include <functional>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdio>
#include <map>

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
    static const int blockSize = std::getenv ("BATCH_BLOCK") != nullptr ? juce::jlimit (16, 8192, std::atoi (std::getenv ("BATCH_BLOCK"))) : 512;
    juce::AudioBuffer<float> buf (2, blockSize);

    for (int pos = 0; pos < totalSamples; pos += blockSize)
    {
        const int n = std::min (blockSize, totalSamples - pos);
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

    if (juce::String (argv[1]) == "tonalidad" && argc > 3)
    {
        // tonalidad <folder | list.txt> <out.csv> [every N]: for each file with its key in the name, the key its audio
        // gives and how sure, against the name: exact, relative (same notes), same root other mode, a fifth, wrong.
        std::FILE* csv = nullptr;
        if (fopen_s (&csv, argv[3], "w") != 0)
            return 1;
        std::fprintf (csv, "file,name_key,audio_key,confidence,verdict,seconds\n");
        const int every = argc > 4 ? std::max (1, juce::String (argv[4]).getIntValue()) : 1;
        int i = 0;
        struct Bin { int all = 0, exact = 0, relative = 0, mode = 0, fifth = 0; };
        std::array<Bin, 5> bins {};      // confidence 0-20, 20-40, 40-60, 60-80, 80-100 %
        for (const auto& f : filesIn (juce::File (juce::String::fromUTF8 (argv[2]))))
        {
            const auto named = KeyDetect::fromName (f.getFileName());
            if (named.root < 0 || i++ % every != 0)
                continue;
            juce::AudioBuffer<float> audio;
            double rate = 0.0;
            if (! SampleAnalysis::loadFile (f, audio, rate))
                continue;
            const auto got = KeyDetect::fromAudio (audio, rate);
            if (std::getenv ("TONO_DIAG") != nullptr)
            {
                std::printf ("%s rate %.0f ch %d n %d:", f.getFileName().toRawUTF8(), rate, audio.getNumChannels(), audio.getNumSamples());
                for (const float c : got.chroma)
                    std::printf (" %.2f", c);
                std::printf ("\n");
            }
            const int relativeRoot = named.minor ? (named.root + 3) % 12 : (named.root + 9) % 12;
            const char* verdict = got.root < 0 ? "none"
                                : got.root == named.root && got.minor == named.minor ? "exact"
                                : got.root == relativeRoot && got.minor != named.minor ? "relative"
                                : got.root == named.root ? "mode"
                                : (got.root == (named.root + 7) % 12 || got.root == (named.root + 5) % 12) ? "fifth" : "wrong";
            auto& bin = bins[(size_t) juce::jlimit (0, 4, (int) (got.confidence * 5.0f))];
            ++bin.all;
            bin.exact += juce::String (verdict) == "exact";
            bin.relative += juce::String (verdict) == "relative";
            bin.mode += juce::String (verdict) == "mode";
            bin.fifth += juce::String (verdict) == "fifth";
            std::fprintf (csv, "\"%s\",%s,%s,%.2f,%s,%.2f\n", f.getFileName().replace ("\"", "'").toRawUTF8(), named.text().toRawUTF8(), got.text().toRawUTF8(),
                          got.confidence, verdict, audio.getNumSamples() / rate);
            std::fflush (csv);
        }
        std::fclose (csv);
        std::printf ("confianza | archivos | exacta | relativa | misma tonica | quinta | resto\n");
        for (size_t k = 0; k < bins.size(); ++k)
        {
            const auto& bn = bins[k];
            const auto pct = [&bn] (int x) { return bn.all > 0 ? 100.0 * x / bn.all : 0.0; };
            std::printf ("%3d-%3d %% | %5d | %5.1f %% | %5.1f %% | %5.1f %% | %5.1f %% | %5.1f %%\n", (int) k * 20, (int) k * 20 + 20, bn.all, pct (bn.exact), pct (bn.relative),
                         pct (bn.mode), pct (bn.fifth), pct (bn.all - bn.exact - bn.relative - bn.mode - bn.fifth));
        }
        return 0;
    }
    if (juce::String (argv[1]) == "motores_sinteticos" && argc > 2)
    {
        // motores_sinteticos <out.csv> [rate] [engines]: sounds whose ideal result is known at any transposition, so that what
        // an engine adds can be told from what it is meant to do. Each at the root, +5, +12, -7, -12, held 3 s with its
        // detected envelope:
        //   tono 220: 10 harmonics at 1/h, steady; bajo 55: the same at 55 Hz; pulsado 330: decaying 0.8 s, 2 ms rise;
        //   tono con aliento: the 220 tone with noise 30 dB under it (to keep); pad ancho: 220 and 220.8 Hz sets, one
        //   per side (a wide, slowly beating stereo).
        // Measured in the held part (0.4 s to 2.6 s; the pluck from 0.05 s to 0.6 s):
        //   harmErrDb: RMS of the harmonics 2..10 against the fundamental, against the source's own (timbre);
        //   noiseDb: energy between the harmonics against the harmonics (the source's: ~-90 dB; with breath, -30);
        //   flutterDb: how much the level moves from 5 ms to 5 ms (the source's ~0; the pluck, its own decay taken out);
        //   cents, wobble: pitch against the note and its spread; monoDb: what summing to mono loses.
        std::FILE* csv = nullptr;
        if (fopen_s (&csv, argv[2], "w") != 0)
            return 1;
        const double rate = argc > 3 ? juce::String (argv[3]).getDoubleValue() : 48000.0;
        const juce::String engines = argc > 4 ? juce::String (argv[4]) : juce::String ("ABCDEF");
        std::fprintf (csv, "sound,engine,case,levelDb,harmErrDb,noiseDb,noiseSourceDb,flutterDb,cents,wobble,monoDb,msPerS\n");
        struct Sound { const char* name; double f0; bool pluck, breath, wide; };
        const Sound sounds[] = { { "tono 220", 220.0, false, false, false }, { "bajo 55", 55.0, false, false, false },
                                 { "pulsado 330", 330.0, true, false, false }, { "tono con aliento", 220.0, false, true, false },
                                 { "pad ancho", 220.0, false, false, true } };
        constexpr int N = 16384;
        juce::dsp::FFT fftBig (14);
        std::vector<float> wbig ((size_t) N), big ((size_t) N * 2);
        for (int i = 0; i < N; ++i)
            wbig[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / (float) N);
        // the spectrum of the held part (averaged power), mono
        const auto spectrum = [&] (const juce::AudioBuffer<float>& y, double from, double to)
        {
            std::vector<double> p ((size_t) N / 2, 0.0);
            int frames = 0;
            for (int start = (int) (from * rate); start + N <= (int) (to * rate) && start + N <= y.getNumSamples(); start += N / 4, ++frames)
            {
                std::fill (big.begin(), big.end(), 0.0f);
                for (int i = 0; i < N; ++i)
                    big[(size_t) i] = 0.5f * (y.getSample (0, start + i) + y.getSample (1, start + i)) * wbig[(size_t) i];
                fftBig.performFrequencyOnlyForwardTransform (big.data(), true);
                for (int k = 0; k < N / 2; ++k)
                    p[(size_t) k] += (double) big[(size_t) k] * big[(size_t) k];
            }
            return p;
        };
        const auto harmonics = [&] (const std::vector<double>& p, double f0, std::array<double, 10>& h, double& noise)
        {
            h.fill (1.0e-20);
            noise = 1.0e-20;
            double harm = 0.0;
            for (int k = 1; k < N / 2; ++k)
            {
                const double hz = k * rate / N;
                if (hz < 30.0 || hz > std::min (16000.0, 10.5 * f0))
                    continue;
                const double x = hz / f0, n = std::round (x);
                if (n >= 1.0 && n <= 10.0 && std::abs (x - n) * f0 <= std::max (4.0 * rate / N, 0.02 * f0))
                {
                    h[(size_t) n - 1] += p[(size_t) k];
                    harm += p[(size_t) k];
                }
                else if (std::abs (x - n) > 0.2)
                    noise += p[(size_t) k];
            }
            noise = 10.0 * std::log10 (noise / (harm + 1.0e-20));
        };
        for (const auto& snd : sounds)
        {
            const int n = (int) (3.0 * rate);
            juce::AudioBuffer<float> src (2, n);
            src.clear();
            juce::Random rng (5);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                {
                    const double t = i / rate;
                    const double f = snd.wide && ch == 1 ? snd.f0 * (220.8 / 220.0) : snd.f0;
                    double v = 0.0;
                    for (int h = 1; h <= 10; ++h)
                        v += std::sin (2.0 * juce::MathConstants<double>::pi * f * h * t + (snd.wide ? ch * 0.7 * h : 0.0)) / h;
                    const double env = snd.pluck ? std::min (1.0, t / 0.002) * std::exp (-t / 0.8) : std::min (1.0, t / 0.01);
                    double x = (snd.breath && std::getenv ("SINT_SOLO_RUIDO") != nullptr ? 0.0 : 0.2) * v * env;
                    if (snd.breath)
                        x += 0.2 * 0.0316 * (rng.nextFloat() * 2.0 - 1.0) * 1.7 * env;
                    src.setSample (ch, i, (float) x);
                }
            const auto s = SampleAnalysis::analyse (src, rate, juce::String (snd.name) + " C3", false, true);
            const double from = snd.pluck ? 0.05 : 0.4, to = snd.pluck ? 0.6 : 2.6;
            std::array<double, 10> hs;
            double noiseSource;
            harmonics (spectrum (src, from, to), snd.f0, hs, noiseSource);
            for (int engine = 0; engine <= P::EngineDrone; ++engine)
            {
                if (! engines.containsChar ((juce::juce_wchar) ('A' + engine)))
                    continue;
                for (const int shift : { 0, 5, 12, -7, -12 })
                {
                    auto q = defaultsFor (*s);
                    q[P::Engine] = (float) engine;
                    q[P::SnapC] = 0.0f;
                    if (std::getenv ("SINT_COHERENTE") != nullptr)
                        q[P::GrainCoherent] = 1.0f;
                    const double t0 = juce::Time::getMillisecondCounterHiRes();
                    if (const char* nl = std::getenv ("SINT_RUIDO"))
                        q[P::NoiseLevel] = (float) std::atof (nl);
                    const auto y = play (s, q, SynthEngine::rootKey + shift, 3.0, 3.0, rate);       // C3 plays the sample as it is
                    if (const char* dir = std::getenv ("SINT_WAV"))
                    {
                        const auto out = juce::File (juce::String::fromUTF8 (dir)).getChildFile (juce::String (snd.name) + " " + juce::String::charToString ((juce::juce_wchar) ('A' + engine))
                                                                                                  + " " + juce::String (shift) + ".wav");
                        out.deleteFile();
                        std::unique_ptr<juce::OutputStream> st = std::make_unique<juce::FileOutputStream> (out);
                        juce::WavAudioFormat wav;
                        if (auto w = wav.createWriterFor (st, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
                            w->writeFromAudioSampleBuffer (y, 0, y.getNumSamples());
                    }
                    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                    const double fOut = snd.f0 * std::exp2 (shift / 12.0);
                    std::array<double, 10> ho;
                    double noise;
                    harmonics (spectrum (y, from, to), fOut, ho, noise);
                    double err = 0.0;
                    for (int h = 1; h < 10; ++h)
                    {
                        const double want = 10.0 * std::log10 (hs[(size_t) h] / hs[0]), got = 10.0 * std::log10 (ho[(size_t) h] / ho[0]);
                        err += (got - want) * (got - want);
                    }
                    // flutter: 5 ms levels, against a 50 ms moving average (the decay taken out)
                    std::vector<double> lv;
                    const double step = std::max (0.005, 1.5 / fOut);       // a window of at least a period and a half
                    for (double t = from; t + step <= to; t += step)
                        lv.push_back (rmsDb (y, t, t + step, rate));
                    double flutter = 0.0;
                    int cnt = 0;
                    for (size_t k = 5; k + 5 < lv.size(); ++k)
                    {
                        double m = 0.0;
                        for (size_t j = k - 5; j <= k + 5; ++j)
                            m += lv[j];
                        m /= 11.0;
                        flutter += (lv[k] - m) * (lv[k] - m);
                        ++cnt;
                    }
                    flutter = cnt > 0 ? std::sqrt (flutter / cnt) : 0.0;
                    // pitch: the strongest peak near the fundamental, every 46 ms
                    std::vector<double> cents;
                    for (double t = from; t + 0.09 <= to; t += 0.046)
                    {
                        const auto p = spectrum (y, t, t + N / rate + 0.001);
                        if (p.empty())
                            break;
                        int best = -1;
                        for (int k = (int) (fOut * 0.9 * N / rate); k <= (int) (fOut * 1.1 * N / rate); ++k)
                            if (k > 0 && k + 1 < N / 2 && (best < 0 || p[(size_t) k] > p[(size_t) best]))
                                best = k;
                        if (best < 1)
                            continue;
                        const double a = std::log (p[(size_t) best - 1] + 1e-30), b = std::log (p[(size_t) best] + 1e-30), c = std::log (p[(size_t) best + 1] + 1e-30);
                        const double off = (a - 2 * b + c) != 0.0 ? 0.5 * (a - c) / (a - 2 * b + c) : 0.0;
                        cents.push_back (1200.0 * std::log2 ((best + off) * rate / N / fOut));
                    }
                    double med = 0.0, wob = 0.0;
                    if (! cents.empty())
                    {
                        std::sort (cents.begin(), cents.end());
                        med = cents[cents.size() / 2];
                        wob = cents.back() - cents.front();
                    }
                    double mid = 0.0, sides = 0.0;
                    for (int i = (int) (from * rate); i < (int) (to * rate) && i < y.getNumSamples(); ++i)
                    {
                        const double l = y.getSample (0, i), r = y.getSample (1, i);
                        mid += 0.25 * (l + r) * (l + r);
                        sides += 0.5 * (l * l + r * r);
                    }
                    std::fprintf (csv, "%s,%c,%+d,%.2f,%.2f,%.1f,%.1f,%.2f,%.1f,%.1f,%.2f,%.1f\n", snd.name, (char) ('A' + engine), shift,
                                  rmsDb (y, from, to, rate), std::sqrt (err / 9.0), noise, noiseSource, flutter, med, wob,
                                  10.0 * std::log10 ((mid + 1e-20) / (sides + 1e-20)), ms / 3.0);
                    std::fflush (csv);
                }
            }
        }
        std::fclose (csv);
        return 0;
    }
    if (juce::String (argv[1]) == "motores_tonal" && argc > 3)
    {
        // motores_tonal <list.txt> <out.csv> [engines]: the engine audit on tonal samples. Each engine at the root,
        // +5, +12, -7 and -12, the note held for the sample's length (4 s at most) with the detected envelope, against
        // engine A in the same case (A untouched is the file; transposed, the file resampled, as a classic sampler):
        //  - hnrDb: harmonics over the noise between them (sustain, 0.15 s on), the output's minus A's: below 0 is
        //    noise or inharmonic energy the engine adds (or harmonics it loses);
        //  - centsErr: the median pitch against the expected note; wobbleCents: how much it moves (IQR), output's
        //    minus A's;
        //  - bandErrDb: 24-band spectrum against A, level-matched, every 20 ms (timbre);
        //  - envErrDb: median of the 20 ms level against A, level-matched; worstDipDb: its worst fall;
        //  - attackMs and attackA: 10-90 % rise; crestDb vs crestA: peak over RMS of the first 30 ms (the transient);
        //  - monoDb: what summing to mono loses, minus A's; msPerS: render time per second of sound (CPU).
        std::FILE* csv = nullptr;
        if (fopen_s (&csv, argv[3], "w") != 0)
            return 1;
        const juce::String engines = argc > 4 ? juce::String (argv[4]) : juce::String ("ABCDEF");
        std::fprintf (csv, "file,root,engine,case,levelDb,hnrDb,hnrA,centsErr,wobbleCents,bandErrDb,envErrDb,worstDipDb,attackMs,attackA,crestDb,crestA,monoDb,msPerS\n");
        constexpr int N = 8192;
        juce::dsp::FFT fftBig (13);
        std::vector<float> wbig ((size_t) N), big ((size_t) N * 2);
        for (int i = 0; i < N; ++i)
            wbig[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / (float) N);
        const auto monoAt = [] (const juce::AudioBuffer<float>& y, int i) { return 0.5f * (y.getSample (0, i) + y.getSample (1, i)); };
        // harmonics-to-noise over the held part: energy within 3 % of each harmonic (to 8 kHz) against the rest of 60 Hz-8 kHz
        const auto hnr = [&] (const juce::AudioBuffer<float>& y, double f0, double from, double to, double rate)
        {
            double harm = 0.0, noise = 0.0;
            for (int start = (int) (from * rate); start + N <= (int) (to * rate) && start + N <= y.getNumSamples(); start += N / 2)
            {
                std::fill (big.begin(), big.end(), 0.0f);
                for (int i = 0; i < N; ++i)
                    big[(size_t) i] = monoAt (y, start + i) * wbig[(size_t) i];
                fftBig.performFrequencyOnlyForwardTransform (big.data(), true);
                for (int k = 1; k < N / 2; ++k)
                {
                    const double hz = k * rate / N;
                    if (hz < 60.0 || hz > 8000.0)
                        continue;
                    const double h = hz / f0, nearest = std::round (h);
                    const double p = (double) big[(size_t) k] * big[(size_t) k];
                    if (nearest >= 1.0 && std::abs (h - nearest) * f0 <= std::max (0.03 * nearest * f0, 1.5 * rate / N))
                        harm += p;
                    else if (std::abs (h - nearest) > 0.25)
                        noise += p;
                }
            }
            return 10.0 * std::log10 ((harm + 1.0e-20) / (noise + 1.0e-20));
        };
        // pitch every 40 ms (autocorrelation, within 3 semitones of the expected note): cents against it, median and IQR
        const auto pitch = [&] (const juce::AudioBuffer<float>& y, double f0, double from, double to, double rate, double& median, double& iqr)
        {
            std::vector<double> cents;
            const int w = (int) std::max (0.04 * rate, 3.0 * rate / f0);       // 40 ms, or 3 periods of a low note
            const int lagLo = (int) (rate / (f0 * std::exp2 (3.0 / 12.0))), lagHi = (int) (rate / (f0 * std::exp2 (-3.0 / 12.0))) + 1;
            for (int s0 = (int) (from * rate); s0 + w + lagHi < std::min ((int) (to * rate), y.getNumSamples()); s0 += w)
            {
                double e0 = 0.0;
                for (int i = 0; i < w; ++i)
                    e0 += (double) monoAt (y, s0 + i) * monoAt (y, s0 + i);
                if (e0 < 1.0e-8)
                    continue;
                int best = -1;
                double bestR = 0.0;
                std::vector<double> r ((size_t) (lagHi + 2), 0.0);
                for (int lag = std::max (2, lagLo - 1); lag <= lagHi + 1; ++lag)
                {
                    double num = 0.0, e1 = 0.0;
                    for (int i = 0; i < w; ++i)
                    {
                        const double a = monoAt (y, s0 + i), b = monoAt (y, s0 + i + lag);
                        num += a * b;
                        e1 += b * b;
                    }
                    r[(size_t) lag] = num / std::sqrt (e0 * e1 + 1.0e-20);
                }
                for (int lag = std::max (3, lagLo); lag <= lagHi; ++lag)
                    if (r[(size_t) lag] > bestR && r[(size_t) lag] >= r[(size_t) lag - 1] && r[(size_t) lag] >= r[(size_t) lag + 1])
                    {
                        bestR = r[(size_t) lag];
                        best = lag;
                    }
                if (best < 0 || bestR < 0.5)
                    continue;
                const double a = r[(size_t) best - 1], b = r[(size_t) best], c = r[(size_t) best + 1];
                const double den = a - 2.0 * b + c;
                const double lag = best + (std::abs (den) > 1.0e-12 ? 0.5 * (a - c) / den : 0.0);
                cents.push_back (1200.0 * std::log2 ((rate / lag) / f0));
            }
            if (cents.size() < 3)
            {
                median = iqr = std::numeric_limits<double>::quiet_NaN();
                return;
            }
            std::sort (cents.begin(), cents.end());
            median = cents[cents.size() / 2];
            iqr = cents[cents.size() * 3 / 4] - cents[cents.size() / 4];
        };
        const auto levels = [] (const juce::AudioBuffer<float>& y, double seconds, double rate)
        {
            std::vector<double> e;
            for (double t = 0.0; t + 0.02 <= seconds; t += 0.02)
                e.push_back (rmsDb (y, t, t + 0.02, rate));
            return e;
        };
        const auto crest = [&] (const juce::AudioBuffer<float>& y, double rate)
        {
            const int n = std::min (y.getNumSamples(), (int) (0.03 * rate));
            double peak = 0.0, e = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double v = monoAt (y, i);
                peak = std::max (peak, std::abs (v));
                e += v * v;
            }
            return 20.0 * std::log10 ((peak + 1.0e-12) / std::sqrt (e / std::max (1, n) + 1.0e-24));
        };
        const auto rise = [] (const juce::AudioBuffer<float>& y, double rate)
        {
            const int w = std::max (1, (int) (0.001 * rate)), n = std::min (y.getNumSamples(), (int) rate);
            std::vector<double> env;
            for (int i = 0; i + w <= n; i += w)
            {
                double e = 0.0;
                for (int j = i; j < i + w; ++j)
                    e = std::max (e, (double) std::abs (y.getSample (0, j)) + std::abs (y.getSample (1, j)));
                env.push_back (e);
            }
            if (env.empty())
                return 0.0;
            const double top = *std::max_element (env.begin(), env.end());
            int a = -1, b = -1;
            for (int k = 0; k < (int) env.size(); ++k)
            {
                if (a < 0 && env[(size_t) k] >= 0.1 * top)
                    a = k;
                if (env[(size_t) k] >= 0.9 * top)
                {
                    b = k;
                    break;
                }
            }
            return a >= 0 && b >= a ? (b - a) * 1000.0 * w / rate : 0.0;
        };
        const auto monoLoss = [] (const juce::AudioBuffer<float>& y, int to)
        {
            double mid = 0.0, sides = 0.0;
            for (int i = 0; i < std::min (to, y.getNumSamples()); ++i)
            {
                const double l = y.getSample (0, i), r = y.getSample (1, i);
                mid += 0.25 * (l + r) * (l + r);
                sides += 0.5 * (l * l + r * r);
            }
            return 10.0 * std::log10 ((mid + 1.0e-20) / (sides + 1.0e-20));
        };
        constexpr int bands = 24;
        juce::dsp::FFT fft (11);
        std::vector<float> win (2048), fbuf (4096);
        for (int i = 0; i < 2048; ++i)
            win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / 2048.0f);
        const auto bandsAt = [&] (const juce::AudioBuffer<float>& x, int centre, double rate, std::array<double, bands>& band)
        {
            std::fill (fbuf.begin(), fbuf.end(), 0.0f);
            for (int i = 0; i < 2048; ++i)
                if (const int t = centre - 1024 + i; t >= 0 && t < x.getNumSamples())
                    fbuf[(size_t) i] = monoAt (x, t) * win[(size_t) i];
            fft.performFrequencyOnlyForwardTransform (fbuf.data(), true);
            band.fill (1.0e-12);
            for (int k = 2; k < 1024; ++k)
                if (const double hz = k * rate / 2048.0; hz >= 40.0 && hz <= 16000.0)
                    band[(size_t) juce::jlimit (0, bands - 1, (int) (bands * std::log (hz / 40.0) / std::log (16000.0 / 40.0)))] += (double) fbuf[(size_t) k] * fbuf[(size_t) k];
        };
        for (const auto& f : filesIn (juce::File (juce::String::fromUTF8 (argv[2]))))
        {
            juce::AudioBuffer<float> raw;
            double rate = 0.0;
            if (! SampleAnalysis::loadFile (f, raw, rate))
                continue;
            const auto s = SampleAnalysis::analyse (raw, rate, f.getFileNameWithoutExtension(), false, true);
            if (! s->pitched)
                continue;
            const double held = juce::jlimit (0.3, 4.0, (s->length - s->onset) / rate), total = held + 0.3;
            const double rootHz = 440.0 * std::exp2 ((s->rootMidi - 69.0) / 12.0);
            const auto name = f.getFileName().replace (",", "_");
            for (const int shift : { 0, 5, 12, -7, -12 })
            {
                auto q = defaultsFor (*s);
                q[P::Engine] = (float) P::EngineRebuild;
                const int note = SynthEngine::rootKey + shift;
                const auto a = play (s, q, note, held, total, rate);
                const double f0 = rootHz * std::exp2 (shift / 12.0);
                const auto envA = levels (a, held, rate);
                double peakA = -200.0;
                for (const double v : envA)
                    peakA = std::max (peakA, v);
                const double hnrA = hnr (a, f0, 0.15, held, rate);
                double medA, iqrA;
                pitch (a, f0, 0.1, held, rate, medA, iqrA);
                const double attackA = rise (a, rate), crestA = crest (a, rate), monoA = monoLoss (a, (int) (held * rate));
                for (int engine = 0; engine <= P::EngineDrone; ++engine)
                {
                    if (! engines.containsChar ((juce::juce_wchar) ('A' + engine)))
                        continue;
                    q[P::Engine] = (float) engine;
                    const double t0 = juce::Time::getMillisecondCounterHiRes();
                    const auto y = engine == 0 ? a : play (s, q, note, held, total, rate);
                    const double ms = engine == 0 ? 0.0 : juce::Time::getMillisecondCounterHiRes() - t0;
                    const auto env = levels (y, held, rate);
                    std::vector<double> diffs;
                    for (size_t k = 0; k < env.size() && k < envA.size(); ++k)
                        if (envA[k] > peakA - 40.0)
                            diffs.push_back (env[k] - envA[k]);
                    double gain = 0.0, envErr = 0.0, dip = 0.0, bandErr = 0.0, weight = 0.0;
                    if (! diffs.empty())
                    {
                        auto sorted = diffs;
                        std::sort (sorted.begin(), sorted.end());
                        gain = sorted[sorted.size() / 2];
                        std::vector<double> errs;
                        for (const double d : diffs)
                        {
                            errs.push_back (std::abs (d - gain));
                            dip = std::min (dip, d - gain);
                        }
                        std::sort (errs.begin(), errs.end());
                        envErr = errs[errs.size() / 2];
                        for (size_t k = 0; k < env.size() && k < envA.size(); ++k)
                        {
                            if (envA[k] <= peakA - 40.0)
                                continue;
                            std::array<double, bands> ba, by;
                            const int centre = (int) ((k * 0.02 + 0.01) * rate);
                            bandsAt (a, centre, rate, ba);
                            bandsAt (y, centre, rate, by);
                            double totalA = 0.0;
                            for (const double v : ba)
                                totalA += v;
                            const double wt = std::pow (10.0, (envA[k] - peakA) / 20.0);
                            for (int bb = 0; bb < bands; ++bb)
                                if (const double share = ba[(size_t) bb] / totalA; share >= 1.0e-4)
                                {
                                    bandErr += wt * share * std::abs (10.0 * std::log10 (by[(size_t) bb] / ba[(size_t) bb]) - gain);
                                    weight += wt * share;
                                }
                        }
                    }
                    double med, iqr;
                    pitch (y, f0, 0.1, held, rate, med, iqr);
                    std::fprintf (csv, "\"%s\",%.2f,%c,%+d,%.2f,%.2f,%.2f,%.1f,%.1f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.1f\n", name.toRawUTF8(),
                                  s->rootMidi, (char) ('A' + engine), shift, rmsDb (y, 0.0, held, rate), hnr (y, f0, 0.15, held, rate) - hnrA, hnrA,
                                  med, std::isfinite (iqr) && std::isfinite (iqrA) ? iqr - iqrA : std::numeric_limits<double>::quiet_NaN(),
                                  weight > 0.0 ? bandErr / weight : -1.0, envErr, dip, rise (y, rate), attackA, crest (y, rate), crestA,
                                  monoLoss (y, (int) (held * rate)) - monoA, ms / total);
                }
            }
            std::fflush (csv);
        }
        std::fclose (csv);
        return 0;
    }
    if (juce::String (argv[1]) == "colorplay" && argc > 3)
    {
        // colorplay <list.txt> <out.csv> [wavdir]: the four functions taken from the Color Play research
        // (docs/investigacion_colorplay_2026-11-23.md), each against the same note without it, on every engine.
        // SIDE=<wav>: the sidechain for Impronta and the Sidechain source (a drum loop; read round).
        // Cases: off; deriva (LFO 1 Deriva 0.4 Hz, Suavidad 0.5, on Tono fino at 15 cents); armonico+ / armonico-
        // (filter Armónico, Keytrack 100 %, 2 kHz, Realce +9 / -12 dB); impronta (Impronta Color in Main 1); sc_fuente
        // (Lowpass 800 Hz opened by the Sidechain source); and in B and D, Sostenido held 6 s with Movimiento 0 and 1.
        // Per render: rmsDb (the held part), peakDb, clicks, centroidHz, tonalDb (energy on the harmonics of the note
        // against between them, 1 s from 0.3 s), ms per second of sound, and for Movimiento: handDipDb (the worst 50 ms
        // fall within 0.4 s of the hand-over to the sustain against the level before it), holdSlopeDb (from 0.5 s after
        // the hand-over to the end of the hold) and followCorr (how the centroid follows the sample's own over the
        // sample's length after the hand-over; empty when the sample is too short to tell).
        const auto list = juce::File (juce::String::fromUTF8 (argv[2]));
        juce::StringArray lines;
        list.readLines (lines);
        const juce::File wavDir = argc > 4 ? juce::File (juce::String::fromUTF8 (argv[4])) : juce::File();
        if (wavDir != juce::File())
            wavDir.createDirectory();

        juce::AudioBuffer<float> side;
        double sideRate = 0.0;
        if (const char* path = std::getenv ("SIDE"))
            SampleAnalysis::loadFile (juce::File (juce::String::fromUTF8 (path)), side, sideRate);

        auto* csv = std::fopen (argv[3], "w");
        if (csv == nullptr)
            return 1;
        std::fprintf (csv, "file,engine,case,rmsDb,peakDb,clicks,centroidHz,tonalDb,msPerSec,handDipDb,holdSlopeDb,followCorr\n");

        constexpr double rate = 48000.0;
        const auto centroid = [] (const juce::AudioBuffer<float>& a, int from, int length, double sr)
        {
            constexpr int order = 12, size = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<float> w ((size_t) size * 2);
            double num = 0.0, den = 0.0;
            for (int at = from; at + size <= std::min (a.getNumSamples(), from + length); at += size / 2)
            {
                std::fill (w.begin(), w.end(), 0.0f);
                for (int i = 0; i < size; ++i)
                    w[(size_t) i] = 0.5f * (a.getSample (0, at + i) + a.getSample (1, at + i))
                                  * (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / size));
                fft.performFrequencyOnlyForwardTransform (w.data());
                for (int k = 1; k < size / 2; ++k)
                {
                    const double pw = (double) w[(size_t) k] * w[(size_t) k];
                    num += pw * k * sr / size;
                    den += pw;
                }
            }
            return den > 0.0 ? num / den : 0.0;
        };
        // Energy within a quarter of the note's frequency of its harmonics, against the rest (up to 8 kHz).
        const auto tonal = [] (const juce::AudioBuffer<float>& a, int from, double f0, double sr)
        {
            constexpr int order = 15, size = 1 << order;
            if (from + size > a.getNumSamples() || f0 <= 0.0)
                return 0.0;
            juce::dsp::FFT fft (order);
            std::vector<float> w ((size_t) size * 2, 0.0f);
            for (int i = 0; i < size; ++i)
                w[(size_t) i] = 0.5f * (a.getSample (0, from + i) + a.getSample (1, from + i))
                              * (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / size));
            fft.performFrequencyOnlyForwardTransform (w.data());
            double on = 1.0e-20, off = 1.0e-20;
            for (int k = 1; k < size / 2; ++k)
            {
                const double hz = k * sr / size;
                if (hz > 8000.0 || hz < 0.75 * f0)
                    continue;
                const double h = hz / f0;
                const double pw = (double) w[(size_t) k] * w[(size_t) k];
                (std::abs (h - std::round (h)) < 0.25 ? on : off) += pw;
            }
            return 10.0 * std::log10 (on / off);
        };
        const auto writeWav = [&wavDir] (const juce::AudioBuffer<float>& a, const juce::String& name, double sr, double matchDb)
        {
            if (wavDir == juce::File())
                return;
            juce::AudioBuffer<float> b (a);
            b.applyGain ((float) std::pow (10.0, matchDb / 20.0));
            const float peak = b.getMagnitude (0, b.getNumSamples());
            if (peak > 0.97f)
                b.applyGain (0.97f / peak);
            auto file = wavDir.getChildFile (juce::File::createLegalFileName (name) + ".wav");
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
            juce::WavAudioFormat wav;
            if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)))
                writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        };

        for (const auto& line : lines)
        {
            const juce::File f (line.trim());
            if (! f.existsAsFile())
                continue;
            juce::AudioBuffer<float> raw;
            double sr = 0.0;
            if (! SampleAnalysis::loadFile (f, raw, sr))
                continue;
            if (std::getenv ("CPDEBUG") != nullptr)
            {
                std::printf ("analysing %s\n", f.getFileName().toRawUTF8());
                std::fflush (stdout);
            }
            const auto s = SampleAnalysis::analyse (raw, sr, f.getFileNameWithoutExtension(), false, true);
            const double len = s->length / sr;
            const double held = juce::jlimit (1.0, 4.0, len);
            if (std::getenv ("CPDEBUG") != nullptr)
            {
                std::printf ("analysed\n");
                std::fflush (stdout);
            }
            const double f0 = s->pitched ? 440.0 * std::pow (2.0, (s->rootMidi - 69.0) / 12.0) : 0.0;
            const auto name = f.getFileNameWithoutExtension();

            struct Case { const char* name; int engine; std::function<void (ParamSnapshot&)> set; double hold; };
            std::vector<Case> cases;
            for (int e = 0; e < 6; ++e)
            {
                cases.push_back ({ "off", e, [] (ParamSnapshot&) {}, held });
                cases.push_back ({ "deriva", e, [] (ParamSnapshot& q)
                {
                    q[P::Lfo1Shape] = (float) P::Drift; q[P::Lfo1Rate] = 0.4f; q[P::Lfo1Smooth] = 0.5f;
                    q[P::M1Src] = (float) P::SrcLfo1; q[P::M1Dst] = (float) P::DstFine; q[P::M1Amt] = 0.15f;
                }, held });
                const auto harmonic = [] (float gain) { return [gain] (ParamSnapshot& q)
                {
                    q[P::FltOn] = 1.0f; q[P::FltType] = (float) P::Harmonic; q[P::FltFreq] = 1000.0f; q[P::FltKey] = 1.0f;
                    q[P::FltHarmGain] = gain; q[P::FltRes] = 0.5f;
                }; };
                cases.push_back ({ "armonico+", e, harmonic (9.0f), held });
                cases.push_back ({ "armonico-", e, harmonic (-12.0f), held });
                if (side.getNumSamples() > 0)
                {
                    cases.push_back ({ "impronta", e, [] (ParamSnapshot& q)
                    {
                        q[(size_t) P::fxType (0)] = (float) Fx::Impronta;
                        q[(size_t) P::fxOn (0)] = 1.0f;
                        q[(size_t) P::fxMix (0)] = 1.0f;
                        for (int k = 0; k < P::fxParamsPerSlot && k < (int) Fx::spec (Fx::Impronta).params.size(); ++k)
                            q[(size_t) P::fxParam (0, k)] = Fx::toNorm (Fx::spec (Fx::Impronta).params[(size_t) k], Fx::spec (Fx::Impronta).params[(size_t) k].def);
                    }, held });
                    cases.push_back ({ "sc_fuente", e, [] (ParamSnapshot& q)
                    {
                        q[P::FltOn] = 1.0f; q[P::FltType] = (float) P::LP; q[P::FltFreq] = 800.0f;
                        q[P::M1Src] = (float) P::SrcSidechain; q[P::M1Dst] = (float) P::DstCutoff; q[P::M1Amt] = 0.5f;
                    }, held });
                }
                if (e == P::EngineReenvelope || e == P::EngineHybrid)
                    for (const float m : { 0.0f, 1.0f })
                        cases.push_back ({ m > 0.0f ? "mov1" : "mov0", e, [m] (ParamSnapshot& q)
                        {
                            q[P::SustainType] = (float) P::SustainHold; q[P::AmpS] = 1.0f; q[P::SustainMotion] = m;
                        }, 6.0 });
            }

            double offRms[6] {};
            for (const auto& c : cases)
            {
                auto q = defaultsFor (*s);
                q[P::Engine] = (float) c.engine;
                c.set (q);
                const double total = c.hold + 0.6;
                if (std::getenv ("CPDEBUG") != nullptr)
                {
                    std::printf ("  %c %s\n", 'A' + c.engine, c.name);
                    std::fflush (stdout);
                }

                // The note, with the sidechain fed block by block (the loop read round).
                SynthEngine e;
                e.prepare (rate);
                e.setSample (s);
                e.prepareFx (q);
                const int totalSamples = (int) (total * rate), heldSamples = (int) (c.hold * rate);
                juce::AudioBuffer<float> out (2, totalSamples), buf (2, 512), sc (2, 512);
                out.clear();
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                for (int pos = 0; pos < totalSamples; pos += 512)
                {
                    const int n = std::min (512, totalSamples - pos);
                    buf.setSize (2, n, false, false, true);
                    juce::MidiBuffer midi;
                    if (pos == 0)
                        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
                    if (pos <= heldSamples && heldSamples < pos + n)
                        midi.addEvent (juce::MidiMessage::noteOff (1, 60), heldSamples - pos);
                    if (side.getNumSamples() > 0)
                    {
                        for (int ch = 0; ch < 2; ++ch)
                            for (int i = 0; i < n; ++i)
                            {
                                const int at = (int) (std::fmod ((pos + i) * sideRate / rate, (double) side.getNumSamples()));
                                sc.setSample (ch, i, side.getSample (std::min (ch, side.getNumChannels() - 1), at));
                            }
                        e.setSidechain (sc.getReadPointer (0), sc.getReadPointer (1), n);
                    }
                    e.process (buf, midi, q, 120.0);
                    for (int ch = 0; ch < 2; ++ch)
                        out.copyFrom (ch, pos, buf, ch, 0, n);
                }
                const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                if (std::getenv ("CPDEBUG") != nullptr)
                {
                    std::puts ("    rendered");
                    std::fflush (stdout);
                }

                const double level = rmsDb (out, 0.0, c.hold, rate);
                if (juce::String (c.name) == "off")
                    offRms[c.engine] = level;
                const double peak = 20.0 * std::log10 (std::max (1.0e-9f, out.getMagnitude (0, out.getNumSamples())));
                const int clicks = countClicks (out, rate);
                const double cen = centroid (out, 0, heldSamples, rate);
                const double ton = tonal (out, (int) (0.3 * rate), f0, rate);

                double dip = 0.0, slope = 0.0, follow = 0.0;
                bool followKnown = false;
                if (juce::String (c.name).startsWith ("mov") && s->early.loop.getNumSamples() > 0)
                {
                    // The hand-over: where the original reaches the sustain point (at the root, one sample a sample).
                    const double hand = (s->early.start - s->onset) / sr;
                    // The worst 50 ms within 0.4 s after it against the steady level that follows (0.5 to 1 s after it):
                    // a hole where the sample hands over to its sustain.
                    const double steady = rmsDb (out, hand + 0.5, hand + 1.0, rate);
                    double worst = 0.0;
                    for (double t = std::max (0.0, hand); t < hand + 0.4; t += 0.01)
                        worst = std::min (worst, rmsDb (out, t, t + 0.05, rate) - steady);
                    dip = worst;
                    slope = rmsDb (out, c.hold - 0.6, c.hold - 0.1, rate) - rmsDb (out, hand + 0.5, hand + 1.0, rate);
                    // The centroid of the render and of the sample, every 100 ms over what the sample lasts after it.
                    std::vector<double> a, b;
                    for (double t = hand + 0.2; t + 0.1 < std::min (c.hold, len - s->onset / sr); t += 0.1)
                    {
                        a.push_back (centroid (out, (int) (t * rate), (int) (0.1 * rate) + 4096, rate));
                        b.push_back (centroid (s->audio, (int) ((t + s->onset / sr) * sr), (int) (0.1 * sr) + 4096, sr));
                    }
                    if (a.size() >= 8)
                    {
                        double ma = 0, mb = 0;
                        for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
                        ma /= (double) a.size(); mb /= (double) b.size();
                        double ab = 0, aa = 1.0e-12, bb = 1.0e-12;
                        for (size_t i = 0; i < a.size(); ++i) { ab += (a[i] - ma) * (b[i] - mb); aa += juce::square (a[i] - ma); bb += juce::square (b[i] - mb); }
                        follow = ab / std::sqrt (aa * bb);
                        followKnown = true;
                    }
                }

                std::fprintf (csv, "\"%s\",%c,%s,%.2f,%.2f,%d,%.0f,%.2f,%.2f,%.2f,%.2f,%s\n", name.toRawUTF8(), 'A' + c.engine, c.name, level, peak,
                              clicks, cen, ton, ms / total, dip, slope, followKnown ? juce::String (follow, 3).toRawUTF8() : "");
                // WAVCASES="A:off,A:deriva,B:mov1": only those renders written (all of them without it).
                const auto key = juce::String::charToString ((juce::juce_wchar) ('A' + c.engine)) + ":" + c.name;
                const char* wanted = std::getenv ("WAVCASES");
                if (wanted == nullptr || juce::StringArray::fromTokens (juce::String (wanted), ",", "").contains (key))
                    writeWav (out, name + "_" + key.replace (":", "_"), rate, offRms[c.engine] - level);
            }
            std::fflush (csv);
            std::printf ("%s\n", name.toRawUTF8());
        }
        std::fclose (csv);
        return 0;
    }
    if (juce::String (argv[1]) == "calidad" && argc > 3)
    {
        // calidad <list.txt> <out.csv> [engines e.g. ABCDEF]: the reference library's sound check, per file, engine and
        // case (root; +7 and -12 semitones; soft, velocity 0.3; a chord of root, +4, +7), the note held for the sample's
        // length (at most 4 s) with the detected envelope:
        //  - fidelityDb: at the root, the mean error of 24 bands every 20 ms against the sample itself (engine A
        //    untouched), level-matched;
        //  - worstDipDb / worstBumpDb: the worst fall and rise of the 20 ms level against the sample's own at the root
        //    (elsewhere against a 100 ms median of itself);
        //  - monoDb: what summing to mono loses (0 = nothing; -3 = uncorrelated; much lower = out of phase), and
        //    monoVsSampleDb: that against the sample's own;
        //  - corr: left/right correlation; attackMs: 10-90 % rise; sampleAttackMs: the sample's;
        //  - onsetClickDb, releaseClickDb: the slope at the start and at the release against around them;
        //  - tailDb: what is left 0.3 s after the release ends, against the held level (noise, hum, leftovers);
        //  - clicks / clicksRef: discontinuities (a sample-to-sample bend 10 times over the 5 ms around it, and over
        //    -54 dBFS), in the engine's output and in engine A's in the same case (the file, or the file resampled):
        //    more than A is the engine's own;
        //  - dcDb: the mean of the held part against its RMS; overPeakDb: the output's peak against A's in the same case;
        //  - hitSpreadDb / hitSpreadRefDb (case "repetida": the note struck 4 times, 150 ms apart): how much the four
        //    hits' peaks differ (standard deviation), and A's.
        std::FILE* csv = nullptr;
        if (fopen_s (&csv, argv[3], "w") != 0)
            return 1;
        const juce::String engines = argc > 4 ? juce::String (argv[4]) : juce::String ("ABCDEF");
        std::fprintf (csv, "file,engine,case,finite,peakDb,levelDb,fidelityDb,worstDipDb,worstBumpDb,monoDb,monoVsSampleDb,corr,attackMs,sampleAttackMs,"
                           "onsetClickDb,releaseClickDb,tailDb,clicks,clicksRef,dcDb,overPeakDb,hitSpreadDb,hitSpreadRefDb\n");
        const auto monoLoss = [] (const juce::AudioBuffer<float>& y, int from, int to)
        {
            double mid = 0.0, sides = 0.0;
            for (int i = std::max (0, from); i < std::min (to, y.getNumSamples()); ++i)
            {
                const double l = y.getSample (0, i), r = y.getSample (1, i);
                mid += 0.25 * (l + r) * (l + r);
                sides += 0.5 * (l * l + r * r);
            }
            return 10.0 * std::log10 ((mid + 1.0e-20) / (sides + 1.0e-20));
        };
        const auto correlation = [] (const juce::AudioBuffer<float>& y, int from, int to)
        {
            double lr = 0.0, ll = 0.0, rr = 0.0;
            for (int i = std::max (0, from); i < std::min (to, y.getNumSamples()); ++i)
            {
                const double l = y.getSample (0, i), r = y.getSample (1, i);
                lr += l * r;
                ll += l * l;
                rr += r * r;
            }
            return lr / std::sqrt (ll * rr + 1.0e-30);
        };
        const auto riseMs = [] (const juce::AudioBuffer<float>& y, double rate)
        {
            // the 1 ms level: from 10 % to 90 % of its peak, in the first second
            const int w = std::max (1, (int) (0.001 * rate)), n = std::min (y.getNumSamples(), (int) rate);
            std::vector<double> env;
            for (int i = 0; i + w <= n; i += w)
            {
                double e = 0.0;
                for (int j = i; j < i + w; ++j)
                    e = std::max (e, (double) std::abs (y.getSample (0, j)) + std::abs (y.getSample (1, j)));
                env.push_back (e);
            }
            if (env.empty())
                return 0.0;
            const double top = *std::max_element (env.begin(), env.end());
            int a = -1, b = -1;
            for (int k = 0; k < (int) env.size(); ++k)
            {
                if (a < 0 && env[(size_t) k] >= 0.1 * top)
                    a = k;
                if (env[(size_t) k] >= 0.9 * top)
                {
                    b = k;
                    break;
                }
            }
            return a >= 0 && b >= a ? (b - a) * 1000.0 * w / rate : 0.0;
        };
        const auto envelope = [] (const juce::AudioBuffer<float>& y, double seconds, double rate)
        {
            std::vector<double> e;
            for (double t = 0.0; t + 0.02 <= seconds; t += 0.02)
                e.push_back (rmsDb (y, t, t + 0.02, rate));
            return e;
        };
        const auto clicksIn = [] (const juce::AudioBuffer<float>& y, int from, int to, double rate)
        {
            const int w = std::max (8, (int) (0.005 * rate));
            from = std::max (2 + w, from);
            to = std::min (y.getNumSamples() - w - 1, to);
            int count = 0;
            if (to <= from)
                return 0;
            std::vector<double> d2 ((size_t) y.getNumSamples(), 0.0), sum ((size_t) y.getNumSamples() + 1, 0.0);
            std::vector<int> at;
            for (int ch = 0; ch < y.getNumChannels(); ++ch)
            {
                const float* x = y.getReadPointer (ch);
                for (int i = 2; i < y.getNumSamples(); ++i)
                    d2[(size_t) i] = (double) x[i] - 2.0 * x[i - 1] + x[i - 2];
                for (int i = 0; i < y.getNumSamples(); ++i)
                    sum[(size_t) i + 1] = sum[(size_t) i] + d2[(size_t) i] * d2[(size_t) i];
                int last = -1000000;
                for (int i = from; i < to; ++i)
                {
                    const double v = std::abs (d2[(size_t) i]);
                    if (v < 2.0e-3)
                        continue;
                    const double around = (sum[(size_t) i + w] - sum[(size_t) i - w] - v * v) / (2.0 * w - 1.0);
                    if (v > 10.0 * std::sqrt (std::max (around, 1.0e-30)) && i - last > w)
                    {
                        last = i;
                        at.push_back (i);
                    }
                }
            }
            // the same click on both sides counts once
            std::sort (at.begin(), at.end());
            int prev = -1000000;
            for (const int i : at)
                if (i - prev > w)
                {
                    ++count;
                    prev = i;
                }
            return count;
        };
        const auto hitSpread = [] (const juce::AudioBuffer<float>& y, double rate)
        {
            std::vector<double> peaks;
            for (int h = 0; h < 4; ++h)
            {
                const int a = (int) (h * 0.15 * rate), b = std::min (y.getNumSamples(), (int) ((h * 0.15 + 0.15) * rate));
                float peak = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = a; i < b; ++i)
                        peak = std::max (peak, std::abs (y.getSample (ch, i)));
                peaks.push_back (20.0 * std::log10 (peak + 1.0e-9));
            }
            double m = 0.0, v = 0.0;
            for (const double x : peaks)
                m += x / 4.0;
            for (const double x : peaks)
                v += (x - m) * (x - m) / 4.0;
            return std::sqrt (v);
        };
        constexpr int bands = 24;
        juce::dsp::FFT fft (11);
        std::vector<float> win (2048), fbuf (4096);
        for (int i = 0; i < 2048; ++i)
            win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / 2048.0f);
        // CAL_CANAL=1: the fidelity of each side against the same side of the sample (their mean), not of the sum to
        // mono (which suits a model made of the sum, and counts how two sides add as timbre)
        const bool perSide = std::getenv ("CAL_CANAL") != nullptr;
        int bandChannel = -1;
        const auto bandsAt = [&] (const juce::AudioBuffer<float>& x, int centre, double rate, std::array<double, bands>& band)
        {
            std::fill (fbuf.begin(), fbuf.end(), 0.0f);
            for (int i = 0; i < 2048; ++i)
                if (const int t = centre - 1024 + i; t >= 0 && t < x.getNumSamples())
                    fbuf[(size_t) i] = (bandChannel < 0 ? 0.5f * (x.getSample (0, t) + x.getSample (1, t)) : x.getSample (bandChannel, t)) * win[(size_t) i];
            fft.performFrequencyOnlyForwardTransform (fbuf.data(), true);
            band.fill (1.0e-12);
            for (int k = 2; k < 1024; ++k)
                if (const double hz = k * rate / 2048.0; hz >= 40.0 && hz <= 16000.0)
                    band[(size_t) juce::jlimit (0, bands - 1, (int) (bands * std::log (hz / 40.0) / std::log (16000.0 / 40.0)))] += (double) fbuf[(size_t) k] * fbuf[(size_t) k];
        };
        for (const auto& f : filesIn (juce::File (juce::String::fromUTF8 (argv[2]))))
        {
            juce::AudioBuffer<float> raw;
            double rate = 0.0;
            if (! SampleAnalysis::loadFile (f, raw, rate))
                continue;
            const auto s = SampleAnalysis::analyse (raw, rate, f.getFileNameWithoutExtension(), false, true);
            const double held = juce::jlimit (0.3, 4.0, (s->length - s->onset) / rate), total = held + 0.8;
            auto base = defaultsFor (*s);
            base[P::Engine] = (float) P::EngineRebuild;
            const auto reference = play (s, base, SynthEngine::rootKey, held, total, rate);     // the sample itself
            const auto refEnv = envelope (reference, held, rate);
            double refPeak = -200.0;
            for (const double v : refEnv)
                refPeak = std::max (refPeak, v);
            const double refMono = monoLoss (reference, 0, (int) (held * rate));
            const double refRise = riseMs (reference, rate);
            const auto name = f.getFileName().replace (",", "_");
            std::array<juce::AudioBuffer<float>, 6> refCase;       // engine A in each case (when A is among the engines)
            for (int engine = 0; engine <= P::EngineDrone; ++engine)
            {
                if (! engines.containsChar ((juce::juce_wchar) ('A' + engine)))
                    continue;
                for (int c = 0; c < 6; ++c)
                {
                    static const char* cases[] = { "raiz", "+7", "-12", "suave", "acorde", "repetida" };
                    auto q = base;
                    q[P::Engine] = (float) engine;
                    juce::AudioBuffer<float> y;
                    if (c == 5)
                    {
                        // repeated: the root struck 4 times, 150 ms apart, each held 120 ms (velocity 0.9)
                        SynthEngine e;
                        e.prepare (rate);
                        e.setSample (s);
                        e.prepareFx (q);
                        const int totalSamples = (int) (1.0 * rate);
                        y.setSize (2, totalSamples);
                        y.clear();
                        juce::AudioBuffer<float> b (2, 256);
                        for (int pos = 0; pos < totalSamples; pos += 256)
                        {
                            const int n = std::min (256, totalSamples - pos);
                            b.setSize (2, n, false, false, true);
                            juce::MidiBuffer m;
                            for (int h = 0; h < 4; ++h)
                            {
                                const int on = (int) (h * 0.15 * rate), offAt = on + (int) (0.12 * rate);
                                if (on >= pos && on < pos + n)
                                    m.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey, 0.9f), on - pos);
                                if (offAt >= pos && offAt < pos + n)
                                    m.addEvent (juce::MidiMessage::noteOff (1, SynthEngine::rootKey), offAt - pos);
                            }
                            e.process (b, m, q, 120.0);
                            for (int ch = 0; ch < 2; ++ch)
                                y.copyFrom (ch, pos, b, ch, 0, n);
                        }
                    }
                    else if (c == 4)
                    {
                        // a chord: three notes started together, each rendered and added (the engine plays one voice each)
                        y.setSize (2, (int) (total * rate));
                        y.clear();
                        for (const int k : { 0, 4, 7 })
                        {
                            const auto v = play (s, q, SynthEngine::rootKey + k, held, total, rate);
                            for (int ch = 0; ch < 2; ++ch)
                                y.addFrom (ch, 0, v, ch, 0, std::min (v.getNumSamples(), y.getNumSamples()));
                        }
                    }
                    else
                    {
                        const int note = SynthEngine::rootKey + (c == 1 ? 7 : c == 2 ? -12 : 0);
                        if (c == 3)
                        {
                            // soft: velocity 0.3 (the engine's play() strikes at 1.0: a lower velocity through its own MIDI)
                            SynthEngine e;
                            e.prepare (rate);
                            e.setSample (s);
                            e.prepareFx (q);
                            const int totalSamples = (int) (total * rate), heldSamples = (int) (held * rate);
                            y.setSize (2, totalSamples);
                            y.clear();
                            juce::AudioBuffer<float> b (2, 512);
                            for (int pos = 0; pos < totalSamples; pos += 512)
                            {
                                const int n = std::min (512, totalSamples - pos);
                                b.setSize (2, n, false, false, true);
                                juce::MidiBuffer m;
                                if (pos == 0)
                                    m.addEvent (juce::MidiMessage::noteOn (1, note, 0.3f), 0);
                                if (pos <= heldSamples && heldSamples < pos + n)
                                    m.addEvent (juce::MidiMessage::noteOff (1, note), heldSamples - pos);
                                e.process (b, m, q, 120.0);
                                for (int ch = 0; ch < 2; ++ch)
                                    y.copyFrom (ch, pos, b, ch, 0, n);
                            }
                        }
                        else
                            y = play (s, q, note, held, total, rate);
                    }
                    if (engine == 0)
                        refCase[(size_t) c] = y;
                    if (const char* dir = std::getenv ("CAL_WAV"))
                    {
                        const auto out = juce::File (juce::String::fromUTF8 (dir)).getChildFile (f.getFileNameWithoutExtension() + " " + juce::String::charToString ((juce::juce_wchar) ('A' + engine)) + " " + cases[c] + ".wav");
                        out.deleteFile();
                        std::unique_ptr<juce::OutputStream> st = std::make_unique<juce::FileOutputStream> (out);
                        juce::WavAudioFormat wav;
                        if (auto w = wav.createWriterFor (st, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
                            w->writeFromAudioSampleBuffer (y, 0, y.getNumSamples());
                    }
                    const auto& ref = refCase[(size_t) c];
                    const bool hasRef = ref.getNumSamples() > 0;
                    const int clicks = clicksIn (y, (int) (0.005 * rate), y.getNumSamples(), rate);
                    const int clicksRef = hasRef ? clicksIn (ref, (int) (0.005 * rate), ref.getNumSamples(), rate) : -1;
                    double dc = 0.0;
                    {
                        const int to = std::min (y.getNumSamples(), (int) (std::min (held, c == 5 ? 0.9 : held) * rate));
                        double sum = 0.0;
                        for (int i = 0; i < to; ++i)
                            sum += 0.5 * ((double) y.getSample (0, i) + y.getSample (1, i));
                        dc = 20.0 * std::log10 (std::abs (sum / std::max (1, to)) + 1.0e-12) - rmsDb (y, 0.0, to / rate, rate);
                    }
                    const double overPeak = hasRef ? juce::Decibels::gainToDecibels (y.getMagnitude (0, y.getNumSamples()))
                                                       - juce::Decibels::gainToDecibels (ref.getMagnitude (0, ref.getNumSamples())) : 0.0;
                    const double spread = c == 5 ? hitSpread (y, rate) : -1.0, spreadRef = c == 5 && hasRef ? hitSpread (ref, rate) : -1.0;
                    const auto env = envelope (y, c == 5 ? 0.9 : held, rate);
                    double fidelity = -1.0, dip = 0.0, bump = 0.0;
                    if (c == 0)
                    {
                        // against the sample: level-matched (the median difference), in the frames within 40 dB of its peak
                        std::vector<double> diffs;
                        for (size_t k = 0; k < env.size() && k < refEnv.size(); ++k)
                            if (refEnv[k] > refPeak - 40.0)
                                diffs.push_back (env[k] - refEnv[k]);
                        if (! diffs.empty())
                        {
                            auto sorted = diffs;
                            std::sort (sorted.begin(), sorted.end());
                            const double gain = sorted[sorted.size() / 2];
                            for (const double d : diffs)
                            {
                                dip = std::min (dip, d - gain);
                                bump = std::max (bump, d - gain);
                            }
                            double err = 0.0, weight = 0.0;
                            for (const int side : perSide ? std::vector<int> { 0, 1 } : std::vector<int> { -1 })
                            for (size_t k = 0; k < env.size() && k < refEnv.size(); ++k)
                            {
                                bandChannel = side;
                                if (refEnv[k] <= refPeak - 40.0)
                                    continue;
                                std::array<double, bands> a, b;
                                const int centre = (int) ((k * 0.02 + 0.01) * rate);
                                bandsAt (reference, centre, rate, a);
                                bandsAt (y, centre, rate, b);
                                double totalA = 0.0;
                                for (const double v : a)
                                    totalA += v;
                                const double wt = std::pow (10.0, (refEnv[k] - refPeak) / 20.0);
                                for (int bb = 0; bb < bands; ++bb)
                                    if (const double share = a[(size_t) bb] / totalA; share >= 1.0e-4)
                                    {
                                        err += wt * share * std::abs (10.0 * std::log10 (b[(size_t) bb] / a[(size_t) bb]) - gain);
                                        weight += wt * share;
                                    }
                            }
                            fidelity = weight > 0.0 ? err / weight : -1.0;
                        }
                    }
                    else
                    {
                        // against itself: the 20 ms level against the median of the 100 ms around it
                        double peak = -200.0;
                        for (const double v : env)
                            peak = std::max (peak, v);
                        for (size_t k = 2; k + 2 < env.size(); ++k)
                        {
                            if (env[k] < peak - 40.0)
                                continue;
                            std::array<double, 5> around { env[k - 2], env[k - 1], env[k], env[k + 1], env[k + 2] };
                            std::sort (around.begin(), around.end());
                            dip = std::min (dip, env[k] - around[2]);
                            bump = std::max (bump, env[k] - around[2]);
                        }
                    }
                    const int off = (int) (held * rate);
                    const double heldDb = rmsDb (y, 0.0, held, rate);
                    std::fprintf (csv, "\"%s\",%c,%s,%d,%.2f,%.2f,%.2f,%.1f,%.1f,%.2f,%.2f,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%.1f,%.2f,%.2f,%.2f\n", name.toRawUTF8(),
                                  (char) ('A' + engine), cases[c], allFinite (y) ? 1 : 0, juce::Decibels::gainToDecibels (y.getMagnitude (0, y.getNumSamples())),
                                  heldDb, fidelity, dip, bump, monoLoss (y, 0, off), monoLoss (y, 0, off) - refMono, correlation (y, 0, off),
                                  riseMs (y, rate), refRise,
                                  edgeDb (y, 0, (int) (0.003 * rate)) - edgeDb (y, (int) (0.003 * rate), (int) (0.05 * rate)),
                                  edgeDb (y, off, off + (int) (0.005 * rate)) - edgeDb (y, off - (int) (0.05 * rate), off),
                                  rmsDb (y, total - 0.15, total, rate) - heldDb, clicks, clicksRef, dc, overPeak, spread, spreadRef);
                }
            }
            std::fflush (csv);
        }
        std::fclose (csv);
        return 0;
    }
    if (juce::String (argv[1]) == "acordes" && argc > 2)
    {
        // acordes <list.txt>: the chord one-shots (chord or stab in the name, a key in it: "..._chord_Cm9.wav", the
        // chord's root and whether it is minor) against the chord their sound gives, by how sure it was.
        struct Bin { int all = 0, right = 0, rootOnly = 0; };
        std::array<Bin, 5> bins {};
        int total = 0, right = 0;
        for (const auto& f : filesIn (juce::File (juce::String::fromUTF8 (argv[2]))))
        {
            const auto lower = f.getFileName().toLowerCase();
            if (! lower.contains ("chord") && ! lower.contains ("stab"))
                continue;
            // one chord only: no loop, no tempo in the name (a loop of chords carries the key, not a chord)
            bool tempo = false;
            for (const auto& token : juce::StringArray::fromTokens (lower, "_- .", ""))
                tempo = tempo || (token.containsOnly ("0123456789") && token.length() >= 2 && token.getIntValue() >= 60 && token.getIntValue() <= 200)
                              || token.startsWith ("bpm") || token.endsWith ("bpm");
            if (lower.contains ("loop") || tempo)
                continue;
            const auto named = KeyDetect::fromName (f.getFileName());
            if (named.root < 0)
                continue;
            juce::AudioBuffer<float> audio;
            double rate = 0.0;
            if (! SampleAnalysis::loadFile (f, audio, rate))
                continue;
            const auto chroma = KeyDetect::fromAudio (audio, rate).chroma;
            const auto c = KeyDetect::chordFromChroma (chroma);
            const bool minor = c.root >= 0 && KeyDetect::chordKinds()[(size_t) c.kind].minorish;
            const bool ok = c.root == named.root && minor == named.minor;
            auto& b = bins[(size_t) juce::jlimit (0, 4, (int) (c.confidence * 5.0f))];
            ++b.all;
            b.right += ok;
            b.rootOnly += c.root == named.root;
            ++total;
            right += ok;
            if (std::getenv ("ACORDES_LISTA") != nullptr)
                std::printf ("%s -> %s %.2f %s\n", f.getFileName().toRawUTF8(), c.name().toRawUTF8(), c.confidence, ok ? "bien" : "MAL");
        }
        std::printf ("%d acordes, bien (tónica y mayor/menor) %.0f %%\n", total, total > 0 ? 100.0 * right / total : 0.0);
        for (int k = 0; k < 5; ++k)
            std::printf ("  confianza %3d-%3d %%: %4d, bien %5.1f %%, tónica bien %5.1f %%\n", k * 20, k * 20 + 20, bins[(size_t) k].all,
                         bins[(size_t) k].all > 0 ? 100.0 * bins[(size_t) k].right / bins[(size_t) k].all : 0.0,
                         bins[(size_t) k].all > 0 ? 100.0 * bins[(size_t) k].rootOnly / bins[(size_t) k].all : 0.0);
        return 0;
    }
    if (juce::String (argv[1]) == "resin_debug" && argc > 2)
    {
        // resin_debug <file>: the first 150 ms of engine C (5 ms steps) as it is, with Warp off, with a short transient
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        const juce::File f (juce::String::fromUTF8 (argv[2]));
        if (! SampleAnalysis::loadFile (f, raw, rate))
            return 1;
        const auto s = SampleAnalysis::analyse (raw, rate, f.getFileNameWithoutExtension(), false, true);
        std::printf ("onset %d peak %d transientMs %.1f pitched %d partials %d warp default %.0f\n", s->onset, s->peak, s->transientMs, (int) s->pitched,
                     s->partials.numPartials, P::defs()[(size_t) P::WarpMode].def);
        std::printf ("attack %.3f decay %.3f sustain %.2f curve %.2f fade %.1f\n", s->detected.attack, s->detected.decay, s->detected.sustain,
                     s->detected.curve, s->detected.fadeDbPerSec);
        const double step = std::getenv ("STEP") != nullptr ? std::atof (std::getenv ("STEP")) : 0.02;
        const int frames = 40;
        std::printf ("%-12s", "sample");
        for (int k = 0; k < frames; ++k)
            std::printf (" %4.0f", rmsDb (s->audio, s->onset / rate + k * step, s->onset / rate + (k + 1) * step, s->sampleRate));
        std::printf ("\n");
        const char* names[] = { "A", "C", "C sin ruido", "C sin parc", "C trans 5", "B", "D" };
        for (int variant = 0; variant < 7; ++variant)
        {
            auto q = defaultsFor (*s);
            q[P::Engine] = (float) (variant == 0 ? P::EngineRebuild : variant == 5 ? P::EngineReenvelope : variant == 6 ? P::EngineHybrid : P::EngineResynth);
            if (variant == 2)
                q[P::NoiseLevel] = 0.0f;
            if (variant == 3)
                q[P::Partials] = 4.0f;
            if (variant == 4)
                q[P::TransientMs] = 5.0f;
            const auto y = play (s, q, SynthEngine::rootKey, frames * step, frames * step, rate);
            std::printf ("%-12s", names[variant]);
            for (int k = 0; k < frames; ++k)
                std::printf (" %4.0f", rmsDb (y, k * step, (k + 1) * step, rate));
            std::printf ("\n");
        }
        return 0;
    }
    if (juce::String (argv[1]) == "resintesis" && argc > 3)
    {
        // resintesis <list.txt> <out.csv> [wav folder]: each one-shot played at its own pitch for its length by
        // engine A (the sample itself) and engine C (partials + noise), compared every 20 ms where A is within
        // 40 dB of its peak: the level (worst dip, median error), the spectrum (24 bands, mean error weighted by
        // level), how noisy (spectral flatness, C against A) and the attack (the first 30 ms).
        std::FILE* csv = nullptr;
        if (fopen_s (&csv, argv[3], "w") != 0)
            return 1;
        const juce::File wavDir = argc > 4 ? juce::File (juce::String::fromUTF8 (argv[4])) : juce::File();
        if (wavDir != juce::File())
            wavDir.createDirectory();
        std::fprintf (csv, "file,seconds,pitched,partials,gainDb,worstDipDb,worstBumpDb,medianErrDb,bandErrDb,flatnessDb,attackDb,lowDb,highDb\n");
        constexpr int bands = 24;
        for (const auto& f : filesIn (juce::File (juce::String::fromUTF8 (argv[2]))))
        {
            juce::AudioBuffer<float> raw;
            double rate = 0.0;
            if (! SampleAnalysis::loadFile (f, raw, rate))
                continue;
            const auto s = SampleAnalysis::analyse (raw, rate, f.getFileNameWithoutExtension(), false, true);
            const double seconds = std::min (6.0, s->length / rate);
            // the detected envelope (engine A then plays the sample untouched), held for the sample's length
            auto q = defaultsFor (*s);
            q[P::Engine] = (float) P::EngineRebuild;
            const int note = SynthEngine::rootKey + (std::getenv ("RESIN_NOTA") != nullptr ? std::atoi (std::getenv ("RESIN_NOTA")) : 0);
            const auto a = play (s, q, note, seconds, seconds + 0.1, rate);
            q[P::Engine] = (float) P::EngineResynth;
            if (const char* engine = std::getenv ("RESIN_MOTOR"))          // B, D...: that engine instead of C
                q[P::Engine] = (float) juce::jlimit (0, (int) P::EngineDrone, engine[0] - 'A');
            if (std::getenv ("RESIN_COHERENTE") != nullptr)
                q[P::GrainCoherent] = 1.0f;
            if (const char* level = std::getenv ("RESIN_RUIDO"))
                q[P::NoiseLevel] = (float) std::atof (level);
            const auto c = play (s, q, note, seconds, seconds + 0.1, rate);
            if (wavDir != juce::File())
                for (const auto& [buf, tag] : { std::pair<const juce::AudioBuffer<float>*, const char*> { &a, "A" }, { &c, "C" } })
                {
                    const auto out = wavDir.getChildFile (f.getFileNameWithoutExtension() + " " + tag + ".wav");
                    out.deleteFile();
                    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (out);
                    juce::WavAudioFormat wav;
                    if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
                        w->writeFromAudioSampleBuffer (*buf, 0, buf->getNumSamples());
                }
            // frames of 20 ms: level and 24 log bands (FFT 2048 around each frame's centre)
            const int frame = (int) (0.02 * rate), n = (int) (seconds * rate);
            juce::dsp::FFT fft (11);
            std::vector<float> win (2048), buf (4096);
            for (int i = 0; i < 2048; ++i)
                win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / 2048.0f);
            const auto analyseAt = [&] (const juce::AudioBuffer<float>& x, int centre, std::array<double, bands>& band, double& flat)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < 2048; ++i)
                {
                    const int t = centre - 1024 + i;
                    if (t >= 0 && t < x.getNumSamples())
                        buf[(size_t) i] = 0.5f * (x.getSample (0, t) + x.getSample (1, t)) * win[(size_t) i];
                }
                fft.performFrequencyOnlyForwardTransform (buf.data(), true);
                band.fill (1.0e-12);
                double logSum = 0.0, linSum = 0.0;
                int used = 0;
                for (int k = 2; k < 1024; ++k)
                {
                    const double hz = k * rate / 2048.0;
                    if (hz < 40.0 || hz > 16000.0)
                        continue;
                    const double p = (double) buf[(size_t) k] * buf[(size_t) k] + 1.0e-14;
                    const int b = juce::jlimit (0, bands - 1, (int) (bands * std::log (hz / 40.0) / std::log (16000.0 / 40.0)));
                    band[(size_t) b] += p;
                    logSum += std::log (p);
                    linSum += p;
                    ++used;
                }
                flat = 10.0 * (logSum / used - std::log (linSum / used)) / std::log (10.0);     // dB, 0 = white noise
            };
            std::vector<double> levelA, levelC;
            for (int t = 0; t + frame <= n; t += frame)
            {
                levelA.push_back (rmsDb (a, t / rate, (t + frame) / rate, rate));
                levelC.push_back (rmsDb (c, t / rate, (t + frame) / rate, rate));
            }
            double peakA = -200.0;
            for (const double v : levelA)
                peakA = std::max (peakA, v);
            std::vector<double> diffs;
            for (size_t i = 0; i < levelA.size(); ++i)
                if (levelA[i] > peakA - 40.0)
                    diffs.push_back (levelC[i] - levelA[i]);
            if (diffs.empty())
                continue;
            auto sorted = diffs;
            std::sort (sorted.begin(), sorted.end());
            const double gain = sorted[sorted.size() / 2];
            double worstDip = 0.0, worstBump = 0.0;
            std::vector<double> errs;
            for (const double d : diffs)
            {
                worstDip = std::min (worstDip, d - gain);
                worstBump = std::max (worstBump, d - gain);
                errs.push_back (std::abs (d - gain));
            }
            std::sort (errs.begin(), errs.end());
            double bandErr = 0.0, bandWeight = 0.0, flatA = 0.0, flatC = 0.0, low = 0.0, high = 0.0, lowW = 0.0, highW = 0.0;
            int flatCount = 0;
            for (size_t i = 0; i < levelA.size(); ++i)
            {
                if (levelA[i] <= peakA - 40.0)
                    continue;
                std::array<double, bands> ba, bc;
                double fa, fc;
                const int centre = (int) i * frame + frame / 2;
                analyseAt (a, centre, ba, fa);
                analyseAt (c, centre, bc, fc);
                double totalA = 0.0;
                for (const double v : ba)
                    totalA += v;
                const double weight = std::pow (10.0, (levelA[i] - peakA) / 20.0);
                for (int b = 0; b < bands; ++b)
                {
                    const double share = ba[(size_t) b] / totalA;
                    if (share < 1.0e-4)
                        continue;
                    const double d = 10.0 * std::log10 (bc[(size_t) b] / ba[(size_t) b]) - gain;
                    bandErr += weight * share * std::abs (d);
                    bandWeight += weight * share;
                    if (b < 8)
                    {
                        low += weight * share * d;
                        lowW += weight * share;
                    }
                    else if (b >= 16)
                    {
                        high += weight * share * d;
                        highW += weight * share;
                    }
                }
                flatA += fa;
                flatC += fc;
                ++flatCount;
            }
            // the attack: from the onset, 30 ms
            const double onset = s->onset / rate;
            const double attack = rmsDb (c, onset, onset + 0.03, rate) - rmsDb (a, onset, onset + 0.03, rate) - gain;
            std::fprintf (csv, "\"%s\",%.2f,%d,%d,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f\n", f.getFileName().replace ("\"", "'").toRawUTF8(), seconds,
                          (int) s->pitched, s->partials.numPartials, gain, worstDip, worstBump, errs[errs.size() / 2], bandWeight > 0.0 ? bandErr / bandWeight : 0.0,
                          flatCount > 0 ? (flatC - flatA) / flatCount : 0.0, attack, lowW > 0.0 ? low / lowW : 0.0, highW > 0.0 ? high / highW : 0.0);
            std::fflush (csv);
        }
        std::fclose (csv);
        return 0;
    }
    if (juce::String (argv[1]) == "tonalidad_flujo" && argc > 2)
    {
        // tonalidad_flujo <list.txt> [groups]: a stand-in for a track coming in through the sidechain: samples whose
        // name says the same key, one after the other (each up to 8 s), fed by blocks of 512 to the key's stream;
        // the estimate after 10, 20 and 40 s against that key.
        std::map<int, std::vector<juce::File>> byKey;
        for (const auto& f : filesIn (juce::File (juce::String::fromUTF8 (argv[2]))))
        {
            const auto named = KeyDetect::fromName (f.getFileName());
            if (named.root >= 0)
                byKey[named.root + (named.minor ? 12 : 0)].push_back (f);
        }
        const int groups = argc > 3 ? juce::String (argv[3]).getIntValue() : 60;
        juce::Random rng (7);
        const double checks[3] = { 10.0, 20.0, 40.0 };
        struct Bin { int all = 0, right = 0; };
        std::array<std::array<Bin, 5>, 3> bins {};
        std::array<int, 3> sureAll {}, sureRight {}, total {}, right {};
        std::vector<int> keys;
        for (const auto& [k, files] : byKey)
            if (files.size() >= 6)
                keys.push_back (k);
        for (int g = 0; g < groups && ! keys.empty(); ++g)
        {
            const int key = keys[(size_t) (g % (int) keys.size())];
            auto& files = byKey[key];
            double rate = 0.0;      // the first file's; the others at the same rate only (no resampling here)
            KeyDetect::AudioStream stream;
            double fed = 0.0;
            int check = 0;
            std::vector<float> mono;
            for (int tries = 0; check < 3 && tries < 300; ++tries)
            {
                const auto& f = files[(size_t) rng.nextInt ((int) files.size())];
                juce::AudioBuffer<float> audio;
                double fileRate = 0.0;
                if (! SampleAnalysis::loadFile (f, audio, fileRate) || (rate > 0.0 && std::abs (fileRate - rate) > 1.0))
                    continue;
                if (rate <= 0.0)
                {
                    rate = fileRate;
                    stream.reset (rate);
                }
                const int n = std::min (audio.getNumSamples(), (int) (8.0 * rate));
                mono.assign ((size_t) n, 0.0f);
                for (int c = 0; c < audio.getNumChannels(); ++c)
                    for (int i = 0; i < n; ++i)
                        mono[(size_t) i] += audio.getSample (c, i);
                for (int i = 0; i < n && check < 3; i += 512)
                {
                    const int len = std::min (512, n - i);
                    stream.push (mono.data() + i, len);
                    fed += len / rate;
                    if (fed >= checks[check])
                    {
                        const auto e = stream.estimate();
                        const int root = key % 12;
                        const bool minor = key >= 12;
                        const int relativeRoot = minor ? (root + 3) % 12 : (root + 9) % 12;
                        const bool ok = (e.root == root && e.minor == minor) || (e.root == relativeRoot && e.minor != minor);
                        auto& bn = bins[(size_t) check][(size_t) juce::jlimit (0, 4, (int) (e.confidence * 5.0f))];
                        ++bn.all;
                        bn.right += ok;
                        ++total[(size_t) check];
                        right[(size_t) check] += ok;
                        if (e.confidence >= 0.6f)
                        {
                            ++sureAll[(size_t) check];
                            sureRight[(size_t) check] += ok;
                        }
                        ++check;
                    }
                }
            }
        }
        for (int c = 0; c < 3; ++c)
        {
            std::printf ("tras %2.0f s: %d pistas, tonalidad o relativa %.0f %%; seguras (>=60 %%) %d, de ellas bien %.0f %%\n", checks[c], total[(size_t) c],
                         total[(size_t) c] > 0 ? 100.0 * right[(size_t) c] / total[(size_t) c] : 0.0, sureAll[(size_t) c],
                         sureAll[(size_t) c] > 0 ? 100.0 * sureRight[(size_t) c] / sureAll[(size_t) c] : 0.0);
            for (int b = 0; b < 5; ++b)
                std::printf ("   confianza %3d-%3d %%: %3d, bien %5.1f %%\n", b * 20, b * 20 + 20, bins[(size_t) c][(size_t) b].all,
                             bins[(size_t) c][(size_t) b].all > 0 ? 100.0 * bins[(size_t) c][(size_t) b].right / bins[(size_t) c][(size_t) b].all : 0.0);
        }
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
