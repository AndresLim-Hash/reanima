#include "Mechanism.h"
#include "MechanismData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

// The level of the bow's noise (its clips made equally loud past their bite), set so that on samples of the
// library it sounds as loud as it did before it came from real bows (mechmix.py, 150-800 ms).
static constexpr float bowLevel = 0.209f, bowRealLevel = 0.0343f;


namespace
{
using Library = MechanismLibrary;

float up (float x, float from, float to) noexcept { return juce::jlimit (0.0f, 1.0f, (x - from) / (to - from)); }

// The distance from a note to an interval as heard: the same note in another octave is 0.
float folded (float semitones) noexcept { return semitones - 12.0f * std::round (semitones / 12.0f); }

float hz (float midi) noexcept { return 440.0f * std::exp2 ((midi - 69.0f) / 12.0f); }

// Four-point Hermite: between the samples of a recording without dulling its top end.
float hermite (const float* x, int at, int last, float f) noexcept
{
    const float xm = x[std::max (0, at - 1)], x0 = x[at], x1 = x[std::min (last, at + 1)], x2 = x[std::min (last, at + 2)];
    const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

// How a type behaves: a "real" type as the gesture it is a version of (its level, envelope, release,
// continuous noise, pedal); only its strikes differ.
int behaviour (int type) noexcept
{
    switch (type)
    {
        case Mechanism::RealPiano:  return Mechanism::Piano;
        case Mechanism::RealPick:   return Mechanism::Pick;
        case Mechanism::RealBow:    return Mechanism::Bow;
        case Mechanism::RealBreath: return Mechanism::Breath;
        case Mechanism::RealGuitar: return Mechanism::Pick;
        default:                    return type;
    }
}

bool strikes (int type) noexcept
{
    const int b = behaviour (type);
    return b == Mechanism::Piano || b == Mechanism::Pick || b == Mechanism::Key;
}

// How a type takes the sample's register and colour. A real pluck is the whole attack of a harp or a
// pizzicato, low end included: cut and coloured as hard as the bass-pick recordings of Púa, it loses
// what makes it a pluck (measured on real notes: 9.8 dB from the real attack's colour with the piano's
// light treatment, 14.7 with Púa's). It is treated as the piano is.
int treatment (int type) noexcept
{
    return type == Mechanism::RealPick || type == Mechanism::RealGuitar ? Mechanism::Piano : behaviour (type);
}
}

//==============================================================================
const MechanismLibrary& MechanismLibrary::get()
{
    static const MechanismLibrary library;
    return library;
}

// index.csv says what each recording is: type, event, key, strength, file, tuned.
MechanismLibrary::MechanismLibrary()
{
    auto resource = [] (const juce::String& file, int& size) -> const char*
    {
        for (int i = 0; i < MechanismData::namedResourceListSize; ++i)
            if (file == MechanismData::originalFilenames[i])
                return MechanismData::getNamedResource (MechanismData::namedResourceList[i], size);
        size = 0;
        return nullptr;
    };

    int indexSize = 0;
    const char* index = resource ("index.csv", indexSize);
    if (index == nullptr)
        return;

    const juce::StringArray types { "piano", "pick", "bow", "key", "breath", "hammer", "pickreal", "bowreal", "breathreal", "guitarreal" },
                            events { "down", "up", "pedaldown", "pedalup" };
    juce::FlacAudioFormat flac;

    for (const auto& line : juce::StringArray::fromLines (juce::String::fromUTF8 (index, indexSize)))
    {
        const auto cells = juce::StringArray::fromTokens (line, ",", "");
        const int type = types.indexOf (cells[0]), event = events.indexOf (cells[1]);
        if (cells.size() < 5 || type < 0 || event < 0)
            continue;

        int size = 0;
        const char* data = resource (cells[4].trim(), size);
        if (data == nullptr)
            continue;

        std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (new juce::MemoryInputStream (data, (size_t) size, false), true));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            continue;

        Clip clip;
        clip.note = cells[2].getIntValue();
        clip.layer = cells[3].getIntValue();
        clip.tuned = cells[5].trim() == "1";
        clip.audio.setSize (2, (int) reader->lengthInSamples);
        reader->read (&clip.audio, 0, clip.audio.getNumSamples(), 0, true, true);
        if (reader->numChannels == 1)
            clip.audio.copyFrom (1, 0, clip.audio, 0, 0, clip.audio.getNumSamples());
        clip.peak = std::max (1.0e-6f, clip.audio.getMagnitude (0, clip.audio.getNumSamples()));
        sets[(size_t) type][(size_t) event].push_back (std::move (clip));
    }

