#include "PluginProcessor.h"
#include <rubberband/RubberBandStretcher.h>
#include "PluginEditor.h"
#include "FxRack.h"
#include "FactoryPresets.h"
#include "MechanismData.h"
#include "Randomizer.h"

namespace
{
constexpr int currentStateVersion = 5;      // 2: "Inicio" is a fraction of the whole file; 3: engines renamed; 4: Soplo;
                                            // 5: the attack of a swelling note ends where it settles
}

ReanimaProcessor::ReanimaProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "REANIMA", P::createLayout())
{
    for (int i = 0; i < P::Count; ++i)
    {
        paramPtrs[(size_t) i] = apvts.getRawParameterValue (P::defs()[(size_t) i].id);
        jassert (paramPtrs[(size_t) i] != nullptr);
    }

    apvts.addParameterListener (P::defs()[P::Reverse].id, this);
    apvts.addParameterListener (P::defs()[P::Engine].id, this);
    apvts.addParameterListener (P::defs()[P::EnvMode].id, this);
    for (auto* param : getParameters())
        param->addListener (this);
    snapshot (committed);
    startTimerHz (10);
}

ReanimaProcessor::~ReanimaProcessor()
{
    stopTimer();
    apvts.removeParameterListener (P::defs()[P::Reverse].id, this);
    apvts.removeParameterListener (P::defs()[P::Engine].id, this);
    apvts.removeParameterListener (P::defs()[P::EnvMode].id, this);
    for (auto* param : getParameters())
        param->removeListener (this);
    ++latestLoad;
    loader.removeAllJobs (true, 60000);
    cancelPendingUpdate();
}

double ReanimaProcessor::getTailLengthSeconds() const
{
    // The release reaches -100 dB at 1.67 times its value; delays and reverbs ring on after it.
    double tail = std::max (paramValue (P::AmpR) * 0.001f, SynthEngine::minReshapedRelease) * 1.67 + 0.03;
    for (int slot = 0; slot < P::numFxSlots; ++slot)
    {
        const int type = juce::roundToInt (paramValue (P::fxType (slot)));
        if (type == Fx::Delay || type == Fx::Reverb)
            return tail + 20.0;
    }
    return tail;
}

void ReanimaProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);
    masterStage.prepare (sampleRate, std::max (64, samplesPerBlock));
    setLatencySamples (masterStage.latency());          // the output stage's linear-phase oversampling
    // K-weighting (ITU-R BS.1770): a shelf for the head, a high-pass for what the ear barely weighs.
    loudnessRate = sampleRate;
    kShelf.set (Dsp::Biquad::HighShelf, sampleRate, 1681.0f, 0.7071f, 4.0f);
    kHighPass.set (Dsp::Biquad::HighPass, sampleRate, 38.0f, 0.5f);
    kShelf.reset();
    kHighPass.reset();
    ParamSnapshot p;
    snapshot (p);
    engine.prepareFx (p);
}

void ReanimaProcessor::timerCallback()
{
    // A moment after an instance is made: if it is still empty, the demo.
    if (! demoSettled && --demoCountdown <= 0)
    {
        demoSettled = true;
        if (offerDemo && uiSample == nullptr && rawAudio == nullptr && ! loading.load() && sampleFile == juce::File())
            loadDemo();
    }

    // Creates effect modules for newly chosen rack types (never on the audio thread).
    ParamSnapshot p;
    snapshot (p);
    engine.prepareFx (p);

    commitHistory();
    updateSlices();
    serviceRubber();

    // The audio thread hands the replaced sample back here, to be freed away from it (once no note
    // plays it any more).
    const juce::SpinLock::ScopedTryLockType lock (swapLock);
    if (lock.isLocked() && ! hasPending)
    {
        keepLingering();
        pending.reset();
        if (! engine.previousInUse.load (std::memory_order_acquire))
            lingering.reset();
    }
}

// The loop stretched offline by Rubber Band R3 for one tempo and pitch: three rounds, the middle one kept, its
// seam closed. Runs on the background thread (and offline renders wait for it).
std::unique_ptr<RubberRender> makeRubberRender (std::shared_ptr<const SampleData> data, int length, double tempo, double pitch)
{
    using RB = RubberBand::RubberBandStretcher;
    const auto& buf = data->loopAudio.getNumSamples() >= data->length ? data->loopAudio : data->audio;
    const float* a = buf.getReadPointer (0);
    const float* b = buf.getReadPointer (std::min (1, buf.getNumChannels() - 1));
    const int total = 3 * length;
    std::vector<float> inL ((size_t) total), inR ((size_t) total);
    for (int i = 0; i < total; ++i)
    {
        inL[(size_t) i] = a[i % length];
        inR[(size_t) i] = b[i % length];
    }
    RB rb ((size_t) std::lround (data->sampleRate), 2, RB::OptionProcessOffline | RB::OptionEngineFiner | RB::OptionChannelsTogether,
           1.0 / tempo, pitch);
    rb.setExpectedInputDuration ((size_t) total);
    const float* in[2] = { inL.data(), inR.data() };
    rb.study (in, (size_t) total, true);
    const int outTotal = (int) std::lround (total / tempo);
    std::vector<float> outL ((size_t) outTotal + 16384, 0.0f), outR ((size_t) outTotal + 16384, 0.0f);
    int got = 0;
    for (int pos = 0; pos < total; pos += 8192)
    {
        const int n = std::min (8192, total - pos);
        const float* p[2] = { inL.data() + pos, inR.data() + pos };
        rb.process (p, (size_t) n, pos + n >= total);
        while (rb.available() > 0 && got < (int) outL.size())
        {
            float* w[2] = { outL.data() + got, outR.data() + got };
            got += (int) rb.retrieve (w, (size_t) std::min ((int) rb.available(), (int) outL.size() - got));
        }
    }
    auto render = std::make_unique<RubberRender>();
    render->length = std::max (1, (int) std::lround (length / tempo));
    render->l.assign (outL.begin() + render->length, outL.begin() + 2 * render->length);
    render->r.assign (outR.begin() + render->length, outR.begin() + 2 * render->length);
    // Its seam closed: R3's rounds are not exactly the loop's length apart (measured: 20 samples short at
    // 0.83x), so the end did not join the start (a click each round). Over its first 50 ms the render goes
    // from what follows its end to its own start (raised cosine): the end runs on into the start.
    const int seam = std::min (render->length / 4, (int) (0.05 * data->sampleRate));
    for (int i = 0; i < seam && 2 * render->length + i < (int) outL.size(); ++i)
    {
        const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) i / (float) seam);
        render->l[(size_t) i] = w * render->l[(size_t) i] + (1.0f - w) * outL[(size_t) (2 * render->length + i)];
        render->r[(size_t) i] = w * render->r[(size_t) i] + (1.0f - w) * outR[(size_t) (2 * render->length + i)];
    }
    render->tempo = tempo;
    render->pitch = pitch;
    render->sample = data.get();
    render->loopLength = length;
    return render;
}

// LOOP "Rubber Band": when a voice asks for a tempo and pitch the render does not have, the loop is stretched
// offline by Rubber Band R3 on a background thread (three rounds of it, the middle one kept, its seam closed),
// then handed to the engine; a replaced render is freed a second later.
void ReanimaProcessor::serviceRubber()
{
    const juce::ScopedLock lock (rubberLock);
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (auto it = rubberRetired.begin(); it != rubberRetired.end();)
        it = now - it->second > 1000.0 ? rubberRetired.erase (it) : it + 1;
    if (auto* done = rubberDone.exchange (nullptr))
    {
        std::unique_ptr<RubberRender> fresh (done);
        if (fresh->sample == uiSample.get())
        {
            engine.rubberReady.store (fresh.get(), std::memory_order_release);
            if (rubberCurrent != nullptr)
                rubberRetired.push_back ({ std::move (rubberCurrent), now });
            rubberCurrent = std::move (fresh);
        }
    }
    if (! engine.rubberWanted.exchange (false) || rubberBusy.load() || uiSample == nullptr)
        return;
    const double tempo = engine.rubberWantTempo.load(), pitch = engine.rubberWantPitch.load();
    const auto data = uiSample;
    const int length = std::max (1, data->loopInfo.length > 0 ? std::min (data->loopInfo.length, data->length) : data->length);
    if (rubberCurrent != nullptr && rubberCurrent->sample == data.get() && rubberCurrent->loopLength == length
        && std::abs (rubberCurrent->tempo - tempo) < 1.0e-6 && std::abs (rubberCurrent->pitch - pitch) < 1.0e-6)
        return;
    if (tempo < 0.05 || tempo > 20.0 || pitch < 0.05 || pitch > 20.0)
        return;
    rubberBusy = true;
    rubberPool.addJob ([this, data, tempo, pitch, length]
    {
        delete rubberDone.exchange (makeRubberRender (data, length, tempo, pitch).release());
        rubberBusy = false;
    });
}

double ReanimaProcessor::loopBeatsNow() const
{
    if (uiSample == nullptr)
        return 0.0;
    const auto& s = *uiSample;
    const int length = std::max (1, s.loopInfo.length > 0 ? std::min (s.loopInfo.length, s.length) : s.length);
    if (paramValue (P::LoopBpm) > 0.0f)
        return paramValue (P::LoopBpm) * (length / s.sampleRate) / 60.0;
    const int chosen = P::loopBeatChoices[(size_t) juce::jlimit (0, 12, (int) paramValue (P::LoopBeats))];
    return chosen > 0 ? chosen : s.loopInfo.beats;
}

