// Measures what happens after the note-off in engine A, over a folder of samples and with
// the envelope changed in several ways: how hard the level falls at first, whether a burst of
// high frequencies appears, and whether the level climbs back up once the key is released.
// Usage: ReanimaRelease.exe <samples folder> <out.csv> [every Nth file] [file to save renders of]

#include "Params.h"
#include "SampleAnalysis.h"
#include "SynthEngine.h"
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

    const auto set = [&p] (int index, float v)
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
    p[P::Engine] = (float) P::EngineRebuild;
    p[P::StartPos] = s.length > 0 ? (float) (s.onset / s.length) : 0.0f;
    return p;
}

juce::AudioBuffer<float> play (std::shared_ptr<const SampleData> s, const ParamSnapshot& p, int note, double held, double total, double sr)
{
    SynthEngine e;
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

double rmsDb (const juce::AudioBuffer<float>& a, int from, int to)
{
    double e = 0.0;
    from = std::max (0, from);
    to = std::min (to, a.getNumSamples());
    for (int i = from; i < to; ++i)
    {
        const double l = a.getSample (0, i), r = a.getSample (1, i);
        e += 0.5 * (l * l + r * r);
    }
    return 10.0 * std::log10 (e / std::max (1, to - from) + 1.0e-14);
}

// Energy of the second difference: what a click adds, whatever the sound underneath.
double edgeDb (const juce::AudioBuffer<float>& a, int from, int to)
{
    double e = 0.0;
    from = std::max (2, from);
    to = std::min (to, a.getNumSamples());
    for (int i = from; i < to; ++i)
        for (int ch = 0; ch < 2; ++ch)
        {
            const double d = (double) a.getSample (ch, i) - 2.0 * a.getSample (ch, i - 1) + a.getSample (ch, i - 2);
            e += d * d;
        }
    return 10.0 * std::log10 (e / std::max (1, to - from) + 1.0e-14);
}

// The same note, repeated: how much does its level differ from one time to the next?
double repeatSpread (std::shared_ptr<const SampleData> s, const ParamSnapshot& p, double from, double sr)
{
    SynthEngine e;
    e.prepare (sr);
    e.setSample (s);
    e.prepareFx (p);

    const int length = (int) (1.2 * sr), off = (int) (0.6 * sr);
    double low = 1.0e9, high = -1.0e9;
    juce::AudioBuffer<float> out (2, length), buf (2, 512);

    for (int note = 0; note < 8; ++note)
    {
        for (int pos = 0; pos < length; pos += 512)
        {
            const int n = std::min (512, length - pos);
            juce::MidiBuffer midi;
            if (pos == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, SynthEngine::rootKey, 1.0f), 0);
            if (off >= pos && off < pos + n)
                midi.addEvent (juce::MidiMessage::noteOff (1, SynthEngine::rootKey), off - pos);
            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, n);
            e.process (view, midi, p, 120.0);
            out.copyFrom (0, pos, buf, 0, 0, n);
            out.copyFrom (1, pos, buf, 1, 0, n);
        }
        const double db = rmsDb (out, (int) (from * sr), (int) ((from + 0.06) * sr));
        low = std::min (low, db);
        high = std::max (high, db);
    }
    return high - low;
}

struct Result { double dropDb, edgeDbJump, reboundDb; };

