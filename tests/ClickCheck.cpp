// Clicks and crackles, measured: every chosen preset on every sample, played with host blocks of 64 and of
// 512 samples (and an odd 37). For each: the slowest block against its real time (a block over its time is
// a crackle in the host), clicks found in the output (a jump in the second difference far above what the
// signal around it does), and how far the 64-sample render differs from the 512 one.
//
// Usage: ReanimaClickCheck <sample.wav> [...]      PRESETS=index|index... (default: all)

#include "PluginProcessor.h"
#include "FactoryPresets.h"
#include <cstdio>

namespace
{
struct Render
{
    juce::AudioBuffer<float> audio;
    double worstMs = 0.0, budgetMs = 0.0;
    int over = 0;
};

Render render (const juce::File& file, int preset, int block, double rate)
{
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, rate, block);
    proc.prepareToPlay (rate, block);
    proc.loadSampleNow (file);
    proc.applyFactoryPreset (preset);
    // SET=id=value|id=value: parameters changed after the preset (to find what a click depends on)
    if (const char* set = std::getenv ("SET"))
        for (const auto& kv : juce::StringArray::fromTokens (set, "|", ""))
            if (auto* param = proc.apvts.getParameter (kv.upToFirstOccurrenceOf ("=", false, false)))
                param->setValueNotifyingHost (param->convertTo0to1 (kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
    for (int k = 0; k < 600; ++k)
    {
        proc.serviceMessageThread();
        if (! proc.isLoading())
            break;
        juce::Thread::sleep (20);
    }
    proc.serviceMessageThread();

    // Notes that start, overlap, steal and end, the way the captures were played.
    struct Ev { double t; int note; bool on; };
    std::vector<Ev> ev;
    const auto hold = [&] (int n, double a, double b) { ev.push_back ({ a, n, true }); ev.push_back ({ b, n, false }); };
    hold (60, 0.05, 1.2);
    hold (67, 0.6, 1.4);
    hold (48, 1.5, 3.5);
    hold (72, 1.6, 1.7);
    hold (72, 1.75, 1.85);
    hold (64, 2.0, 4.0);
    // NOTES=note:on:off|...: only these
    if (const char* notes = std::getenv ("NOTES"))
    {
        ev.clear();
        for (const auto& n : juce::StringArray::fromTokens (notes, "|", ""))
        {
            const auto f = juce::StringArray::fromTokens (n, ":", "");
            hold (f[0].getIntValue(), f[1].getDoubleValue(), f[2].getDoubleValue());
        }
    }
    std::sort (ev.begin(), ev.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });

    const double seconds = 7.0;
    const int total = (int) (seconds * rate);
    Render r;
    r.audio.setSize (2, total);
    r.budgetMs = 1000.0 * block / rate;
    juce::AudioBuffer<float> buf (2, block);
    size_t next = 0;
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min (block, total - pos);
        buf.setSize (2, n, false, false, true);
        juce::MidiBuffer midi;
        while (next < ev.size() && ev[next].t * rate < pos + n)
        {
            midi.addEvent (ev[next].on ? juce::MidiMessage::noteOn (1, ev[next].note, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, ev[next].note),
                           juce::jlimit (0, n - 1, (int) (ev[next].t * rate) - pos));
            ++next;
        }
        buf.clear();
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        proc.processBlock (buf, midi);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        r.worstMs = std::max (r.worstMs, ms);
        r.over += ms > r.budgetMs ? 1 : 0;
        for (int c = 0; c < 2; ++c)
            r.audio.copyFrom (c, pos, buf, c, 0, n);
        if ((pos / block) % 64 == 0)
            proc.serviceMessageThread();
    }
    return r;
}