// The slices of SLICE, worked out again when the sample or how they are found changes:
//  - Transitorios: the hits the analysis found, the weaker ones left out as Sensibilidad goes down, at least
//    30 ms apart (two hits closer than that are one: a flam, a hit and its echo);
//  - Rejilla: the bar divided as División says, from the loop's length in beats (a bar of 4); each line moves
//    back to a hit that begins up to 12 ms before it, so that no slice starts after its hit's attack;
//  - A mano: the markers the user placed (saved with the project).
// Always from the start, at most 128 (a key each from Primera nota).
void ReanimaProcessor::updateSlices()
{
    const auto data = uiSample;
    const auto marks = apvts.state.getProperty ("sliceMarks").toString();
    const auto orderText = apvts.state.getProperty ("sliceOrder").toString();
    const int by = (int) paramValue (P::SliceBy), division = (int) paramValue (P::SliceDivision);
    const float sensitivity = paramValue (P::SliceSensitivity);
    const double beats = loopBeatsNow();
    const juce::String signature = juce::String::toHexString ((juce::pointer_sized_int) data.get()) + "|" + juce::String (by) + "|"
                                 + juce::String (division) + "|" + juce::String (sensitivity, 3) + "|" + juce::String (beats, 4) + "|" + marks + "|" + orderText;
    if (signature == sliceSignature)
        return;
    sliceSignature = signature;
    std::vector<int> points { 0 };
    if (data != nullptr)
    {
        const auto& s = *data;
        const int length = std::max (1, s.loopInfo.length > 0 ? std::min (s.loopInfo.length, s.length) : s.length);
        const int minGap = std::max (1, (int) (0.03 * s.sampleRate));
        if (by == P::SliceManual)
        {
            for (const auto& t : juce::StringArray::fromTokens (marks, ",", ""))
                if (const int at = t.getIntValue(); at > 0 && at < length)
                    points.push_back (at);
        }
        else if (by == P::SliceGrid)
        {
            constexpr double perBeat[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 };
            const double count = std::max (1.0, (beats > 0.0 ? beats : 4.0) * perBeat[(size_t) juce::jlimit (0, 5, division)]);
            const int n = std::min (P::maxSlices, (int) std::round (count));
            const int back = (int) (0.012 * s.sampleRate);
            for (int k = 1; k < n; ++k)
            {
                int at = (int) std::round (k * (double) length / count);
                for (const auto& h : s.loopInfo.hits)
                    if (h.first <= at && at - h.first <= back)
                        at = h.first;
                points.push_back (at);
            }
        }
        else
        {
            // Only where the sound rises: its peak over the next 15 ms above the 10 ms before (+0.4 dB). A
            // spectral flux peak in a decaying tone (measured: a cut 45 ms after a hit, which moved every key
            // after it one slice back) falls there, and is no hit. 3 dB left a chord loop with 6 cuts of 134.
            const auto& buf = s.loopAudio.getNumSamples() >= s.length ? s.loopAudio : s.audio;
            const auto peak = [&] (int from, int to)
            {
                float m = 0.0f;
                for (int c = 0; c < buf.getNumChannels(); ++c)
                    for (int i = std::max (0, from); i < std::min (to, buf.getNumSamples()); ++i)
                        m = std::max (m, std::abs (buf.getSample (c, i)));
                return m;
            };
            const int before = (int) (0.010 * s.sampleRate), after = (int) (0.015 * s.sampleRate);
            const float threshold = 0.1f + 1.9f * (1.0f - sensitivity) * (1.0f - sensitivity);
            for (const auto& h : s.loopInfo.hits)
                if (h.second >= threshold && peak (h.first, h.first + after) >= 1.047f * peak (h.first - before, h.first))
                    points.push_back (h.first);
        }
        std::sort (points.begin(), points.end());
        std::vector<int> kept { 0 };
        for (const int at : points)
            if (at - kept.back() >= minGap && length - at >= minGap)
                kept.push_back (at);
        if (kept.size() > (size_t) P::maxSlices)
            kept.resize ((size_t) P::maxSlices);
        points = std::move (kept);
    }
    slicePoints = points;
    // The order of the slices on the keys (EDICIÓN): kept while it fits these slices, else the file's order.
    sliceOrder.clear();
    for (const auto& t : juce::StringArray::fromTokens (orderText, ",", ""))
        sliceOrder.push_back (t.getIntValue());
    std::vector<int> check (sliceOrder);
    std::sort (check.begin(), check.end());
    bool valid = check.size() == points.size();
    for (size_t k = 0; k < check.size() && valid; ++k)
        valid = check[k] == (int) k;
    if (! valid)
    {
        sliceOrder.resize (points.size());
        for (size_t k = 0; k < points.size(); ++k)
            sliceOrder[k] = (int) k;
    }
    engine.setSlices (points, sliceOrder);
}

void ReanimaProcessor::setSliceOrder (const std::vector<int>& order)
{
    juce::StringArray text;
    bool identity = true;
    for (size_t k = 0; k < order.size(); ++k)
    {
        text.add (juce::String (order[k]));
        identity = identity && order[k] == (int) k;
    }
    if (identity)
        apvts.state.removeProperty ("sliceOrder", nullptr);
    else
        apvts.state.setProperty ("sliceOrder", text.joinIntoString (","), nullptr);
    updateSlices();
}

// The slices as the keys play them in order (key k: slice order[k]), one after another, each as long as it is:
// the loop rebuilt in the order of EDICIÓN. Beats from the loop's start; the loop's tempo.
std::vector<ReanimaProcessor::SliceNote> ReanimaProcessor::sliceSequence (double& bpm, double& beats) const
{
    std::vector<SliceNote> notes;
    bpm = beats = 0.0;
    if (uiSample == nullptr || slicePoints.empty())
        return notes;
    const auto& s = *uiSample;
    const int length = s.loopInfo.length > 0 ? std::min (s.loopInfo.length, s.length) : s.length;
    const double seconds = length / s.sampleRate;
    beats = loopBeatsNow();
    if (beats <= 0.0)
        beats = 4.0 * std::max (1.0, std::round (seconds / 2.0));        // unknown: bars of 120 BPM
    bpm = beats * 60.0 / seconds;
    const int first = juce::roundToInt (paramValue (P::SliceStart));
    double at = 0.0;
    for (size_t k = 0; k < slicePoints.size() && first + (int) k <= 127; ++k)
    {
        const int slice = k < sliceOrder.size() ? sliceOrder[k] : (int) k;
        const int from = slicePoints[(size_t) slice];
        const int to = slice + 1 < (int) slicePoints.size() ? slicePoints[(size_t) slice + 1] : length;
        const double len = (to - from) / s.sampleRate * bpm / 60.0;
        notes.push_back ({ first + (int) k, at, len });
        at += len;
    }
    return notes;
}

// The loop as a MIDI clip: a note per slice (Primera nota upwards, the keys that play them), each where its slice
// falls in the loop and as long as it, at the loop's tempo in 4/4, the clip as long as the loop. Dragged into
// the DAW it plays the loop back through SLICE; its notes can then be moved, removed or copied.
juce::File ReanimaProcessor::writeSliceMidi() const
{
    double bpm = 0.0, beats = 0.0;
    const auto notes = sliceSequence (bpm, beats);
    if (notes.empty())
        return {};
    constexpr int ppq = 9600;       // a tick is a sample or two at 120 BPM: the slices fall where they were
    juce::MidiMessageSequence track;
    track.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::round (60000000.0 / bpm)), 0.0);
    track.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);
    for (const auto& n : notes)
    {
        const double from = std::round (n.beat * ppq), to = std::round ((n.beat + n.length) * ppq);
        // Full velocity: the loop rebuilt as loud and as bright as it is (Vel -> Amp and Fuerza -> brillo).
        track.addEvent (juce::MidiMessage::noteOn (1, n.key, (juce::uint8) 127), from);
        track.addEvent (juce::MidiMessage::noteOff (1, n.key), std::max (from + 1.0, to - 1.0));
    }
    track.addEvent (juce::MidiMessage::endOfTrack(), std::round (beats * ppq));
    track.updateMatchedPairs();
    juce::MidiFile file;
    file.setTicksPerQuarterNote (ppq);
    file.addTrack (track);
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Reanima MIDI");
    folder.createDirectory();
    const auto out = folder.getChildFile (juce::File::createLegalFileName (uiSample->name.isNotEmpty() ? uiSample->name : juce::String ("loop")) + " slices.mid");
    out.deleteFile();
    juce::FileOutputStream stream (out);
    if (! stream.openedOk() || ! file.writeTo (stream, 1))
        return {};
    return out;
}