Result measure (const juce::AudioBuffer<float>& a, double held, double sr)
{
    const int off = (int) (held * sr);
    const int ms = (int) (0.001 * sr);

    // The steepest fall over 5 ms in the 40 ms after the note-off, against the same in the 40 ms before.
    const auto steepest = [&] (int from, int to)
    {
        double worst = 0.0;
        for (int i = from; i + 10 * ms < to; i += ms)
            worst = std::max (worst, rmsDb (a, i, i + 5 * ms) - rmsDb (a, i + 5 * ms, i + 10 * ms));
        return worst;
    };
    const double drop = steepest (off - ms, off + 40 * ms) - steepest (off - 40 * ms, off - ms);

    // A burst of edges around the note-off, against the moment before it.
    const double jump = edgeDb (a, off - ms, off + 3 * ms) - edgeDb (a, off - 24 * ms, off - 4 * ms);

    // Once released the level can only go down: how far does it climb from its lowest point so far?
    // The window is long enough for a sub bass not to look like a level change.
    double lowest = rmsDb (a, off, off + 60 * ms), rebound = 0.0;
    for (int i = off; i + 60 * ms < a.getNumSamples(); i += 10 * ms)
    {
        const double db = rmsDb (a, i, i + 60 * ms);
        if (db < -80.0)
            break;
        lowest = std::min (lowest, db);
        rebound = std::max (rebound, db - lowest);
    }
    return { drop, jump, rebound };
}
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 3)
        return 1;

    const juce::File folder (juce::String::fromUTF8 (argv[1]));
    const int every = argc > 3 ? juce::String (argv[3]).getIntValue() : 8;
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.wav");
    files.sort();

    std::FILE* csv = nullptr;
    if (fopen_s (&csv, argv[2], "w") != 0)
        return 1;
    std::fprintf (csv, "file,seconds,detectedR,setting,held,releaseMs,drop,edge,rebound\n");

    struct Setting { const char* name; float sustain, decayScale, releaseMs; bool hold; };
    const Setting settings[] = {
        { "natural", -1.0f, 1.0f, 0.0f, false },
        { "sus80", 0.8f, 1.0f, 0.0f, false },
        { "sus80_r50", 0.8f, 1.0f, 50.0f, false },
        { "sus80_r300", 0.8f, 1.0f, 300.0f, false },
        { "sus80_r2000", 0.8f, 1.0f, 2000.0f, false },
        { "dec3", -1.0f, 3.0f, 0.0f, false },
        { "dec3_r300", -1.0f, 3.0f, 300.0f, false },
        { "hold", -1.0f, 1.0f, 0.0f, true },
        { "hold_r100", -1.0f, 1.0f, 100.0f, true },
        { "sus10_r300", 0.1f, 1.0f, 300.0f, false },
    };

    for (int index = 0; index < files.size(); index += every)
    {
        const auto& file = files.getReference (index);
        juce::AudioBuffer<float> raw;
        double rate = 0.0;
        if (! SampleAnalysis::loadFile (file, raw, rate))
            continue;

        const auto s = SampleAnalysis::analyse (raw, rate, file.getFileNameWithoutExtension(), false, false);
        const auto base = defaultsFor (*s);
        const double seconds = (double) s->length / rate;

        {
            auto p = base;
            p[P::AmpS] = 0.8f;
            p[P::AmpR] = 100.0f;
            std::fprintf (csv, "%s,%.2f,%.0f,repeat,%.2f,0,%.1f,0,0\n", file.getFileName().replaceCharacter (',', '_').toRawUTF8(), seconds,
                          s->detected.release * 1000.0f, 0.15, repeatSpread (s, p, 0.15, rate));
            std::fprintf (csv, "%s,%.2f,%.0f,repeat,%.2f,0,%.1f,0,0\n", file.getFileName().replaceCharacter (',', '_').toRawUTF8(), seconds,
                          s->detected.release * 1000.0f, 0.45, repeatSpread (s, p, 0.45, rate));
        }

        for (const auto& setting : settings)
        {
            auto p = base;
            if (setting.sustain >= 0.0f)
                p[P::AmpS] = setting.sustain;
            p[P::AmpD] = juce::jmin (30000.0f, p[P::AmpD] * setting.decayScale);
            if (setting.releaseMs > 0.0f)
                p[P::AmpR] = setting.releaseMs;
            if (setting.hold)
                p[P::SustainType] = (float) P::SustainHold;

            for (const double held : { 0.25, 0.8 })
            {
                const double total = std::min (held + (double) p[P::AmpR] * 0.001 + 3.0, seconds + 6.0);
                const auto out = play (s, p, SynthEngine::rootKey, held, total, rate);
                const auto r = measure (out, held, rate);

                if (argc > 4 && file.getFileName().contains (juce::String::fromUTF8 (argv[4])))
                {
                    const auto path = juce::File (juce::String::fromUTF8 (argv[2])).getSiblingFile (juce::String (setting.name) + "_" + juce::String (held) + ".wav");
                    path.deleteFile();
                    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (path);
                    juce::WavAudioFormat wav;
                    if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
                        w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
                }
                std::fprintf (csv, "%s,%.2f,%.0f,%s,%.2f,%.0f,%.1f,%.1f,%.1f\n", file.getFileName().replaceCharacter (',', '_').toRawUTF8(),
                              seconds, s->detected.release * 1000.0f, setting.name, held, (double) p[P::AmpR], r.dropDb, r.edgeDbJump, r.reboundDb);
            }
        }
        std::fflush (csv);
    }

    std::fclose (csv);
    return 0;
}
