#define _CRT_SECURE_NO_WARNINGS
// CPU of the audio thread measured as a host would see it: a fresh instance (default settings and the
// sample's detected envelope), notes played in patterns a player uses, the time of every block taken.
// Prints, per scenario: mean and worst load (% of the block's real time), blocks over their time,
// voices sounding, and how long the load takes to come back to rest after the last note.
//
// Usage: ReanimaCpuBench <sample.wav> [engine 0-5] [unison] [block]

#include "PluginProcessor.h"
#include "FactoryPresets.h"
#include "FxRack.h"
#include <cstdio>
#include <chrono>
#include <functional>
#if JUCE_WINDOWS
 #define NOMINMAX
 #include <windows.h>
 #include <psapi.h>
 #include <avrt.h>
 #pragma comment(lib, "psapi.lib")
 #pragma comment(lib, "avrt.lib")
 #include "Profiler.h"
#elif JUCE_MAC
 #include <mach/mach.h>
#endif

namespace
{
struct Bench
{
    ReanimaProcessor& proc;
    double rate;
    int block;
    juce::AudioBuffer<float> buffer;
    FILE* dump = nullptr;

    Bench (ReanimaProcessor& p, double r, int b) : proc (p), rate (r), block (b), buffer (2, b) {}

    void set (int index, float value)
    {
        auto* param = proc.apvts.getParameter (P::defs()[(size_t) index].id);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    int voices() const
    {
        int n = 0;
        for (auto& p : proc.engine.playheads)
            n += p.load() >= 0.0f ? 1 : 0;
        return n;
    }

    // events: (time in s, note, on?)
    void run (const char* name, std::vector<std::tuple<double, int, bool>> events, double seconds)
    {
        std::sort (events.begin(), events.end(), [] (auto& a, auto& b) { return std::get<0> (a) < std::get<0> (b); });
        const double blockMs = 1000.0 * block / rate;
        const int blocks = (int) (seconds * rate / block);
        size_t next = 0;
        double sum = 0, worst = 0, lastNote = std::get<0> (events.back());
        int over = 0, maxVoices = 0, counted = 0;
        double restAt = -1.0;
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            const double t0s = (double) b * block / rate, t1s = (double) (b + 1) * block / rate;
            juce::MidiBuffer midi;
            while (next < events.size() && std::get<0> (events[next]) < t1s)
            {
                const auto& [t, n, on] = events[next++];
                const int pos = juce::jlimit (0, block - 1, (int) ((t - t0s) * rate));
                midi.addEvent (on ? juce::MidiMessage::noteOn (1, n, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, n), pos);
            }
            buffer.clear();
            const double s = juce::Time::getMillisecondCounterHiRes();
            proc.processBlock (buffer, midi);
            const double ms = juce::Time::getMillisecondCounterHiRes() - s;
            peak = std::max (peak, buffer.getMagnitude (0, block));
            if (dump != nullptr)     // the sound itself, to prove an optimisation changes nothing
                for (int c = 0; c < 2; ++c)
                    std::fwrite (buffer.getReadPointer (c), sizeof (float), (size_t) block, dump);
            if (b % 32 == 0)
                proc.serviceMessageThread();
            const double load = 100.0 * ms / blockMs;
            const int v = voices();
            maxVoices = std::max (maxVoices, v);
            if (t0s <= lastNote + 0.05)
            {
                sum += load;
                ++counted;
                worst = std::max (worst, load);
                over += load > 100.0 ? 1 : 0;
            }
            else if (restAt < 0.0 && v == 0)
                restAt = t0s - lastNote;
        }
        std::printf ("%-34s mean %6.1f %%  worst %7.1f %%  over %4d  voices max %2d  rest after %s  peak %.2f\n", name,
                     sum / std::max (1, counted), worst, over, maxVoices,
                     restAt < 0 ? "never" : juce::String (restAt, 1).toRawUTF8(), peak);
        std::fflush (stdout);
        // silence until everything stops, so the next scenario starts clean
        for (int b = 0; b < (int) (20.0 * rate / block) && voices() > 0; ++b)
        {
            juce::MidiBuffer none;
            proc.processBlock (buffer, none);
        }
    }
};

std::vector<std::tuple<double, int, bool>> held (std::vector<int> notes, double on, double off)
{
    std::vector<std::tuple<double, int, bool>> e;
    for (int n : notes)
    {
        e.push_back ({ on, n, true });
        e.push_back ({ off, n, false });
    }
    return e;
}

// A player improvising: up to 'poly' keys down at once, random notes and lengths.
std::vector<std::tuple<double, int, bool>> improvise (int poly, double minLen, double maxLen, double gap, double seconds, int seed)
{
    juce::Random rng (seed);
    std::vector<std::tuple<double, int, bool>> e;
    std::vector<std::pair<double, int>> down;    // release time, note
    for (double t = 0.1; t < seconds; t += gap * (0.5 + rng.nextDouble()))
    {
        down.erase (std::remove_if (down.begin(), down.end(), [t] (auto& d) { return d.first <= t; }), down.end());
        if ((int) down.size() >= poly)
            continue;
        int note;
        do
            note = 45 + rng.nextInt (30);
        while (std::any_of (down.begin(), down.end(), [note] (auto& d) { return d.second == note; }));
        const double len = minLen + (maxLen - minLen) * rng.nextDouble();
        e.push_back ({ t, note, true });
        e.push_back ({ t + len, note, false });
        down.push_back ({ t + len, note });
    }
    return e;
}

// ---- suite: the load a host sees at small buffers, per block, with the worst blocks kept apart from the mean.
using Events = std::vector<std::tuple<double, int, bool>>;

void setId (ReanimaProcessor& proc, const juce::String& id, float value)
{
    if (auto* param = proc.apvts.getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    else
        std::printf ("unknown parameter %s\n", id.toRawUTF8());
}

void settle (ReanimaProcessor& proc)
{
    proc.serviceMessageThread();
    for (int k = 0; k < 1200 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (25);
        proc.serviceMessageThread();
    }
    proc.serviceMessageThread();
}

// Every effect type in the three chains, both buses sent, the filter on, the eight matrix slots moving.
void fullChain (ReanimaProcessor& proc)
{
    const int types[P::numFxSlots] = { Fx::HyperDim, Fx::Distortion, Fx::Chorus, Fx::Phaser, Fx::Delay, Fx::Reverb,
                                       Fx::Flanger, Fx::Compressor, Fx::EQ, Fx::Reverb,
                                       Fx::Filter, Fx::Bode, Fx::Delay, Fx::Utility };
    for (int slot = 0; slot < P::numFxSlots; ++slot)
        setId (proc, "fx" + juce::String (slot) + "Type", (float) types[slot]);
    setId (proc, "fxBus1Send", 0.5f);
    setId (proc, "fxBus2Send", 0.5f);
    setId (proc, "fltOn", 1.0f);
    setId (proc, "fltType", 4.0f);
    setId (proc, "fltCircuit", 2.0f);
    setId (proc, "fltEnvAmt", 0.4f);
    setId (proc, "fltFollow", 0.3f);
    const float matrix[8][3] = { { 1, 1, 0.1f }, { 2, 4, 0.5f }, { 3, 3, 0.5f }, { 4, 5, 0.3f },
                                 { 6, 2, 0.3f }, { 14, 7, 0.3f }, { 1, 9, 0.4f }, { 5, 6, 0.3f } };
    for (int m = 0; m < 8; ++m)
    {
        setId (proc, "m" + juce::String (m + 1) + "Src", matrix[m][0]);
        setId (proc, "m" + juce::String (m + 1) + "Dst", matrix[m][1]);
        setId (proc, "m" + juce::String (m + 1) + "Amt", matrix[m][2]);
    }
    setId (proc, "lfo2Rate", 0.7f);
    setId (proc, "lfo3Rate", 5.0f);
}

// fullChain and, on top, the functions of 23-11: the filter Armónico, Movimiento, LFO 1 Deriva on Tono fino, the
// Sidechain source on the cutoff and Impronta in the first slot (the sidechain silent: the host gives none here).
void fullChainNew (ReanimaProcessor& proc)
{
    fullChain (proc);
    setId (proc, "fx0Type", (float) Fx::Impronta);
    setId (proc, "fltType", (float) P::Harmonic);
    setId (proc, "fltHarmGain", 9.0f);
    setId (proc, "sustainMotion", 1.0f);
    setId (proc, "lfo1Shape", (float) P::Drift);
    setId (proc, "lfo1Smooth", 0.5f);
    setId (proc, "m1Src", (float) P::SrcLfo1);
    setId (proc, "m1Dst", (float) P::DstFine);
    setId (proc, "m1Amt", 0.2f);
    setId (proc, "m8Src", (float) P::SrcSidechain);
    setId (proc, "m8Dst", (float) P::DstCutoff);
    setId (proc, "m8Amt", 0.3f);
}

double processMemoryMB()
{
   #if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX m {};
    GetProcessMemoryInfo (GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*) &m, sizeof (m));
    return (double) m.PrivateUsage / (1024.0 * 1024.0);
   #elif JUCE_MAC
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info (mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t) &info, &count) == KERN_SUCCESS)
        return (double) info.resident_size / (1024.0 * 1024.0);
    return 0.0;
   #else
    return 0.0;
   #endif
}