// What the plug-in plays, rendered offline through its own engine (motors, envelopes, filters, effects and
// buses, modulation) and its output stage, at the rate asked:
//  - SLICE: the slices in the order of EDICIÓN, one after another (the MIDI clip), at the host's tempo if known;
//  - LOOP Normal: one loop held, as long as the loop at the tempo it plays (the host's with Warp and Tempo del
//    host, else its own);
//  - ONE SHOT: Do3 held for the sample's length, then its release (up to 4 s).
bool ReanimaProcessor::renderExport (const juce::File& out, int format, double rate, int bits, juce::String& error) const
{
    const auto data = uiSample;
    if (data == nullptr)
    {
        error = P::u8 ("No hay sample.");
        return false;
    }
    ParamSnapshot p;
    snapshot (p);
    const auto& s = *data;
    const double host = lastBpm.load();
    const bool looping = (int) p[P::PlayMode] == P::PlayLoop, slicing = looping && (int) p[P::LoopKind] == P::LoopSlice;
    const int length = s.loopInfo.length > 0 ? std::min (s.loopInfo.length, s.length) : s.length;
    struct Ev { double at; int key; bool on; };
    std::vector<Ev> events;
    double seconds = 0.0, tempo = host > 0.0 ? host : 120.0;
    if (slicing)
    {
        double bpm = 0.0, beats = 0.0;
        const auto notes = sliceSequence (bpm, beats);
        tempo = host > 0.0 ? host : bpm;
        for (const auto& n : notes)
        {
            events.push_back ({ n.beat * 60.0 / tempo, n.key, true });
            events.push_back ({ (n.beat + n.length) * 60.0 / tempo, n.key, false });
        }
        seconds = beats * 60.0 / tempo;
    }
    else if (looping)
    {
        const double beats = loopBeatsNow();
        const bool follow = p[P::LoopWarp] > 0.5f && p[P::LoopSync] > 0.5f && host > 0.0 && beats > 0.0;
        tempo = follow ? host : (beats > 0.0 ? beats * 60.0 / (length / s.sampleRate) : tempo);
        seconds = follow ? beats * 60.0 / host : length / s.sampleRate;
        events.push_back ({ 0.0, SynthEngine::rootKey, true });
        events.push_back ({ seconds, SynthEngine::rootKey, false });
    }
    else
    {
        const double held = (s.length - s.startPosition (p[P::StartPos])) / s.sampleRate;
        events.push_back ({ 0.0, SynthEngine::rootKey, true });
        events.push_back ({ held, SynthEngine::rootKey, false });
        seconds = held + std::min (4.0, p[P::AmpR] * 0.001 + 0.2);
    }
    std::sort (events.begin(), events.end(), [] (const Ev& a, const Ev& b) { return a.at < b.at || (a.at == b.at && ! a.on && b.on); });

    SynthEngine e;
    e.prepare (rate);
    e.setSample (data);
    e.prepareFx (p);
    std::array<float, Drone::curvePoints> curve;
    for (size_t i = 0; i < curve.size(); ++i)
        curve[i] = droneCurve[i].load (std::memory_order_relaxed);
    e.setDroneCurve (curve.data());
    e.setSlices (slicePoints, sliceOrder);
    constexpr int block = 256;
    std::unique_ptr<RubberRender> rubber;
    if (looping && ! slicing && (int) p[P::LoopAlgo] == P::LoopRubberBand && ! events.empty())
    {
        // Rubber Band: what tempo and pitch the note asks for (a first block), its render, then the export.
        SynthEngine probe;
        probe.prepare (rate);
        probe.setSample (data);
        probe.prepareFx (p);
        juce::AudioBuffer<float> b (2, block);
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, events.front().key, 1.0f), 0);
        probe.setTransport (false, 0.0);
        probe.process (b, m, p, tempo);
        if (probe.rubberWanted.load())
        {
            rubber = makeRubberRender (data, length, probe.rubberWantTempo.load(), probe.rubberWantPitch.load());
            e.rubberReady.store (rubber.get());
        }
    }
    MasterStage stage;
    stage.prepare (rate, block);
    const int latency = stage.latency();
    const int total = (int) std::ceil (seconds * rate);
    juce::AudioBuffer<float> result (2, total), buf (2, block);
    result.clear();
    size_t next = 0;
    // The engine runs on the latency of the output stage more, so the file starts with the first sample.
    for (int pos = 0; pos < total + latency; pos += block)
    {
        juce::MidiBuffer midi;
        while (next < events.size() && (int) std::llround (events[next].at * rate) < pos + block)
        {
            const auto& ev = events[next++];
            const int at = juce::jlimit (0, block - 1, (int) std::llround (ev.at * rate) - pos);
            midi.addEvent (ev.on ? juce::MidiMessage::noteOn (1, ev.key, 1.0f) : juce::MidiMessage::noteOff (1, ev.key), at);
        }
        e.setTransport (false, 0.0);
        buf.clear();
        e.process (buf, midi, p, tempo);
        stage.process (buf.getWritePointer (0), buf.getWritePointer (1), block);
        for (int i = 0; i < block; ++i)
        {
            const int to = pos + i - latency;
            if (to >= 0 && to < total)
            {
                result.setSample (0, to, buf.getSample (0, i));
                result.setSample (1, to, buf.getSample (1, i));
            }
        }
    }

    std::unique_ptr<juce::AudioFormat> fmt;
    if (format == 1)
        fmt = std::make_unique<juce::AiffAudioFormat>();
    else if (format == 2)
        fmt = std::make_unique<juce::FlacAudioFormat>();
    else
        fmt = std::make_unique<juce::WavAudioFormat>();
    if (format == 2)
        bits = std::min (bits, 24);
    out.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (out);
    if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
    {
        error = P::u8 ("No se puede escribir el archivo.");
        return false;
    }
    auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (bits);
    if (bits == 32 && format == 0)
        options = options.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = fmt->createWriterFor (stream, options);
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (result, 0, total))
    {
        error = P::u8 ("El formato no admite esos ajustes.");
        return false;
    }
    return true;
}

void ReanimaProcessor::setSliceMarks (std::vector<int> marks)
{
    std::sort (marks.begin(), marks.end());
    juce::StringArray text;
    for (const int m : marks)
        if (m > 0)
            text.add (juce::String (m));
    apvts.state.setProperty ("sliceMarks", text.joinIntoString (","), nullptr);
    if ((int) paramValue (P::SliceBy) != P::SliceManual)
        setParam (P::SliceBy, (float) P::SliceManual);
    updateSlices();
}

void ReanimaProcessor::setFxSlotType (int slot, int type)
{
    const auto& spec = Fx::spec (type);
    setParam (P::fxType (slot), (float) type);
    setParam (P::fxOn (slot), 1.0f);
    setParam (P::fxMix (slot), spec.defaultMix);
    for (int k = 0; k < P::fxParamsPerSlot; ++k)
        setParam (P::fxParam (slot, k), k < (int) spec.params.size() ? Fx::toNorm (spec.params[(size_t) k], spec.params[(size_t) k].def) : 0.5f);
    timerCallback();
}

void ReanimaProcessor::moveFxSlot (int fromSlot, int toChain, int toIndex)
{
    using State = std::array<float, P::fxSlotStride>;
    auto read = [this] (int slot)
    {
        State s {};
        for (int j = 0; j < P::fxSlotStride; ++j)
            s[(size_t) j] = paramValue (P::fxType (slot) + j);
        return s;
    };
    State empty {};
    empty[0] = 0.0f; empty[1] = 1.0f; empty[2] = 1.0f;
    for (int j = 3; j < P::fxSlotStride; ++j)
        empty[(size_t) j] = 0.5f;

    std::array<std::vector<State>, 3> chains;
    int fromChain = 0, fromIndex = 0;
    for (int c = 0; c < 3; ++c)
    {
        for (int i = 0; i < FxRack::chainSize (c); ++i)
        {
            const int slot = FxRack::chainStart (c) + i;
            chains[(size_t) c].push_back (read (slot));
            if (slot == fromSlot) { fromChain = c; fromIndex = i; }
        }
    }

    toChain = juce::jlimit (0, 2, toChain);
    toIndex = juce::jlimit (0, FxRack::chainSize (toChain) - 1, toIndex);
    if (fromChain == toChain && fromIndex == toIndex)
        return;

    auto& src = chains[(size_t) fromChain];
    auto& dst = chains[(size_t) toChain];
    const State moving = src[(size_t) fromIndex];

    auto hasEmpty = [] (const std::vector<State>& v) { for (const auto& s : v) if (s[0] < 0.5f) return true; return false; };

    if (fromChain != toChain && ! hasEmpty (dst))
    {
        std::swap (src[(size_t) fromIndex], dst[(size_t) toIndex]);   // full chain: swap places
    }
    else
    {
        src.erase (src.begin() + fromIndex);
        src.push_back (empty);
        dst.insert (dst.begin() + toIndex, moving);
        // Keep the chain size: drop an empty slot (the last one) to make room.
        for (int i = (int) dst.size() - 1; i >= 0 && (int) dst.size() > FxRack::chainSize (toChain); --i)
            if (dst[(size_t) i][0] < 0.5f)
                dst.erase (dst.begin() + i);
        dst.resize ((size_t) FxRack::chainSize (toChain));
    }

    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < FxRack::chainSize (c); ++i)
        {
            const int slot = FxRack::chainStart (c) + i;
            const auto& s = chains[(size_t) c][(size_t) i];
            for (int j = 0; j < P::fxSlotStride; ++j)
                if (std::abs (paramValue (P::fxType (slot) + j) - s[(size_t) j]) > 1.0e-6f)
                    setParam (P::fxType (slot) + j, s[(size_t) j]);
        }
    timerCallback();
}

bool ReanimaProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void ReanimaProcessor::snapshot (ParamSnapshot& out) const
{
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = paramPtrs[i]->load (std::memory_order_relaxed);
}

void ReanimaProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    {
        const juce::SpinLock::ScopedTryLockType lock (swapLock);
        if (lock.isLocked() && hasPending)
        {
            // The old sample is released on the message thread.
            pending = pendingSameSound && engine.canUpgrade() ? engine.upgradeSample (std::move (pending))
                                                              : engine.setSample (std::move (pending));
            hasPending = false;
        }
    }

    double bpm = 0.0;           // 0: the host gave none (the engine then takes 120 for its LFOs, and a loop keeps its own)
    bool playing = false;
    double ppq = 0.0;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            if (const auto b = pos->getBpm())
                bpm = *b;
            playing = pos->getIsPlaying();
            if (const auto q = pos->getPpqPosition())
                ppq = *q;
            else if (const auto t = pos->getTimeInSeconds())
                ppq = *t * (bpm > 0.0 ? bpm : 120.0) / 60.0;
        }
    engine.setTransport (playing, ppq);
    lastBpm.store (bpm);

    snapshot (audioParams);
    std::array<float, Drone::curvePoints> curve;
    for (size_t i = 0; i < curve.size(); ++i)
        curve[i] = droneCurve[i].load (std::memory_order_relaxed);
    engine.setDroneCurve (curve.data());
    engine.process (buffer, midi, audioParams, bpm);
    if (isNonRealtime() && engine.rubberWanted.load())
    {
        // An offline render (bounce, freeze, the DAW's export) runs faster than the background render: it
        // waits for it here (allowed offline), so that what is written is Rubber Band from its first blocks.
        serviceRubber();
        for (int k = 0; k < 2000 && rubberBusy.load(); ++k)
            juce::Thread::sleep (5);
        serviceRubber();
    }
    matchLoudness (buffer);
    measureOutput (buffer);     // the meter shows the level before the output stage: 0 dB is where saturation starts
    saturateOutput (buffer);
}

