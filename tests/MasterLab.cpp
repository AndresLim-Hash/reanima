// The MASTER output stage lab (24-11-2026): renders what enters the output stage, and runs the old stage
// (MasterStageLegacy.h, as it was before) or the new one over the same signal, for the measurements of
// docs/master_valvula_limitador_2026-11-24.md (analysis in research/master_2026-11-24/*.py).
//
//   pre <sample> <out.wav> <rate> <seconds> <notes,csv> [k=v ...]   the processor without its output stage
//   stage <in.wav> <out.wav> <gainDb> legacy|new [k=v ...]          the stage alone, after a gain
#include "PluginProcessor.h"
#include "MasterStageLegacy.h"
#include <cstdio>
#include <map>

using Audio = juce::AudioBuffer<float>;

namespace
{
bool readWav (const juce::File& f, Audio& a, double& rate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr)
        return false;
    rate = r->sampleRate;
    a.setSize (2, (int) r->lengthInSamples);
    r->read (&a, 0, (int) r->lengthInSamples, 0, true, true);
    if (r->numChannels == 1)
        a.copyFrom (1, 0, a, 0, 0, a.getNumSamples());
    return true;
}

void writeWav (const juce::File& f, const Audio& a, double rate)
{
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
    juce::WavAudioFormat wav;
    if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)
                                                  .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)))
        w->writeFromAudioSampleBuffer (a, 0, a.getNumSamples());
}

std::map<juce::String, float> keyValues (int from, int argc, char* argv[])
{
    std::map<juce::String, float> kv;
    for (int i = from; i < argc; ++i)
    {
        const juce::String s (argv[i]);
        if (s.contains ("="))
            kv[s.upToFirstOccurrenceOf ("=", false, false)] = s.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
    }
    return kv;
}

void setParams (ReanimaProcessor& proc, const std::map<juce::String, float>& kv)
{
    for (const auto& [id, v] : kv)
        if (auto* param = proc.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (v));
    proc.serviceMessageThread();
}

int renderPre (int argc, char* argv[])
{
    const juce::File in (juce::String::fromUTF8 (argv[2])), out (juce::String::fromUTF8 (argv[3]));
    const double rate = juce::String (argv[4]).getDoubleValue();
    const double seconds = juce::String (argv[5]).getDoubleValue();
    juce::StringArray notes;
    notes.addTokens (argv[6], ",", "");
    auto kv = keyValues (7, argc, argv);
    const int block = kv.count ("block") ? (int) kv["block"] : 64;
    const float velocity = kv.count ("vel") ? kv["vel"] : 1.0f;
    kv.erase ("block");
    kv.erase ("vel");

    ReanimaProcessor::skipMasterStageForTests = true;
    ReanimaProcessor proc;
    proc.setPlayConfigDetails (0, 2, rate, block);
    proc.prepareToPlay (rate, block);
    proc.loadSampleNow (in);
    for (int k = 0; k < 800 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (10);
        proc.serviceMessageThread();
    }
    proc.serviceMessageThread();
    std::map<juce::String, float> base { { "volume", 0.0f }, { "monoBass", 0.0f } };
    for (const auto& [k, v] : kv)
        base[k] = v;
    setParams (proc, base);

    const int total = (int) (seconds * rate), off = (int) (seconds * 0.75 * rate);
    Audio result (2, total), buf (2, block);
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min (block, total - pos);
        juce::MidiBuffer m;
        for (const auto& note : notes)
        {
            if (pos == 0)
                m.addEvent (juce::MidiMessage::noteOn (1, note.getIntValue(), velocity), 0);
            if (off >= pos && off < pos + n)
                m.addEvent (juce::MidiMessage::noteOff (1, note.getIntValue()), off - pos);
        }
        Audio view (buf.getArrayOfWritePointers(), 2, n);
        view.clear();
        proc.processBlock (view, m);
        result.copyFrom (0, pos, view, 0, 0, n);
        result.copyFrom (1, pos, view, 1, 0, n);
    }
    writeWav (out, result, rate);
    std::printf ("ok %s pico %.2f dB\n", out.getFileName().toRawUTF8(), juce::Decibels::gainToDecibels (result.getMagnitude (0, total), -200.0f));
    return 0;
}