// Puts this thread where a host puts its audio thread, as far as the platform lets a plain program.
struct AudioPriority
{
    AudioPriority()
    {
       #if JUCE_WINDOWS
        SetPriorityClass (GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        handle = AvSetMmThreadCharacteristicsW (L"Pro Audio", &task);
        if (handle != nullptr)
            AvSetMmThreadPriority (handle, AVRT_PRIORITY_CRITICAL);
        name = handle != nullptr ? "Pro Audio" : "no";
       #else
        juce::Process::setPriority (juce::Process::RealtimePriority);
        name = "realtime";
       #endif
    }
    ~AudioPriority()
    {
       #if JUCE_WINDOWS
        if (handle != nullptr)
            AvRevertMmThreadCharacteristics (handle);
       #endif
    }
   #if JUCE_WINDOWS
    DWORD task = 0;
    HANDLE handle = nullptr;
   #endif
    const char* name = "";
};

double nowMs()
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Result
{
    double mean = 0, p99 = 0, p999 = 0, worst = 0;
    int over = 0, over50 = 0, blocks = 0, voices = 0;
    float peak = 0.0f;
    bool finite = true;
};

FILE* spikeLog = std::getenv ("SUITE_SPIKES") != nullptr ? stdout : nullptr;

// A host with a tempo (HOST_BPM=...), for LOOP to follow; without it the processor sees no tempo.
struct TempoHead : juce::AudioPlayHead
{
    double bpm = 0.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        if (bpm > 0.0)
            info.setBpm (bpm);
        return info;
    }
};
TempoHead tempoHead;
void useHostTempo (ReanimaProcessor& proc)
{
    if (const char* b = std::getenv ("HOST_BPM"))
    {
        tempoHead.bpm = std::atof (b);
        proc.setPlayHead (&tempoHead);
    }
}