// The output stage (MasterStage): clean up to the meter's 0 dB, saturation from 0 to +6 dB, never past -1 dBFS,
// four times oversampled.
void ReanimaProcessor::saturateOutput (juce::AudioBuffer<float>& buffer) noexcept
{
    if (buffer.getNumChannels() >= 2)
        masterStage.process (buffer.getWritePointer (0), buffer.getWritePointer (1), buffer.getNumSamples());
    else if (buffer.getNumChannels() == 1)
    {
        // A mono bus: the same stage on one channel (the other side runs on a copy and is dropped).
        auto* x = buffer.getWritePointer (0);
        for (int start = 0; start < buffer.getNumSamples(); start += monoScratchSize)
        {
            const int n = std::min (monoScratchSize, buffer.getNumSamples() - start);
            std::copy (x + start, x + start + n, monoScratch.begin());
            masterStage.process (x + start, monoScratch.data(), n);
        }
    }
}

// A/B at the same loudness: the loudness of what each setting plays is followed (K-weighted mean
// square over a few seconds, only while something sounds), and the louder one is turned down to
// the other's. Never up: matching never takes the output nearer to clipping.
void ReanimaProcessor::matchLoudness (juce::AudioBuffer<float>& buffer) noexcept
{
    const int n = buffer.getNumSamples();
    if (n == 0 || buffer.getNumChannels() < 2)
        return;

    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float l = kHighPass.process (0, kShelf.process (0, buffer.getSample (0, i)));
        const float r = kHighPass.process (1, kShelf.process (1, buffer.getSample (1, i)));
        sum += (double) l * l + (double) r * r;
    }
    const float meanSquare = (float) (sum / n);
    const int slot = audioSlot.load (std::memory_order_relaxed);
    if (meanSquare > 1.0e-7f)                       // -70 dB: silence does not count
    {
        auto& loudness = slotLoudness[(size_t) slot];
        const float old = loudness.load (std::memory_order_relaxed);
        const float a = 1.0f - std::exp (-(float) n / (3.0f * (float) loudnessRate));
        loudness.store (old <= 0.0f ? meanSquare : old + (meanSquare - old) * a, std::memory_order_relaxed);
    }

    float target = 1.0f;
    const float mine = slotLoudness[(size_t) slot].load (std::memory_order_relaxed);
    const float other = slotLoudness[(size_t) (1 - slot)].load (std::memory_order_relaxed);
    if (matchLevels.load (std::memory_order_relaxed) && mine > 0.0f && other > 0.0f)
        target = std::min (1.0f, std::sqrt (other / mine));
    matchGain.store (target, std::memory_order_relaxed);

    if (target != 1.0f || appliedMatch != 1.0f)
    {
        buffer.applyGainRamp (0, 0, n, appliedMatch, target);
        buffer.applyGainRamp (1, 0, n, appliedMatch, target);
    }
    appliedMatch = target;
}

void ReanimaProcessor::compareSelect (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    commitHistory();
    ParamSnapshot now;
    snapshot (now);
    const auto curve = currentCurve();

    if (! slotsUsed)
    {
        slots = { now, now };
        slotCurves = { curve, curve };
        slotLoudness[0].store (0.0f);
        slotLoudness[1].store (0.0f);
        slotsUsed = true;
    }
    if (slot == activeSlot)
        return;

    // What was done in this setting stays in it; the other comes back as one step of the history.
    slots[(size_t) activeSlot] = now;
    slotCurves[(size_t) activeSlot] = curve;
    Step step;
    step.before = now;
    step.after = slots[(size_t) slot];
    step.curveBefore = curve;
    step.curveAfter = slotCurves[(size_t) slot];
    pushStep (step);
    restore (history.back(), true);

    activeSlot = slot;
    audioSlot.store (slot);
}

void ReanimaProcessor::measureOutput (const juce::AudioBuffer<float>& buffer) noexcept
{
    // With the editor closed nobody empties the sums, so they stop growing.
    constexpr int maxSamples = 1 << 20;
    const int n = buffer.getNumSamples();
    const bool sum = meterSamples.load (std::memory_order_relaxed) < maxSamples;

    for (int ch = 0; ch < std::min (2, buffer.getNumChannels()); ++ch)
    {
        const float peak = buffer.getMagnitude (ch, 0, n);
        if (peak > meterPeak[(size_t) ch].load (std::memory_order_relaxed))
            meterPeak[(size_t) ch].store (peak, std::memory_order_relaxed);

        if (sum)
        {
            const float rms = buffer.getRMSLevel (ch, 0, n);
            meterSquares[(size_t) ch].fetch_add (rms * rms * (float) n, std::memory_order_relaxed);
        }
    }

    if (sum)
        meterSamples.fetch_add (n, std::memory_order_relaxed);
}

ReanimaProcessor::MeterReading ReanimaProcessor::takeMeterReading()
{
    MeterReading reading;
    const int n = meterSamples.exchange (0);
    for (size_t ch = 0; ch < 2; ++ch)
    {
        reading.peak[ch] = meterPeak[ch].exchange (0.0f);
        const float squares = meterSquares[ch].exchange (0.0f);
        reading.meanSquare[ch] = n > 0 ? squares / (float) n : 0.0f;
    }
    return reading;
}

//==============================================================================
void ReanimaProcessor::loadSample (const juce::File& file)
{
    commitHistory (true);       // what was done just before is its own step: Deshacer brings it back whole
    Load load;
    rememberCurrentSample (load);
    load.fresh = true;
    edited = false;
    load.file = file;
    load.name = file.getFileNameWithoutExtension();
    // A new sample plays forward: Reverse belongs to the sample it was set for (or to a preset).
    setParam (P::Reverse, 0.0f);
    load.reverse = false;
    startLoad (std::move (load));
}

bool ReanimaProcessor::loadSampleNow (const juce::File& file)
{
    demoSettled = true;
    history.clear();
    historyPos = 0;
    edited = false;

    Load load;
    load.id = ++latestLoad;
    load.file = file;
    load.name = file.getFileNameWithoutExtension();
    setParam (P::Reverse, 0.0f);
    load.reverse = false;
    wantedReverse = false;
    load.forAllEngines = needsAllEngines();
    sampleFile = file;
    runLoad (load);
    const bool ok = load.data != nullptr;
    finishLoad (load);
    return ok;
}

void ReanimaProcessor::loadDemo()
{
    // The WAV inside the plugin (Resources/Demo): nothing to find on disk, the same on Windows and macOS.
    // If it is not there or cannot be read, the instance simply stays empty.
    int size = 0;
    const char* data = MechanismData::getNamedResource ("demo_pluck_wav", size);
    if (data == nullptr || size <= 0)
        return;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::MemoryInputStream (data, (size_t) size, false), true));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > 60 * (juce::int64) reader->sampleRate)
        return;
    auto audio = std::make_shared<juce::AudioBuffer<float>> ((int) std::max (2u, reader->numChannels), (int) reader->lengthInSamples);
    reader->read (audio.get(), 0, (int) reader->lengthInSamples, 0, true, true);
    if (reader->numChannels == 1)
        audio->copyFrom (1, 0, *audio, 0, 0, audio->getNumSamples());

    Load load;
    load.audio = std::move (audio);
    load.rate = reader->sampleRate;
    load.name = juce::String::fromUTF8 (demoName);
    startLoad (std::move (load));
}

void ReanimaProcessor::startLoad (Load load)
{
    demoSettled = true;
    load.id = ++latestLoad;
    load.forAllEngines = needsAllEngines();
    loading = true;
    wantedReverse = load.reverse;
    if (load.file != juce::File())
        sampleFile = load.file;     // saved with the project even if it is saved before the analysis ends

    loader.addJob ([this, load = std::move (load)]() mutable
    {
        if (load.id != latestLoad.load())
            return;

        runLoad (load);

        const juce::ScopedLock lock (finishedLock);
        finished = std::make_unique<Load> (std::move (load));
        triggerAsyncUpdate();
    });
}

void ReanimaProcessor::runLoad (Load& load)
{
    if (load.audio == nullptr)
    {
        // A project brings its own copy; the file is only read when there is none (or it is damaged).
        auto audio = std::make_shared<juce::AudioBuffer<float>>();
        const bool ok = (load.encoded.isNotEmpty() && decodeAudio (load.encoded, *audio, load.rate))
                     || SampleAnalysis::loadFile (load.file, *audio, load.rate);
        load.encoded = {};
        if (! ok)
            return;
        load.audio = std::move (audio);
    }

    load.data = SampleAnalysis::analyse (*load.audio, load.rate, load.name, load.reverse, load.forAllEngines);
}