// Clicks: |second difference| above 8 times its own RMS over the surrounding 10 ms, and above -60 dBFS.
std::vector<double> clicks (const juce::AudioBuffer<float>& a, double rate)
{
    std::vector<double> at;
    const int n = a.getNumSamples(), w = (int) (0.01 * rate);
    const float floor = 0.004f * std::max (1.0e-6f, a.getMagnitude (0, n));      // -48 dB under the render's peak
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
            // ...and at least 5 % of the signal around it: a smooth low wave has a small second difference
            // that can still stand out from a smoother neighbourhood without being a click.
            float local = 0.0f;
            for (int k = i - 64; k <= i + 64; k += 4)
                local = std::max (local, std::abs (x[juce::jlimit (0, n - 1, k)]));
            if (std::abs (d2[(size_t) i]) > std::max (0.001f, floor) && std::abs (d2[(size_t) i]) > 8.0 * around + 1.0e-6
                && std::abs (d2[(size_t) i]) > 0.05f * local)
            {
                const double t = i / rate;
                if (at.empty() || t - at.back() > 0.02)
                    at.push_back (t);
            }
        }
    }
    std::sort (at.begin(), at.end());
    return at;
}
} // namespace

int main (int argc, char* argv[])
{
    if (argc < 2)
    {
        std::printf ("usage: ReanimaClickCheck <sample.wav> [...]\n");
        return 1;
    }
    juce::ScopedJuceInitialiser_GUI juce;
    ReanimaProcessor::offerDemo = false;
    const double rate = 44100.0;
    juce::StringArray only;
    if (const char* s = std::getenv ("PRESETS"))
        only = juce::StringArray::fromTokens (s, "|", "");
    int problems = 0;
    for (int a = 1; a < argc; ++a)
    {
        const juce::File file (juce::String::fromUTF8 (argv[a]));
        for (int preset = 0; preset < (int) Factory::presets().size(); ++preset)
        {
            if (! only.isEmpty() && ! only.contains (juce::String (preset)))
                continue;
            const auto small = render (file, preset, 64, rate);
            const auto large = render (file, preset, 512, rate);
            const auto odd = render (file, preset, 37, rate);
            const auto c64 = clicks (small.audio, rate), c512 = clicks (large.audio, rate);
            float diff = 0.0f, peak = 0.0f;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < small.audio.getNumSamples(); ++i)
                {
                    diff = std::max (diff, std::abs (small.audio.getSample (c, i) - large.audio.getSample (c, i)));
                    peak = std::max (peak, std::abs (large.audio.getSample (c, i)));
                }
            if (const char* dir = std::getenv ("CLICKDUMP"))
            {
                juce::File out (juce::File (juce::String::fromUTF8 (dir)).getChildFile (juce::String (preset) + "_" + file.getFileNameWithoutExtension().substring (0, 12) + "_64.wav"));
                out.deleteFile();
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (out);
                juce::WavAudioFormat wav;
                if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
                    w->writeFromAudioSampleBuffer (small.audio, 0, small.audio.getNumSamples());
            }
            juce::String where;
            for (size_t k = 0; k < std::min<size_t> (c64.size(), 6); ++k)
                where << juce::String (c64[k], 3) << " ";
            const bool bad = small.over > 0 || ! c64.empty() || ! c512.empty() || odd.over > 0;
            problems += bad ? 1 : 0;
            std::printf ("%s;%s;%d;64: peor %.2f/%.2f ms, %d tarde;37: %d tarde;clicks 64: %d [%s];clicks 512: %d;dif 64-512 %.1f dB\n",
                         file.getFileNameWithoutExtension().substring (0, 22).toRawUTF8(), Factory::presets()[(size_t) preset].name, preset,
                         small.worstMs, small.budgetMs, small.over, odd.over, (int) c64.size(), where.toRawUTF8(), (int) c512.size(),
                         juce::Decibels::gainToDecibels (diff / std::max (1.0e-9f, peak), -200.0f));
            std::fflush (stdout);
        }
    }
    std::printf ("%d con problemas\n", problems);
    return 0;
}
