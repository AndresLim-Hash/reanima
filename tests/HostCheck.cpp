// Loads the built .vst3 the way a host does and checks what only shows from outside: the
// plugin as seen through the VST3 interface, at every sample rate and block size.
// Usage: ReanimaHostCheck <path to Reanima.vst3>
//        ReanimaHostCheck <path to Reanima.vst3> resistencia <minutes of audio> <list of samples.txt>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <map>
#include <thread>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
 #pragma comment (lib, "psapi.lib")
#endif

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("[%s] %s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8(), detail.toRawUTF8());
    failures += ok ? 0 : 1;
}

void wait (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

std::unique_ptr<juce::AudioPluginInstance> open (juce::VST3PluginFormat& format, const juce::PluginDescription& d, double rate, int block)
{
    juce::String error;
    auto p = format.createInstanceFromDescription (d, rate, block, error);
    if (p == nullptr)
        check (false, "abrir el plugin", error);
    return p;
}

// Plays a chord and returns the peak; false if anything is not a finite number.
bool play (juce::AudioPluginInstance& p, int block, double seconds, double rate, float& peak)
{
    juce::AudioBuffer<float> buffer (std::max (2, p.getTotalNumOutputChannels()), block);
    bool finite = true;
    const int blocks = (int) (seconds * rate / block);
    for (int b = 0; b < blocks; ++b)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        if (b == 0)
            for (const int note : { 48, 55, 60, 64 })
                midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
        if (b == blocks / 2)
            for (const int note : { 48, 55, 60, 64 })
                midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        p.processBlock (buffer, midi);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < block; ++i)
            {
                const float v = buffer.getSample (ch, i);
                finite = finite && std::isfinite (v);
                peak = std::max (peak, std::abs (v));
            }
    }
    return finite;
}

// Memory the process holds, in MB (Windows only; 0 elsewhere).
double memoryMb()
{
   #if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS counters {};
    if (GetProcessMemoryInfo (GetCurrentProcess(), &counters, sizeof (counters)))
        return (double) counters.WorkingSetSize / (1024.0 * 1024.0);
   #endif
    return 0.0;
}