    // Every clip made as loud as the others over its loudest 50 ms (a short click counts over
    // the whole 50 ms, as the ear hears it), so that the knob means the same for every one. A
    // clip far quieter than its set is mostly room: it is lifted no more than 6 dB past the rest.
    const int window = (int) (0.05 * clipRate), hop = window / 10;
    for (auto& byEvent : sets)
        for (auto& set : byEvent)
        {
            std::vector<float> loudness;
            for (const auto& c : set)
            {
                const int n = c.audio.getNumSamples();
                std::vector<double> energy ((size_t) n + 1, 0.0);
                for (int i = 0; i < n; ++i)
                {
                    const double a = c.audio.getSample (0, i), b = c.audio.getSample (1, i);
                    energy[(size_t) i + 1] = energy[(size_t) i] + 0.5 * (a * a + b * b);
                }
                double most = 0.0;
                for (int i = 0; i == 0 || i + window <= n; i += hop)
                    most = std::max (most, energy[(size_t) std::min (n, i + window)] - energy[(size_t) i]);
                loudness.push_back ((float) std::sqrt (most / window));
            }
            if (loudness.empty())
                continue;
            auto sorted = loudness;
            std::nth_element (sorted.begin(), sorted.begin() + (long) sorted.size() / 2, sorted.end());
            const float median = sorted[sorted.size() / 2];
            for (size_t i = 0; i < set.size(); ++i)
                set[i].gain = 1.0f / std::max ({ 1.0e-5f, loudness[i], 0.5f * median });

            // What goes on after the first 0.2 s (in a real bowed note, the bow alone), made as loud in
            // every clip: the bow's noise is taken from there, and its level must not depend on the
            // note that picks the clip (it ranged over 25 dB).
            std::vector<float> sustained;
            const int from = (int) (0.2 * clipRate);
            for (const auto& c : set)
            {
                const int n = c.audio.getNumSamples();
                double sum = 0.0;
                for (int i = from; i < n; ++i)
                    sum += 0.5 * ((double) c.audio.getSample (0, i) * c.audio.getSample (0, i) + (double) c.audio.getSample (1, i) * c.audio.getSample (1, i));
                sustained.push_back (n > from ? (float) std::sqrt (sum / (n - from)) : 0.0f);
            }
            auto sortedSustain = sustained;
            std::nth_element (sortedSustain.begin(), sortedSustain.begin() + (long) sortedSustain.size() / 2, sortedSustain.end());
            const float medianSustain = sortedSustain[sortedSustain.size() / 2];
            for (size_t i = 0; i < set.size(); ++i)
                set[i].sustainGain = 1.0f / std::max ({ 1.0e-5f, sustained[i], 0.5f * medianSustain });
        }

    // The moment each recording strikes (its first strong peak in 40 ms), and the colour of each
    // set: the power of its recordings, made equally loud, in the bands of the sample analysis.
    constexpr int order = 14, size = 1 << order;
    const juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) size * 2);
    const int strikeSpan = (int) (0.04 * clipRate);
    using X = SampleData::Excitation;
    for (size_t t = 0; t < sets.size(); ++t)
        for (size_t e = 0; e < (size_t) NumEvents; ++e)
        {
            std::array<double, numBands> power {};
            for (auto& c : sets[t][e])
            {
                const int n = c.audio.getNumSamples();
                const float* l = c.audio.getReadPointer (0);
                const float* r = c.audio.getReadPointer (1);
                // As the sample analysis finds the sample's strike: where it first reaches half
                // its height and climbs on to its first top (RMS over 1 ms, every quarter ms).
                const int w = (int) (0.001 * clipRate), hopW = w / 4;
                std::vector<float> level;
                for (int i = 0; i + w <= std::min (n, strikeSpan + w); i += hopW)
                {
                    float sum = 0.0f;
                    for (int j = i; j < i + w; ++j)
                        sum += (l[j] + r[j]) * (l[j] + r[j]);
                    level.push_back (sum);
                }
                size_t k = 0;
                if (! level.empty())
                {
                    const float top = *std::max_element (level.begin(), level.end());
                    while (k < level.size() && level[k] < 0.25f * top)
                        ++k;
                    while (k + 1 < level.size() && level[k + 1] >= level[k])
                        ++k;
                }
                const int from = (int) k * hopW;
                int at = std::min (n - 1, from);
                for (int j = from; j < std::min (n, from + w); ++j)
                    if (std::abs (l[j] + r[j]) > std::abs (l[at] + r[at]))
                        at = j;
                c.peakAt = std::min (n - 1, from + w / 2);
                c.peakSign = l[at] + r[at] < 0.0f ? -1.0f : 1.0f;

                std::fill (buf.begin(), buf.end(), 0.0f);
                const int len = std::min (n, size);
                for (int i = 0; i < len; ++i)
                    buf[(size_t) i] = 0.5f * (l[i] + r[i]) * c.gain * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) len));
                fft.performFrequencyOnlyForwardTransform (buf.data());
                for (int bin = 1; bin < size / 2; ++bin)
                {
                    const double f = bin * clipRate / size;
                    for (int b = 0; b < numBands; ++b)
                        if (f >= X::bandEdges[b] && f < X::bandEdges[b + 1])
                            power[(size_t) b] += (double) buf[(size_t) bin] * buf[(size_t) bin];
                }
            }
            double top = 0.0;
            for (const double p : power)
                top = std::max (top, p);
            float mean = 0.0f;
            for (int b = 0; b < numBands; ++b)
            {
                colours[t][e][(size_t) b] = (float) (10.0 * std::log10 (std::max (power[(size_t) b], top * 1.0e-6) + 1.0e-30));
                mean += colours[t][e][(size_t) b] / (float) numBands;
            }
            for (auto& c : colours[t][e])
                c -= mean;
        }
}

//==============================================================================
void Mechanism::prepare (double sr)
{
    rate = sr;
    const size_t longest = (size_t) (sr / 25.0) + 8;       // a comb down to 25 Hz
    combL.assign (longest, 0.0f);
    combR.assign (longest, 0.0f);
    grainLength = std::max (64, (int) (0.064 * sr));
    grainWindow.resize ((size_t) grainLength);
    for (int i = 0; i < grainLength; ++i)
        grainWindow[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) grainLength);
    reset();
}