int voicesOf (ReanimaProcessor& proc)
{
    int v = 0;
    for (auto& ph : proc.engine.playheads)
        v += ph.load() >= 0.0f ? 1 : 0;
    return v;
}

// Plays the events block by block and times each block. 'each' runs between blocks, as the message thread would.
Result play (ReanimaProcessor& proc, double rate, int block, Events events, double seconds, std::function<void (double)> each = {})
{
    std::sort (events.begin(), events.end(), [] (auto& a, auto& b) { return std::get<0> (a) < std::get<0> (b); });
    juce::AudioBuffer<float> buffer (2, block);
    const double blockMs = 1000.0 * block / rate;
    const int blocks = (int) (seconds * rate / block);
    const int serviceEvery = std::max (1, (int) (0.01 * rate / block));
    std::vector<double> loads;
    loads.reserve ((size_t) blocks);
    Result r;
    size_t next = 0;
    double sum = 0;
    for (int b = 0; b < blocks; ++b)
    {
        const double t0s = (double) b * block / rate, t1s = (double) (b + 1) * block / rate;
        juce::MidiBuffer midi;
        while (next < events.size() && std::get<0> (events[next]) < t1s)
        {
            const auto& [t, n, on] = events[next++];
            const int pos = juce::jlimit (0, block - 1, (int) ((t - t0s) * rate));
            midi.addEvent (on ? juce::MidiMessage::noteOn (1, n, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, n), pos);
        }
        buffer.clear();
        const double a = nowMs();
        proc.processBlock (buffer, midi);
        const double ms = nowMs() - a;
        const double load = 100.0 * ms / blockMs;
        loads.push_back (load);
        sum += load;
        r.worst = std::max (r.worst, load);
        r.over += load > 100.0 ? 1 : 0;
        r.over50 += load > 50.0 ? 1 : 0;
        if (spikeLog != nullptr && load > 80.0)
            std::fprintf (spikeLog, "spike at %.3f s: %.0f %% (voices %d)\n", t0s, load, voicesOf (proc));
        for (int c = 0; c < 2; ++c)
        {
            const float* x = buffer.getReadPointer (c);
            for (int i = 0; i < block; ++i)
            {
                r.finite = r.finite && std::isfinite (x[i]);
                r.peak = std::max (r.peak, std::abs (x[i]));
            }
        }
        r.voices = std::max (r.voices, voicesOf (proc));
        if (b % serviceEvery == 0)     // the message thread, every 10 ms
        {
            proc.serviceMessageThread();
            if (each)
                each (t0s);
        }
    }
    std::sort (loads.begin(), loads.end());
    r.blocks = blocks;
    r.mean = sum / std::max (1, blocks);
    r.p99 = loads[(size_t) (0.99 * (double) (loads.size() - 1))];
    r.p999 = loads[(size_t) (0.999 * (double) (loads.size() - 1))];
    // every key up, then silence until everything stops, so the next scenario starts clean
    juce::MidiBuffer ups;
    for (int n = 0; n < 128; ++n)
        ups.addEvent (juce::MidiMessage::noteOff (1, n), 0);
    for (int b = 0; b < (int) (30.0 * rate / block); ++b)
    {
        if (b > (int) (0.5 * rate / block) && voicesOf (proc) == 0)
            break;
        proc.processBlock (buffer, ups);
        ups.clear();
        if (b % serviceEvery == 0)
            proc.serviceMessageThread();
    }
    return r;
}