// A long session through the VST3 interface: samples of every family loaded through the saved
// state, every engine, knobs moved at random, notes, pitch bend, aftertouch and controllers, block
// sizes and rates changed, the state saved and reopened, the window opened and closed. It reports
// what a long night in a studio would show: memory that grows, a block that takes too long,
// a number that is not a number.
int soak (juce::VST3PluginFormat& format, const juce::PluginDescription& desc, double minutes, const juce::File& list)
{
    juce::StringArray samples;
    samples.addLines (list.loadFileAsString());
    samples.removeEmptyStrings();
    if (samples.isEmpty())
        return 2;

    juce::String error;
    auto p = format.createInstanceFromDescription (desc, 48000.0, 256, error);
    if (p == nullptr)
        return 1;

    juce::Array<juce::AudioProcessorParameter*> params;
    juce::AudioProcessorParameter* engine = nullptr;
    for (auto* param : p->getParameters())
    {
        const auto name = param->getName (32);
        if (name.startsWith ("MIDI CC") || name.containsIgnoreCase ("Volumen") || name == "Mono")
            continue;
        if (name == "Motor")
            engine = param;
        params.add (param);
    }

    // The blocks are played as a host plays them: on a thread above everything else, so that
    // what is timed is the plugin and not the scheduler.
   #if JUCE_WINDOWS
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
   #endif

    juce::Random rng (2026);
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    const int blocks[] = { 32, 64, 128, 256, 512, 1024 };
    double audioSeconds = 0.0, worstRatio = 0.0, worstAt = 0.0, worstRate = 0.0;
    int worstBlock = 0, worstEngine = 0, slowShown = 0;
    std::map<int, double> worstByBlock;     // the slowest block, as a share of its time, per block size
    long nonFinite = 0, rounds = 0, loads = 0;
    float loudest = 0.0f;
    double memStart = 0.0, memMax = 0.0;
    std::vector<double> memPerPass;         // memory each time the whole list has been loaded
    const auto t0 = juce::Time::getMillisecondCounterHiRes();

    while (audioSeconds < minutes * 60.0)
    {
        const double rate = rates[rng.nextInt (3)];
        const int block = blocks[rng.nextInt (6)];
        p->releaseResources();
        p->setRateAndBufferSizeDetails (rate, block);
        p->prepareToPlay (rate, block);

        // A new sample every few rounds, through the state as a project would bring it.
        if (rounds % 4 == 0)
        {
            juce::MemoryBlock state;
            p->getStateInformation (state);
            if (auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize()))
            {
                // The list in order, again and again: what the first pass holds, later passes
                // must not add to. Growth from one pass to the next is a leak.
                xml->setAttribute ("samplePath", samples[(int) (loads % samples.size())].trim());
                for (const auto* id : { "sampleAudio", "sampleName", "sampleEdited" })
                    xml->removeAttribute (id);
                juce::MemoryBlock next;
                juce::AudioProcessor::copyXmlToBinary (*xml, next);
                p->setStateInformation (next.getData(), (int) next.getSize());
                wait (600);
                ++loads;
            }
        }

        const auto warpText = [&]
        {
            for (auto* q : params)
                if (q->getName (24) == "Warp")
                    return q->getCurrentValueAsText();
            return juce::String ("?");
        };
        // Every engine, and a handful of knobs moved anywhere in their range.
        if (engine != nullptr)
            engine->setValueNotifyingHost ((float) rng.nextInt (6) / 5.0f);
        juce::String moved;
        for (int k = 0; k < 12; ++k)
        {
            auto* param = params[rng.nextInt (params.size())];
            param->setValueNotifyingHost (rng.nextFloat());
            moved << param->getName (24) << "=" << param->getCurrentValueAsText() << "  ";
        }
        // REANIMA_SIN_WARP: the same session with Warp held at Re-Pitch, to compare.
        if (std::getenv ("REANIMA_SIN_WARP") != nullptr)
            for (auto* q : params)
                if (q->getName (24) == "Warp")
                    q->setValueNotifyingHost (0.0f);
        wait (30);

        // Ten seconds of playing.
        juce::AudioBuffer<float> buffer (2, block);
        const int count = (int) (10.0 * rate / block);
        for (int b = 0; b < count; ++b)
        {
            juce::MidiBuffer midi;
            if (rng.nextInt (8) == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 36 + rng.nextInt (48), 0.2f + 0.8f * rng.nextFloat()), rng.nextInt (block));
            if (rng.nextInt (10) == 0)
                midi.addEvent (juce::MidiMessage::noteOff (1, 36 + rng.nextInt (48)), rng.nextInt (block));
            if (rng.nextInt (40) == 0)
                midi.addEvent (juce::MidiMessage::pitchWheel (1, rng.nextInt (16384)), 0);
            if (rng.nextInt (40) == 0)
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, rng.nextInt (128)), 0);
            if (rng.nextInt (40) == 0)
                midi.addEvent (juce::MidiMessage::channelPressureChange (1, rng.nextInt (128)), 0);
            if (rng.nextInt (600) == 0)
                midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);

            buffer.clear();
            const auto s0 = juce::Time::getMillisecondCounterHiRes();
            p->processBlock (buffer, midi);
            const double ratio = (juce::Time::getMillisecondCounterHiRes() - s0) / (block / rate * 1000.0);
            if (ratio > worstRatio)
            {
                if (ratio > 0.3)
                    std::printf ("  peor bloque hasta ahora %.0f %%: ronda %ld, %d muestras a %.1f kHz, motor %c, Warp %s\n      %s\n", ratio * 100.0, rounds, block,
                                 rate / 1000.0, (char) ('A' + (engine != nullptr ? juce::roundToInt (engine->getValue() * 5.0f) : 0)),
                                 warpText().toRawUTF8(), moved.toRawUTF8());
                worstRatio = ratio;
                worstAt = audioSeconds + b * block / rate;
                worstRate = rate;
                worstBlock = block;
                worstEngine = engine != nullptr ? juce::roundToInt (engine->getValue() * 5.0f) : 0;
            }
            worstByBlock[block] = std::max (worstByBlock[block], ratio);
            // Every block that takes more than its time, with what was going on.
            if (ratio > 1.0 && slowShown < 40)
            {
                ++slowShown;
                std::printf ("  LENTO %.0f %%: ronda %ld, bloque %d de %d (%d muestras, %.1f kHz), motor %c, %d eventos MIDI, sample %s\n      %s\n",
                             ratio * 100.0, rounds, b, count, block, rate / 1000.0,
                             (char) ('A' + (engine != nullptr ? juce::roundToInt (engine->getValue() * 5.0f) : 0)), midi.getNumEvents(),
                             samples[(int) ((loads + samples.size() - 1) % samples.size())].trim().fromLastOccurrenceOf ("\\", false, false).toRawUTF8(),
                             moved.toRawUTF8());
            }
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < block; ++i)
                {
                    const float v = buffer.getSample (ch, i);
                    if (! std::isfinite (v))
                        ++nonFinite;
                    else
                    {
                        if (std::abs (v) > loudest && std::abs (v) > 0.4f)
                        {
                            std::printf ("  pico %.1f dBFS: ronda %ld, motor %c, Warp %s, %.1f kHz\n      %s\n", juce::Decibels::gainToDecibels (std::abs (v)), rounds,
                                         (char) ('A' + (engine != nullptr ? juce::roundToInt (engine->getValue() * 5.0f) : 0)), warpText().toRawUTF8(), rate / 1000.0, moved.toRawUTF8());
                            loudest = std::abs (v) * 1.12f;   // shown again only 1 dB higher
                        }
                        loudest = std::max (loudest, std::abs (v));
                    }
                }
        }
        audioSeconds += count * block / rate;
        ++rounds;

        // Now and then: the project saved and reopened, the window opened and closed.
        if (rounds % 5 == 0)
        {
            juce::MemoryBlock state;
            p->getStateInformation (state);
            p->setStateInformation (state.getData(), (int) state.getSize());
            wait (300);
        }
        if (rounds % 15 == 0)
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditorAndMakeActive());
            wait (100);
            editor.reset();
        }

        const double mem = memoryMb();
        if (rounds == 1)
            memStart = mem;
        memMax = std::max (memMax, mem);
        if (rounds % 4 == 3 && loads > 0 && loads % samples.size() == 0)
        {
            memPerPass.push_back (mem);
            std::printf ("  vuelta %d a la lista completa: %.0f MB\n", (int) memPerPass.size(), mem);
        }
        if (rounds % 30 == 0)
            std::printf ("  %.0f min de audio, %.0f MB, peor bloque %.0f %% de su tiempo\n", audioSeconds / 60.0, mem, worstRatio * 100.0);
    }

    const double memEnd = memoryMb();
    std::printf ("%.0f minutos de audio en %.1f minutos reales, %ld rondas, %ld samples cargados\n", audioSeconds / 60.0,
                 (juce::Time::getMillisecondCounterHiRes() - t0) / 60000.0, rounds, loads);
    check (nonFinite == 0, "ningún valor no válido en toda la sesión", juce::String (nonFinite));
    check (worstRatio < 1.0, "ningún bloque tarda más que el audio que produce",
           juce::String (worstRatio * 100.0, 0) + " % en el peor: bloque de " + juce::String (worstBlock) + " a " + juce::String (worstRate / 1000.0, 1)
           + " kHz, motor " + juce::String::charToString ((juce::juce_wchar) ('A' + worstEngine)) + ", a " + juce::String (worstAt / 60.0, 1) + " min");
    {
        juce::String perBlock;
        for (const auto& [size, ratio] : worstByBlock)
            perBlock << size << ": " << juce::String (ratio * 100.0, 0) << " %   ";
        std::printf ("peor bloque por tamaño: %s\n", perBlock.toRawUTF8());
    }
    if (memPerPass.size() >= 2)
        // The first passes create what is made on first use (effects, the drone buffers); after
        // that, a pass over the same samples must not add more.
        check (memPerPass.back() < memPerPass[memPerPass.size() - 2] + 10.0, "la memoria no crece de una vuelta a otra",
               juce::String (memStart, 0) + " MB al empezar; tras cada vuelta: " + [&] { juce::String t; for (auto m : memPerPass) t << juce::String (m, 0) << " "; return t; }()
               + "MB; máximo " + juce::String (memMax, 0) + " MB, al final " + juce::String (memEnd, 0) + " MB");
    else
        std::printf ("memoria: %.0f MB al empezar, %.0f al final (haz más minutos para comparar vueltas)\n", memStart, memEnd);
    std::printf ("pico más alto: %.1f dBFS\n", juce::Decibels::gainToDecibels (loudest));
    return failures == 0 ? 0 : 1;
}

