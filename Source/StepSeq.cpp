#include "StepSeq.h"
#include <algorithm>

namespace StepSeq
{
LaneRange laneRange (int lane) noexcept
{
    switch (lane)
    {
        case On:       return { 0.0f, 1.0f, 1.0f, true };
        case Note:     return { -7.0f, 14.0f, 0.0f, true };        // degrees of the key
        case Octave:   return { -2.0f, 2.0f, 0.0f, true };
        case Velocity: return { 1.0f, 127.0f, 100.0f, true };
        case Gate:     return { 0.05f, 2.0f, 0.5f, false };        // of the step (or of a repeat)
        case Chance:   return { 0.0f, 1.0f, 1.0f, false };
        case Ratchet:  return { 1.0f, 4.0f, 1.0f, true };
        default:       return { 0.0f, 1.0f, 0.0f, false };
    }
}

const char* laneName (int lane) noexcept
{
    static const char* names[] = { "PASO", "NOTA", "OCTAVA", "VELOCIDAD", "GATE", "PROBABILIDAD", "REPETIR" };
    return lane >= 0 && lane < numLanes ? names[lane] : "";
}

void Pattern::clear() noexcept
{
    for (int l = 0; l < numLanes; ++l)
    {
        value[(size_t) l].fill (laneRange (l).def);
        length[(size_t) l] = maxSteps;
    }
}

juce::String Pattern::toString() const
{
    juce::String s ("v1");
    for (int l = 0; l < numLanes; ++l)
    {
        s << ";" << length[(size_t) l] << ":";
        for (int k = 0; k < maxSteps; ++k)
            s << (k > 0 ? "," : "") << juce::String (value[(size_t) l][(size_t) k], laneRange (l).whole ? 0 : 3);
    }
    return s;
}

Pattern Pattern::fromString (const juce::String& text)
{
    Pattern p;
    const auto parts = juce::StringArray::fromTokens (text, ";", "");
    if (parts.size() != numLanes + 1 || parts[0] != "v1")
        return p;
    for (int l = 0; l < numLanes; ++l)
    {
        const auto& part = parts[l + 1];
        const auto values = juce::StringArray::fromTokens (part.fromFirstOccurrenceOf (":", false, false), ",", "");
        if (values.size() != maxSteps)
            return Pattern();
        const auto r = laneRange (l);
        p.length[(size_t) l] = juce::jlimit (1, maxSteps, part.upToFirstOccurrenceOf (":", false, false).getIntValue());
        for (int k = 0; k < maxSteps; ++k)
        {
            float v = juce::jlimit (r.lo, r.hi, values[k].getFloatValue());
            p.value[(size_t) l][(size_t) k] = r.whole ? std::round (v) : v;
        }
    }
    return p;
}

void generate (Pattern& p, int lane, float density, juce::Random& rng)
{
    const auto pick = [&rng] (std::initializer_list<std::pair<float, float>> choices)     // (value, weight)
    {
        float total = 0.0f;
        for (const auto& c : choices)
            total += c.second;
        float x = rng.nextFloat() * total;
        for (const auto& c : choices)
            if ((x -= c.second) <= 0.0f)
                return c.first;
        return choices.begin()->first;
    };
    for (int l = 0; l < numLanes; ++l)
    {
        if (lane >= 0 && l != lane)
            continue;
        auto& v = p.value[(size_t) l];
        int degree = 0;
        for (int k = 0; k < maxSteps; ++k)
        {
            const bool downbeat = k % 4 == 0;
            switch (l)
            {
                case On:
                    // the beats more often than the steps between them; the first always
                    v[(size_t) k] = k == 0 || rng.nextFloat() < juce::jlimit (0.0f, 1.0f, density * (downbeat ? 1.4f : 0.9f)) ? 1.0f : 0.0f;
                    break;
                case Note:
                    // a walk over the chord's degrees (0, 2, 4, 7), now and then a step between
                    degree = k == 0 ? 0 : (int) pick ({ { 0.0f, 3.0f }, { 2.0f, 2.5f }, { 4.0f, 2.5f }, { 7.0f, 1.5f }, { (float) degree + 1.0f, 1.0f },
                                                       { (float) degree - 1.0f, 1.0f }, { 5.0f, 0.7f }, { -3.0f, 0.7f } });
                    v[(size_t) k] = (float) juce::jlimit (-7, 14, degree);
                    break;
                case Octave:   v[(size_t) k] = pick ({ { 0.0f, 8.0f }, { 1.0f, 1.2f }, { -1.0f, 0.8f } }); break;
                case Velocity: v[(size_t) k] = (float) juce::jlimit (1, 127, (downbeat ? 105 : 70) + rng.nextInt (26)); break;
                case Gate:     v[(size_t) k] = pick ({ { 0.25f, 2.0f }, { 0.5f, 3.0f }, { 0.8f, 2.0f }, { 1.0f, 1.0f } }); break;
                case Chance:   v[(size_t) k] = pick ({ { 1.0f, 7.0f }, { 0.75f, 1.5f }, { 0.5f, 1.0f } }); break;
                case Ratchet:  v[(size_t) k] = pick ({ { 1.0f, 10.0f }, { 2.0f, 1.2f }, { 3.0f, 0.5f } }); break;
                default: break;
            }
        }
    }
}

void SharedPattern::write (const Pattern& p) noexcept
{
    version.fetch_add (1, std::memory_order_acq_rel);       // odd: being written
    std::atomic_thread_fence (std::memory_order_release);
    data = p;
    std::atomic_thread_fence (std::memory_order_release);
    version.fetch_add (1, std::memory_order_acq_rel);
}

bool SharedPattern::read (Pattern& out) const noexcept
{
    for (int tries = 0; tries < 4; ++tries)
    {
        const unsigned before = version.load (std::memory_order_acquire);
        if (before & 1u)
            continue;
        Pattern copy = data;
        std::atomic_thread_fence (std::memory_order_acquire);
        if (version.load (std::memory_order_acquire) == before)
        {
            out = copy;
            return true;
        }
    }
    return false;
}

double rateBeats (int choice) noexcept
{
    static constexpr double beats[] = { 4.0, 2.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0, 0.375, 0.25, 1.0 / 6.0, 0.125 };
    return beats[juce::jlimit (0, (int) std::size (beats) - 1, choice)];
}

int moveInKey (int note, int degrees, int keyRoot, bool minor) noexcept
{
    if (degrees == 0)
        return note;
    static constexpr int major[7] = { 0, 2, 4, 5, 7, 9, 11 }, minorScale[7] = { 0, 2, 3, 5, 7, 8, 10 };
    const int* scale = minor ? minorScale : major;
    const int rel = ((note - keyRoot) % 12 + 12) % 12;
    int index = 0;
    for (int i = 0; i < 7; ++i)
        if (scale[i] <= rel)
            index = i;
    const int base = note - rel;                    // the key's root at or below the note
    const int d = index + degrees;
    const int octave = d >= 0 ? d / 7 : -((-d + 6) / 7);
    return base + 12 * octave + scale[d - 7 * octave];
}

//==============================================================================
void Player::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void Player::reset() noexcept
{
    heldCount = 0;
    keysDown = false;
    physicallyDown.fill (false);
    pendingCount = 0;
    sounding.fill (0);
    running = false;
    wasOn = false;
    wasHost = false;
    freeBeat = 0.0;
    lastPpqEnd = -1.0;
    arpIndex = 0;
    lastFired = std::numeric_limits<juce::int64>::min();
    for (auto& s : shownStep)
        s.store (-1, std::memory_order_relaxed);
    heldShown.store (0, std::memory_order_relaxed);
}

Player::Chord Player::lastChord() const noexcept
{
    Chord c;
    for (int tries = 0; tries < 8; ++tries)
    {
        const unsigned before = chordVersion.load (std::memory_order_acquire);
        if (before & 1u)
            continue;
        c = chordCopy;
        std::atomic_thread_fence (std::memory_order_acquire);
        if (chordVersion.load (std::memory_order_acquire) == before)
            return c;
    }
    return {};
}

void Player::keepChord() noexcept
{
    if (heldCount <= 0)
        return;
    chordVersion.fetch_add (1, std::memory_order_acq_rel);
    std::atomic_thread_fence (std::memory_order_release);
    chordCopy.count = heldCount;
    for (int i = 0; i < heldCount; ++i)
    {
        chordCopy.note[(size_t) i] = held[(size_t) i].note;
        chordCopy.velocity[(size_t) i] = held[(size_t) i].velocity;
    }
    std::atomic_thread_fence (std::memory_order_release);
    chordVersion.fetch_add (1, std::memory_order_acq_rel);
}

void Player::addPending (juce::int64 at, int note, bool on, int velocity) noexcept
{
    if (pendingCount < (int) pending.size())
        pending[(size_t) pendingCount++] = { at, note, on, velocity };
}

void Player::allOff (juce::MidiBuffer& out, int at) noexcept
{
    for (int n = 0; n < 128; ++n)
        if (sounding[(size_t) n] > 0)
            out.addEvent (juce::MidiMessage::noteOff (1, n), at);
    sounding.fill (0);
    pendingCount = 0;
}

int Player::arpNote (const Settings& s) noexcept
{
    // the chord, by pitch (or in the order played), over the octaves asked
    std::array<Held, 16> chord = held;
    const int n = heldCount;
    if (s.style == 4)
        std::sort (chord.begin(), chord.begin() + n, [] (const Held& a, const Held& b) { return a.order < b.order; });
    else
        std::sort (chord.begin(), chord.begin() + n, [] (const Held& a, const Held& b) { return a.note < b.note; });
    const int octaves = juce::jlimit (1, 4, s.octaves);
    const int total = n * octaves;
    const auto noteAt = [&] (int i) { return chord[(size_t) (i % n)].note + 12 * (i / n); };
    const juce::int64 i = arpIndex++;
    switch (s.style)
    {
        case 1:  return noteAt (total - 1 - (int) (i % total));                     // down
        case 2:                                                                      // up and down
        case 3:                                                                      // down and up
        {
            if (total == 1)
                return noteAt (0);
            const int period = 2 * total - 2;
            int m = (int) (i % period);
            m = m < total ? m : period - m;
            return noteAt (s.style == 2 ? m : total - 1 - m);
        }
        case 5:  return noteAt (rng.nextInt (total));                                // random
        default: return noteAt ((int) (i % total));                                  // up, as played
    }
}

void Player::playStep (juce::int64 step, juce::int64 atSample, double stepSamples, const Settings& s, const Pattern& p)
{
    const auto indexOf = [&] (int lane)
    {
        const int len = juce::jlimit (1, maxSteps, p.length[(size_t) lane]);
        const juce::int64 k = step < 0 ? 0 : step;
        int idx = (int) (k % len);
        const int dir = s.seq ? s.direction : 0;
        if (dir == 1)
            idx = len - 1 - idx;
        else if (dir == 2 && len > 1)
        {
            const int period = 2 * len - 2;
            const int m = (int) (k % period);
            idx = m < len ? m : period - m;
        }
        else if (dir == 3)
            idx = rng.nextInt (len);
        shownStep[(size_t) lane].store (idx, std::memory_order_relaxed);
        return idx;
    };
    int at[numLanes];
    for (int l = 0; l < numLanes; ++l)
        at[l] = indexOf (l);
    if (p.at (On, at[On]) < 0.5f || heldCount <= 0)
        return;
    if (rng.nextFloat() >= p.at (Chance, at[Chance]))
        return;
    int bases[16];
    int count = 0;
    if (! s.seq && s.style == 6)                    // the chord at once
        for (int i = 0; i < heldCount; ++i)
            bases[count++] = held[(size_t) i].note;
    else if (! s.seq)
        bases[count++] = arpNote (s);
    else
    {
        // the key pressed last transposes the phrase
        const Held* last = &held[0];
        for (int i = 1; i < heldCount; ++i)
            if (held[(size_t) i].order > last->order)
                last = &held[(size_t) i];
        bases[count++] = last->note;
    }
    const int degrees = (int) p.at (Note, at[Note]);
    const int octave = (int) p.at (Octave, at[Octave]);
    const int velocity = juce::jlimit (1, 127, (int) p.at (Velocity, at[Velocity]));
    const int repeats = juce::jlimit (1, 4, (int) p.at (Ratchet, at[Ratchet]));
    const double sub = stepSamples / repeats;
    const double length = std::max (0.005 * rate, (double) p.at (Gate, at[Gate]) * s.gate * sub);
    for (int r = 0; r < repeats; ++r)
    {
        const auto on = atSample + (juce::int64) std::llround (r * sub);
        const auto off = on + std::max ((juce::int64) 1, (juce::int64) std::llround (length));
        for (int b = 0; b < count; ++b)
        {
            const int note = moveInKey (bases[b], degrees, s.keyRoot, s.keyMinor) + 12 * octave;
            if (note < 0 || note > 127)
                continue;
            addPending (on, note, true, velocity);
            addPending (off, note, false, 0);
        }
    }
}

void Player::process (const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples, const Settings& s, const Pattern& pattern,
                      bool hostPlaying, double ppq, double hostBpm)
{
    out.clear();
    if (! s.on)
    {
        if (wasOn)
        {
            allOff (out, 0);
            reset();
        }
        for (const auto meta : in)
            out.addEvent (meta.getMessage(), meta.samplePosition);
        now += numSamples;
        return;
    }
    wasOn = true;
    const bool host = s.sync && hostPlaying && ppq >= -1.0e9 && hostBpm > 0.0;
    const double bpm = s.sync && hostBpm > 0.0 ? hostBpm : s.freeBpm;
    const double beatsPerSample = bpm / 60.0 / rate;
    const double sb = std::max (1.0 / 64.0, s.stepBeats);
    const double stepSamples = sb / beatsPerSample;
    const double swingBeats = juce::jlimit (0.0f, 1.0f, s.swing) * sb / 3.0;

    if (host != wasHost)
    {
        // into the DAW's bars, or out of them: the clock starts again where it is
        lastFired = std::numeric_limits<juce::int64>::min();
        freeBeat = 0.0;
        running = false;
        wasHost = host;
    }
    if (host && lastPpqEnd >= 0.0 && std::abs (ppq - lastPpqEnd) > 0.5 * sb)
    {
        // the DAW jumped (a loop, a click on the timeline): the notes out, and the steps from here
        for (int i = 0; i < pendingCount;)
            if (pending[(size_t) i].on)
                pending[(size_t) i] = pending[(size_t) --pendingCount];
            else
                ++i;
        lastFired = std::numeric_limits<juce::int64>::min();
    }
    if (! s.latch)
    {
        // Latch let go: only the keys down stay
        for (int i = 0; i < heldCount;)
            if (! physicallyDown[(size_t) held[(size_t) i].note])
                held[(size_t) i] = held[(size_t) --heldCount];
            else
                ++i;
    }

    // the clock over [from, to) of the block
    const auto runClock = [&] (int from, int to)
    {
        if (to <= from)
            return;
        if (heldCount <= 0)
        {
            running = false;
            return;
        }
        double b0;
        if (host)
            b0 = ppq + from * beatsPerSample;
        else
        {
            if (! running)
            {
                freeBeat = 0.0;
                lastFired = -1;          // step 0 at once
                arpIndex = 0;
            }
            b0 = freeBeat;
        }
        if (! running && host)
            arpIndex = 0;
        running = true;
        const double b1 = b0 + (to - from) * beatsPerSample;
        const auto first = (juce::int64) std::floor (b0 / sb) - 1;
        const auto last = (juce::int64) std::floor (b1 / sb);
        for (juce::int64 k = std::max (first, lastFired + 1); k <= last; ++k)
        {
            const double start = k * sb + ((k & 1) != 0 ? swingBeats : 0.0);
            if (start >= b1)
                break;
            // a step that began a moment ago (up to 30 ms: a chord pressed just after the beat) plays at once
            if (start < b0 - std::min (0.5 * sb, 0.03 * rate * beatsPerSample))
            {
                lastFired = k;          // long gone (a jump): not played late
                continue;
            }
            const int offset = juce::jlimit (from, to - 1, from + (int) std::llround ((start - b0) / beatsPerSample));
            playStep (k, now + offset, stepSamples, s, pattern);
            lastFired = k;
        }
        if (! host)
            freeBeat = b1;
    };

    int position = 0;
    for (const auto meta : in)
    {
        const auto msg = meta.getMessage();
        const int at = juce::jlimit (0, numSamples, meta.samplePosition);
        if (! msg.isNoteOnOrOff() && ! msg.isAllNotesOff() && ! msg.isAllSoundOff())
        {
            out.addEvent (msg, at);         // pitch bend, controllers, sustain: as they come
            continue;
        }
        runClock (position, at);
        position = std::max (position, at);
        if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            heldCount = 0;
            physicallyDown.fill (false);
            for (int i = 0; i < pendingCount;)
                if (pending[(size_t) i].on)
                    pending[(size_t) i] = pending[(size_t) --pendingCount];
                else
                    ++i;
            continue;
        }
        const int note = msg.getNoteNumber();
        if (msg.isNoteOn())
        {
            bool anyDown = false;
            for (const bool d : physicallyDown)
                anyDown = anyDown || d;
            if (s.latch && ! anyDown)
                heldCount = 0;              // a new chord after all keys were let go
            physicallyDown[(size_t) note] = true;
            bool known = false;
            for (int i = 0; i < heldCount; ++i)
                if (held[(size_t) i].note == note)
                {
                    held[(size_t) i].order = ++pressCounter;
                    known = true;
                }
            if (! known && heldCount < (int) held.size())
                held[(size_t) heldCount++] = { note, msg.getVelocity(), ++pressCounter };
            keepChord();
        }
        else
        {
            physicallyDown[(size_t) note] = false;
            if (! s.latch)
                for (int i = 0; i < heldCount; ++i)
                    if (held[(size_t) i].note == note)
                    {
                        held[(size_t) i] = held[(size_t) --heldCount];
                        break;
                    }
        }
        if (heldCount == 0)
        {
            // let go: the repeats not yet played are dropped; the notes playing end as they would
            for (int i = 0; i < pendingCount;)
                if (pending[(size_t) i].on)
                    pending[(size_t) i] = pending[(size_t) --pendingCount];
                else
                    ++i;
        }
    }
    runClock (position, numSamples);
    if (heldCount == 0)
        for (auto& st : shownStep)
            st.store (-1, std::memory_order_relaxed);
    heldShown.store (heldCount, std::memory_order_relaxed);

