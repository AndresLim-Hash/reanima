#include "KeyDetect.h"
#include <juce_dsp/juce_dsp.h>

namespace KeyDetect
{
namespace
{
// Krumhansl & Kessler (1982): how well each degree fits a major and a minor key.
constexpr float majorProfile[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
constexpr float minorProfile[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

float correlation (const std::array<float, 12>& x, const float* profile, int root)
{
    float mx = 0.0f, my = 0.0f;
    for (int i = 0; i < 12; ++i)
    {
        mx += x[(size_t) i];
        my += profile[i];
    }
    mx /= 12.0f;
    my /= 12.0f;
    float sxy = 0.0f, sxx = 0.0f, syy = 0.0f;
    for (int i = 0; i < 12; ++i)
    {
        const float a = x[(size_t) ((i + root) % 12)] - mx, b = profile[i] - my;
        sxy += a * b;
        sxx += a * a;
        syy += b * b;
    }
    return sxx > 0.0f && syy > 0.0f ? sxy / std::sqrt (sxx * syy) : 0.0f;
}

int pitchClassOf (const juce::String& letter, const juce::String& accidental)
{
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };      // A B C D E F G
    const int l = letter[0] - 'A';
    if (l < 0 || l > 6)
        return -1;
    int pc = base[l];
    if (accidental == "#" || accidental == "s")
        ++pc;
    else if (accidental == "b")
        --pc;
    return (pc + 12) % 12;
}
}

const char* noteName (int pc)
{
    static const char* names[12] = { "Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si" };
    return pc >= 0 && pc < 12 ? names[pc] : "?";
}

juce::String Estimate::text() const
{
    if (root < 0)
        return "?";
    return juce::String::fromUTF8 (noteName (root)) + (minor ? " menor" : " mayor");
}

// The last word of the name that is a key: a note (C, F#, Bb, Gs) with "m", "min", "minor", "maj" or "major" or
// nothing after it. Without m/min it is major: a one-shot labelled "_C" is a note, and its key is taken as C major.
Estimate fromName (const juce::String& fileName)
{
    Estimate e;
    const auto stem = fileName.upToLastOccurrenceOf (".", false, false);
    auto words = juce::StringArray::fromTokens (stem, "_- .()[]", "");
    words.removeEmptyStrings();
    for (int w = words.size(); --w >= 0;)
    {
        auto t = words[w];
        // the mode as a word of its own: "Fs minor", "C Major"
        const auto lower = t.toLowerCase();
        if ((lower == "minor" || lower == "min" || lower == "major" || lower == "maj") && w > 0 && words[w - 1].length() <= 2)
        {
            t = words[w - 1] + (lower.startsWith ("min") ? "m" : "");
            --w;
        }
        if (t.isEmpty() || t.length() > 7 || ! juce::CharacterFunctions::isUpperCase (t[0]) || t[0] < 'A' || t[0] > 'G')
            continue;
        juce::String accidental, rest = t.substring (1);
        if (rest.startsWith ("#") || rest.startsWith ("b") || (rest.startsWith ("s") && ! rest.startsWithIgnoreCase ("sus")))
        {
            accidental = rest.substring (0, 1);
            rest = rest.substring (1);
        }
        const bool minor = rest == "m" || rest == "min" || rest == "Min" || rest == "minor" || rest == "Minor" || rest == "MIN";
        const bool major = rest.isEmpty() || rest == "maj" || rest == "Maj" || rest == "major" || rest == "Major" || rest == "M" || rest == "MAJ";
        if (! minor && ! major)
            continue;
        // a lone capital letter in the middle of a name ("A_Day") is weak evidence; only near the end
        if (rest.isEmpty() && accidental.isEmpty() && w < words.size() - 3)
            continue;
        e.root = pitchClassOf (t.substring (0, 1), accidental);
        e.minor = minor;
        e.fromName = true;
        e.confidence = 0.95f;
        return e;
    }
    return e;
}

// tonality: how much of the sound is pitched (0..1); the confidence never goes over it.
Estimate fromChroma (const std::array<float, 12>& chroma, float tonality)
{
    Estimate e;
    e.chroma = chroma;
    float best = -2.0f, second = -2.0f;
    int bestRoot = -1;
    bool bestMinor = false;
    for (int r = 0; r < 12; ++r)
        for (const bool minor : { false, true })
        {
            const float c = correlation (chroma, minor ? minorProfile : majorProfile, r);
            if (c > best)
            {
                // the relative key (same notes) is not a rival: what tells them apart is weak by nature
                second = std::max (second, best);
                best = c;
                bestRoot = r;
                bestMinor = minor;
            }
            else if (c > second)
                second = c;
        }
    if (bestRoot < 0)
        return e;
    e.root = bestRoot;
    e.minor = bestMinor;
    // Sure when the best fits well and clearly better than the next; never surer than the sound is pitched.
    const float fit = juce::jlimit (0.0f, 1.0f, (best - 0.45f) / 0.4f);
    const float margin = juce::jlimit (0.0f, 1.0f, (best - second) / 0.12f);
    e.confidence = juce::jlimit (0.0f, 1.0f, fit * (0.4f + 0.6f * margin)) * juce::jlimit (0.0f, 1.0f, tonality);
    return e;
}

namespace
{
struct Peak { double hz; float mag; int pc; };

// One frame (windowed, in the first `size` of `frame`, which is 2 * size long): its peaks on notes into the
// chroma, and its energy (all of the band, and in those peaks) for how pitched the sound is.
void analyseFrame (juce::dsp::FFT& fft, std::vector<float>& frame, int size, double sr, std::array<float, 12>& chroma,
                   double& peaksEnergy, double& allEnergy, std::vector<Peak>& peaks)
{
    const int lo = (int) std::ceil (55.0 * size / sr), hi = std::min (size / 2 - 2, (int) (2000.0 * size / sr));
    fft.performFrequencyOnlyForwardTransform (frame.data(), true);
    // the peaks only (a bin over both neighbours and over a tenth of the frame's loudest): the noise between
    // the partials does not fill the chroma
    float top = 0.0f;
    for (int k = lo; k <= hi; ++k)
        top = std::max (top, frame[(size_t) k]);
    if (top <= 1.0e-6f)
        return;
    for (int k = lo; k <= hi; ++k)
    {
        const float m = frame[(size_t) k];
        allEnergy += (double) m * m;
        if (m > frame[(size_t) k - 1] && m >= frame[(size_t) k + 1] && m > 0.1f * top)
        {
            // the peak's frequency from the parabola through it and its neighbours
            const float a = frame[(size_t) k - 1], b = m, c = frame[(size_t) k + 1];
            // (a peak's curvature is negative; a flat top: the bin itself)
            const float curve = a - 2.0f * b + c;
            const float shift = curve < -1.0e-12f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / curve) : 0.0f;
            const double hz = (k + shift) * sr / size;
            if (! (hz > 0.0))
                continue;
            const double midi = 69.0 + 12.0 * std::log2 (hz / 440.0);
            const double off = midi - std::round (midi);
            if (! (std::abs (off) <= 0.35))
                continue;                   // between two notes: not counted
            peaks.push_back ({ hz, m, ((int) std::round (midi) % 12 + 12) % 12 });
            peaksEnergy += (double) m * m;
        }
    }
    // A peak at twice or three times a louder-than-a-third lower one is most likely its harmonic (the octave
    // is the same note; the third harmonic is the fifth, which pulled the key a fifth up): it counts less.
    for (const auto& pk : peaks)
    {
        float weight = 1.0f;
        for (const auto& low : peaks)
            for (const double h : { 2.0, 3.0, 5.0 })
                if (h != 2.0 && std::abs (pk.hz / low.hz - h) < 0.03 * h && low.mag > 0.3f * pk.mag)
                    weight = std::min (weight, h == 3.0 ? 0.25f : 0.4f);
        chroma[(size_t) pk.pc] += weight * std::sqrt (pk.mag);     // compressed: one loud partial does not decide alone
    }
    peaks.clear();
}

// pitched: most of the energy in the band is in peaks on notes
float tonalityOf (double peaksEnergy, double allEnergy)
{
    return allEnergy > 0.0 ? (float) juce::jlimit (0.0, 1.0, (peaksEnergy / allEnergy - 0.15) / 0.45) : 0.0f;
}

std::vector<float> hann (int size)
{
    std::vector<float> window ((size_t) size);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);
    return window;
}
}