// SLICE through the VST3 interface, as Ableton plays it: a bar at 120 BPM of 16 sixteenths, each a tone of its
// own (whole tones from 200 Hz, so the slice that sounds is told by its pitch). The loop and the settings come
// in through the state, as a project brings them; the notes are MIDI numbers (C3 = 60 in Ableton). Prints the
// table slice -> key -> what sounded.
double toneAt (const juce::AudioBuffer<float>& y, int from, int n, double hz, double rate)
{
    double re = 0.0, im = 0.0;
    n = std::min (n, y.getNumSamples() - from);
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / n);
        const double a = 2.0 * juce::MathConstants<double>::pi * hz * i / rate;
        re += y.getSample (0, from + i) * w * std::cos (a);
        im += y.getSample (0, from + i) * w * std::sin (a);
    }
    return std::sqrt (re * re + im * im) / std::max (1, n);
}

// The plug-in's own state inside what a VST3 host keeps (VST3PluginState > IComponent, base64), and back.
std::unique_ptr<juce::XmlElement> pluginXml (const juce::MemoryBlock& hostState)
{
    auto outer = juce::AudioProcessor::getXmlFromBinary (hostState.getData(), (int) hostState.getSize());
    if (outer == nullptr)
        outer = juce::parseXML (hostState.toString());
    if (outer == nullptr)
        return nullptr;
    if (auto* comp = outer->getChildByName ("IComponent"))
    {
        juce::MemoryBlock inner;
        inner.fromBase64Encoding (comp->getAllSubText().trim());
        return juce::AudioProcessor::getXmlFromBinary (inner.getData(), (int) inner.getSize());
    }
    return outer;
}

juce::MemoryBlock hostStateWith (const juce::MemoryBlock& hostState, const juce::XmlElement& plugin)
{
    juce::MemoryBlock inner;
    juce::AudioProcessor::copyXmlToBinary (plugin, inner);
    auto outer = juce::AudioProcessor::getXmlFromBinary (hostState.getData(), (int) hostState.getSize());
    if (outer == nullptr)
        outer = juce::parseXML (hostState.toString());
    if (outer == nullptr || outer->getChildByName ("IComponent") == nullptr)
        return inner;
    auto* comp = outer->getChildByName ("IComponent");
    comp->deleteAllTextElements();
    comp->addTextElement (inner.toBase64Encoding());
    juce::MemoryBlock out;
    juce::AudioProcessor::copyXmlToBinary (*outer, out);
    return out;
}