FILE* csv = nullptr;

void report (const char* sample, double rate, int block, const char* config, const char* scenario, const Result& r)
{
    std::printf ("%-10s %6.0f %3d %-8s %-20s mean %5.1f  p99 %5.1f  p99.9 %5.1f  worst %6.1f  >50%% %4d  >100%% %3d  voices %2d%s\n",
                 sample, rate, block, config, scenario, r.mean, r.p99, r.p999, r.worst, r.over50, r.over, r.voices, r.finite ? "" : "  NAN!");
    std::fflush (stdout);
    if (csv != nullptr)
    {
        std::fprintf (csv, "%s,%.0f,%d,%s,%s,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%.3f,%d\n", sample, rate, block, config, scenario, r.mean, r.p99,
                      r.p999, r.worst, r.over50, r.over, r.blocks, r.voices, r.peak, (int) r.finite);
        std::fflush (csv);
    }
}

Events chords (double seconds, double every)
{
    Events e;
    for (int k = 0; k * every < seconds; ++k)
        for (int n : { 48, 55, 60, 64 })
        {
            e.push_back ({ 0.1 + every * k, n + (k % 5), true });
            e.push_back ({ 0.1 + every * k + every * 0.8, n + (k % 5), false });
        }
    return e;
}

std::unique_ptr<ReanimaProcessor> fresh (const juce::File& sample, double rate, int block, int engine, int full, bool sostenido)
{
    auto proc = std::make_unique<ReanimaProcessor>();
    useHostTempo (*proc);
    proc->setPlayConfigDetails (0, 2, rate, block);
    proc->prepareToPlay (rate, block);
    proc->loadSampleNow (sample);
    settle (*proc);
    setId (*proc, "engine", (float) engine);
    if (sostenido)
        setId (*proc, "sustainType", 1.0f);
    if (engine == 0)
        setId (*proc, "grainMix", 0.5f);     // A's Textura: grains over the reconstruction
    if (full == 1)
        fullChain (*proc);
    else if (full == 2)
        fullChainNew (*proc);
    settle (*proc);
    juce::AudioBuffer<float> b (2, block);
    juce::MidiBuffer none;
    for (int i = 0; i < 50; ++i)
        proc->processBlock (b, none);
    return proc;
}