    // what falls in this block, in time order (at the same sample, the ends before the starts)
    std::sort (pending.begin(), pending.begin() + pendingCount,
               [] (const Pending& a, const Pending& b) { return a.at != b.at ? a.at < b.at : (! a.on && b.on); });
    const auto end = now + numSamples;
    int used = 0;
    for (; used < pendingCount && pending[(size_t) used].at < end; ++used)
    {
        const auto& e = pending[(size_t) used];
        if (e.note < 0)
            continue;
        const int at = (int) juce::jlimit ((juce::int64) 0, (juce::int64) numSamples - 1, e.at - now);
        auto& count = sounding[(size_t) e.note];
        if (e.on)
        {
            if (count > 0)
            {
                // the same note still playing: it ends here, and its own end later is dropped
                out.addEvent (juce::MidiMessage::noteOff (1, e.note), at);
                for (int j = used + 1; j < pendingCount; ++j)
                    if (! pending[(size_t) j].on && pending[(size_t) j].note == e.note)
                    {
                        pending[(size_t) j].note = -1;
                        break;
                    }
                count = 0;
            }
            out.addEvent (juce::MidiMessage::noteOn (1, e.note, (juce::uint8) e.velocity), at);
            ++count;
        }
        else if (count > 0)
        {
            out.addEvent (juce::MidiMessage::noteOff (1, e.note), at);
            --count;
        }
    }
    // keep the rest (the dropped ends out)
    int kept = 0;
    for (int i = used; i < pendingCount; ++i)
        if (pending[(size_t) i].note >= 0)
            pending[(size_t) kept++] = pending[(size_t) i];
    pendingCount = kept;
    now = end;
    lastPpqEnd = host ? ppq + numSamples * beatsPerSample : -1.0;
}
}