int sliceTest (juce::VST3PluginFormat& format, const juce::PluginDescription& desc)
{
    const double rate = 48000.0;
    constexpr int count = 16;
    const int seg = (int) (0.125 * rate), total = count * seg;
    double freqs[count];
    for (int k = 0; k < count; ++k)
        freqs[k] = 200.0 * std::pow (2.0, 2.0 * k / 12.0);
    juce::AudioBuffer<float> loop (2, total);
    for (int k = 0; k < count; ++k)
        for (int i = 0; i < seg; ++i)
        {
            const double t = i / rate;
            const float v = (float) (0.5 * std::exp (-t / 0.06) * std::sin (2.0 * juce::MathConstants<double>::pi * freqs[k] * t));
            loop.setSample (0, k * seg + i, v);
            loop.setSample (1, k * seg + i, v);
        }
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Slice 16 tonos 120bpm.wav");
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24)))
            w->writeFromAudioSampleBuffer (loop, 0, total);
    }
    const int block = 64;
    double busy = 0.0, worst = 0.0, played = 0.0;
    auto p = open (format, desc, rate, block);
    if (p == nullptr)
        return 1;
    p->setPlayConfigDetails (0, 2, rate, block);
    p->prepareToPlay (rate, block);

    // The project: this loop, LOOP, Slice, Rejilla 1/16, Poly.
    const auto load = [&] (juce::AudioPluginInstance& q, const std::map<juce::String, float>& values)
    {
        juce::MemoryBlock state;
        q.getStateInformation (state);
        auto xml = pluginXml (state);
        if (xml == nullptr)
            return false;
        xml->setAttribute ("samplePath", file.getFullPathName());
        for (const auto* id : { "sampleAudio", "sampleName", "sampleEdited" })
            xml->removeAttribute (id);
        for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
            if (auto it = values.find (e->getStringAttribute ("id")); it != values.end())
                e->setAttribute ("value", it->second);
        const auto next = hostStateWith (state, *xml);
        q.setStateInformation (next.getData(), (int) next.getSize());
        wait (1500);
        return true;
    };
    const std::map<juce::String, float> sliceSetup { { "playMode", 1.0f }, { "loopKind", 1.0f }, { "sliceBy", 1.0f }, { "sliceDivision", 4.0f },
                                                     { "sliceStart", 60.0f }, { "polyMode", 0.0f }, { "glide", 0.0f }, { "monoBass", 0.0f } };
    load (*p, sliceSetup);
    {
        juce::MemoryBlock st;
        p->getStateInformation (st);
        if (auto x = pluginXml (st))
        {
            juce::String v;
            for (auto* e : x->getChildWithTagNameIterator ("PARAM"))
                if (sliceSetup.count (e->getStringAttribute ("id")))
                    v << e->getStringAttribute ("id") << "=" << e->getStringAttribute ("value") << " ";
            std::printf ("  raiz %s hijos %d\n", x->getTagName().toRawUTF8(), x->getNumChildElements());
            for (auto* c : x->getChildIterator())
                std::printf ("   hijo %s\n", c->getTagName().toRawUTF8());
            std::printf ("  estado: sample %s | %s | nombre %s\n", x->getStringAttribute ("samplePath").toRawUTF8(), v.toRawUTF8(),
                         x->getStringAttribute ("sampleName").toRawUTF8());
        }
    }

    struct Note { int key; double at, len; };
    const auto play = [&] (juce::AudioPluginInstance& q, const std::vector<Note>& notes, double seconds)
    {
        const int length = (int) (seconds * rate);
        juce::AudioBuffer<float> out (2, length), buf (std::max (2, q.getTotalNumOutputChannels()), block);
        out.clear();
        for (int pos = 0; pos < length; pos += block)
        {
            juce::MidiBuffer midi;
            for (const auto& nt : notes)
            {
                const int on = (int) (nt.at * rate), off = (int) ((nt.at + nt.len) * rate);
                if (on >= pos && on < pos + block)
                    midi.addEvent (juce::MidiMessage::noteOn (1, nt.key, 0.9f), on - pos);
                if (off >= pos && off < pos + block)
                    midi.addEvent (juce::MidiMessage::noteOff (1, nt.key), off - pos);
            }
            buf.clear();
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            q.processBlock (buf, midi);
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            busy += ms;
            played += 1000.0 * block / rate;
            worst = std::max (worst, ms / (1000.0 * block / rate));
            const int k = std::min (block, length - pos);
            out.copyFrom (0, pos, buf, 0, 0, k);
            out.copyFrom (1, pos, buf, 1, 0, k);
            if (pos % (block * 64) == 0)
                wait (1);     // the message thread runs as in a host
        }
        return out;
    };
    // The strongest of the 16 tones in a stretch, and by how much over the next.
    const auto which = [&] (const juce::AudioBuffer<float>& y, int from, int n, double& margin)
    {
        double best = 0.0, second = 0.0;
        int at = -1;
        for (int k = 0; k < count; ++k)
        {
            const double a = toneAt (y, from, n, freqs[k], rate);
            if (a > best) { second = best; best = a; at = k; }
            else second = std::max (second, a);
        }
        margin = juce::Decibels::gainToDecibels (best / std::max (1.0e-12, second));
        return best > 1.0e-5 ? at : -1;
    };
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto name = [] (int key) { return juce::String (names[key % 12]) + juce::String (key / 12 - 2); };

    // 1. Each key alone (a tap), 0.3 s apart, C3 to D#4, then E4 (no slice).
    std::vector<Note> taps;
    for (int k = 0; k <= count; ++k)
        taps.push_back ({ 60 + k, 0.1 + 0.3 * k, 0.03 });
    const auto y = play (*p, taps, 0.1 + 0.3 * (count + 1) + 0.2);
    std::printf ("  trozo  tecla   MIDI  inicio-fin (s)   suena   margen\n");
    int right = 0;
    std::vector<int> heard;
    for (int k = 0; k < count; ++k)
    {
        double margin = 0.0;
        const int found = which (y, (int) ((0.1 + 0.3 * k + 0.005) * rate), (int) (0.1 * rate), margin);
        heard.push_back (found);
        right += found == k && margin > 20.0 ? 1 : 0;
        std::printf ("  %5d  %-6s  %4d  %.3f-%.3f     %5d   %5.1f dB\n", k + 1, name (60 + k).toRawUTF8(), 60 + k, k * 0.125, (k + 1) * 0.125, found + 1, margin);
    }
    std::vector<int> sorted (heard);
    std::sort (sorted.begin(), sorted.end());
    const bool distinct = std::unique (sorted.begin(), sorted.end()) == sorted.end();
    double tail = 0.0;
    for (int i = (int) ((0.1 + 0.3 * count) * rate); i < y.getNumSamples(); ++i)
        tail = std::max (tail, (double) std::abs (y.getSample (0, i)));
    check (right == count && distinct, "VST3 SLICE: las 16 teclas C3..D#4 tocan 16 trozos distintos, cada una el suyo", juce::String (right) + "/16");
    check (tail < 1.0e-4, "VST3 SLICE: E4 (fuera de rango) no suena (no cae al primer trozo)", "pico " + juce::String (tail, 6));

    // 2. Each slice ends at its end: from 0.135 s after a key to the next key, silence.
    bool ends = true;
    for (int k = 0; k < count; ++k)
    {
        double m = 0.0;
        for (int i = (int) ((0.1 + 0.3 * k + 0.135) * rate); i < (int) ((0.1 + 0.3 * (k + 1) - 0.002) * rate); ++i)
            m = std::max (m, (double) std::abs (y.getSample (0, i)));
        ends = ends && m < 1.0e-4;
    }
    check (ends, "VST3 SLICE: cada trozo acaba en su final (Note Off no lo corta ni lo alarga)");

    // 3. A chord: C3, E3, G3, C4 held together: four slices sound at once.
    {
        const auto z = play (*p, { { 60, 0.05, 0.3 }, { 64, 0.05, 0.3 }, { 67, 0.05, 0.3 }, { 72, 0.05, 0.3 } }, 0.5);
        int present = 0;
        for (const int k : { 0, 4, 7, 12 })
            present += toneAt (z, (int) (0.06 * rate), (int) (0.08 * rate), freqs[k], rate) > 0.01 ? 1 : 0;
        check (present == 4, "VST3 SLICE: polifonía, C3+E3+G3+C4 a la vez suenan los trozos 1, 5, 8 y 13", juce::String (present) + "/4");
    }

    // 4. A fast run, a key every 40 ms, held 80 ms (overlapping), up the 16 and down.
    {
        std::vector<int> order;
        for (int k = 0; k < count; ++k) order.push_back (k);
        for (int k = count - 2; k >= 0; --k) order.push_back (k);
        std::vector<Note> run;
        for (size_t i = 0; i < order.size(); ++i)
            run.push_back ({ 60 + order[i], 0.05 + 0.04 * (double) i, 0.08 });
        const auto z = play (*p, run, 0.05 + 0.04 * (double) order.size() + 0.3);
        int ok = 0;
        juce::String seq;
        for (size_t i = 0; i < order.size(); ++i)
        {
            // The new slice's tone against what the one before still plays: its onset, 3-28 ms.
            double margin = 0.0;
            const int from = (int) ((0.05 + 0.04 * (double) i + 0.003) * rate);
            const double mine = toneAt (z, from, (int) (0.025 * rate), freqs[order[i]], rate);
            const double prev = i > 0 ? toneAt (z, from, (int) (0.025 * rate), freqs[order[i - 1]], rate) : 0.0;
            (void) margin;
            ok += mine > 0.01 && (i == 0 || mine > prev * 0.5) ? 1 : 0;
            seq << (mine > 0.01 ? "+" : "-");
        }
        check (ok == (int) order.size(), "VST3 SLICE: secuencia rápida (una tecla cada 40 ms, solapadas) arranca el trozo de cada tecla",
               juce::String (ok) + "/" + juce::String ((int) order.size()) + " " + seq);
    }

    // 5. Saved and opened again in a new instance: the same mapping.
    {
        juce::MemoryBlock state;
        p->getStateInformation (state);
        auto q = open (format, desc, rate, block);
        q->setPlayConfigDetails (0, 2, rate, block);
        q->prepareToPlay (rate, block);
        q->setStateInformation (state.getData(), (int) state.getSize());
        wait (1500);
        std::vector<Note> some;
        for (const int k : { 0, 3, 9, 15 })
            some.push_back ({ 60 + k, 0.1 + 0.3 * (double) some.size(), 0.03 });
        const auto z = play (*q, some, 1.4);
        int ok = 0;
        juce::String got;
        int i = 0;
        for (const int k : { 0, 3, 9, 15 })
        {
            double m = 0.0;
            const int found = which (z, (int) ((0.1 + 0.3 * i++ + 0.005) * rate), (int) (0.1 * rate), m);
            got << name (60 + k) << "=" << (found + 1) << " ";
            ok += found == k ? 1 : 0;
        }
        check (ok == 4, "VST3 SLICE: al reabrir el proyecto, la misma tecla toca el mismo trozo", got);
        q->releaseResources();
    }

    // 6. Regenerated: Rejilla 1/8 (8 slices of two tones each): C3..G3 start at tones 1, 3, 5 ... 15.
    {
        auto values = sliceSetup;
        values["sliceDivision"] = 3.0f;
        load (*p, values);
        std::vector<Note> eight;
        for (int k = 0; k < 9; ++k)
            eight.push_back ({ 60 + k, 0.1 + 0.3 * k, 0.03 });
        const auto z = play (*p, eight, 0.1 + 0.3 * 9 + 0.2);
        int ok = 0;
        juce::String got;
        for (int k = 0; k < 8; ++k)
        {
            double m = 0.0;
            const int found = which (z, (int) ((0.1 + 0.3 * k + 0.005) * rate), (int) (0.08 * rate), m);
            got << (found + 1) << " ";
            ok += found == 2 * k ? 1 : 0;
        }
        double tail = 0.0;
        for (int i = (int) ((0.1 + 0.3 * 8) * rate); i < z.getNumSamples(); ++i)
            tail = std::max (tail, (double) std::abs (z.getSample (0, i)));
        check (ok == 8 && tail < 1.0e-4, "VST3 SLICE: regenerado a 1/8, C3..G3 = trozos que empiezan en los tonos 1,3,..,15; G#3 calla", got);
    }
    p->releaseResources();
    file.deleteFile();
    check (worst < 1.0, "VST3 SLICE a 64 muestras: ningún bloque tarda más que el audio que produce",
           "CPU media " + juce::String (100.0 * busy / played, 2) + " % de un núcleo, peor bloque " + juce::String (100.0 * worst, 0) + " %");
    std::printf ("\n%s\n", failures == 0 ? "SLICE VST3 OK" : "SLICE VST3 CON FALLOS");
    return failures == 0 ? 0 : 1;
}