void Mechanism::reset() noexcept
{
    for (auto& p : players)
        p.active = false;
    texture = false;
    toneLevel = 0.0f;
    std::fill (combL.begin(), combL.end(), 0.0f);
    std::fill (combR.begin(), combR.end(), 0.0f);
    combPos = 0;
    combWet = 0.0f;
    combLowL = combLowR = ringing = 0.0f;
}

bool Mechanism::busy() const noexcept
{
    for (const auto& p : players)
        if (p.active)
            return true;
    return texture || ringing > 1.0e-6f;
}

Mechanism::Player& Mechanism::freePlayer() noexcept
{
    for (auto& p : players)
        if (! p.active)
            return p;
    return players[0];
}

void Mechanism::keyDown (int type, float amount, const SampleData& s, const Note& note) noexcept
{
    heldFor = 0.0;
    if (amount <= 0.0f)
        return;
    if (type == FromSample)
    {
        playSample (true, amount, s, note);
        return;
    }
    if (type < 0 || type == FromSample || type >= NumTypes)
        return;

    setResonance (type, s, note);
    play (type, MechanismLibrary::Down, amount, s, note);

    // The bow on the string and the air in the tube go on for as long as the note sounds.
    const int kind = behaviour (type);
    if (kind == Bow || kind == Breath)
    {
        texture = true;
        textureType = type;
        textureNote = note;
        // The bow at the level it had before its noise came from real bows, measured on samples of the
        // library (mechmix.py, 150-800 ms): the same loudness, its character changed only.
        textureLevel = amount * amount * (kind == Breath ? 0.4f : (type == RealBow ? bowRealLevel : bowLevel));
        setChain (textureChain, type, kind == Breath ? MechanismLibrary::Up : MechanismLibrary::Down, s, note);
        for (auto& g : grains)
            g.clip = nullptr;
        sinceGrain = grainLength;
        nextGrain = 0;
    }
}

void Mechanism::keyUp (int type, float amount, const SampleData& s, const Note& note) noexcept
{
    if (amount <= 0.0f)
        return;
    if (type == FromSample)
        playSample (false, amount, s, note);
    else if (type >= 0 && type < NumTypes)
    {
        // A short note is one gesture: the pick, the bow or the breath that started it has hardly
        // left before it ends, and its ending is quieter. Dampers and jacks fall all the same.
        float scale = 1.0f;
        if (const int kind = behaviour (type); kind == Pick || kind == Bow || kind == Breath)
        {
            const float x = juce::jlimit (0.0f, 1.0f, (float) (heldFor / rate - 0.03) / 0.22f);
            scale = 0.3f + 0.7f * x * x * (3.0f - 2.0f * x);
        }
        play (type, MechanismLibrary::Up, amount, s, note, scale);
    }
}

void Mechanism::pedal (bool down, float amount, const SampleData& s, float gain) noexcept
{
    Note note;
    note.velocity = 0.8f;
    note.adapt = 0.0f;
    note.downGain = note.upGain = gain;
    if (amount > 0.0f)
        play (Piano, down ? MechanismLibrary::PedalDown : MechanismLibrary::PedalUp, amount, s, note);
}

const MechanismLibrary::Clip* Mechanism::choose (int type, int event, float velocity, float sounding, bool remember) noexcept
{
    const auto& set = MechanismLibrary::get().clips (type, event);
    if (set.empty())
        return nullptr;

    // How far a recording is from this note: first how far its note is, in any octave (a
    // semitone weighs two octaves), then how far its register is, a register under the note
    // twice as far as one over it (what is left of its note must not sound under the sample's).
    // The pedal has no note.
    const bool tuned = set.front().tuned;
    const auto cost = [tuned, sounding] (const Clip& c)
    {
        const float apart = (float) c.note - sounding;
        return tuned ? 24.0f * std::abs (folded (apart)) + std::abs (apart) / (apart < 0.0f ? 6.0f : 12.0f) : std::abs (apart);
    };
    const float slack = tuned ? 0.5f : 2.0f;

    // The nearest recordings, at the strength nearest to this one, and not the one heard last.
    float nearest = 1.0e9f;
    for (const auto& c : set)
        nearest = std::min (nearest, cost (c));
    int lowest = 9, highest = 0;
    for (const auto& c : set)
        if (cost (c) <= nearest + slack)
        {
            lowest = std::min (lowest, c.layer);
            highest = std::max (highest, c.layer);
        }
    const int layer = juce::jlimit (lowest, highest, lowest + juce::roundToInt (velocity * (float) (highest - lowest)));

    const Clip* candidates[64];
    int count = 0;
    for (const auto& c : set)
        if (cost (c) <= nearest + slack && c.layer == layer && count < 64)
            candidates[count++] = &c;
    if (count == 0)
        return nullptr;
    int pick = rng.nextInt (count);
    if (remember && count > 1 && candidates[pick] == last[type][event])
        pick = (pick + 1 + rng.nextInt (count - 1)) % count;
    if (remember)
        last[type][event] = candidates[pick];
    return candidates[pick];
}

