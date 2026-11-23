// Real-time safety of the audio thread, measured: every allocation and free made inside processBlock is
// counted (the global operator new / delete of this program are replaced), and the slowest block is timed.
// The processor is driven as a host would: notes in every engine, every Warp mode, Mecanismo, unison,
// the filter with each circuit, every effect type, automation of parameters between and inside blocks.
// What the message thread does (the timer that prepares effects, async updates) runs outside the count.
//
// Usage: ReanimaRtCheck <sample.wav> [more samples...]

#include "PluginProcessor.h"
#include "FxRack.h"
#include "Mechanism.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace
{
thread_local bool inAudio = false;
std::atomic<long long> allocs { 0 }, frees { 0 }, bytes { 0 };

void* allocate (std::size_t n)
{
    if (inAudio)
    {
        ++allocs;
        bytes += (long long) n;
    }
    if (void* p = std::malloc (n != 0 ? n : 1))
        return p;
    throw std::bad_alloc();
}

void release (void* p) noexcept
{
    if (p != nullptr && inAudio)
        ++frees;
    std::free (p);
}

void* allocateAligned (std::size_t n, std::align_val_t a)
{
    if (inAudio)
    {
        ++allocs;
        bytes += (long long) n;
    }
   #if JUCE_WINDOWS
    if (void* p = _aligned_malloc (n != 0 ? n : 1, (std::size_t) a))
        return p;
   #else
    void* p = nullptr;
    if (posix_memalign (&p, std::max ((std::size_t) a, sizeof (void*)), n != 0 ? n : 1) == 0)
        return p;
   #endif
    throw std::bad_alloc();
}

void releaseAligned (void* p) noexcept
{
    if (p != nullptr && inAudio)
        ++frees;
   #if JUCE_WINDOWS
    _aligned_free (p);
   #else
    std::free (p);
   #endif
}
} // namespace