// suite <out.csv> <block> <rates, comma> <engines, e.g. 012345> <sample>...   env SUITE_LONG=minutes: the long session
int suite (int argc, char* argv[])
{
    AudioPriority priority;
    csv = std::fopen (argv[2], "wb");
    std::fprintf (csv, "sample,rate,block,config,scenario,mean,p99,p999,worst,over50,over100,blocks,voices,peak,finite\n");
    const int block = std::atoi (argv[3]);
    const auto rates = juce::StringArray::fromTokens (argv[4], ",", "");
    const juce::String engines (argv[5]);
    std::printf ("memory at start %.0f MB, audio priority %s\n", processMemoryMB(), priority.name);
    for (int a = 6; a < argc; ++a)
    {
        const juce::File sample (juce::String::fromUTF8 (argv[a]));
        const auto label = sample.getFileNameWithoutExtension().substring (0, 10);
        for (const auto& rs : rates)
        {
            const double rate = rs.getDoubleValue();
            for (int ci = 0; ci < engines.length(); ++ci)
            {
                const int engine = engines[ci] - '0';
                // SUITE_CONFIGS=012: base, todo, todo + the functions of 23-11 (default: 01)
                const juce::String configs = std::getenv ("SUITE_CONFIGS") != nullptr ? juce::String (std::getenv ("SUITE_CONFIGS")) : juce::String ("01");
                for (int ki = 0; ki < configs.length(); ++ki)
                {
                    const int full = configs[ki] - '0';
                    const juce::String config = juce::String::charToString ("ABCDEF"[engine]) + (full == 2 ? " nuevo" : full == 1 ? " todo" : " base");
                    auto proc = fresh (sample, rate, block, engine, full, true);
                    report (label.toRawUTF8(), rate, block, config.toRawUTF8(), "4 voces largas 8 s", play (*proc, rate, block, held ({ 55, 60, 64, 67 }, 0.1, 8.1), 10));
                    report (label.toRawUTF8(), rate, block, config.toRawUTF8(), "improvisa poly 4", play (*proc, rate, block, improvise (4, 0.1, 3.0, 0.25, 20, 7), 23));
                    report (label.toRawUTF8(), rate, block, config.toRawUTF8(), "acordes 0.5 s", play (*proc, rate, block, chords (15, 0.5), 16));
                    // The target of 23-11: 32 voices at once (64 samples, 48 kHz), held 6 s, the notes 2 ms apart.
                    {
                        Events e;
                        for (int k = 0; k < 32; ++k)
                        {
                            e.push_back ({ 0.1 + 0.002 * k, 36 + k, true });
                            e.push_back ({ 6.1, 36 + k, false });
                        }
                        report (label.toRawUTF8(), rate, block, config.toRawUTF8(), "32 voces 6 s", play (*proc, rate, block, e, 8));
                    }
                }
            }
        }
    }
    // Preset changes while playing (every factory preset once), and the long session.
    {
        const juce::File sample (juce::String::fromUTF8 (argv[6]));
        auto proc = fresh (sample, 48000.0, block, 0, 0, false);
        int nextPreset = 0;
        const int presets = (int) Factory::presets().size();
        report ("presets", 48000.0, block, "todos", "cambio cada 0.4 s", play (*proc, 48000.0, block, chords (0.4 * presets, 0.4), 0.4 * presets + 1.0,
            [&] (double t) {
                if (t > 0.4 * nextPreset && nextPreset < presets)
                {
                    proc->applyFactoryPreset (nextPreset++);
                    if (spikeLog != nullptr)
                        std::fprintf (spikeLog, "preset %d %s: engine %d unison %d\n", nextPreset - 1, ReanimaProcessor::factoryPresetName (nextPreset - 1).toRawUTF8(), (int) proc->paramValue (P::Engine), (int) proc->paramValue (P::Unison));
                }
            }));
        const double minutes = std::getenv ("SUITE_LONG") != nullptr ? std::atof (std::getenv ("SUITE_LONG")) : 0.0;
        if (minutes > 0.0)
        {
            auto longProc = fresh (sample, 48000.0, block, 0, 1, true);
            const double before = processMemoryMB();
            Result total;
            for (int m = 0; m < (int) minutes; ++m)
            {
                const auto r = play (*longProc, 48000.0, block, improvise (4, 0.1, 4.0, 0.3, 58, 100 + m), 60);
                if (m % 3 == 2)
                {
                    longProc->applyFactoryPreset ((m * 37) % presets);
                    fullChain (*longProc);
                    settle (*longProc);
                }
                total.worst = std::max (total.worst, r.worst);
                total.over += r.over;
                total.over50 += r.over50;
                total.blocks += r.blocks;
                total.mean += r.mean / minutes;
                total.p999 = std::max (total.p999, r.p999);
                total.p99 = std::max (total.p99, r.p99);
                total.voices = std::max (total.voices, r.voices);
                total.finite = total.finite && r.finite;
                std::printf ("  minute %d: mean %.1f p99.9 %.1f worst %.1f over %d memory %.0f MB\n", m + 1, r.mean, r.p999, r.worst, r.over, processMemoryMB());
                std::fflush (stdout);
            }
            report ("sesion", 48000.0, block, "A todo", (juce::String (minutes, 0) + " min").toRawUTF8(), total);
            std::printf ("memory: before %.0f MB, after %.0f MB\n", before, processMemoryMB());
        }
    }
    std::printf ("memory at end %.0f MB\n", processMemoryMB());
    std::fclose (csv);
    return 0;
}