// The register and the colour of the sample, given to a recording (see the class comment).
void Mechanism::setChain (Chain& chain, int type, int event, const SampleData& s, const Note& note) const noexcept
{
    chain.count = 0;
    chain.reset();
    if (event == MechanismLibrary::PedalDown || event == MechanismLibrary::PedalUp || note.adapt <= 0.0f)
        return;

    const float adapt = juce::jlimit (0.0f, 1.0f, note.adapt);
    const auto& e = s.excitation;
    const auto& from = MechanismLibrary::get().colour (type, event);
    type = treatment (type);

    // Nothing under the note (0.7 of its fundamental), or under the sample's lowest register.
    // A pick cuts harder and closer to the note (0.85, 36 dB per octave): its low thump is what
    // made it sound like a bass of its own. A piano keeps its thump: the key landing on the
    // keybed is part of a piano's sound, whatever note it plays.
    float floorHz = s.pitched ? (type == Pick ? 0.85f : 0.7f) * hz (note.sounding) : 0.8f * e.lowEdgeHz;
    floorHz = type == Piano ? 20.0f : juce::jlimit (20.0f, 1500.0f, floorHz);
    const float highPass = 20.0f * std::pow (floorHz / 20.0f, adapt);
    if (highPass > 25.0f)
        for (int i = 0; i < (type == Pick ? 3 : 1); ++i)
            chain.stage[(size_t) chain.count++].set (Dsp::Biquad::HighPass, rate, highPass, 0.707f);

    // The colour of the sample's attack, as much as Adaptar says and each gesture allows: a
    // breath that took all of a dark sample's colour would be no breath any more.
    // The piano and the key were already right as recorded: they take little.
    const float match = adapt * (type == Pick ? 0.85f : type == Bow ? 0.65f : type == Breath ? 0.4f : type == Key ? 0.35f : 0.2f);
    if (match < 0.01f)
        return;
    std::array<float, MechanismLibrary::numBands> gain {};
    for (size_t b = 0; b < gain.size(); ++b)
        gain[b] = juce::jlimit (-12.0f, 12.0f, match * (e.attackColour[b] - from[b]));

    // The recording keeps its power: the bands where its energy lies weigh in the correction.
    float weightSum = 0.0f, power = 0.0f;
    for (size_t b = 0; b < gain.size(); ++b)
    {
        const float w = std::pow (10.0f, from[b] / 10.0f);
        power += w * std::pow (10.0f, gain[b] / 10.0f);
        weightSum += w;
    }
    const float makeUp = -10.0f * std::log10 (std::max (1.0e-9f, power / std::max (1.0e-9f, weightSum)));

    static constexpr float centre[] { 177.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 5680.0f };
    for (size_t b = 0; b < gain.size(); ++b)
    {
        float g = juce::jlimit (-12.0f, 12.0f, gain[b] + makeUp);
        if (centre[b] < highPass)
            g = std::min (g, 0.0f);         // never lifting what the high-pass takes away
        if (std::abs (g) < 0.25f || chain.count >= maxEq)
            continue;
        const auto kind = b == 0 ? Dsp::Biquad::LowShelf : b + 1 == gain.size() ? Dsp::Biquad::HighShelf : Dsp::Biquad::Peak;
        chain.stage[(size_t) chain.count++].set (kind, rate, centre[b], kind == Dsp::Biquad::Peak ? 1.41f : 0.707f, g);
    }
}

// A short comb at the note that sounds: the string or the tube the noise excites. It rings for
// a few hundredths of a second, so that the noise takes the note without becoming a note itself.
void Mechanism::setResonance (int type, const SampleData& s, const Note& note) noexcept
{
    combWet = 0.0f;
    type = treatment (type);
    if (! s.pitched || type == FromSample || combL.empty() || note.adapt <= 0.0f)
        return;
    const double delay = rate / hz (note.sounding);
    if (delay < 2.0 || delay > (double) combL.size() - 4.0)
        return;
    const float ring = type == Pick ? 0.12f : type == Bow ? 0.1f : type == Breath ? 0.07f : 0.05f;      // seconds to -60 dB
    const float wet = type == Pick ? 0.55f : type == Bow ? 0.45f : type == Breath ? 0.4f : type == Key ? 0.2f : 0.1f;
    combDelay = delay;
    combFeedback = (float) std::pow (10.0, -3.0 * delay / (rate * ring));
    combNorm = std::sqrt (1.0f - combFeedback * combFeedback);
    combWet = wet * juce::jlimit (0.0f, 1.0f, note.adapt);
    combLow.set (Dsp::Biquad::HighPass, rate, 0.6f * hz (note.sounding), 0.707f);     // a comb also rings at 0 Hz
    combLow.reset();
}