void ReanimaProcessor::finishLoad (Load& load)
{
    if (load.id != latestLoad.load())
        return;

    loading = false;

    if (load.data == nullptr)
    {
        if (load.file.existsAsFile())
            loadError = "No se pudo leer el archivo de audio.";
        sendChangeMessage();
        return;
    }

    if (load.record)
    {
        Step step;
        step.before = committed;
        step.curveBefore = committedCurve;
        step.audioBefore = load.before;
        step.rateBefore = load.rateBefore;
        step.nameBefore = load.nameBefore;
        step.fileBefore = load.fileBefore;
        step.editedBefore = load.editedBefore;
        step.audioAfter = load.audio;
        step.rateAfter = load.rate;
        step.nameAfter = load.name;
        step.fileAfter = sampleFile;
        step.editedAfter = edited;
        pushStep (std::move (step));
        extendLast = true;          // the values the new sample sets belong to this step
    }

    rawAudio = load.audio;
    rawRate = load.rate;
    if (load.fresh)
    {
        startFresh();
        apvts.state.removeProperty ("sliceMarks", nullptr);     // a new sample's slices are its own
        apvts.state.removeProperty ("sliceOrder", nullptr);
        if ((int) paramValue (P::SliceBy) == P::SliceManual)
            setParam (P::SliceBy, (float) P::SliceTransients);
    }
    install (load.data, load.applyDetected, load.sameSound);
    // A loop the analysis is sure of plays as a loop; in doubt the sample stays a one shot (the choice is the user's).
    if (load.fresh && load.data->loopInfo.likely)
    {
        setParam (P::PlayMode, (float) P::PlayLoop);
        knownPlayMode = P::PlayLoop;
        if (paramValue (P::EnvLock) < 0.5f)
            applyLoopGate();
    }
    encodeInBackground();

    // After an edit that moved the timing the note starts at the new onset and plays to the end.
    // An envelope that was still the detected one follows the new detection.
    if (load.edit)
    {
        const auto& d = *load.data;
        if (load.structural)
        {
            setParam (P::StartPos, d.length > 0 ? (float) (d.onset / d.length) : 0.0f);
            setParam (P::EndPos, 1.0f);
        }
        if (load.followDetected && paramValue (P::EnvLock) < 0.5f && (int) paramValue (P::EnvMode) != P::EnvSynth)
            applyDetected (d);
    }

    // A project from before the attack of a swelling note was read where it settles, with its envelope left
    // as detected then: the new detection, which still plays the file as it is.
    if (load.earlierDetection && load.data->hasEarlierDetected)
    {
        const auto& e = load.data->earlierDetected;
        auto same = [] (float a, float b) { return std::abs (a - b) <= 1.0e-3f * std::max (std::abs (a), std::abs (b)) + 1.0e-3f; };
        if (same (paramValue (P::AmpA), e.attack * 1000.0f) && same (paramValue (P::AmpCurve), e.curve)
            && same (paramValue (P::AmpD), e.decay * 1000.0f) && same (paramValue (P::AmpS), e.sustain)
            && same (paramValue (P::AmpFade), e.fadeDbPerSec))
        {
            const float release = paramValue (P::AmpR);
            applyDetected (*load.data);
            if (! same (release, e.release * 1000.0f))
                setParam (P::AmpR, release);        // a release of its own stays
        }
    }

    if (load.oldStart >= 0.0f && load.data->length > 0)
    {
        const auto& d = *load.data;
        setParam (P::StartPos, (float) ((d.onset + load.oldStart * (d.usableEnd - d.onset)) / d.length));
    }
}

juce::String ReanimaProcessor::takeLoadError()
{
    return std::exchange (loadError, {});
}

// Message thread, with swapLock held: while notes still play the analysis the audio thread handed back,
// it is kept here, so the audio thread never frees it.
void ReanimaProcessor::keepLingering()
{
    if (pending != nullptr && engine.previousInUse.load (std::memory_order_acquire) && lingering == nullptr)
        lingering = pending;
}

void ReanimaProcessor::install (std::shared_ptr<SampleData> data, bool applyDetectedValues, bool sameSound)
{
    uiSample = data;
    slotLoudness[0].store (0.0f);
    slotLoudness[1].store (0.0f);

    {
        const juce::SpinLock::ScopedLockType lock (swapLock);
        if (! hasPending)
            keepLingering();
        pending = data;
        pendingSameSound = sameSound;
        hasPending = true;
    }

    if (applyDetectedValues)
    {
        // Sintetizador keeps its ADSR: the sample's own envelope does not apply to it.
        if (paramValue (P::EnvLock) < 0.5f && (int) paramValue (P::EnvMode) != P::EnvSynth)
            applyDetected (*data);
        else
            setParam (P::TransientMs, data->transientMs);
        setParam (P::StartPos, data->length > 0 ? (float) (data->onset / data->length) : 0.0f);
    }

    sendChangeMessage();
}

void ReanimaProcessor::setParam (int index, float value)
{
    if (auto* param = apvts.getParameter (P::defs()[(size_t) index].id))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        param->endChangeGesture();
    }
}

void ReanimaProcessor::applyDetected (const SampleData& data)
{
    const auto& d = data.detected;
    setParam (P::AmpA, d.attack * 1000.0f);
    setParam (P::AmpCurve, d.curve);
    setParam (P::AmpD, d.decay * 1000.0f);
    setParam (P::AmpS, d.sustain);
    setParam (P::AmpFade, d.fadeDbPerSec);
    setParam (P::AmpR, d.release * 1000.0f);
    setParam (P::TransientMs, data.transientMs);
}

// A sample the user loads (Cargar, or dropped on the window) starts a session of its own: nothing of what
// shaped the sound before carries over. Every control that shapes the sound goes back to where it starts
// (engine A, Warp Re-Pitch, no reverse, no granular texture, no filter, modulation or effects, Transponer 0),
// and the sample's own envelope comes in after this (install), so the note plays the file as it is. The
// analysis of the sample and what it recognises (its note, its envelope, its instrument for Mecanismo) run as
// always; only what it would have inherited does not. What is the user's own stays: the output (Volumen,
// Pan, Mono, Graves mono), Snap a C, the way they play (Modo, Glide, Bend, aftertouch, vibrato, Vel → Amp) and
// Fijar ADSR (closed, the envelope stays too). A project or a preset never comes through here.
void ReanimaProcessor::startFresh()
{
    constexpr int keep[] = { P::Volume, P::Pan, P::MonoOut, P::MonoBass, P::SnapC, P::EnvLock,
                             P::PolyMode, P::Glide, P::BendRange, P::AtAmount, P::VibDepth, P::VibRate, P::VelAmp };
    constexpr int envelope[] = { P::EnvMode, P::AmpA, P::AmpCurve, P::AmpD, P::AmpS, P::AmpFade, P::AmpR };
    const bool lockEnvelope = paramValue (P::EnvLock) > 0.5f;
    const auto kept = [&] (int i)
    {
        return std::find (std::begin (keep), std::end (keep), i) != std::end (keep)
            || (lockEnvelope && std::find (std::begin (envelope), std::end (envelope), i) != std::end (envelope));
    };
    for (int i = 0; i < P::FxSlotBase; ++i)
        if (! kept (i) && i != P::Reverse)          // Reverse is already off (loadSample): a change would reload
            setParam (i, P::defs()[(size_t) i].def);
    for (int slot = 0; slot < P::numFxSlots; ++slot)
        if (paramValue (P::fxType (slot)) > 0.5f)
            setFxSlotType (slot, Fx::Empty);
    for (int point = 0; point < Drone::curvePoints; ++point)
        setDroneCurve (point, 0.0f);
    knownEnvMode = (int) paramValue (P::EnvMode);
    knownPlayMode = (int) paramValue (P::PlayMode);
    apvts.state.setProperty ("presetName", juce::String(), nullptr);
}

// A loop is gated: it starts at once and stops in 30 ms (Ataque and Release are its fades in LOOP).
void ReanimaProcessor::applyLoopGate()
{
    setParam (P::AmpA, 1.0f);
    setParam (P::AmpR, 30.0f);
}

// What a synthesizer patch starts from: a quick attack, a decay to a held level, a short release.
void ReanimaProcessor::applySynthEnvelope()
{
    setParam (P::AmpA, 5.0f);
    setParam (P::AmpCurve, 1.0f);
    setParam (P::AmpD, 600.0f);
    setParam (P::AmpS, 0.6f);
    setParam (P::AmpFade, 0.0f);
    setParam (P::AmpR, 350.0f);
}

void ReanimaProcessor::setDroneCurve (int point, float db) noexcept
{
    if (point >= 0 && point < Drone::curvePoints)
        droneCurve[(size_t) point].store (juce::jlimit (Drone::curveMinDb, Drone::curveMaxDb, db));
}

void ReanimaProcessor::resetEnvelopeToDetected()
{
    if (uiSample != nullptr)
        applyDetected (*uiSample);
}

//==============================================================================
juce::AudioBuffer<float> ReanimaProcessor::shownAudio() const
{
    const int n = rawAudio->getNumSamples();
    juce::AudioBuffer<float> out (rawAudio->getNumChannels(), n);
    const bool reversed = uiSample != nullptr && uiSample->reversed;
    for (int ch = 0; ch < out.getNumChannels(); ++ch)
    {
        const float* src = rawAudio->getReadPointer (ch);
        float* dst = out.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
            dst[i] = src[reversed ? n - 1 - i : i];
    }
    return out;
}

bool ReanimaProcessor::envelopeIsDetected() const
{
    if (uiSample == nullptr)
        return false;
    const auto& d = uiSample->detected;
    auto same = [] (float a, float b) { return std::abs (a - b) <= 1.0e-3f * std::max (std::abs (a), std::abs (b)) + 1.0e-3f; };
    return same (paramValue (P::AmpA), d.attack * 1000.0f) && same (paramValue (P::AmpD), d.decay * 1000.0f)
        && same (paramValue (P::AmpS), d.sustain) && same (paramValue (P::AmpR), d.release * 1000.0f);
}

