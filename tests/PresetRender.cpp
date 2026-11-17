// Renders every factory preset on several samples, the same phrase each time, for the preset audit
// (research/presets/auditar_presets.py): a held middle C, a low C, a high C and a four-note chord, then the
// tail. One WAV per preset and sample, and the audio-thread time it took.
//
// Usage: ReanimaPresetRender <out folder> <sample.wav> [more samples...]

#include "PluginProcessor.h"
#include "FactoryPresets.h"
#include <cstdio>

namespace
{
struct Event { double t; int note; bool on; };

std::vector<Event> phrase()
{
    std::vector<Event> e;
    const auto hold = [&] (std::initializer_list<int> notes, double on, double off)
    {
        for (int n : notes)
        {
            e.push_back ({ on, n, true });
            e.push_back ({ off, n, false });
        }
    };
    hold ({ 60 }, 0.05, 2.05);
    hold ({ 36 }, 2.6, 3.6);
    hold ({ 84 }, 4.1, 5.1);
    hold ({ 60, 64, 67, 71 }, 5.6, 7.1);
    std::sort (e.begin(), e.end(), [] (const Event& a, const Event& b) { return a.t < b.t; });
    return e;
}
} // namespace

int main (int argc, char* argv[])
{
    if (argc < 3)
    {
        std::printf ("usage: ReanimaPresetRender <out folder> <sample.wav> [...]\n");
        return 1;
    }
    juce::ScopedJuceInitialiser_GUI juce;
    ReanimaProcessor::offerDemo = false;
    const double rate = 48000.0;
    const int block = 256;
    const juce::File out (juce::String::fromUTF8 (argv[1]));
    out.createDirectory();
    const auto events = phrase();
    const double seconds = 13.0;

    for (int a = 2; a < argc; ++a)
    {
        const juce::File file (juce::String::fromUTF8 (argv[a]));
        for (int preset = 0; preset < (int) Factory::presets().size(); ++preset)
        {
            // PRESETS=index|index...: only those
            if (const char* only = std::getenv ("PRESETS"))
                if (! juce::StringArray::fromTokens (juce::String::fromUTF8 (only), "|", "").contains (juce::String (preset)))
                    continue;
            ReanimaProcessor proc;
            proc.setPlayConfigDetails (0, 2, rate, block);
            proc.prepareToPlay (rate, block);
            if (! proc.loadSampleNow (file))
            {
                std::printf ("cannot load %s\n", argv[a]);
                break;
            }
            if (std::getenv ("SNAP") != nullptr)
                proc.setSnapPreference (true);      // as a user who turned on Snap a C
            proc.applyFactoryPreset (preset);
            for (int k = 0; k < 600; ++k)
            {
                proc.serviceMessageThread();
                if (! proc.isLoading())
                    break;
                juce::Thread::sleep (20);
            }
            proc.serviceMessageThread();

            const int blocks = (int) (seconds * rate / block);
            juce::AudioBuffer<float> all (2, blocks * block), buf (2, block);
            size_t next = 0;
            double ms = 0.0, worst = 0.0;
            for (int b = 0; b < blocks; ++b)
            {
                const double t0 = (double) b * block / rate, t1 = (double) (b + 1) * block / rate;
                juce::MidiBuffer midi;
                while (next < events.size() && events[next].t < t1)
                {
                    const auto& e = events[next++];
                    const int pos = juce::jlimit (0, block - 1, (int) ((e.t - t0) * rate));
                    midi.addEvent (e.on ? juce::MidiMessage::noteOn (1, e.note, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, e.note), pos);
                }
                buf.clear();
                const double s = juce::Time::getMillisecondCounterHiRes();
                proc.processBlock (buf, midi);
                const double d = juce::Time::getMillisecondCounterHiRes() - s;
                ms += d;
                worst = std::max (worst, d);
                for (int c = 0; c < 2; ++c)
                    all.copyFrom (c, b * block, buf, c, 0, block);
                if (b % 16 == 0)
                    proc.serviceMessageThread();
            }
            const auto name = file.getFileNameWithoutExtension().substring (0, 24) + "__" + juce::String (preset);
            auto wavFile = out.getChildFile (name + ".wav");
            wavFile.deleteFile();
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (wavFile);
            juce::WavAudioFormat wav;
            if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)))
                writer->writeFromAudioSampleBuffer (all, 0, all.getNumSamples());
            std::printf ("%s;%d;%s;%.2f;%.3f\n", wavFile.getFileName().toRawUTF8(), preset, Factory::presets()[(size_t) preset].name,
                         100.0 * ms / (seconds * 1000.0), worst);
            std::fflush (stdout);
        }
    }
    return 0;
}