void Mechanism::play (int type, int event, float amount, const SampleData& s, const Note& note, float scale) noexcept
{
    const auto* clip = choose (type, event, note.velocity, note.sounding, true);
    if (clip == nullptr)
        return;
    const auto& e = s.excitation;
    const bool keyEvent = event == MechanismLibrary::Down || event == MechanismLibrary::Up;
    const bool down = event == MechanismLibrary::Down;

    // It meets the note that sounds in its own octave: with the notes the libraries have, a
    // semitone and a half at most (9 % faster or slower, which a noise does not betray).
    const float semis = clip->tuned ? juce::jlimit (-1.5f, 1.5f, folded (note.sounding - (float) clip->note)) : 0.0f;
    const double step = MechanismLibrary::clipRate / rate * std::exp2 (semis / 12.0f);

    // The knob is heard over a wide range: at full, the key 9 dB under the loudest moment of the
    // sample, the release 11 dB and the pedal 13 dB, about as loud as a close microphone hears
    // them; at a quarter, 24 dB further down. Harder notes make somewhat louder mechanisms (7 dB
    // from the softest to the hardest), and no two are exactly as loud (half a dB).
    const bool going = event == MechanismLibrary::Down || event == MechanismLibrary::PedalDown;
    const float eventLevel = 0.35f * (down ? 1.0f : keyEvent ? 0.8f : 0.63f);
    const float userGain = going ? note.downGain : note.upGain;     // Entrada / Salida, applied after the cap below
    const float strength = keyEvent ? 0.45f + 0.55f * note.velocity : 1.0f;
    const float wobble = Dsp::dbToGain (0.5f * (rng.nextFloat() * 2.0f - 1.0f));

    // A slow attack has no strike: it softens and spreads a hammer, a pick or a key, and a bow or
    // a breath swells with it. And where the sample strikes hard by itself, ours joins its strike
    // rather than adding another.
    float shape = 1.0f, fadeMs = 0.3f;
    if (down && note.adapt > 0.0f)
    {
        const float softness = up (note.attackMs, 25.0f, 200.0f);
        const float clipMs = (float) (clip->audio.getNumSamples() / MechanismLibrary::clipRate * 1000.0);
        if (strikes (type))
        {
            shape *= 1.0f - 0.65f * softness;
            fadeMs = std::max (fadeMs, 30.0f * softness);
            if (behaviour (type) != Piano)
                shape *= 1.0f - 0.3f * up (e.transientDb, 6.0f, 18.0f);
        }
        else
        {
            shape *= 1.0f - 0.35f * softness;
            // The bite of the bow and the tongue come first even in a slow note (measured: they peak 3 ms
            // into the rise of an Iowa violin or flute that takes half a second to grow); only a few ms
            // of fade, the swell is the continuous noise's.
            fadeMs = std::max (fadeMs, std::min ({ 0.5f * note.attackMs, 0.6f * clipMs, 5.0f }));
        }
    }

    // In time: what sets an instrument going sounds before its tone has grown. Measured on real notes
    // (Iowa, VSCO), the hammer, the pluck, the bow and the tongue peak where the note starts to rise,
    // 6 to 45 ms before its first peak. So the strike lands where the sample's attack reaches a tenth of
    // its height (2 ms past it), or on its first peak if that comes sooner, as in a sample that strikes
    // by itself. Being in time is not taking the sample's colour: it does not depend on Adaptar.
    int wait = 0;
    double start = 0.0;
    if (down)
    {
        // An organ key is the exception: its contact and the pipe's chiff start with the tone and peak
        // late in its rise (VSCO organ: 23 ms in, about the tone's own first peak). It lands on the sample's
        // first peak, as before (measured: on time, and its shape follows the real one best there).
        const double strikeAt = behaviour (type) == Key
            ? (double) e.transientAt
            : std::min ((double) e.transientAt, (double) e.riseAt + 0.002);
        const double toPeak = (s.onset + strikeAt * s.sampleRate - note.startPos) / s.sampleRate / std::max (0.05f, note.speed);
        if (toPeak >= 0.0 && toPeak < 0.1)
        {
            const double offset = toPeak * rate - clip->peakAt / step;
            if (offset >= 0.0)
                wait = (int) std::lround (offset);
            else
                start = std::min (-offset * step, std::max (0.0, clip->peakAt - 0.0005 * MechanismLibrary::clipRate));
        }
        if (start > 0.0)
            fadeMs = std::max (fadeMs, 1.0f);
    }

    // With the same polarity as the sample's strike: the two add up instead of hollowing each other.
    float sign = 1.0f;
    if (down && strikes (type) && note.adapt > 0.0f && e.transientDb > 3.0f)
        sign = clip->peakSign == e.transientSign ? 1.0f : -1.0f;

    auto& p = freePlayer();
    p = Player {};
    p.active = true;
    p.clip = clip;
    p.pos = start;
    p.end = (double) clip->audio.getNumSamples() - 1.0;
    p.step = step;
    p.wait = wait;
    p.ramp = 0.0f;
    // Never a step at the start: a recording cut at its onset begins away from zero, and the jump clicked
    // (0.5 ms is too short to soften a strike).
    p.rampStep = 1.0f / std::max (1.0f, std::max (0.5f, fadeMs) * 0.001f * (float) rate);
    p.gain = amount * amount * std::max (1.0e-3f, s.peakRms) * clip->gain * eventLevel * strength * wobble * scale * shape * sign;
    // Its level follows the sample's RMS; on a sample with little crest (a pad, a compressed one-shot)
    // a sharp click would then stand up to 6 dB over the sample's own peak, after the engines' ceiling.
    // Its peak is held at half the sample's (-6 dB): only those cases change, and nothing is clipped.
    // Entrada and Salida come after: what the player asks for on purpose is not held back.
    {
        const float clipPeak = clip->peak * std::abs (p.gain), cap = 0.5f * std::max (1.0e-6f, s.peakLevel);
        if (clipPeak > cap)
            p.gain *= cap / clipPeak;
        p.gain *= userGain;
    }
    // A real bow or breath recording goes on with the noise of its own note, which does not follow this
    // one's level and would stop dead with the recording: only its bite or tongue is played (120 ms past
    // its peak, the last 40 faded), and the continuous noise, which follows the note, takes over.
    if (down && (type == RealBow || type == RealBreath))
    {
        p.end = std::min (p.end, start + clip->peakAt + 0.12 * MechanismLibrary::clipRate);
        p.fadeOut = 0.04 * MechanismLibrary::clipRate;
    }
    setChain (p.chain, type, event, s, note);
}