void ReanimaProcessor::copyAudio (int from, int to)
{
    if (rawAudio == nullptr || uiSample == nullptr)
        return;
    const auto shown = shownAudio();
    from = juce::jlimit (0, shown.getNumSamples(), from);
    to = juce::jlimit (from, shown.getNumSamples(), to);
    if (to - from < 1)
        return;
    clipboard.setSize (shown.getNumChannels(), to - from);
    for (int ch = 0; ch < shown.getNumChannels(); ++ch)
        clipboard.copyFrom (ch, 0, shown, ch, from, to - from);
    clipboardRate = rawRate;
}

bool ReanimaProcessor::edit (Edit what, int from, int to, float amount)
{
    if (rawAudio == nullptr || uiSample == nullptr || loading.load())
        return false;

    auto shown = shownAudio();
    const int n = shown.getNumSamples(), channels = shown.getNumChannels();
    from = juce::jlimit (0, n, from);
    to = juce::jlimit (from, n, to);
    const int count = to - from;
    constexpr int shortest = 64;        // an edit never leaves less than this

    // A new buffer made of: the start, an insert, and the end.
    const auto join = [&] (const juce::AudioBuffer<float>* insert, int insertLength)
    {
        juce::AudioBuffer<float> out (channels, from + insertLength + (n - to));
        for (int ch = 0; ch < channels; ++ch)
        {
            out.copyFrom (ch, 0, shown, ch, 0, from);
            if (insert != nullptr && insertLength > 0)
                out.copyFrom (ch, from, *insert, std::min (ch, insert->getNumChannels() - 1), 0, insertLength);
            out.copyFrom (ch, from + insertLength, shown, ch, to, n - to);
        }
        return out;
    };

    bool structural = false;
    switch (what)
    {
        case Edit::Trim:
        {
            if (count < shortest)
                return false;
            juce::AudioBuffer<float> out (channels, count);
            for (int ch = 0; ch < channels; ++ch)
                out.copyFrom (ch, 0, shown, ch, from, count);
            shown = std::move (out);
            structural = true;
            break;
        }
        case Edit::Cut:
        case Edit::Delete:
            if (count < 1 || n - count < shortest)
                return false;
            if (what == Edit::Cut)
                copyAudio (from, to);
            shown = join (nullptr, 0);
            structural = true;
            break;

        case Edit::Paste:
        {
            if (! hasClipboard())
                return false;
            // Audio from a sample at another rate keeps its pitch.
            const double ratio = clipboardRate / rawRate;
            const int length = std::max (1, (int) std::round (clipboard.getNumSamples() / ratio));
            juce::AudioBuffer<float> clip (clipboard.getNumChannels(), length);
            for (int ch = 0; ch < clip.getNumChannels(); ++ch)
                for (int i = 0; i < length; ++i)
                {
                    const double at = i * ratio;
                    const int a = std::min ((int) at, clipboard.getNumSamples() - 1), b = std::min (a + 1, clipboard.getNumSamples() - 1);
                    const float f = (float) (at - a);
                    clip.setSample (ch, i, clipboard.getSample (ch, a) * (1.0f - f) + clipboard.getSample (ch, b) * f);
                }
            shown = join (&clip, length);
            structural = true;
            break;
        }
        case Edit::Silence:
            if (count < 1)
                return false;
            for (int ch = 0; ch < channels; ++ch)
                shown.clear (ch, from, count);
            break;

        case Edit::Reverse:
            if (count < 2)
                return false;
            for (int ch = 0; ch < channels; ++ch)
                std::reverse (shown.getWritePointer (ch) + from, shown.getWritePointer (ch) + to);
            break;

        case Edit::FadeIn:
        case Edit::FadeOut:
            if (count < 2)
                return false;
            for (int i = 0; i < count; ++i)
            {
                // A smooth curve: it leaves and arrives without a corner.
                const float x = (float) i / (float) (count - 1);
                const float rise = std::sin (x * Dsp::pi * 0.5f);
                const float g = what == Edit::FadeIn ? rise * rise : 1.0f - rise * rise;
                for (int ch = 0; ch < channels; ++ch)
                    shown.getWritePointer (ch)[from + i] *= g;
            }
            break;

        case Edit::Normalize:
        case Edit::Gain:
        {
            if (count < 1)
                return false;
            float gain = Dsp::dbToGain (amount);
            if (what == Edit::Normalize)
            {
                float peak = 0.0f;
                for (int ch = 0; ch < channels; ++ch)
                    peak = std::max (peak, shown.getMagnitude (ch, from, count));
                if (peak <= 1.0e-6f)
                    return false;
                gain = Dsp::dbToGain (-0.1f) / peak;
            }
            for (int ch = 0; ch < channels; ++ch)
                shown.applyGain (ch, from, count, gain);
            break;
        }
        case Edit::SeamlessLoop:
        {
            // The loop crossfade, written into the audio: what precedes the loop start fades in
            // under the end of the loop, so the jump back lands on the same sound.
            const int a = (int) (std::min (paramValue (P::LoopStart), paramValue (P::LoopEnd)) * n);
            const int b = (int) (std::max (paramValue (P::LoopStart), paramValue (P::LoopEnd)) * n);
            const int fade = std::min ({ (int) (paramValue (P::LoopXfade) * 0.001f * rawRate), (b - a) / 2, a });
            if (fade < 16)
                return false;
            const auto before = shownAudio();
            for (int ch = 0; ch < channels; ++ch)
            {
                float* d = shown.getWritePointer (ch);
                const float* s = before.getReadPointer (ch);
                for (int i = 0; i < fade; ++i)
                {
                    const float w = (float) i / (float) fade * Dsp::pi * 0.5f;
                    d[b - fade + i] = s[b - fade + i] * std::cos (w) + s[a - fade + i] * std::sin (w);
                }
            }
            setParam (P::LoopXfade, 0.0f);      // done in the audio now
            break;
        }
    }

    // Back to the order of the file.
    if (uiSample->reversed)
        for (int ch = 0; ch < channels; ++ch)
            std::reverse (shown.getWritePointer (ch), shown.getWritePointer (ch) + shown.getNumSamples());

    installEdit (std::make_shared<const juce::AudioBuffer<float>> (std::move (shown)), structural);
    return true;
}

void ReanimaProcessor::installEdit (Audio audio, bool structural)
{
    Load load;
    rememberCurrentSample (load);
    edited = true;
    load.audio = audio;
    load.rate = rawRate;
    load.name = uiSample != nullptr ? uiSample->name : sampleFile.getFileNameWithoutExtension();
    load.reverse = paramPtrs[P::Reverse]->load() > 0.5f;
    load.applyDetected = false;
    load.edit = true;
    load.structural = structural;
    load.followDetected = envelopeIsDetected();
    rawAudio = std::move (audio);
    startLoad (std::move (load));
}

void ReanimaProcessor::rememberCurrentSample (Load& load) const
{
    load.record = true;
    load.before = rawAudio;
    load.rateBefore = rawRate;
    load.nameBefore = uiSample != nullptr ? uiSample->name : juce::String();
    load.fileBefore = sampleFile;
    load.editedBefore = edited;
}

//==============================================================================
void ReanimaProcessor::parameterGestureChanged (int, bool starting)
{
    if (restoring)
        return;
    gestures += starting ? 1 : -1;
    if (gestures.load() < 0)
        gestures = 0;
    touched = true;
    lastGesture = juce::Time::getMillisecondCounterHiRes();
}

ReanimaProcessor::Curve ReanimaProcessor::currentCurve() const
{
    Curve c;
    for (size_t i = 0; i < c.size(); ++i)
        c[i] = droneCurve[i].load();
    return c;
}

// Once the hands are off the controls for a moment, what changed becomes one step. Changes
// nobody made by hand (automation, a project being opened) only move the starting point.
void ReanimaProcessor::commitHistory (bool atOnce)
{
    // Normally a step waits until loads end and knobs have been still for 250 ms (a drag is one step);
    // atOnce closes it right away, between two programmatic changes (two rolls of the dice).
    if (restoring || gestures.load() > 0
        || (! atOnce && (loading.load() || juce::Time::getMillisecondCounterHiRes() - lastGesture.load() < 250.0)))
        return;

    ParamSnapshot now;
    snapshot (now);
    const auto curve = currentCurve();

    if (extendLast && historyPos == (int) history.size() && ! history.empty())
    {
        history.back().after = now;
        history.back().curveAfter = curve;
    }
    else if (touched.load() && (now != committed || curve != committedCurve))
    {
        Step step;
        step.before = committed;
        step.after = now;
        step.curveBefore = committedCurve;
        step.curveAfter = curve;
        pushStep (std::move (step));
    }

    extendLast = false;
    touched = false;
    committed = now;
    committedCurve = curve;
}

void ReanimaProcessor::pushStep (Step step)
{
    constexpr size_t maxSteps = 100;
    history.resize ((size_t) historyPos);
    history.push_back (std::move (step));
    if (history.size() > maxSteps)
        history.erase (history.begin());
    historyPos = (int) history.size();
}

bool ReanimaProcessor::undo()
{
    commitHistory (true);       // what was just done is the step to undo, even within 250 ms
    if (historyPos == 0 || loading.load())
        return false;
    restore (history[(size_t) --historyPos], false);
    return true;
}

bool ReanimaProcessor::redo()
{
    commitHistory (true);       // what was just done is the step to undo, even within 250 ms
    if (historyPos == (int) history.size() || loading.load())
        return false;
    restore (history[(size_t) historyPos++], true);
    return true;
}