Estimate fromAudio (const juce::AudioBuffer<float>& audio, double sr)
{
    constexpr int order = 13, size = 1 << order;          // 8192: 5.4 Hz apart at 44.1 kHz
    const int hop = size / 2;
    const int n = audio.getNumSamples();
    std::array<float, 12> chroma {};
    if (n < 2048 || sr <= 0.0)
        return fromChroma (chroma, 0.0f);
    juce::dsp::FFT fft (order);
    const auto window = hann (size);
    std::vector<float> frame ((size_t) size * 2);
    double peaksEnergy = 0.0, allEnergy = 0.0;
    std::vector<Peak> peaks;
    peaks.reserve (256);
    const int frames = std::max (1, (n - size) / hop + 1);
    const int stride = std::max (1, frames / 400);         // at most some 400 frames looked at
    for (int f = 0; f < frames; f += stride)
    {
        const int start = f * hop;
        std::fill (frame.begin(), frame.end(), 0.0f);
        for (int i = 0; i < size && start + i < n; ++i)
        {
            float v = 0.0f;
            for (int c = 0; c < audio.getNumChannels(); ++c)
                v += audio.getSample (c, start + i);
            frame[(size_t) i] = v * window[(size_t) i];
        }
        analyseFrame (fft, frame, size, sr, chroma, peaksEnergy, allEnergy, peaks);
    }
    return fromChroma (chroma, tonalityOf (peaksEnergy, allEnergy));
}