// SLICE with a real loop through the VST3, cut by Transitorios (the default): each key from C3 is tapped and
// where its first 40 ms come from in the file is found by correlation. Keys must come from different places,
// in order, each later than the one before (C3 at the start), until the keys with no slice, which are silent.
int sliceReal (juce::VST3PluginFormat& format, const juce::PluginDescription& desc, const juce::File& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr)
        return 2;
    const double rate = reader->sampleRate;      // the host at the file's rate: positions compare directly
    juce::AudioBuffer<float> src ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&src, 0, (int) reader->lengthInSamples, 0, true, true);
    const int block = 256;
    auto p = open (format, desc, rate, block);
    p->setPlayConfigDetails (0, 2, rate, block);
    p->prepareToPlay (rate, block);
    juce::MemoryBlock state;
    p->getStateInformation (state);
    auto xml = pluginXml (state);
    xml->setAttribute ("samplePath", file.getFullPathName());
    for (const auto* id : { "sampleAudio", "sampleName", "sampleEdited" })
        xml->removeAttribute (id);
    const std::map<juce::String, float> setup { { "playMode", 1.0f }, { "loopKind", 1.0f }, { "sliceBy", 0.0f }, { "sliceSensitivity", 0.5f },
                                                { "sliceStart", 60.0f }, { "polyMode", 0.0f }, { "monoBass", 0.0f }, { "envLock", 1.0f } };
    for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
        if (auto it = setup.find (e->getStringAttribute ("id")); it != setup.end())
            e->setAttribute ("value", it->second);
    const auto next = hostStateWith (state, *xml);
    p->setStateInformation (next.getData(), (int) next.getSize());
    wait (6000);

    const int keys = 24;
    const double gap = 0.6;
    const int length = (int) ((0.1 + gap * keys) * rate);
    juce::AudioBuffer<float> out (2, length), buf (2, block);
    out.clear();
    for (int pos = 0; pos < length; pos += block)
    {
        juce::MidiBuffer midi;
        for (int k = 0; k < keys; ++k)
        {
            const int on = (int) ((0.1 + gap * k) * rate), off = on + (int) (0.02 * rate);
            if (on >= pos && on < pos + block) midi.addEvent (juce::MidiMessage::noteOn (1, 60 + k, 0.9f), on - pos);
            if (off >= pos && off < pos + block) midi.addEvent (juce::MidiMessage::noteOff (1, 60 + k), off - pos);
        }
        buf.clear();
        p->processBlock (buf, midi);
        const int n = std::min (block, length - pos);
        out.copyFrom (0, pos, buf, 0, 0, n);
        if (pos % (block * 64) == 0) wait (1);
    }
    // Where each key's sound comes from: the best normalised correlation of its first 40 ms with the file.
    const int w = (int) (0.04 * rate);
    const float* x = src.getReadPointer (0);
    std::printf ("  tecla   MIDI   sale del archivo en (s)   correlación\n");
    int last = -1, sounding = 0;
    bool ordered = true, silentAfter = true, ended = false;
    for (int k = 0; k < keys; ++k)
    {
        const int from = (int) ((0.1 + gap * k) * rate);
        const float* y = out.getReadPointer (0) + from;
        double ey = 0.0;
        for (int i = 0; i < w; ++i) ey += (double) y[i] * y[i];
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const juce::String nm = juce::String (names[(60 + k) % 12]) + juce::String ((60 + k) / 12 - 2);
        if (ey < 1.0e-8)
        {
            std::printf ("  %-6s  %4d   (silencio)\n", nm.toRawUTF8(), 60 + k);
            ended = true;
            continue;
        }
        silentAfter = silentAfter && ! ended;
        double best = -1.0;
        int at = 0;
        for (int s0 = 0; s0 + w < src.getNumSamples(); s0 += 4)
        {
            double xy = 0.0, ex = 0.0;
            for (int i = 0; i < w; i += 2) { xy += (double) x[s0 + i] * y[i]; ex += (double) x[s0 + i] * x[s0 + i]; }
            const double c = xy / std::sqrt (ex * ey * 0.5 + 1.0e-20);
            if (c > best) { best = c; at = s0; }
        }
        std::printf ("  %-6s  %4d   %8.3f                 %.3f\n", nm.toRawUTF8(), 60 + k, at / rate, best);
        ordered = ordered && at > last && best > 0.9 && (k > 0 || at < (int) (0.01 * rate));
        last = at;
        ++sounding;
    }
    // Informative: music that repeats (the same chord in another bar) correlates as well with the repeat, so a
    // key can show a later or earlier bar. The exact table is "ReanimaAudit cortes <file>".
    std::printf ("  (informativo) %d teclas con trozo, %s\n", sounding, ordered ? "en orden" : "alguna coincide con un pasaje repetido");
    p->releaseResources();
    return failures == 0 ? 0 : 1;
}