void ReanimaProcessor::restore (const Step& step, bool forward)
{
    restoring = true;
    const auto& values = forward ? step.after : step.before;
    const auto& curve = forward ? step.curveAfter : step.curveBefore;

    // Only what this step changed goes back: what moved since (automation, say) stays.
    if (step.before[P::EnvMode] != step.after[P::EnvMode])
        knownEnvMode = (int) values[P::EnvMode];    // the mode comes back with its own envelope
    if (step.before[P::PlayMode] != step.after[P::PlayMode])
        knownPlayMode = (int) values[P::PlayMode];
    for (int i = 0; i < P::Count; ++i)
        if (step.before[(size_t) i] != step.after[(size_t) i])
            setParam (i, values[(size_t) i]);
    if (step.curveBefore != step.curveAfter)
        for (int i = 0; i < Drone::curvePoints; ++i)
            setDroneCurve (i, curve[(size_t) i]);

    const auto& audio = forward ? step.audioAfter : step.audioBefore;
    if (audio != nullptr && audio != rawAudio)
    {
        Load load;
        load.audio = audio;
        load.rate = forward ? step.rateAfter : step.rateBefore;
        load.name = forward ? step.nameAfter : step.nameBefore;
        load.reverse = values[P::Reverse] > 0.5f;
        load.applyDetected = false;
        sampleFile = forward ? step.fileAfter : step.fileBefore;
        edited = forward ? step.editedAfter : step.editedBefore;
        rawAudio = audio;
        startLoad (std::move (load));
    }

    snapshot (committed);
    committedCurve = currentCurve();
    touched = false;
    restoring = false;
    sendChangeMessage();
}

// The edited audio travels inside the project as FLAC (24 bit, lossless at that depth), scaled
// down first if an edit took it over full scale.
juce::String ReanimaProcessor::encodeAudio (const juce::AudioBuffer<float>& audio, double rate)
{
    float peak = 0.0f;
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        peak = std::max (peak, audio.getMagnitude (ch, 0, audio.getNumSamples()));
    const float scale = peak > 0.999f ? peak / 0.999f : 1.0f;

    juce::AudioBuffer<float> scaled (audio);
    scaled.applyGain (1.0f / scale);

    juce::MemoryBlock block;
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (block, false);
        juce::FlacAudioFormat flac;
        const auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (audio.getNumChannels()).withBitsPerSample (24);
        auto writer = flac.createWriterFor (stream, options);
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (scaled, 0, scaled.getNumSamples()))
            return {};
    }
    return juce::String (scale, 6) + ":" + block.toBase64Encoding();
}

void ReanimaProcessor::encodeInBackground()
{
    {
        const juce::ScopedLock lock (savedLock);
        if (savedFor == rawAudio)
            return;
    }
    loader.addJob ([this, audio = rawAudio, rate = rawRate]
    {
        auto text = encodeAudio (*audio, rate);
        const juce::ScopedLock lock (savedLock);
        savedFor = audio;
        saved = std::move (text);
    });
}

// The current sample for a project: ready from the background, or made now if a save comes first.
juce::String ReanimaProcessor::savedAudio()
{
    if (rawAudio == nullptr)
        return {};
    const juce::ScopedLock lock (savedLock);
    if (savedFor != rawAudio)
    {
        saved = encodeAudio (*rawAudio, rawRate);
        savedFor = rawAudio;
    }
    return saved;
}

bool ReanimaProcessor::decodeAudio (const juce::String& text, juce::AudioBuffer<float>& audio, double& rate)
{
    const float scale = text.upToFirstOccurrenceOf (":", false, false).getFloatValue();
    juce::MemoryBlock block;
    if (scale <= 0.0f || ! block.fromBase64Encoding (text.fromFirstOccurrenceOf (":", false, false)))
        return false;

    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (new juce::MemoryInputStream (block, true), true));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;

    const int channels = (int) std::min<unsigned> (2u, reader->numChannels);
    audio.setSize (channels, (int) reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, channels > 1);
    audio.applyGain (scale);
    rate = reader->sampleRate;
    return true;
}

void ReanimaProcessor::parameterChanged (const juce::String&, float)
{
    triggerAsyncUpdate();   // may be called from the audio thread
}

void ReanimaProcessor::handleAsyncUpdate()
{
    std::unique_ptr<Load> done;
    {
        const juce::ScopedLock lock (finishedLock);
        done = std::move (finished);
    }
    if (done != nullptr)
        finishLoad (*done);

    // Switching between one shot and loop by hand: a loop starts at once and stops quickly (Ataque and
    // Release are its fades); back to one shot, the sample's own envelope.
    const int playMode = (int) paramPtrs[P::PlayMode]->load();
    if (playMode != knownPlayMode)
    {
        knownPlayMode = playMode;
        if (paramValue (P::EnvLock) < 0.5f && ! restoring)
        {
            if (playMode == P::PlayLoop)
                applyLoopGate();
            else if (uiSample != nullptr && (int) paramValue (P::EnvMode) != P::EnvSynth)
                applyDetected (*uiSample);
        }
    }

    // Switching the ADSR mode by hand moves the knobs to where each mode starts from.
    const int envMode = (int) paramPtrs[P::EnvMode]->load();
    if (envMode != knownEnvMode)
    {
        knownEnvMode = envMode;
        if (paramValue (P::EnvLock) < 0.5f)
        {
            if (envMode == P::EnvSynth)
                applySynthEnvelope();
            else if (uiSample != nullptr)
                applyDetected (*uiSample);
        }
    }

    if (loading.load() || uiSample == nullptr || rawAudio == nullptr)
        return;

    // Reverse needs a new analysis; so does leaving engine A with a sample analysed only for it.
    const bool reverse = paramPtrs[P::Reverse]->load() > 0.5f;
    const bool reversing = wantedReverse != reverse;
    if (! reversing && (uiSample->forAllEngines || ! needsAllEngines()))
        return;

    Load load;
    load.audio = rawAudio;
    load.rate = rawRate;
    load.name = uiSample->name;
    load.reverse = reverse;
    load.applyDetected = reversing;
    load.sameSound = ! reversing;       // only the other engines' part of the analysis is new
    startLoad (std::move (load));
}

bool ReanimaProcessor::needsAllEngines() const
{
    const int mode = (int) paramPtrs[P::Engine]->load();
    return mode != P::EngineRebuild && mode != P::EngineGranular;
}

//==============================================================================
void ReanimaProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("samplePath", sampleFile.getFullPathName(), nullptr);
    state.setProperty ("stateVersion", currentStateVersion, nullptr);

    // The sample travels inside the project: it opens the same on any computer.
    if (rawAudio != nullptr)
    {
        state.setProperty ("sampleAudio", savedAudio(), nullptr);
        state.setProperty ("sampleName", uiSample != nullptr ? uiSample->name : sampleFile.getFileNameWithoutExtension(), nullptr);
        state.setProperty ("sampleEdited", edited, nullptr);
    }

    juce::StringArray curve;
    for (const auto& point : droneCurve)
        curve.add (juce::String (point.load(), 2));
    state.setProperty ("droneCurve", curve.joinIntoString (","), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

//==============================================================================
juce::File ReanimaProcessor::getPresetFolder() const
{
    if (presetFolder != juce::File())
        return presetFolder;
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Blackpanda/Reanima/Presets");
}

bool ReanimaProcessor::deletePreset (const juce::File& file)
{
    if (! file.existsAsFile() || ! file.hasFileExtension (presetExtension) || file.getParentDirectory() != getPresetFolder())
        return false;
    return file.moveToTrash() || file.deleteFile();
}

juce::Array<juce::File> ReanimaProcessor::getPresets() const
{
    auto files = getPresetFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + presetExtension);
    files.sort();
    return files;
}

bool ReanimaProcessor::savePreset (const juce::String& name)
{
    auto clean = juce::File::createLegalFileName (name.trim());
    while (clean.contains ("  "))
        clean = clean.replace ("  ", " ");
    if (clean.isEmpty())
        return false;

    const auto folder = getPresetFolder();
    if (! folder.createDirectory())
        return false;

    apvts.state.setProperty ("presetName", clean, nullptr);
    juce::MemoryBlock state;
    getStateInformation (state);
    const auto xml = getXmlFromBinary (state.getData(), (int) state.getSize());
    if (xml == nullptr)
        return false;
    xml->removeAttribute ("welcomeShown");      // of this instance, not of the preset
    return xml->writeTo (folder.getChildFile (clean + presetExtension));
}

bool ReanimaProcessor::loadPreset (const juce::File& file)
{
    const auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return false;

    commitHistory (true);       // what was done just before is its own step, not part of the preset's
    xml->setAttribute ("presetName", file.getFileNameWithoutExtension());
    juce::MemoryBlock state;
    copyXmlToBinary (*xml, state);
    const bool pinned = snapPinned(), welcomed = welcomeShown();
    presetLoading = true;           // a preset is a step of the history; a project being opened is not
    setStateInformation (state.getData(), (int) state.getSize());
    presetLoading = false;
    apvts.state.setProperty ("snapPinned", pinned, nullptr);       // the user's, not the preset's
    apvts.state.setProperty ("welcomeShown", welcomed, nullptr);   // the instance's, not the preset's
    if (pinned)
        setParam (P::SnapC, 1.0f);
    touched = true;
    return true;
}

juce::String ReanimaProcessor::factoryPresetName (int index)
{
    return P::u8 ("Fábrica · ") + P::u8 (Factory::presets()[(size_t) index].name);
}

// Everything goes back to its default except what belongs to the sample (its start, end, loop and
// detected envelope) and to the mix (volume, pan, mono); then the template sets its own values.
void ReanimaProcessor::applyFactoryPreset (int index)
{
    const auto& preset = Factory::presets()[(size_t) juce::jlimit (0, (int) Factory::presets().size() - 1, index)];
    commitHistory (true);       // what was done just before is its own step, not part of the preset's
    applySetup (preset.values, preset.effects, preset.fx, false);
    knownEnvMode = (int) paramValue (P::EnvMode);     // the template's ADSR is not replaced
    knownPlayMode = (int) paramValue (P::PlayMode);
    apvts.state.setProperty ("presetName", factoryPresetName (index), nullptr);
    sendChangeMessage();
}