//==============================================================================
struct AudioStream::Impl
{
    static constexpr int order = 13, size = 1 << order, hop = size / 2;
    juce::dsp::FFT fft { order };
    std::vector<float> window = hann (size), frame = std::vector<float> ((size_t) size * 2), pending;
    std::vector<Peak> peaks;
};

AudioStream::AudioStream() : impl (std::make_unique<Impl>()) { impl->peaks.reserve (256); }
AudioStream::~AudioStream() = default;

void AudioStream::reset (double newRate)
{
    rate = newRate;
    chroma = {};
    peaksEnergy = allEnergy = 0.0;
    heardSeconds = 0.0;
    impl->pending.clear();
    recent = {};
    recentSeconds = 0.0;
}

void AudioStream::push (const float* mono, int n)
{
    if (rate <= 0.0 || n <= 0)
        return;
    auto& m = *impl;
    m.pending.insert (m.pending.end(), mono, mono + n);
    size_t used = 0;
    while (m.pending.size() - used >= (size_t) Impl::size)
    {
        float level = 0.0f;
        for (int i = 0; i < Impl::size; ++i)
        {
            const float v = m.pending[used + (size_t) i];
            level = std::max (level, std::abs (v));
            m.frame[(size_t) i] = v * m.window[(size_t) i];
        }
        std::fill (m.frame.begin() + Impl::size, m.frame.end(), 0.0f);
        const double before = peaksEnergy;
        std::array<float, 12> frameChroma {};
        if (level > 1.0e-4f)        // -80 dB: silence does not count as heard
            analyseFrame (m.fft, m.frame, Impl::size, rate, frameChroma, peaksEnergy, allEnergy, m.peaks);
        for (size_t c = 0; c < 12; ++c)
        {
            chroma[c] += frameChroma[c];
            recent[c] += frameChroma[c];
        }
        if (peaksEnergy > before)
        {
            heardSeconds += Impl::hop / rate;
            recentSeconds += Impl::hop / rate;
        }
        used += Impl::hop;
    }
    m.pending.erase (m.pending.begin(), m.pending.begin() + (std::ptrdiff_t) used);
}

void AudioStream::fade (double seconds, double halfLife) noexcept
{
    const float k = (float) std::exp2 (-seconds / halfLife);
    for (auto& c : chroma)
        c *= k;
    peaksEnergy *= k;
    allEnergy *= k;
    heardSeconds *= k;
    const float fast = (float) std::exp2 (-seconds / 0.5);
    for (auto& c : recent)
        c *= fast;
    recentSeconds *= fast;
}

Chord AudioStream::chord() const
{
    return recentSeconds > 0.25 ? chordFromChroma (recent) : Chord();
}