// Two settings as the plugin reports them: exact for steps, within float rounding for knobs.
bool sameSetting (const juce::AudioProcessorParameter& param, const juce::String& a, const juce::String& b)
{
    if (param.isDiscrete() || param.isBoolean())
        return a == b;
    const double x = a.getDoubleValue(), y = b.getDoubleValue();
    return std::abs (x - y) <= 1.0e-5 * std::max (1.0, std::abs (x));
}
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    if (argc < 2)
        return 2;

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, juce::String::fromUTF8 (argv[1]));
    check (found.size() == 1, "el host encuentra el plugin", found.isEmpty() ? juce::String() : found[0]->name + " " + found[0]->version);
    if (found.isEmpty())
        return 1;

    const auto& desc = *found[0];
    if (argc > 3 && juce::String (argv[2]) == "slices")
        return sliceReal (format, desc, juce::File (juce::String::fromUTF8 (argv[3])));
    if (argc > 2 && juce::String (argv[2]) == "slices")
        return sliceTest (format, desc);
    if (argc > 4 && juce::String (argv[2]) == "resistencia")
        return soak (format, desc, juce::String (argv[3]).getDoubleValue(), juce::File (juce::String::fromUTF8 (argv[4])));

    check (desc.isInstrument, "se declara como instrumento");
    check (desc.manufacturerName == "Blackpanda", "fabricante", desc.manufacturerName);

    {
        auto p = open (format, desc, 48000.0, 512);
        // The output stage's linear-phase oversampling: 38 samples at 48 kHz, reported once the rate is known.
        p->prepareToPlay (48000.0, 512);
        check (p->getLatencySamples() == 38, "latencia declarada (la etapa de salida, 0,8 ms a 48 kHz)", juce::String (p->getLatencySamples()) + " muestras");
        p->releaseResources();
        check (p->getTotalNumInputChannels() == 0 && p->getTotalNumOutputChannels() == 2, "sin entradas, salida estéreo",
               juce::String (p->getTotalNumOutputChannels()) + " salidas");
        check (p->acceptsMidi(), "acepta MIDI");

        // Besides its own, JUCE adds 16 x 130 hidden ones: how VST3 passes pitch bend and CCs.
        check (p->getParameters().size() > 200, "parámetros visibles", juce::String (p->getParameters().size()));

        juce::String badNames;
        for (auto* param : p->getParameters())
            if (param->getName (64).isEmpty())
                badNames << param->getParameterIndex() << " ";
        check (badNames.isEmpty(), "todos los parámetros tienen nombre", badNames);
    }

    // Every rate and block size, also changing them on a running instance as hosts do.
    {
        auto p = open (format, desc, 44100.0, 512);
        juce::String bad;
        for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
            for (const int block : { 16, 32, 64, 100, 128, 256, 441, 512, 1024, 2048, 4096 })
            {
                p->releaseResources();
                p->setRateAndBufferSizeDetails (rate, block);
                p->prepareToPlay (rate, block);
                float peak = 0.0f;
                if (! play (*p, block, 0.1, rate, peak))
                    bad << juce::String (rate) << "/" << block << " ";
            }
        check (bad.isEmpty(), "6 frecuencias x 11 tamaños de bloque en la misma instancia", bad);
    }

    // State through the VST3 interface, parameter by parameter.
    {
        auto a = open (format, desc, 48000.0, 512);
        a->prepareToPlay (48000.0, 512);
        juce::Random rng (5);
        for (auto* param : a->getParameters())
            if (rng.nextInt (4) == 0)
                param->setValue (rng.nextFloat());

        juce::MemoryBlock state;
        a->getStateInformation (state);

        auto b = open (format, desc, 96000.0, 256);
        b->prepareToPlay (96000.0, 256);
        b->setStateInformation (state.getData(), (int) state.getSize());
        wait (200);

        juce::String different;
        const auto& pa = a->getParameters();
        const auto& pb = b->getParameters();
        for (int i = 0; i < pa.size() && i < pb.size(); ++i)
            if (! pa[i]->getName (32).startsWith ("MIDI CC") && ! sameSetting (*pa[i], pa[i]->getCurrentValueAsText(), pb[i]->getCurrentValueAsText()))
                different << pa[i]->getName (32) << " ";
        check (pa.size() == pb.size() && different.isEmpty(), "guardar y abrir conserva los parámetros", different);

        const juce::MemoryBlock garbage ("esto no es un estado", 20);
        b->setStateInformation (garbage.getData(), (int) garbage.getSize());
        b->setStateInformation (nullptr, 0);
        float peak = 0.0f;
        check (play (*b, 256, 0.1, 96000.0, peak), "un estado corrupto no rompe nada");
    }

    // Going back to a saved state on the same instance, after a host has written values between
    // the steps of switches, menus and whole-number knobs. The host may keep showing those raw
    // values, so each parameter is judged by the setting the plugin reports, within float rounding.
    {
        auto p = open (format, desc, 48000.0, 512);
        p->prepareToPlay (48000.0, 512);

        juce::Array<juce::AudioProcessorParameter*> params;
        for (auto* param : p->getParameters())
            if (! param->getName (32).startsWith ("MIDI CC"))
                params.add (param);

        juce::Random rng (11);
        juce::String wrongStep, wrongValue;

        for (int round = 0; round < 20; ++round)
        {
            for (auto* param : params)
                param->setValue (rng.nextFloat());
            wait (50);

            juce::MemoryBlock saved;
            p->getStateInformation (saved);
            juce::StringArray texts;
            for (auto* param : params)
                texts.add (param->getCurrentValueAsText());

            for (auto* param : params)
                param->setValue (rng.nextFloat());
            wait (50);

            p->setStateInformation (saved.getData(), (int) saved.getSize());
            wait (50);

            for (int i = 0; i < params.size(); ++i)
            {
                const auto now = params[i]->getCurrentValueAsText();
                if (! sameSetting (*params[i], texts[i], now))
                    (params[i]->isDiscrete() || params[i]->isBoolean() ? wrongStep : wrongValue) << params[i]->getName (32) << " ";
            }
        }

        check (wrongStep.isEmpty(), "volver a un estado recupera interruptores y menús", wrongStep);
        check (wrongValue.isEmpty(), "volver a un estado recupera los mandos", wrongValue);
    }

    // Opening and closing the window repeatedly, with audio running on another thread.
    {
        auto p = open (format, desc, 48000.0, 512);
        p->prepareToPlay (48000.0, 512);

        std::atomic<bool> running { true };
        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            while (running)
            {
                buffer.clear();
                p->processBlock (buffer, midi);
            }
        });

        for (int i = 0; i < 6; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditorAndMakeActive());
            check (editor != nullptr && editor->getWidth() > 0, "la ventana se abre (" + juce::String (i + 1) + ")");
            wait (150);
            editor.reset();
        }

        running = false;
        audio.join();
    }

    // Several instances at the same time.
    {
        std::vector<std::unique_ptr<juce::AudioPluginInstance>> many;
        for (int i = 0; i < 8; ++i)
        {
            many.push_back (open (format, desc, 48000.0, 256));
            many.back()->prepareToPlay (48000.0, 256);
        }

        std::atomic<int> bad { 0 };
        std::vector<std::thread> threads;
        for (auto& p : many)
            threads.emplace_back ([&bad, instance = p.get()]
            {
                float peak = 0.0f;
                if (! play (*instance, 256, 1.0, 48000.0, peak))
                    ++bad;
            });
        for (auto& t : threads)
            t.join();
        check (bad.load() == 0, "8 instancias en paralelo");
    }

    std::printf ("\n%s\n", failures == 0 ? "VST3 OK" : "VST3 CON FALLOS");
    return failures == 0 ? 0 : 1;
}