int runStage (int argc, char* argv[])
{
    const juce::File in (juce::String::fromUTF8 (argv[2])), out (juce::String::fromUTF8 (argv[3]));
    const float gain = juce::Decibels::decibelsToGain (juce::String (argv[4]).getFloatValue());
    const juce::String which (argv[5]);
    auto kv = keyValues (6, argc, argv);
    const int block = kv.count ("block") ? (int) kv["block"] : 64;
    Audio a;
    double rate = 0.0;
    if (! readWav (in, a, rate))
        return 1;
    a.applyGain (gain);
    int latency = 0;
    if (which == "legacy")
    {
        MasterStageLegacy stage;
        stage.prepare (rate, 512);
        latency = stage.latency();
        for (int pos = 0; pos < a.getNumSamples(); pos += block)
        {
            const int n = std::min (block, a.getNumSamples() - pos);
            stage.process (a.getWritePointer (0) + pos, a.getWritePointer (1) + pos, n);
        }
    }
    else if (which == "new")
    {
        MasterStage::oversamplingOrder = kv.count ("os") ? (int) kv["os"] : 0;
        if (kv.count ("maxdrive"))
            MasterStage::maxDrive = kv["maxdrive"];
        MasterStage stage;
        stage.prepare (rate, 512);
        MasterStage::Settings s;
        s.valveOn = kv.count ("on") ? kv["on"] > 0.5f : true;
        s.amount = kv.count ("amount") ? kv["amount"] * 0.01f : 0.35f;
        s.character = kv.count ("character") ? kv["character"] * 0.01f : 0.5f;
        s.outputDb = kv.count ("output") ? kv["output"] : 0.0f;
        s.limiterOn = kv.count ("limiter") ? kv["limiter"] > 0.5f : true;
        s.ceilingDb = kv.count ("ceiling") ? kv["ceiling"] : -1.0f;
        stage.setSettings (s);
        latency = stage.latency();
        for (int pos = 0; pos < a.getNumSamples(); pos += block)
        {
            const int n = std::min (block, a.getNumSamples() - pos);
            stage.process (a.getWritePointer (0) + pos, a.getWritePointer (1) + pos, n);
        }
        if (kv.count ("align") && kv["align"] > 0.5f)
        {
            // the delay taken out (the file then lines up with its input)
            Audio aligned (2, a.getNumSamples());
            aligned.clear();
            for (int ch = 0; ch < 2; ++ch)
                aligned.copyFrom (ch, 0, a, ch, latency, a.getNumSamples() - latency);
            a = aligned;
        }
    }
    else
    {
        std::printf ("etapa desconocida\n");
        return 1;
    }
    writeWav (out, a, rate);
    std::printf ("ok latencia %d\n", latency);
    return 0;
}
// bench: the stage's own time per block, as a share of the block's real time, for each rate, block, and setting.
int bench()
{
    std::printf ("frecuencia;bloque;sobremuestreo;ajuste;carga media %%;peor bloque %%\n");
    for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0 })
        for (const int block : { 64, 128 })
        {
            MasterStageLegacy stage;
            stage.prepare (rate, block);
            juce::Random rng (3);
            std::vector<float> l ((size_t) block), r ((size_t) block);
            const int blocks = (int) (20.0 * rate / block);
            double total = 0.0;
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < block; ++i)
                {
                    l[(size_t) i] = 2.0f * (rng.nextFloat() - 0.5f) * 0.5f;
                    r[(size_t) i] = l[(size_t) i] * 0.9f;
                }
                const auto t0 = juce::Time::getHighResolutionTicks();
                stage.process (l.data(), r.data(), block);
                total += juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
            }
            std::printf ("%.1f;%d;%s;etapa ANTERIOR;%.3f;-\n", rate / 1000.0, block, rate < 60000.0 ? "4x" : "2x", 100.0 * total / (blocks * block / rate));
        }
    for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0 })
        for (const int block : { 64, 128 })
            for (const int order : { 0, 3 })
                for (int setting = 0; setting < 3; ++setting)
                {
                    MasterStage::oversamplingOrder = order;
                    MasterStage stage;
                    stage.prepare (rate, block);
                    MasterStage::Settings s;
                    s.valveOn = setting > 0;
                    s.limiterOn = setting != 0 || true;
                    s.amount = 0.6f;
                    stage.setSettings (s);
                    juce::Random rng (3);
                    std::vector<float> l ((size_t) block), r ((size_t) block);
                    const int blocks = (int) (20.0 * rate / block);
                    double total = 0.0, worst = 0.0;
                    for (int b = 0; b < blocks; ++b)
                    {
                        for (int i = 0; i < block; ++i)
                        {
                            l[(size_t) i] = 2.0f * (rng.nextFloat() - 0.5f) * (setting == 2 ? 3.0f : 0.5f);
                            r[(size_t) i] = l[(size_t) i] * 0.9f;
                        }
                        const auto t0 = juce::Time::getHighResolutionTicks();
                        stage.process (l.data(), r.data(), block);
                        const double s1 = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
                        total += s1;
                        if (b > 100)
                            worst = std::max (worst, s1);
                    }
                    const double real = block / rate;
                    std::printf ("%.1f;%d;%s;%s;%.3f;%.2f\n", rate / 1000.0, block, order == 3 ? "8x" : (rate < 60000.0 ? "4x" : "2x"),
                                 setting == 0 ? "valvula apagada, limitador en reposo" : setting == 1 ? "valvula 60 %, limitador en reposo" : "valvula 60 %, limitador -10 dB",
                                 100.0 * total / (blocks * real), 100.0 * worst / real);
                }
    MasterStage::oversamplingOrder = 0;
    return 0;
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 1 && juce::String (argv[1]) == "bench")
        return bench();
    if (argc > 6 && juce::String (argv[1]) == "pre")
        return renderPre (argc, argv);
    if (argc > 5 && juce::String (argv[1]) == "stage")
        return runStage (argc, argv);
    std::printf ("uso: pre <sample> <out.wav> <rate> <s> <notas> [k=v] | stage <in> <out> <gainDb> legacy|new [k=v]\n");
    return 2;
}