// profile <out.txt> <engine 0-5> <full 0|1> <held|improv|chords> <sample> [rate] [repeat]: where the time goes.
int profile (int argc, char* argv[])
{
   #if ! JUCE_WINDOWS
    juce::ignoreUnused (argc, argv);
    std::printf ("the sampling profiler is Windows only (dbghelp); use Instruments on macOS\n");
    return 1;
   #else
    AudioPriority priority;
    const int engine = std::atoi (argv[3]);
    const bool full = std::atoi (argv[4]) != 0;
    const juce::String scenario (argv[5]);
    const juce::File sample (juce::String::fromUTF8 (argv[6]));
    const double rate = argc > 7 ? std::atof (argv[7]) : 48000.0;
    const int repeat = argc > 8 ? std::atoi (argv[8]) : 3;
    const int block = 64;
    auto proc = fresh (sample, rate, block, engine, full ? 1 : 0, true);
    if (const char* preset = std::getenv ("PRESET"))
    {
        proc->applyFactoryPreset (std::atoi (preset));
        settle (*proc);
        std::printf ("preset %s\n", ReanimaProcessor::factoryPresetName (std::atoi (preset)).toRawUTF8());
    }
    SamplingProfiler prof;
    prof.start();
    prof.active = true;
    for (int k = 0; k < repeat; ++k)
    {
        const auto r = scenario == "held" ? play (*proc, rate, block, held ({ 55, 60, 64, 67 }, 0.1, 8.1), 10)
                     : scenario == "improv" ? play (*proc, rate, block, improvise (4, 0.1, 3.0, 0.25, 20, 7), 23)
                                            : play (*proc, rate, block, chords (15, 0.5), 16);
        report (sample.getFileNameWithoutExtension().substring (0, 10).toRawUTF8(), rate, block, full ? "todo" : "base", argv[5], r);
    }
    prof.active = false;
    FILE* out = std::fopen (argv[2], "wb");
    prof.stopAndReport (out);
    std::fclose (out);
    return 0;
   #endif
}
} // namespace