// The sample's own attack, from where the note starts, already at its level: its bright part at
// the key, a dark echo of it at the release, at the pitch the note plays.
void Mechanism::playSample (bool down, float amount, const SampleData& s, const Note& note) noexcept
{
    auto& p = freePlayer();
    p = Player {};
    p.active = true;
    p.fromSample = true;
    p.pos = note.startPos;
    p.end = std::min ((double) s.length - 1.0, note.startPos + 0.08 * s.sampleRate);
    const float shift = note.sounding - (s.pitched ? s.rootMidi : 60.0f);
    p.step = s.sampleRate / rate * std::exp2 (juce::jlimit (-48.0f, 48.0f, shift) / 12.0f);
    p.filter.set (down ? Dsp::Biquad::HighPass : Dsp::Biquad::LowPass, rate, down ? 1000.0f : 1200.0f, 0.7f);
    p.gain = amount * amount * 1.6f * (down ? 1.0f : 0.5f) * (0.35f + 0.65f * note.velocity);
    // At the key it sounds in step with the note: it is read from the zero-phase copy (SampleData::attackHigh),
    // which adds to the note instead of hollowing it. The echo at the release is not in step with anything.
    p.zeroPhase = down && p.end < (double) s.attackHigh.getNumSamples() - 1.0;
    // Filtered twice (forwards and backwards) it is 6 dB down at 1 kHz instead of 3: made up by 3 dB.
    // Its peak, measured where it plays, is held at half the sample's, as for the recordings.
    if (p.zeroPhase)
    {
        p.gain *= 1.41f;
        float most = 1.0e-6f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* x = s.attackHigh.getReadPointer (ch);
            for (int i = (int) p.pos; i <= (int) p.end; ++i)
                most = std::max (most, std::abs (x[i]));
        }
        const float cap = 0.5f * std::max (1.0e-6f, s.peakLevel);
        if (most * p.gain > cap)
            p.gain = cap / most;
    }
    p.gain *= down ? note.downGain : note.upGain;       // Entrada / Salida, after the cap
}

void Mechanism::startGrain (Grain& g) noexcept
{
    // The bow that goes on is taken where real bowed notes are only that: past their bite (from 0.2 s,
    // measured on the clips). Grains of bites made it come and go (roce.py: 5.0 dB of unevenness where a
    // real bow has 2.5; now 2.2-2.3) and coloured it far from a real bow (22 and 16 dB; now 13 and 14).
    // The bites themselves still come from each type's own recordings. The air stays as it was: taken
    // from real blown notes it measured further from real air (24 dB against 21).
    const bool air = behaviour (textureType) == Breath;
    const int event = air ? MechanismLibrary::Up : MechanismLibrary::Down;
    g.clip = choose (air ? textureType : RealBow, event, textureNote.velocity, textureNote.sounding, false);
    if (g.clip == nullptr)
        return;
    const float semis = g.clip->tuned ? juce::jlimit (-1.5f, 1.5f, folded (textureNote.sounding - (float) g.clip->note)) : 0.0f;
    g.step = MechanismLibrary::clipRate / rate * std::exp2 (semis / 12.0f);
    const double skip = (air ? 0.01 : 0.2) * MechanismLibrary::clipRate;     // past the tongue or the bite
    const double final = g.clip->audio.getNumSamples() - 2.0;
    const double need = grainLength * g.step;
    g.pos = skip + rng.nextDouble() * std::max (0.0, final - skip - need);
    g.length = std::max (1, std::min (grainLength, (int) ((final - g.pos) / g.step)));
    g.age = 0;
    g.gain = air ? g.clip->gain : g.clip->sustainGain;
}