void* operator new (std::size_t n) { return allocate (n); }
void* operator new[] (std::size_t n) { return allocate (n); }
void* operator new (std::size_t n, const std::nothrow_t&) noexcept { try { return allocate (n); } catch (...) { return nullptr; } }
void* operator new[] (std::size_t n, const std::nothrow_t&) noexcept { try { return allocate (n); } catch (...) { return nullptr; } }
void operator delete (void* p) noexcept { release (p); }
void operator delete[] (void* p) noexcept { release (p); }
void operator delete (void* p, std::size_t) noexcept { release (p); }
void operator delete[] (void* p, std::size_t) noexcept { release (p); }
void* operator new (std::size_t n, std::align_val_t a) { return allocateAligned (n, a); }
void* operator new[] (std::size_t n, std::align_val_t a) { return allocateAligned (n, a); }
void operator delete (void* p, std::align_val_t) noexcept { releaseAligned (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { releaseAligned (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { releaseAligned (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { releaseAligned (p); }

namespace
{
int failures = 0;

struct Host
{
    ReanimaProcessor& proc;
    double rate;
    int block;
    juce::AudioBuffer<float> buffer;
    juce::Random rng { 0x7e57 };
    double worstMs = 0.0, totalMs = 0.0;
    int overruns = 0, played = 0;
    juce::String worstWhere;

    Host (ReanimaProcessor& p, double r, int b) : proc (p), rate (r), block (b), buffer (2, b) {}

    void set (int index, float value)
    {
        auto* param = proc.apvts.getParameter (P::defs()[(size_t) index].id);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    // The message thread: the timer prepares effect modules, async updates finish loads.
    void messageThread()
    {
        proc.serviceMessageThread();
    }

    // Blocks of audio; notes start and stop on given blocks, a parameter moves now and then (automation,
    // applied on the audio thread as hosts do).
    void play (const juce::String& where, int blocks, std::initializer_list<int> notes, int automate = -1, float lo = 0, float hi = 1)
    {
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0)
                for (int n : notes)
                    midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) (60 + 15 * (n % 4))), 0);
            if (b == blocks * 2 / 3)
                for (int n : notes)
                    midi.addEvent (juce::MidiMessage::noteOff (1, n), block / 2);
            if (b % 7 == 3)
                midi.addEvent (juce::MidiMessage::pitchWheel (1, 8192 + rng.nextInt ({ -3000, 3000 })), 5);
            buffer.clear();
            inAudio = true;
            if (automate >= 0)
                set (automate, lo + (hi - lo) * rng.nextFloat());
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            proc.processBlock (buffer, midi);
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            inAudio = false;
            totalMs += ms;
            ++played;
            if (ms > 1000.0 * block / rate)
                ++overruns;
            if (ms > worstMs)
            {
                worstMs = ms;
                worstWhere = where;
            }
            if (b % 16 == 0)
                messageThread();
        }
    }
};

void report (const char* what, long long a, long long f, long long by)
{
    const bool ok = a == 0 && f == 0;
    std::printf ("[%s] %s  %lld allocations, %lld frees, %lld bytes\n", ok ? "PASS" : "FAIL", what, a, f, by);
    if (! ok)
        ++failures;
}
} // namespace

int main (int argc, char* argv[])
{
    if (argc < 2)
    {
        std::printf ("usage: ReanimaRtCheck <sample.wav> [...]\n");
        return 1;
    }
    juce::ScopedJuceInitialiser_GUI juce;

    for (int a = 1; a < argc; ++a)
        for (const double rate : { 44100.0, 96000.0 })
            for (const int block : { 32, 512 })
            {
                auto proc = std::make_unique<ReanimaProcessor>();
                proc->setPlayConfigDetails (0, 2, rate, block);
                proc->prepareToPlay (rate, block);
                const juce::File file (juce::String::fromUTF8 (argv[a]));
                if (! proc->loadSampleNow (file))
                {
                    std::printf ("cannot load %s\n", argv[a]);
                    return 1;
                }
                Host host (*proc, rate, block);
                host.messageThread();
                const int blocks = (int) (1.2 * rate / block);
                std::printf ("\n== %s, %.0f Hz, %d samples\n", file.getFileName().toRawUTF8(), rate, block);

                allocs = frees = bytes = 0;
                host.play ("first block", 2, { 60 });
                report ("first blocks after loading", allocs, frees, bytes);

                for (int engine = 0; engine < 6; ++engine)
                {
                    host.set (P::Engine, (float) engine);
                    host.messageThread();
                    host.play (juce::String ("engine ") + "ABCDEF"[engine] + " warm-up", 4, {});
                    for (int warp = 0; warp < 3; ++warp)
                    {
                        host.set (P::WarpMode, (float) warp);
                        allocs = frees = bytes = 0;
                        host.play ("engine " + juce::String (engine) + " warp " + juce::String (warp), blocks, { 48, 60, 67, 72, 84 });
                        const auto label = juce::String ("engine ") + "ABCDEF"[engine] + ", Warp " + juce::String (warp) + ", chord";
                        report (label.toRawUTF8(), allocs, frees, bytes);
                    }
                    host.set (P::WarpMode, 0.0f);

                    allocs = frees = bytes = 0;
                    host.set (P::RelNoise, 0.6f);
                    for (int type = 0; type < Mechanism::NumTypes; ++type)
                    {
                        host.set (P::MechType, (float) type);
                        proc->engine.listen.store (type % 3);       // every way of listening too
                        host.play (juce::String ("engine ") + "ABCDEF"[engine] + " mecanismo", blocks / 3, { 55, 62 });
                    }
                    host.set (P::RelNoise, 0.0f);
                    proc->engine.listen.store (SynthEngine::ListenAll);
                    report ((juce::String ("engine ") + "ABCDEF"[engine] + ", Mecanismo, every type").toRawUTF8(), allocs, frees, bytes);

                    allocs = frees = bytes = 0;
                    host.set (P::Unison, 8.0f);
                    host.set (P::GrainMix, 0.5f);
                    host.play (juce::String ("engine ") + "ABCDEF"[engine] + " unison", blocks, { 60, 64, 67 }, P::UniDetune, 0.0f, 100.0f);
                    host.set (P::Unison, 1.0f);
                    host.set (P::GrainMix, 0.0f);
                    report ((juce::String ("engine ") + "ABCDEF"[engine] + ", 8 unison voices + Textura, detune automated").toRawUTF8(), allocs, frees, bytes);

                    allocs = frees = bytes = 0;
                    host.play (juce::String ("engine ") + "ABCDEF"[engine] + " transpose", blocks, { 60 }, P::Transpose, -24.0f, 24.0f);
                    host.set (P::Transpose, 0.0f);
                    report ((juce::String ("engine ") + "ABCDEF"[engine] + ", Transponer automated").toRawUTF8(), allocs, frees, bytes);
                }

                // LOOP, each algorithm, with and without Warp, the key and the length automated.
                host.set (P::Engine, 0.0f);
                host.set (P::PlayMode, (float) P::PlayLoop);
                for (const int algo : { P::LoopAuto, P::LoopBeatsAlgo, P::LoopComplex, P::LoopComplexPro, P::LoopRubberBand })
                    for (const float warp : { 1.0f, 0.0f })
                    {
                        host.set (P::LoopAlgo, (float) algo);
                        host.set (P::LoopWarp, warp);
                        allocs = frees = bytes = 0;
                        host.play ("loop", blocks / 2, { 48, 60, 67 }, P::Transpose, -12.0f, 12.0f);
                        host.play ("loop length", blocks / 3, { 60 }, P::LoopBeats, 0.0f, 12.0f);
                        host.set (P::Transpose, 0.0f);
                        host.set (P::LoopBeats, 0.0f);
                        report ((juce::String ("LOOP algorithm ") + juce::String (algo) + (warp > 0.5f ? ", Warp" : ", no Warp")
                                 + ", key and length automated").toRawUTF8(), allocs, frees, bytes);
                    }
                host.set (P::LoopKind, (float) P::LoopSlice);
                allocs = frees = bytes = 0;
                host.play ("slice", blocks / 2, { 60, 61, 62, 63, 64, 65, 66, 67 }, P::Transpose, -12.0f, 12.0f);
                host.set (P::Transpose, 0.0f);
                report ("LOOP Slice, 8 keys, transpose automated", allocs, frees, bytes);
                host.set (P::LoopKind, (float) P::LoopNormal);
                host.set (P::PlayMode, (float) P::PlayOneShot);

                // The filter, each circuit, cutoff automated.
                host.set (P::Engine, 0.0f);
                host.set (P::FltOn, 1.0f);
                allocs = frees = bytes = 0;
                for (int circuit = 0; circuit < 5; ++circuit)
                    for (int type = 0; type < 8; ++type)
                    {
                        host.set (P::FltCircuit, (float) circuit);
                        host.set (P::FltType, (float) type);
                        host.set (P::FltDrive, 12.0f);
                        host.play ("filter", 12, { 48, 60 }, P::FltFreq, 100.0f, 12000.0f);
                    }
                host.set (P::FltOn, 0.0f);
                report ("filter, 5 circuits x 8 types, cutoff automated", allocs, frees, bytes);

                // Every effect type in the first slot (made on the message thread, then played).
                for (int type = 1; type < Fx::NumTypes; ++type)
                {
                    proc->setFxSlotType (0, type);
                    host.messageThread();
                    host.play ("fx warm-up", 2, {});
                    allocs = frees = bytes = 0;
                    host.play ("fx", blocks / 2, { 60, 67 }, P::fxMix (0), 0.0f, 1.0f);
                    report ((juce::String ("effect type ") + juce::String (type) + " in slot 1, mix automated").toRawUTF8(), allocs, frees, bytes);
                }
                proc->setFxSlotType (0, Fx::Empty);
                host.messageThread();

                // ARP · SEQ: a generated pattern (ratchets, swing), Arp over a chord and Seq, rate automated.
                {
                    StepSeq::Pattern pattern;
                    juce::Random seed (3);
                    StepSeq::generate (pattern, -1, 0.9f, seed);
                    pattern.value[StepSeq::Ratchet].fill (4.0f);
                    proc->setSeqPattern (pattern);
                    host.messageThread();
                    host.set (P::ArpOn, 1.0f);
                    host.set (P::ArpSwing, 60.0f);
                    host.set (P::ArpOctaves, 4.0f);
                    host.play ("arp warm-up", 2, {});
                    allocs = frees = bytes = 0;
                    for (int style = 0; style < 7; ++style)
                    {
                        host.set (P::ArpStyle, (float) style);
                        host.play ("arp", blocks / 2, { 48, 55, 60, 64, 67, 71 }, P::ArpRate, 0.0f, 11.0f);
                    }
                    host.set (P::ArpMode, 1.0f);
                    for (int dir = 0; dir < 4; ++dir)
                    {
                        host.set (P::SeqDir, (float) dir);
                        host.play ("seq", blocks / 2, { 60 }, P::ArpRate, 0.0f, 11.0f);
                    }
                    host.set (P::ArpOn, 0.0f);
                    host.play ("arp off", 4, {});
                    host.set (P::ArpMode, 0.0f);
                    report ("ARP/SEQ: 7 styles, 4 directions, ratchets x4, rate automated", allocs, frees, bytes);
                }

                // Polyphony beyond the voices: stealing.
                allocs = frees = bytes = 0;
                for (int k = 0; k < 6; ++k)
                    host.play ("stealing", 8, { 36 + k, 43 + k, 50 + k, 57 + k, 64 + k, 71 + k, 78 + k, 85 + k });
                report ("48 notes on 32 voices (stealing)", allocs, frees, bytes);

                std::printf ("  slowest block %.2f ms of %.2f ms (%s); %d of %d blocks over their time; average load %.1f %%\n",
                             host.worstMs, 1000.0 * block / rate, host.worstWhere.toRawUTF8(), host.overruns, host.played,
                             100.0 * host.totalMs / (host.played * 1000.0 * block / rate));
            }

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "SOME CHECKS FAILED", failures);
    return failures == 0 ? 0 : 1;
}