Estimate AudioStream::estimate() const
{
    // a few seconds say little: the confidence grows with what was heard (full from some 20 s of pitched sound)
    return fromChroma (chroma, tonalityOf (peaksEnergy, allEnergy) * (float) juce::jlimit (0.0, 1.0, heardSeconds / fullAfter));
}

//==============================================================================
const std::vector<ChordKind>& chordKinds()
{
    static const std::vector<ChordKind> kinds {
        { "", { 0, 4, 7 }, 3, false },          { "m", { 0, 3, 7 }, 3, true },        { "dim", { 0, 3, 6 }, 3, true },
        { "aug", { 0, 4, 8 }, 3, false },       { "sus2", { 0, 2, 7 }, 3, false },    { "sus4", { 0, 5, 7 }, 3, false },
        { "7", { 0, 4, 7, 10 }, 4, false },     { "maj7", { 0, 4, 7, 11 }, 4, false }, { "m7", { 0, 3, 7, 10 }, 4, true },
        { "m7b5", { 0, 3, 6, 10 }, 4, true },   { "dim7", { 0, 3, 6, 9 }, 4, true },  { "6", { 0, 4, 7, 9 }, 4, false },
        { "m6", { 0, 3, 7, 9 }, 4, true },      { "add9", { 0, 4, 7, 2 }, 4, false }, { "madd9", { 0, 3, 7, 2 }, 4, true },
        { "mmaj7", { 0, 3, 7, 11 }, 4, true },  { "9", { 0, 4, 7, 10, 2 }, 5, false }, { "maj9", { 0, 4, 7, 11, 2 }, 5, false },
        { "m9", { 0, 3, 7, 10, 2 }, 5, true },  { "5", { 0, 7 }, 2, false },
    };
    return kinds;
}

juce::String Chord::name() const
{
    if (root < 0)
        return {};
    auto s = juce::String::fromUTF8 (noteName (root)) + chordKinds()[(size_t) kind].suffix;
    if (bass >= 0 && bass != root)
        s << "/" << juce::String::fromUTF8 (noteName (bass));
    return s;
}

juce::String Chord::degreeIn (int keyRoot) const
{
    if (root < 0)
        return {};
    static const char* numerals[12] = { "I", "bII", "II", "bIII", "III", "IV", "#IV", "V", "bVI", "VI", "bVII", "VII" };
    const auto& k = chordKinds()[(size_t) kind];
    juce::String n (numerals[((root - keyRoot) % 12 + 12) % 12]);
    if (k.minorish)
        n = n.toLowerCase();
    const juce::String suffix (k.suffix);
    if (suffix == "m")
        return n;
    if (suffix == "dim")
        return n + juce::String::fromUTF8 ("°");
    if (suffix == "aug")
        return n + "+";
    if (suffix == "m7" || suffix == "m6" || suffix == "m9" || suffix == "madd9")
        return n + suffix.substring (1);
    if (suffix == "m7b5")
        return n + juce::String::fromUTF8 ("ø7");
    if (suffix == "dim7")
        return n + juce::String::fromUTF8 ("°7");
    if (suffix == "mmaj7")
        return n + "maj7";
    return n + suffix;
}

namespace
{
int maskOf (const ChordKind& k, int root, bool withFifth = true)
{
    int m = 0;
    for (int i = 0; i < k.count; ++i)
        if (withFifth || k.intervals[(size_t) i] != 7 || k.count < 4)
            m |= 1 << ((root + k.intervals[(size_t) i]) % 12);
    return m;
}

int bits (int m) noexcept
{
    int c = 0;
    for (; m != 0; m &= m - 1)
        ++c;
    return c;
}
}