void Mechanism::process (float* l, float* r, int num, const SampleData& s, float tone) noexcept
{
    heldFor += num;
    const float peakRms = std::max (1.0e-3f, s.peakRms);

    // The bow and the air follow the note's own level, relatively stronger when it is soft (as a
    // player's breath is more audible in a pianissimo).
    toneLevel += (tone - toneLevel) * (1.0f - std::exp (-(float) num / (0.02f * (float) rate)));
    const float textureGain = texture ? textureLevel * peakRms * std::pow (juce::jlimit (0.0f, 1.0f, toneLevel / peakRms), 0.8f) * 0.8165f : 0.0f;
    const int hop = std::max (1, grainLength / 4);

    for (int done = 0; done < num;)
    {
        const int count = std::min ((int) mixL.size(), num - done);
        std::fill (mixL.begin(), mixL.begin() + count, 0.0f);
        std::fill (mixR.begin(), mixR.begin() + count, 0.0f);

        for (auto& p : players)
        {
            if (! p.active)
                continue;
            const auto& source = p.fromSample ? (p.zeroPhase ? s.attackHigh : s.audio) : p.clip->audio;
            const float* a = source.getReadPointer (0);
            const float* b = source.getReadPointer (1);
            const int lastSample = source.getNumSamples() - 1;
            const double fadeLength = p.fromSample ? 0.01 * s.sampleRate : 1.0;

            for (int i = 0; i < count; ++i)
            {
                if (p.wait > 0)
                {
                    --p.wait;
                    continue;
                }
                if (p.pos >= p.end)
                {
                    p.active = false;
                    break;
                }
                const int at = (int) p.pos;
                const float f = (float) (p.pos - at);
                float x = hermite (a, at, lastSample, f);
                float y = hermite (b, at, lastSample, f);
                if (p.fromSample)
                {
                    const float fade = (float) std::min (1.0, (p.end - p.pos) / fadeLength);
                    x = (p.zeroPhase ? x : p.filter.process (0, x)) * fade;
                    y = (p.zeroPhase ? y : p.filter.process (1, y)) * fade;
                }
                else
                {
                    p.ramp = std::min (1.0f, p.ramp + p.rampStep);
                    const float out = p.fadeOut > 0.0 ? (float) std::min (1.0, (p.end - p.pos) / p.fadeOut) : 1.0f;
                    x *= p.ramp * out;
                    y *= p.ramp * out;
                    p.chain.process (x, y);
                }
                mixL[(size_t) i] += x * p.gain;
                mixR[(size_t) i] += y * p.gain;
                p.pos += p.step;
            }
        }

        if (texture && textureGain > 0.0f)
        {
            for (int i = 0; i < count; ++i)
            {
                if (++sinceGrain >= hop)
                {
                    sinceGrain = 0;
                    startGrain (grains[(size_t) nextGrain]);
                    nextGrain = (nextGrain + 1) % (int) grains.size();
                }
                float x = 0.0f, y = 0.0f;
                for (auto& g : grains)
                {
                    if (g.clip == nullptr || g.age >= g.length)
                        continue;
                    const float w = grainWindow[(size_t) (g.length == grainLength ? g.age : g.age * grainLength / g.length)];
                    const int at = (int) g.pos;
                    const float f = (float) (g.pos - at);
                    const int lastSample = g.clip->audio.getNumSamples() - 1;
                    x += hermite (g.clip->audio.getReadPointer (0), at, lastSample, f) * w * g.gain;
                    y += hermite (g.clip->audio.getReadPointer (1), at, lastSample, f) * w * g.gain;
                    g.pos += g.step;
                    ++g.age;
                }
                textureChain.process (x, y);
                mixL[(size_t) i] += x * textureGain;
                mixR[(size_t) i] += y * textureGain;
            }
        }

        // The string or the tube it all rings in.
        float loudest = 0.0f;
        if (combWet > 0.0f)
        {
            const int size = (int) combL.size();
            for (int i = 0; i < count; ++i)
            {
                double read = combPos - combDelay;
                if (read < 0.0)
                    read += size;
                const int i0 = (int) read, i1 = (i0 + 1) % size;
                const float fr = (float) (read - i0);
                const float dl = combL[(size_t) i0] + fr * (combL[(size_t) i1] - combL[(size_t) i0]);
                const float dr = combR[(size_t) i0] + fr * (combR[(size_t) i1] - combR[(size_t) i0]);
                combLowL = 0.25f * combLowL + 0.75f * dl;           // the highs die first, as on a string
                combLowR = 0.25f * combLowR + 0.75f * dr;
                const float yl = mixL[(size_t) i] + combFeedback * combLowL;
                const float yr = mixR[(size_t) i] + combFeedback * combLowR;
                combL[(size_t) combPos] = yl;
                combR[(size_t) combPos] = yr;
                combPos = (combPos + 1) % size;
                loudest = std::max ({ loudest, std::abs (yl), std::abs (yr) });
                const float wl = combLow.process (0, yl), wr = combLow.process (1, yr);
                l[done + i] += mixL[(size_t) i] * (1.0f - combWet) + wl * combWet * combNorm;
                r[done + i] += mixR[(size_t) i] * (1.0f - combWet) + wr * combWet * combNorm;
            }
        }
        else
        {
            for (int i = 0; i < count; ++i)
            {
                l[done + i] += mixL[(size_t) i];
                r[done + i] += mixR[(size_t) i];
            }
        }
        ringing = loudest;
        done += count;
    }

    // The air and the bow stop with the note.
    if (texture && heldFor > 0.05 * rate && toneLevel < 1.0e-5f * peakRms)
        texture = false;
}