int main (int argc, char* argv[])
{
    if (argc < 2)
    {
        std::printf ("usage: ReanimaCpuBench <sample.wav> [engine] [unison] [block]\n");
        return 1;
    }
    juce::ScopedJuceInitialiser_GUI juce;
    if (juce::String (argv[1]) == "suite" && argc > 6)
        return suite (argc, argv);
    if (juce::String (argv[1]) == "memoria" && argc > 2)
    {
        // memoria <sample>: what one instance takes, step by step (private memory of the process).
        const double start = processMemoryMB();
        std::printf ("proceso al empezar: %.1f MB\n", start);
        auto proc = std::make_unique<ReanimaProcessor>();
        proc->setPlayConfigDetails (0, 2, 48000.0, 64);
        proc->prepareToPlay (48000.0, 64);
        settle (*proc);
        double last = processMemoryMB();
        const auto step = [&last] (const char* what)
        {
            const double now = processMemoryMB();
            std::printf ("%-48s %+8.1f MB  (total %.1f MB)\n", what, now - last, now);
            last = now;
        };
        step ("instancia vacía (motor A)");
        proc->loadSampleNow (juce::File (juce::String::fromUTF8 (argv[2])));
        settle (*proc);
        step ("con el sample cargado y analizado");
        juce::AudioBuffer<float> b (2, 64);
        juce::MidiBuffer none;
        for (int e = 1; e <= 5; ++e)
        {
            setId (*proc, "engine", (float) e);
            settle (*proc);
            for (int k = 0; k < 20; ++k)
                proc->processBlock (b, none);
            settle (*proc);
            step ((juce::String ("motor ") + "ABCDEF"[e] + " elegido").toRawUTF8());
        }
        setId (*proc, "playMode", 1.0f);
        settle (*proc);
        step ("LOOP elegido (estiradores)");
        fullChain (*proc);
        settle (*proc);
        step ("cadena completa de efectos");
        proc.reset();
        step ("instancia destruida");
        // Made and destroyed again, four times: memory that keeps growing is a leak (what stays the same is shared).
        const int rounds = std::getenv ("MEM_ROUNDS") != nullptr ? std::atoi (std::getenv ("MEM_ROUNDS")) : 4;
        for (int round = 0; round < rounds; ++round)
        {
            // MEM_WHAT: s sample, f engine F, l LOOP, x effects (default all), to find where a leak is.
            const juce::String what = std::getenv ("MEM_WHAT") != nullptr ? juce::String (std::getenv ("MEM_WHAT")) : juce::String ("sflx");
            auto again = std::make_unique<ReanimaProcessor>();
            again->setPlayConfigDetails (0, 2, 48000.0, 64);
            again->prepareToPlay (48000.0, 64);
            if (what.containsChar ('s'))
                again->loadSampleNow (juce::File (juce::String::fromUTF8 (argv[2])));
            settle (*again);
            if (what.containsChar ('f'))
                setId (*again, "engine", 5.0f);
            if (what.containsChar ('l'))
                setId (*again, "playMode", 1.0f);
            if (what.containsChar ('x'))
                fullChain (*again);
            settle (*again);
            again.reset();
            // What a host's message thread does between two instances (MEM_DISPATCH=1): messages posted are delivered.
            if (std::getenv ("MEM_DISPATCH") != nullptr)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
            step ("otra instancia completa, creada y destruida");
        }
        // Two at once: what a second instance adds.
        auto one = std::make_unique<ReanimaProcessor>();
        one->setPlayConfigDetails (0, 2, 48000.0, 64);
        one->prepareToPlay (48000.0, 64);
        step ("una instancia vacía más (sin destruir)");
        return 0;
    }
    if (juce::String (argv[1]) == "profile" && argc > 6)
        return profile (argc, argv);
    const int engine = argc > 2 ? std::atoi (argv[2]) : 0;
    const int unison = argc > 3 ? std::atoi (argv[3]) : 1;
    const int block = argc > 4 ? std::atoi (argv[4]) : 256;
    const double rate = 44100.0;

    ReanimaProcessor proc;
    useHostTempo (proc);
    proc.setPlayConfigDetails (0, 2, rate, block);
    proc.prepareToPlay (rate, block);
    if (! proc.loadSampleNow (juce::File (juce::String::fromUTF8 (argv[1]))))
    {
        std::printf ("cannot load\n");
        return 1;
    }
    Bench b (proc, rate, block);
    proc.serviceMessageThread();
    b.set (P::Engine, (float) engine);
    b.set (P::Unison, (float) unison);
    for (int a = 5; a < argc; ++a)     // more settings: id=value (real value)
    {
        const auto arg = juce::String (argv[a]);
        if (auto* param = proc.apvts.getParameter (arg.upToFirstOccurrenceOf ("=", false, false)))
            param->setValueNotifyingHost (param->convertTo0to1 (arg.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
        else
            std::printf ("unknown parameter %s\n", argv[a]);
    }
    proc.serviceMessageThread();
    // A change of engine may need more of the analysis, made in the background and installed when it is
    // done (cutting the notes that sound): wait for it, so every run plays the same thing.
    for (int k = 0; k < 600 && proc.isLoading(); ++k)
    {
        juce::Thread::sleep (50);
        proc.serviceMessageThread();
    }
    proc.serviceMessageThread();
    {
        juce::MidiBuffer none;
        for (int i = 0; i < 20; ++i)
            proc.processBlock (b.buffer, none);
    }
    std::printf ("engine %c, unison %d, block %d, A %.0f ms D %.0f ms S %.2f R %.0f ms, sustain %s, warp %d\n", "ABCDEF"[engine], unison, block,
                 proc.paramValue (P::AmpA), proc.paramValue (P::AmpD), proc.paramValue (P::AmpS), proc.paramValue (P::AmpR),
                 proc.paramValue (P::SustainType) > 0.5f ? "Sostenido" : "Natural", (int) proc.paramValue (P::WarpMode));

    if (auto smp = proc.getSample())
        std::printf ("file %d, beds %d / %d samples, pitched %d, tonesHold %d, cycle %.1f\n", smp->length, smp->bedBright.length, smp->bedDark.length, (int) smp->pitched, (int) smp->tonesHold, smp->cycle);
    if (const char* path = std::getenv ("CPUBENCH_DUMP"))
        b.dump = std::fopen (path, "wb");
    b.run ("1 voice, short", held ({ 60 }, 0.1, 0.4), 12);
    b.run ("1 voice, held 6 s", held ({ 60 }, 0.1, 6.1), 18);
    b.run ("2 voices held 6 s", held ({ 60, 67 }, 0.1, 6.1), 18);
    b.run ("4 voices held 6 s", held ({ 55, 60, 64, 67 }, 0.1, 6.1), 18);
    b.run ("improvise poly 4, short", improvise (4, 0.08, 0.3, 0.15, 20, 1), 40);
    b.run ("improvise poly 4, long", improvise (4, 0.8, 4.0, 0.4, 20, 2), 40);
    b.run ("fast repeated notes", improvise (2, 0.05, 0.12, 0.08, 15, 3), 35);
    b.run ("chords every 0.5 s", [] {
        std::vector<std::tuple<double, int, bool>> e;
        for (int k = 0; k < 30; ++k)
            for (int n : { 48, 55, 60, 64 })
            {
                e.push_back ({ 0.1 + 0.5 * k, n + (k % 5), true });
                e.push_back ({ 0.5 + 0.5 * k, n + (k % 5), false });
            }
        return e; }(), 40);
    if (b.dump != nullptr)
        std::fclose (b.dump);
    return 0;
}