Chord chordFromNotes (const std::vector<int>& notes)
{
    Chord best;
    if (notes.empty())
        return best;
    int held = 0, lowest = 128;
    for (const int n : notes)
    {
        held |= 1 << (n % 12);
        lowest = std::min (lowest, n);
    }
    const int count = bits (held), bass = lowest % 12;
    if (count < 2)
        return best;
    float bestScore = -1.0e9f;
    const auto& kinds = chordKinds();
    for (size_t k = 0; k < kinds.size(); ++k)
    {
        const bool power = juce::String (kinds[k].suffix) == "5";
        if (power != (count == 2))
            continue;           // two notes: only a fifth; three or more: no "5"
        for (int r = 0; r < 12; ++r)
        {
            if ((held & (1 << r)) == 0)
                continue;       // the root is played
            const int full = maskOf (kinds[k], r), core = maskOf (kinds[k], r, false);
            if ((core & ~held) != 0)
                continue;       // a note of the chord is missing (only the fifth of a four-note chord may be)
            const int extra = bits (held & ~full);
            // every note explained first; then the root in the bass; then the simpler chord
            const float score = -3.0f * (float) extra + (r == bass ? 1.0f : 0.0f) - 0.3f * (float) kinds[k].count
                              + ((full & ~held) == 0 ? 0.2f : 0.0f) - 0.01f * (float) k;
            if (score > bestScore)
            {
                bestScore = score;
                best.root = r;
                best.kind = (int) k;
                best.bass = bass;
                best.confidence = juce::jlimit (0.0f, 1.0f, 1.0f - 0.3f * (float) extra);
            }
        }
    }
    return best;
}

Chord chordFromChroma (const std::array<float, 12>& chroma)
{
    Chord best;
    double norm = 0.0;
    for (const float c : chroma)
        norm += (double) c * c;
    if (norm <= 1.0e-12)
        return best;
    norm = std::sqrt (norm);
    // the triads and the three common sevenths; the root, third and seventh weigh 1, the fifth 0.8
    // (no sus chords: a fourth or a second over the root is too easily a harmonic, or the next chord's note)
    static constexpr int candidates[] = { 0, 1, 2, 3, 6, 7, 8 };
    struct Fit { float score; int root, kind; };
    std::vector<Fit> fits;
    for (const int k : candidates)
    {
        const auto& kind = chordKinds()[(size_t) k];
        double t2 = 0.0;
        for (int i = 0; i < kind.count; ++i)
        {
            const double w = kind.intervals[(size_t) i] == 7 ? 0.8 : 1.0;
            t2 += w * w;
        }
        for (int r = 0; r < 12; ++r)
        {
            double dot = 0.0;
            for (int i = 0; i < kind.count; ++i)
                dot += (kind.intervals[(size_t) i] == 7 ? 0.8 : 1.0) * chroma[(size_t) ((r + kind.intervals[(size_t) i]) % 12)];
            // a seventh must fit clearly better than its triad: a little handicap for the fourth note
            fits.push_back ({ (float) (dot / (norm * std::sqrt (t2))) - (kind.count > 3 ? 0.03f : 0.0f), r, k });
        }
    }
    std::sort (fits.begin(), fits.end(), [] (const Fit& a, const Fit& b) { return a.score > b.score; });
    const auto& top = fits.front();
    // the rival: the best other chord that is not the same triad with or without its seventh
    const auto triadOf = [] (int kind) { return kind == 6 || kind == 7 ? 0 : kind == 8 ? 1 : kind; };
    float second = 0.0f;
    for (const auto& f : fits)
        if (f.root != top.root || triadOf (f.kind) != triadOf (top.kind))
        {
            second = f.score;
            break;
        }
    best.root = top.root;
    best.kind = top.kind;
    best.confidence = juce::jlimit (0.0f, 1.0f, (top.score - 0.55f) / 0.3f) * juce::jlimit (0.0f, 1.0f, (top.score - second) / 0.08f);
    return best;
}

float measuredReliability (float confidence) noexcept
{
    constexpr float byFifth[5] = { 0.29f, 0.47f, 0.58f, 0.74f, 0.75f };     // confidence 0-20, 20-40 ... 80-100 %
    return byFifth[juce::jlimit (0, 4, (int) (confidence * 5.0f))];
}

void MidiHistogram::fade (double seconds, double halfLife) noexcept
{
    const float k = (float) std::exp2 (-seconds / halfLife);
    for (auto& w : weight)
        w *= k;
}

Estimate MidiHistogram::estimate() const
{
    // a handful of notes say little: the confidence grows with them (full from some 16 notes)
    auto e = fromChroma (weight, juce::jlimit (0.0f, 1.0f, (float) notes / 16.0f));
    return e;
}
}