//==============================================================================
// Rules of thumb from how each instrument is played, as degrees and not as walls: any gesture
// can be chosen, these only say how naturally it belongs to this sound.
Mechanism::Fit Mechanism::fit (const SampleData& s, float attackMs, float sustain, bool held) noexcept
{
    const auto& e = s.excitation;
    Fit result;

    const float fast = 1.0f - up (attackMs, 12.0f, 90.0f);       // a strike: a few ms
    const float soft = up (attackMs, 15.0f, 150.0f);             // a swell: tens to hundreds of ms
    const float lasting = held ? std::max (e.sustain, std::sqrt (juce::jlimit (0.0f, 1.0f, sustain))) : e.sustain;
    const float strike = up (e.transientDb, 2.0f, 12.0f);         // a strike of its own
    const float tone = s.pitched ? e.tonal : 0.3f * (1.0f - e.noisiness);
    const float f0 = s.pitched ? hz (s.rootMidi) : 0.0f;
    auto& sc = result.score;
    auto& why = result.against;

    sc[Piano] = (0.15f + 0.85f * fast) * (0.6f + 0.4f * (1.0f - up (lasting, 0.7f, 1.0f))) * (0.45f + 0.55f * tone);
    sc[Pick] = (0.15f + 0.85f * fast) * (0.55f + 0.45f * strike) * (0.45f + 0.55f * (1.0f - up (lasting, 0.6f, 1.0f))) * (0.5f + 0.5f * tone);
    sc[Key] = 0.85f * (0.2f + 0.8f * fast) * (0.5f + 0.5f * tone) * (0.7f + 0.3f * (1.0f - strike));   // a key barely strikes
    sc[Bow] = (0.3f + 0.7f * tone) * (0.35f + 0.65f * up (lasting, 0.2f, 0.7f)) * (0.75f + 0.25f * up (attackMs, 5.0f, 80.0f));
    sc[Breath] = (0.3f + 0.7f * soft) * (0.35f + 0.65f * up (lasting, 0.2f, 0.7f)) * (0.5f + 0.5f * tone)
               * (0.8f + 0.2f * e.noisiness) * (f0 > 0.0f && f0 < 70.0f ? 0.7f : 1.0f);
    sc[FromSample] = 0.45f + 0.5f * strike;
    // The real types are the same gestures; which suits better is for the ear (see the help).
    sc[RealPiano] = sc[Piano];
    sc[RealPick] = sc[Pick];
    sc[RealBow] = sc[Bow];
    sc[RealBreath] = sc[Breath];
    sc[RealGuitar] = sc[Pick];

    // What speaks against each, when it hardly suits.
    why[Piano] = attackMs > 50.0f ? "ataque lento para un macillo" : lasting > 0.85f ? "sonido sostenido: el piano se apaga" : "poco tono para un piano";
    why[Pick] = attackMs > 40.0f ? "ataque lento para una púa" : lasting > 0.8f ? "nota sostenida: la púa no la mantiene" : "sin golpe propio";
    why[Key] = attackMs > 50.0f ? "ataque lento para una tecla" : "poco tono para una tecla";
    why[Bow] = tone < 0.3f ? "poco tono: el arco necesita una nota" : "se apaga pronto: el arco pide notas que duren";
    why[Breath] = attackMs < 15.0f ? "ataque muy rápido para un soplo" : f0 > 0.0f && f0 < 70.0f ? "registro muy grave para un soplo"
                : lasting < 0.3f ? "se apaga pronto para un soplo" : "poco tono para un soplo";
    why[FromSample] = "el sample casi no tiene golpe propio";
    why[RealPiano] = why[Piano];
    why[RealPick] = why[Pick];
    why[RealBow] = why[Bow];
    why[RealBreath] = why[Breath];
    why[RealGuitar] = why[Pick];

    for (int t = 1; t < FromSample; ++t)        // a gesture wins a tie with its real version
        if (sc[(size_t) t] > sc[(size_t) result.best])
            result.best = t;

    // A sample named after an instrument gets that instrument's own gesture, recorded on the real thing:
    // for real keyboards, guitars, harps, strings and winds the user preferred the real versions by ear;
    // for synths the classic ones, which the sound alone keeps choosing. Still a suggestion: any type
    // can be chosen.
    if (s.mechanismHint >= 0 && s.mechanismHint < NumTypes)
    {
        const int hinted = s.mechanismHint;
        sc[(size_t) hinted] = std::max (sc[(size_t) hinted], std::min (1.0f, sc[(size_t) result.best] + 0.05f));
        result.best = hinted;
        result.byName = true;
    }
    return result;
}

int Mechanism::nameHint (const juce::String& fileName, juce::String& word)
{
    const auto n = fileName.toLowerCase().replaceCharacters ("_-.()[]", "       ");
    word = {};
    const auto has = [&n, &word] (std::initializer_list<const char*> keys)
    {
        for (const auto* k : keys)
            if ((" " + n + " ").contains (juce::String (" ") + k) )
            {
                word = juce::String (k).trim();
                return true;
            }
        return false;
    };
    // A synth, a drum machine or a sound design file: the sound decides, not a word like "piano" in
    // "synth piano" or "keys" in "synth keys".
    if (has ({ "synth", "syn ", "808", "pluck", "lead", "saw", "square", "bleep", "reese", "wobble", "chip", "8bit",
               "fm ", "arp ", "arpeggio", "sub ", "pad", "riser", "fx" }))
    {
        word = {};
        return -1;
    }
    if (has ({ "piano", "rhodes", "wurli", "wurlitzer", "clav", "clavinet", "epiano", "e piano", "grand",
               "teclado", "pianet", "celesta", "harpsichord", "clave" }))
        return RealPiano;
    if (has ({ "guitar", "guitarra", "gtr", "strum", "nylon", "telecaster", "strat", "les paul", "banjo", "mandolin", "ukulele", "uke" }))
        return RealGuitar;
    if (has ({ "harp", "arpa", "pizz", "pizzicato", "koto", "kalimba", "sitar", "lyre", "lute", "oud", "zither" }))
        return RealPick;
    if (has ({ "violin", "violín", "viola", "cello", "chelo", "violonchelo", "fiddle", "bowed", "arco", "contrabass", "double bass", "strings" }))
        return RealBow;
    if (has ({ "flute", "flauta", "clarinet", "clarinete", "oboe", "bassoon", "fagot", "sax", "saxo", "trumpet", "trompeta",
               "horn", "trompa", "trombone", "tuba", "brass", "woodwind", "recorder", "ocarina", "pan flute", "whistle", "duduk", "shakuhachi" }))
        return RealBreath;
    if (has ({ "organ ", "órgano", "organo ", "hammond", "harmonium", "accordion", "acordeón" }))
        return Key;
    return -1;
}