// A preset or the dice. Snap a C is the user's while pinned; Fijar ADSR keeps the envelope for the dice.
void ReanimaProcessor::applySetup (const std::vector<Factory::Setting>& values, const std::vector<std::pair<int, float>>& effects,
                                   const std::vector<Factory::FxSetting>& fx, bool keepEnvelope)
{
    constexpr int keep[] = { P::StartPos, P::EndPos, P::LoopStart, P::LoopEnd, P::Volume, P::Pan, P::MonoOut, P::EnvLock,
                             P::AmpA, P::AmpCurve, P::AmpD, P::AmpS, P::AmpFade, P::AmpR, P::TransientMs, P::MonoBass,
                             P::PlayMode, P::LoopWarp, P::LoopSync, P::LoopBeats, P::LoopAlgo,
                             P::LoopKind, P::LoopBpm, P::SliceBy, P::SliceDivision, P::SliceSensitivity, P::SliceStart,
                             P::LoopPlay, P::SliceThru, P::SliceFade, P::LoopHitEnv };
    constexpr int envelope[] = { P::EnvMode, P::AmpA, P::AmpCurve, P::AmpD, P::AmpS, P::AmpFade, P::AmpR };
    const auto kept = [&] (int i)
    {
        return std::find (std::begin (keep), std::end (keep), i) != std::end (keep)
            || (keepEnvelope && std::find (std::begin (envelope), std::end (envelope), i) != std::end (envelope));
    };
    const bool pinned = snapPinned();

    for (int i = 0; i < P::FxSlotBase; ++i)
        if (! kept (i))
            setParam (i, P::defs()[(size_t) i].def);
    if (uiSample != nullptr && ! keepEnvelope)
        applyDetected (*uiSample);

    for (const auto& [param, v] : values)
        if (! (keepEnvelope && kept (param)) && param != P::SnapC)
            setParam (param, v);
    // Snap a C: pinned by the user, always on; otherwise as the preset says.
    float snap = 0.0f;
    for (const auto& [param, v] : values)
        if (param == P::SnapC)
            snap = v;
    setParam (P::SnapC, pinned ? 1.0f : snap);

    for (int slot = 0; slot < P::numFxSlots; ++slot)
        if (paramValue (P::fxType (slot)) > 0.5f)
            setFxSlotType (slot, Fx::Empty);
    int slot = 0;
    for (const auto& [type, mix] : effects)
    {
        setFxSlotType (slot, type);
        setParam (P::fxMix (slot), mix);
        ++slot;
    }
    for (const auto& e : fx)
    {
        setFxSlotType (e.slot, e.type);
        setParam (P::fxMix (e.slot), e.mix);
        const auto& spec = Fx::spec (e.type);
        for (const auto& [k, v] : e.params)
            if (k >= 0 && k < (int) spec.params.size() && k < P::fxParamsPerSlot)
                setParam (P::fxParam (e.slot, k), Fx::toNorm (spec.params[(size_t) k], v));
    }
}

void ReanimaProcessor::setSnapPreference (bool on)
{
    apvts.state.setProperty ("snapPinned", on, nullptr);
    setParam (P::SnapC, on ? 1.0f : 0.0f);
}

// A note of the middle of the keyboard played now on a copy of the engine, with the effects: how loud it is
// (RMS over the note) and its peak.
std::pair<float, float> ReanimaProcessor::measure() const
{
    auto s = uiSample;
    if (s == nullptr)
        return { -200.0f, -200.0f };
    ParamSnapshot p;
    snapshot (p);
    const double rate = 48000.0;
    constexpr int block = 512;
    SynthEngine e;
    e.prepare (rate);
    e.setSample (s);
    e.prepareFx (p);
    juce::AudioBuffer<float> b (2, block);
    double sum = 0.0;
    float peak = 0.0f;
    const int blocks = (int) (2.0 * rate / block), off = (int) (1.4 * rate / block);
    for (int k = 0; k < blocks; ++k)
    {
        juce::MidiBuffer midi;
        if (k == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        if (k == off)
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        b.clear();
        e.process (b, midi, p, 120.0);
        for (int c = 0; c < 2; ++c)
        {
            const float* x = b.getReadPointer (c);
            for (int i = 0; i < block; ++i)
                sum += (double) x[i] * x[i];
        }
        peak = std::max (peak, b.getMagnitude (0, block));
    }
    const float rms = (float) std::sqrt (sum / (2.0 * blocks * block));
    return { juce::Decibels::gainToDecibels (rms, -200.0f), juce::Decibels::gainToDecibels (peak, -200.0f) };
}

void ReanimaProcessor::randomize()
{
    if (uiSample == nullptr)
        return;
    // What was done before is a step of its own, even while the analysis an earlier roll asked for is still
    // running (two quick rolls would otherwise be undone together).
    commitHistory (true);
    const bool keepEnvelope = paramValue (P::EnvLock) > 0.5f;

    // The reference: the sample itself, as "Fiel al sample" plays it.
    applySetup (Factory::presets()[0].values, {}, {}, keepEnvelope);
    const auto [refRms, refPeak] = measure();

    // B, C and D need more of the analysis than engine A: until it is there they cannot be heard (or measured),
    // so they are drawn less often and not measured.
    const bool analysed = uiSample->forAllEngines;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const int engines = analysed || dice.nextFloat() < 0.3f ? 0x3f : 0x31;
        const auto r = Randomizer::make (dice, keepEnvelope, engines);
        applySetup (r.values, {}, r.fx, keepEnvelope);
        const int chosen = (int) paramValue (P::Engine);
        if (! analysed && chosen != P::EngineRebuild && chosen != P::EngineGranular && chosen != P::EngineDrone)
            break;
        // As loud as the sample (within 18 dB), never peaking more than 3 dB over it; silence is thrown away.
        const auto [rms, peak] = measure();
        if (rms < refRms - 35.0f)
            continue;
        float gain = juce::jlimit (-18.0f, 18.0f, refRms - rms);
        gain = std::min (gain, refPeak + 3.0f - peak);
        setParam (P::SampleGain, juce::jlimit (-24.0f, 24.0f, gain));
        const auto [rms2, peak2] = measure();
        if (rms2 > refRms - 12.0f && peak2 < std::max (refPeak + 6.0f, -1.0f))
            break;
    }
    knownEnvMode = (int) paramValue (P::EnvMode);
    knownPlayMode = (int) paramValue (P::PlayMode);
    apvts.state.setProperty ("presetName", P::u8 ("Dado"), nullptr);
    touched = true;
    sendChangeMessage();
}

void ReanimaProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // A project (or a preset) is being opened: it decides what the instance holds, never the demo. A demo
    // still being analysed is dropped (unless this is a preset, which keeps the sample loaded).
    if (! demoSettled || (! presetLoading && rawAudio == nullptr && loading.load()))
    {
        ++latestLoad;
        loading = false;        // a load of its own, if the state brings a sample, says otherwise
    }
    demoSettled = true;
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    const int version = state.getProperty ("stateVersion", 1);

    // The sample comes with the state; the parameters do not need to carry it.
    const auto embedded = state.getProperty ("sampleAudio").toString();
    const auto name = state.getProperty ("sampleName").toString();
    const bool wasEdited = state.getProperty ("sampleEdited", false);
    for (const auto* id : { "sampleAudio", "sampleName", "sampleEdited" })
        state.removeProperty (id, nullptr);

    // Up to version 2 the engines were, in order: Reenvolvente, Híbrido, Resíntesis, Reconstrucción, Granular.
    if (version < 3)
        if (auto choice = state.getChildWithProperty ("id", P::defs()[P::Engine].id); choice.isValid())
        {
            constexpr int renamed[] = { P::EngineReenvelope, P::EngineHybrid, P::EngineResynth, P::EngineRebuild, P::EngineGranular };
            const int old = juce::jlimit (0, 4, (int) choice.getProperty ("value"));
            choice.setProperty ("value", renamed[old], nullptr);
        }
    // Up to version 3 "Del sample" was the fifth mechanism; Soplo now comes before it.
    if (version < 4)
        if (auto choice = state.getChildWithProperty ("id", P::defs()[P::MechType].id); choice.isValid())
            if ((int) choice.getProperty ("value") == 4)
                choice.setProperty ("value", 5, nullptr);
    apvts.replaceState (state);
    knownEnvMode = (int) paramPtrs[P::EnvMode]->load();
    knownPlayMode = (int) paramPtrs[P::PlayMode]->load();

    const auto curve = juce::StringArray::fromTokens (state.getProperty ("droneCurve").toString(), ",", "");
    for (int i = 0; i < Drone::curvePoints; ++i)
        setDroneCurve (i, i < curve.size() ? curve[i].getFloatValue() : 0.0f);

    // A template (a preset without a sample) keeps the sample that is loaded.
    const auto path = state.getProperty ("samplePath").toString();
    if (path.isEmpty() && embedded.isEmpty())
        return;

    Load load;
    if (presetLoading)
        rememberCurrentSample (load);
    else
    {
        history.clear();
        historyPos = 0;
    }
    edited = false;
    load.file = juce::File (path);
    load.name = name.isNotEmpty() ? name : load.file.getFileNameWithoutExtension();
    load.reverse = paramPtrs[P::Reverse]->load() > 0.5f;
    load.applyDetected = false;
    load.encoded = embedded;
    edited = wasEdited && embedded.isNotEmpty();
    if (version < 2)
        load.oldStart = paramPtrs[P::StartPos]->load();
    load.earlierDetection = version < 5;
    startLoad (std::move (load));
}

juce::AudioProcessorEditor* ReanimaProcessor::createEditor()
{
    return new ReanimaEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReanimaProcessor();
}
