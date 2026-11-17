// Renders listening demos of engine A for every WAV in a folder. Every render lasts until the
// sound has died away, and the numbers that used to go unnoticed are printed for each file.
// Usage: ReanimaDemos.exe <samples folder> <output folder>

#include "Params.h"
#include "SampleAnalysis.h"
#include "SynthEngine.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdio>
#include <initializer_list>

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

void writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double sr)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    juce::WavAudioFormat wav;
    if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)))
        w->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

// Holds the notes for `held` seconds and keeps rendering until the sound has gone.
juce::AudioBuffer<float> play (std::shared_ptr<const SampleData> s, const ParamSnapshot& p, std::initializer_list<int> notes, double held, double sr)
{
    SynthEngine e;
    e.prepare (sr);
    e.setSample (s);
    e.prepareFx (p);

    const int heldSamples = (int) (held * sr);
    const int maxSamples = heldSamples + (int) ((p[P::AmpR] * 0.001 * 2.0 + 2.0) * sr);
    juce::AudioBuffer<float> out (2, maxSamples);
    out.clear();
    juce::AudioBuffer<float> buf (2, 512);
    int lastLoud = 0;

    for (int pos = 0; pos < maxSamples; pos += 512)
    {
        const int n = std::min (512, maxSamples - pos);
        buf.setSize (2, n, false, false, true);
        juce::MidiBuffer midi;
        for (const int note : notes)
        {
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);
            if (pos <= heldSamples && heldSamples < pos + n)
                midi.addEvent (juce::MidiMessage::noteOff (1, note), heldSamples - pos);
        }
        e.process (buf, midi, p, 120.0);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, buf, ch, 0, n);
        if (buf.getMagnitude (0, n) > 3.0e-5f)
            lastLoud = pos + n;
    }

    out.setSize (2, std::min (maxSamples, lastLoud + (int) (0.05 * sr)), true);
    return out;
}

void report (const char* name, const juce::AudioBuffer<float>& a, double sr)
{
    const int n = a.getNumSamples();
    double mid = 0.0, side = 0.0, worstJump = 0.0;
    int clipped = 0;
    const float* L = a.getReadPointer (0);
    const float* R = a.getReadPointer (1);
    for (int i = 0; i < n; ++i)
    {
        mid += 0.25 * (double) (L[i] + R[i]) * (L[i] + R[i]);
        side += 0.25 * (double) (L[i] - R[i]) * (L[i] - R[i]);
        clipped += (std::abs (L[i]) >= 0.9999f) + (std::abs (R[i]) >= 0.9999f);
    }

    // Biggest jump of the level between neighbouring 10 ms windows after the first 50 ms.
    const int w = (int) (0.01 * sr);
    double previous = -1.0, worstAt = 0.0;
    for (int s = (int) (0.05 * sr); s + w < n; s += w)
    {
        double e = 0.0;
        for (int i = s; i < s + w; ++i)
            e += (double) L[i] * L[i] + (double) R[i] * R[i];
        const double db = 10.0 * std::log10 (e / w + 1.0e-10);
        if (previous > -60.0 && db > -60.0)
            if (std::abs (db - previous) > worstJump) { worstJump = std::abs (db - previous); worstAt = s / sr; }
        previous = db;
    }

    std::printf ("  %-22s %5.2f s  peak %.3f  clipped %5.3f %%  width %6.1f dB  level jump %4.1f dB at %.2f s\n", name, n / sr, a.getMagnitude (0, n),
                 100.0 * clipped / (2.0 * n), 10.0 * std::log10 ((side + 1.0e-12) / (mid + 1.0e-12)), worstJump, worstAt);
}
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 3)
        return 1;

    const juce::File folder (juce::String::fromUTF8 (argv[1])), out (juce::String::fromUTF8 (argv[2]));
    out.createDirectory();
    const double sr = 48000.0;

    for (const auto& file : folder.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (file, raw, rate))
            continue;

        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const auto s = SampleAnalysis::analyse (raw, rate, file.getFileNameWithoutExtension(), false);
        std::printf ("%s  (load %.0f ms)  A %.0f ms  D %.0f ms  S %.3f  R %.0f ms\n", file.getFileName().toRawUTF8(), juce::Time::getMillisecondCounterHiRes() - t0,
                     s->detected.attack * 1000.0f, s->detected.decay * 1000.0f, s->detected.sustain, s->detected.release * 1000.0f);

        const auto base = defaultsFor (*s);
        const int key = SynthEngine::rootKey;
        const auto stem = juce::File::createLegalFileName (file.getFileNameWithoutExtension().substring (0, 24));

        auto save = [&] (const char* name, const juce::AudioBuffer<float>& y)
        {
            report (name, y, sr);
            writeWav (out.getChildFile (stem + " - " + name + ".wav"), y, sr);
        };

        // Optional third argument: only the sustained case, held that many seconds.
        if (argc > 3)
        {
            auto q = base;
            q[P::AmpS] = 0.7f;
            q[P::AmpR] = 800.0f;
            q[P::SustainType] = (float) P::SustainHold;
            save ("3 sostenido", play (s, q, { key }, juce::String (argv[3]).getDoubleValue(), sr));
            continue;
        }

        save ("1 sin tocar", play (s, base, { key }, s->length / rate, sr));

        {
            auto q = base;
            q[P::AmpA] = 500.0f;
            q[P::AmpCurve] = 1.0f;
            q[P::AmpS] = 0.8f;
            q[P::AmpR] = 700.0f;
            q[P::SustainType] = (float) P::SustainHold;
            save ("2 ataque lento", play (s, q, { key }, 3.0, sr));
        }
        {
            auto q = base;
            q[P::AmpS] = 0.7f;
            q[P::AmpR] = 800.0f;
            q[P::SustainType] = (float) P::SustainHold;
            save ("3 sostenido", play (s, q, { key }, 4.0, sr));
        }
        {
            auto q = base;
            q[P::AmpD] = 3000.0f;
            q[P::AmpS] = 0.0f;
            q[P::AmpR] = 500.0f;
            save ("4 decay largo", play (s, q, { key }, 4.0, sr));
        }
        {
            auto q = base;
            q[P::AmpS] = 0.5f;
            q[P::AmpFade] = 6.0f;
            q[P::AmpR] = 1500.0f;
            save ("5 natural largo", play (s, q, { key }, 3.0, sr));
        }
        {
            auto q = base;
            q[P::AmpR] = 120.0f;
            save ("6 release corto", play (s, q, { key }, 0.25, sr));
        }
        {
            auto q = base;
            q[P::AmpS] = 0.6f;
            q[P::AmpR] = 900.0f;
            q[P::SustainType] = (float) P::SustainHold;
            save ("7 acorde", play (s, q, { key, key + 4, key + 7 }, 3.0, sr));
        }
        {
            auto q = base;
            q[P::AmpS] = 0.7f;
            q[P::AmpR] = 600.0f;
            q[P::SustainType] = (float) P::SustainHold;
            save ("8 una octava arriba", play (s, q, { key + 12 }, 2.0, sr));
            save ("9 una octava abajo", play (s, q, { key - 12 }, 2.0, sr));
        }
    }
    return 0;
}
