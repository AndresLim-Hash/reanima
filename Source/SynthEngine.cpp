#include "SynthEngine.h"
#include "DroneStretch.h"
#include "Filters.h"
#include "Mechanism.h"
#include "FxRack.h"
#include "NoiseSynth.h"
#include "Stretch.h"
#include <bungee/Bungee.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#if defined (_MSC_VER)
 #include <intrin.h>
#endif

#if defined (__SSE2__) || defined (_M_X64)
 #include <emmintrin.h>
#elif defined (__ARM_NEON)
 #include <arm_neon.h>
#endif

// LOOP Complex: a Signalsmith Stretch and its scratch, one per note that uses it.
struct LoopStretcher
{
    signalsmith::stretch::SignalsmithStretch<float> stretch;      // Complex Pro (formants)
    std::array<std::vector<float>, 2> in, out;
    bool inUse = false;

    // Complex: Bungee, a grain at a time. Its input is the loop read at the host's rate (a ring of what was
    // read, so each sample is read once), its output a FIFO the voice takes its blocks from.
    std::unique_ptr<Bungee::Stretcher<Bungee::Basic>> bungee;
    Bungee::Request request {};
    std::vector<float> grain;                       // the grain's input, channel after channel
    std::array<std::vector<float>, 2> cache, fifo;
    long long cacheBegin = 0, cacheEnd = 0;
    int fifoStart = 0, fifoCount = 0;
    double startAt = 0.0;
    bool started = false;
    long long owed = 0;         // frames the time line went on while the start waited: dropped from its output
    // Beats's tails: the position of the last frame out, how far the requests run ahead of what plays, and
    // whether the reading is held short of the next hit.
    double lastOut = 0.0, lead = 0.0;
    bool held = false;
    alignas (16) float coefs[128] {};      // the band-limited reader's (SincTable::maxTaps = 96)

};

namespace
{
constexpr int subBlock = 32;
constexpr int controlBlock = 8;         // envelopes are exact every 8 samples, linear in between
constexpr float punchTime = 0.012f;     // seconds for the generated hit to fall by 8.7 dB
constexpr int maxPartials = 256;          // 128 a side when engine C plays the two sides apart (PartialModel::split)
constexpr float limiterCeiling = 4.0f;  // +12 dBFS: only catches runaways (the output stage saturates the 0 to +6 dB of the meter)
constexpr float maxBoost = 64.0f;       // +36 dB: how far engines B, C and D raise a quiet part to follow the ADSR
using Dsp::pi;
using Dsp::dbToGain;

inline float sampleAt (const float* d, int i, int len) noexcept { return (i >= 0 && i < len) ? d[i] : 0.0f; }

inline float hermite (float xm1, float x0, float x1, float x2, float f) noexcept
{
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

// Cubic read with silence outside [0, len).
inline float readCubic (const float* d, double pos, int len) noexcept
{
    const double fl = Dsp::floorFast (pos);
    const int i = (int) fl;
    const float f = (float) (pos - fl);

    if (i >= 1 && i + 2 < len)
        return hermite (d[i - 1], d[i], d[i + 1], d[i + 2], f);

    if (i < -2 || i > len)
        return 0.0f;

    return hermite (sampleAt (d, i - 1, len), sampleAt (d, i, len), sampleAt (d, i + 1, len), sampleAt (d, i + 2, len), f);
}

// Both channels at one position: readCubic twice, with the position worked out once.
inline void readCubicStereo (const float* a, const float* b, double pos, int len, float& outA, float& outB) noexcept
{
    const double fl = Dsp::floorFast (pos);
    const int i = (int) fl;
    const float f = (float) (pos - fl);

    if (i >= 1 && i + 2 < len)
    {
        outA = hermite (a[i - 1], a[i], a[i + 1], a[i + 2], f);
        outB = hermite (b[i - 1], b[i], b[i + 1], b[i + 2], f);
        return;
    }
    if (i < -2 || i > len)
    {
        outA = outB = 0.0f;
        return;
    }
    outA = hermite (sampleAt (a, i - 1, len), sampleAt (a, i, len), sampleAt (a, i + 1, len), sampleAt (a, i + 2, len), f);
    outB = hermite (sampleAt (b, i - 1, len), sampleAt (b, i, len), sampleAt (b, i + 1, len), sampleAt (b, i + 2, len), f);
}

// Cubic read of a periodic buffer.
inline float readCubicWrap (const float* d, double pos, int len) noexcept
{
    const double fl = Dsp::floorFast (pos);
    const int i = (int) fl;
    const float f = (float) (pos - fl);
    if (i >= 1 && i + 2 < len)
        return hermite (d[i - 1], d[i], d[i + 1], d[i + 2], f);
    auto w = [len] (int k) { k %= len; return k < 0 ? k + len : k; };
    return hermite (d[w (i - 1)], d[w (i)], d[w (i + 1)], d[w (i + 2)], f);
}

struct HannTable
{
    static constexpr int size = 2048;
    std::array<float, size + 2> v {};

    HannTable()
    {
        for (int i = 0; i <= size + 1; ++i)
            v[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) size);
    }

    float at (float x01) const noexcept
    {
        const float f = juce::jlimit (0.0f, 1.0f, x01) * size;
        const int i = (int) f;
        return v[(size_t) i] + (f - (float) i) * (v[(size_t) i + 1] - v[(size_t) i]);
    }
};

const HannTable& hann()
{
    static const HannTable table;
    return table;
}

// Dot products of one set of coefficients with both channels; n is a multiple of 4.
inline void dotStereo (const float* c, const float* l, const float* r, int n, float& outL, float& outR) noexcept
{
#if defined (__SSE2__) || defined (_M_X64)
    __m128 accL = _mm_setzero_ps(), accR = _mm_setzero_ps();
    for (int m = 0; m < n; m += 4)
    {
        const __m128 k = _mm_loadu_ps (c + m);
        accL = _mm_add_ps (accL, _mm_mul_ps (k, _mm_loadu_ps (l + m)));
        accR = _mm_add_ps (accR, _mm_mul_ps (k, _mm_loadu_ps (r + m)));
    }
    alignas (16) float sums[8];
    _mm_store_ps (sums, accL);
    _mm_store_ps (sums + 4, accR);
    outL = (sums[0] + sums[1]) + (sums[2] + sums[3]);
    outR = (sums[4] + sums[5]) + (sums[6] + sums[7]);
#elif defined (__ARM_NEON)
    float32x4_t accL = vdupq_n_f32 (0.0f), accR = vdupq_n_f32 (0.0f);
    for (int m = 0; m < n; m += 4)
    {
        const float32x4_t k = vld1q_f32 (c + m);
        accL = vmlaq_f32 (accL, k, vld1q_f32 (l + m));
        accR = vmlaq_f32 (accR, k, vld1q_f32 (r + m));
    }
    outL = vaddvq_f32 (accL);
    outR = vaddvq_f32 (accR);
#else
    float accL = 0.0f, accR = 0.0f;
    for (int m = 0; m < n; ++m)
    {
        accL += c[m] * l[m];
        accR += c[m] * r[m];
    }
    outL = accL;
    outR = accR;
#endif
}

// Engine C's sounding partials: cosine oscillators added into a stereo pair. Four run side by side; each
// does exactly the arithmetic of a loop over one partial (the phase in double, the 4096-point table read
// as Dsp::SineTable reads it), and the products are added in partial order, so the sum is bit for bit
// what the partials played one at a time give.
struct PartialBank
{
    int count = 0;
    std::array<int, maxPartials> q {};
    alignas (16) std::array<double, maxPartials> phase {}, inc {};
    alignas (16) std::array<float, maxPartials> amp {}, da {}, gl {}, gr {};

    // Moves phase to where each partial ends the block.
    void render (int num, float* outL, float* outR) noexcept
    {
        const auto& table = Dsp::sine();
        int a = 0;
#if defined (__SSE2__) || defined (_M_X64)
        const __m128d quarter = _mm_set1_pd (0.25), one = _mm_set1_pd (1.0);
        const __m128 size = _mm_set1_ps ((float) Dsp::SineTable::size);
        const float* v = table.v.data();
        const auto floorOf = [&] (__m128d x)
        {
            const __m128d t = _mm_cvtepi32_pd (_mm_cvttpd_epi32 (x));
            return _mm_sub_pd (t, _mm_and_pd (_mm_cmpgt_pd (t, x), one));
        };
        for (; a + 4 <= count; a += 4)
        {
            __m128d p0 = _mm_load_pd (phase.data() + a), p1 = _mm_load_pd (phase.data() + a + 2);
            const __m128d i0 = _mm_load_pd (inc.data() + a), i1 = _mm_load_pd (inc.data() + a + 2);
            __m128 c = _mm_load_ps (amp.data() + a);
            const __m128 d = _mm_load_ps (da.data() + a), gL = _mm_load_ps (gl.data() + a), gR = _mm_load_ps (gr.data() + a);
            alignas (16) int k[4];
            alignas (16) float l[4], r[4];
            for (int i = 0; i < num; ++i)
            {
                c = _mm_add_ps (c, d);
                const __m128d x0 = _mm_add_pd (p0, quarter), x1 = _mm_add_pd (p1, quarter);
                const __m128d f0 = _mm_sub_pd (x0, floorOf (x0)), f1 = _mm_sub_pd (x1, floorOf (x1));
                const __m128 f = _mm_mul_ps (_mm_movelh_ps (_mm_cvtpd_ps (f0), _mm_cvtpd_ps (f1)), size);
                const __m128i idx = _mm_cvttps_epi32 (f);
                _mm_store_si128 ((__m128i*) k, idx);
                const __m128 lo = _mm_setr_ps (v[k[0]], v[k[1]], v[k[2]], v[k[3]]);
                const __m128 hi = _mm_setr_ps (v[k[0] + 1], v[k[1] + 1], v[k[2] + 1], v[k[3] + 1]);
                const __m128 s = _mm_add_ps (lo, _mm_mul_ps (_mm_sub_ps (f, _mm_cvtepi32_ps (idx)), _mm_sub_ps (hi, lo)));
                const __m128 val = _mm_mul_ps (s, c);
                _mm_store_ps (l, _mm_mul_ps (val, gL));
                _mm_store_ps (r, _mm_mul_ps (val, gR));
                outL[i] = (((outL[i] + l[0]) + l[1]) + l[2]) + l[3];
                outR[i] = (((outR[i] + r[0]) + r[1]) + r[2]) + r[3];
                p0 = _mm_add_pd (p0, i0);
                p1 = _mm_add_pd (p1, i1);
            }
            _mm_store_pd (phase.data() + a, p0);
            _mm_store_pd (phase.data() + a + 2, p1);
            _mm_store_ps (amp.data() + a, c);
        }
#endif
        for (; a < count; ++a)
        {
            float current = amp[(size_t) a];
            double ph = phase[(size_t) a];
            for (int i = 0; i < num; ++i)
            {
                current += da[(size_t) a];
                const float val = table (ph + 0.25) * current;   // cosine, like the analysis phase
                outL[i] += val * gl[(size_t) a];
                outR[i] += val * gr[(size_t) a];
                ph += inc[(size_t) a];
            }
            phase[(size_t) a] = ph;
            amp[(size_t) a] = current;
        }
    }
};

// Band-limited interpolation for engine A: a Kaiser-windowed sinc. Going up in pitch lowers its
// cutoff to the new Nyquist and lengthens it in proportion, so that every transposition keeps
// the same flat response and what still folds back lands above the audible band.
struct SincTable
{
    static constexpr int baseTaps = 24, maxTaps = 96, phases = 512, cuts = 13;
    static constexpr int maxHalf = maxTaps / 2;

    std::array<int, cuts> taps {};
    std::array<size_t, cuts> offset {};
    std::vector<float> table;

    static double bessel0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 24; ++k)
        {
            term *= (x * 0.5 / k) * (x * 0.5 / k);
            sum += term;
        }
        return sum;
    }

    SincTable()
    {
        size_t total = 0;
        for (int c = 0; c < cuts; ++c)
        {
            const double wanted = baseTaps * std::exp2 (c / 4.0);
            taps[(size_t) c] = std::min (maxTaps, 8 * (int) std::ceil (wanted / 8.0));
            offset[(size_t) c] = total;
            total += (size_t) (phases + 1) * (size_t) taps[(size_t) c];
        }
        table.resize (total);

        constexpr double beta = 9.0;
        const double norm = 1.0 / bessel0 (beta);
        for (int c = 0; c < cuts; ++c)
        {
            const double fc = std::exp2 (-c / 4.0);
            const int n = taps[(size_t) c], half = n / 2;
            for (int ph = 0; ph <= phases; ++ph)
            {
                float* row = table.data() + offset[(size_t) c] + (size_t) ph * (size_t) n;
                const double frac = (double) ph / phases;
                double sum = 0.0;
                for (int m = 0; m < n; ++m)
                {
                    const double x = (double) (m - (half - 1)) - frac;
                    const double w = std::abs (x) >= half ? 0.0 : bessel0 (beta * std::sqrt (1.0 - (x / half) * (x / half))) * norm;
                    const double a = pi * fc * x;
                    const double v = fc * (std::abs (a) < 1.0e-9 ? 1.0 : std::sin (a) / a) * w;
                    row[m] = (float) v;
                    sum += v;
                }
                for (int m = 0; m < n; ++m)
                    row[m] = (float) (row[m] / sum);
            }
        }
    }

    static int cutFor (double step) noexcept
    {
        return step <= 1.0 ? 0 : juce::jlimit (0, cuts - 1, juce::roundToInt (4.0 * std::log2 (step)));
    }

    // Coefficients for a position between samples; the first one goes `taps / 2 - 1` samples back.
    void kernel (float frac, int cut, float* out) const noexcept
    {
        const int n = taps[(size_t) cut];
        const float f = frac * phases;
        const int ph = std::min ((int) f, phases - 1);      // a position just under the next sample can round up to it
        const float blend = f - (float) ph;
        const float* a = table.data() + offset[(size_t) cut] + (size_t) ph * (size_t) n;
        const float* b = a + n;
        for (int m = 0; m < n; ++m)
            out[m] = a[m] + blend * (b[m] - a[m]);
    }
};

const SincTable& sinc()
{
    static const SincTable table;
    return table;
}

// Both channels at a position, band-limited as engine A reads the file: the sinc with the cutoff of the
// reading speed, so that nothing above the new Nyquist folds back into the band when the pitch goes up
// (the cubic read before let a 19 kHz tone transposed an octave fold back at -9 dB); at the file's pace
// or slower the full-band sinc, exact on whole samples. `coefs` holds SincTable::maxTaps floats. Silence
// outside the file.
inline void readBandLimited (const float* a, const float* b, double pos, int len, double step, float* coefs, float& outA, float& outB) noexcept
{
    const auto& t = sinc();
    const int cut = SincTable::cutFor (std::abs (step));
    const int taps = t.taps[(size_t) cut], back = taps / 2 - 1;
    const double fl = Dsp::floorFast (pos);
    const int at = (int) fl;
    if (at - back >= len || at - back + taps <= 0)
    {
        outA = outB = 0.0f;
        return;
    }
    t.kernel ((float) (pos - fl), cut, coefs);
    if (at >= back && at - back + taps <= len)
    {
        dotStereo (coefs, a + at - back, b + at - back, taps, outA, outB);
        return;
    }
    float sa = 0.0f, sb = 0.0f;
    for (int m = 0; m < taps; ++m)
    {
        const int k = at - back + m;
        if (k >= 0 && k < len)
        {
            sa += coefs[m] * a[k];
            sb += coefs[m] * b[k];
        }
    }
    outA = sa;
    outB = sb;
}

// The same in a periodic buffer (a seamless loop).
inline void readBandLimitedWrap (const float* a, const float* b, double pos, int len, double step, float* coefs, float& outA, float& outB) noexcept
{
    const auto& t = sinc();
    const int cut = SincTable::cutFor (std::abs (step));
    const int taps = t.taps[(size_t) cut], back = taps / 2 - 1;
    const double fl = Dsp::floorFast (pos);
    const int at = (int) fl;
    t.kernel ((float) (pos - fl), cut, coefs);
    if (at >= back && at - back + taps <= len)
    {
        dotStereo (coefs, a + at - back, b + at - back, taps, outA, outB);
        return;
    }
    float sa = 0.0f, sb = 0.0f;
    for (int m = 0; m < taps; ++m)
    {
        int k = (at - back + m) % len;
        if (k < 0)
            k += len;
        sa += coefs[m] * a[k];
        sb += coefs[m] * b[k];
    }
    outA = sa;
    outB = sb;
}

static_assert (SampleData::Bed::pad >= SincTable::maxHalf, "the beds must have room for the longest kernel");

double lfoRateHz (const ParamSnapshot& p, int lfo, double bpm)
{
    const int base = P::Lfo1Rate + lfo * P::lfoStride;
    if (p[(size_t) base + 1] > 0.5f)
        return bpm / 60.0 / Dsp::beatsForDivision ((int) p[(size_t) base + 2]);
    return p[(size_t) base];
}

// LFO Deriva: the slow wandering of a real player's pitch, of tape or of an old oscillator. Random points a cycle apart
// joined by a smooth curve (Catmull-Rom: no corners, so no clicks and no steps), and two quicker layers of the
// same (3.7 and 11.3 times as fast, so that they never line up) for the small wobble; Suavidad takes those out.
// Each note draws its own points: notes drift apart from each other, as the voices of an analog synth do. The points
// come from a generator of its own, seeded from the note's: drawing them from the note's generator moved every other
// random choice of the note (Variación, the grains, engine F's phases), and a project saved before sounded different.
struct DriftLfo
{
    std::array<std::array<float, 4>, 3> knots {};
    std::array<double, 3> phase {};      // layer 0 follows the LFO's own phase (its rate, Sync and Retrig)
    juce::Random rng;

    void start (juce::int64 seed) noexcept
    {
        rng.setSeed (seed);
        for (auto& k : knots)
            for (auto& v : k)
                v = rng.nextFloat() * 2.0f - 1.0f;
        phase[1] = rng.nextDouble();
        phase[2] = rng.nextDouble();
    }

    static float curve (const std::array<float, 4>& k, float t) noexcept      // between k[1] (t 0) and k[2] (t 1)
    {
        const float a = -0.5f * k[0] + 1.5f * k[1] - 1.5f * k[2] + 0.5f * k[3];
        const float b = k[0] - 2.5f * k[1] + 2.0f * k[2] - 0.5f * k[3];
        const float c = 0.5f * (k[2] - k[0]);
        return ((a * t + b) * t + c) * t + k[1];
    }

    static void shift (std::array<float, 4>& k, juce::Random& rng) noexcept
    {
        k[0] = k[1];
        k[1] = k[2];
        k[2] = k[3];
        k[3] = rng.nextFloat() * 2.0f - 1.0f;
    }

    float value (double lfoPhase, float smooth) const noexcept
    {
        const float rough = 1.0f - juce::jlimit (0.0f, 1.0f, smooth);
        float v = curve (knots[0], (float) lfoPhase);
        if (rough > 0.0f)
            v += rough * (0.5f * curve (knots[1], (float) phase[1]) + 0.3f * curve (knots[2], (float) phase[2]));
        return juce::jlimit (-1.0f, 1.0f, v / (1.0f + 0.4f * rough));
    }

    // Layer 0 moves on when the LFO's phase wraps (nextCycle); the quicker layers by the cycles of this block.
    void advance (double cycles) noexcept
    {
        static constexpr double speed[3] = { 0.0, 3.7, 11.3 };
        for (int j = 1; j < 3; ++j)
        {
            phase[(size_t) j] += cycles * speed[j];
            for (int guard = 0; phase[(size_t) j] >= 1.0 && guard < 64; ++guard)
            {
                phase[(size_t) j] -= 1.0;
                shift (knots[(size_t) j], rng);
            }
            if (phase[(size_t) j] >= 1.0)
                phase[(size_t) j] -= std::floor (phase[(size_t) j]);
        }
    }

    void nextCycle() noexcept { shift (knots[0], rng); }
};

// Movimiento (engines B and D): the sustain source is one moment of the sound held still. This colours it, band by
// band (SampleData::Colour's octaves), as the sample itself was at the point the note has reached, so that a held
// note keeps changing as the sample did (a piano darkening, a string opening up) instead of staying on one colour.
// Only the shape moves, never the level: the bands are set against each other, their mean power kept, so the held
// note neither fades nor swells. A bell at 0 dB passes the sound as it is: with Movimiento at 0 nothing changes.
struct MotionEq
{
    static constexpr int numBands = SampleData::Colour::numBands;
    std::array<float, numBands> coef {}, db {}, a1 {}, a2 {}, a3 {}, m1 {};
    std::array<std::array<float, numBands>, 2> ic1 {}, ic2 {};
    int bands = 0;                      // the bands below the host's Nyquist
    float q = 1.4142f, scale = 1.0f;
    // The level kept: the power of the source's part before and after the bells, followed over some 300 ms, and the
    // gain that brings the one back to the other (bells that overlap, gains clamped at 12 dB: asking for the shape
    // alone is not enough to keep the level exactly).
    float powerIn = 0.0f, powerOut = 0.0f, keep = 1.0f;

    void prepare (double hostRate) noexcept
    {
        bands = 0;
        for (int b = 0; b < numBands; ++b)
        {
            const double fc = SampleData::Colour::lowestHz * std::exp2 ((double) b);
            if (fc < 0.45 * hostRate)
            {
                coef[(size_t) b] = (float) std::tan (juce::MathConstants<double>::pi * fc / hostRate);
                bands = b + 1;
            }
        }
        // Neighbouring octave bells overlap: one dB asked of every band gives more than one (as in Impronta).
        double sum = 0.0;
        const double A = std::pow (10.0, 1.0 / 40.0);
        for (int b = 0; b < numBands; ++b)
        {
            const double w = std::exp2 ((double) (numBands / 2 - b)), re = 1.0 - w * w;
            sum += 10.0 * std::log10 ((re * re + juce::square (w * A / q)) / (re * re + juce::square (w / (A * q))));
        }
        scale = (float) juce::jlimit (0.25, 1.0, 1.0 / std::max (1.0, sum));
        reset();
    }

    void reset() noexcept
    {
        db.fill (0.0f);
        powerIn = powerOut = 0.0f;
        keep = 1.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            ic1[(size_t) ch].fill (0.0f);
            ic2[(size_t) ch].fill (0.0f);
        }
        for (int b = 0; b < numBands; ++b)
            set (b, 0.0f);
    }

    bool engaged() const noexcept
    {
        for (int b = 0; b < bands; ++b)
            if (db[(size_t) b] != 0.0f)
                return true;
        return false;
    }

    void set (int b, float gainDb) noexcept
    {
        const float A = std::pow (10.0f, gainDb / 40.0f), g = coef[(size_t) b];
        const float k = 1.0f / (q * A);
        a1[(size_t) b] = 1.0f / (1.0f + g * (g + k));
        a2[(size_t) b] = g * a1[(size_t) b];
        a3[(size_t) b] = g * a2[(size_t) b];
        m1[(size_t) b] = k * (A * A - 1.0f);
    }

    float tick (int ch, float x) noexcept
    {
        for (int b = 0; b < bands; ++b)
        {
            auto& s1 = ic1[(size_t) ch][(size_t) b];
            auto& s2 = ic2[(size_t) ch][(size_t) b];
            const float v3 = x - s2;
            const float v1 = a1[(size_t) b] * s1 + a2[(size_t) b] * v3;
            const float v2 = s2 + a2[(size_t) b] * s1 + a3[(size_t) b] * v3;
            s1 = 2.0f * v1 - s1;
            s2 = 2.0f * v2 - s2;
            x += m1[(size_t) b] * v1;
        }
        return x;
    }
};

float lfoValue (int shape, double phase, float sampleHold) noexcept
{
    const float ph = (float) phase;
    switch (shape)
    {
        case P::Sine:       return std::sin (2.0f * pi * ph);
        case P::Triangle:   return 1.0f - 4.0f * std::abs (ph - 0.5f);
        case P::SawUp:      return 2.0f * ph - 1.0f;
        case P::SawDown:    return 1.0f - 2.0f * ph;
        case P::Square:     return ph < 0.5f ? 1.0f : -1.0f;
        case P::SampleHold: return sampleHold;
        default:            return 0.0f;
    }
}

// Detected model, measured envelope, or a geometric blend of both ("Aplanar").
// The model of the sample's envelope where the envelope asked for is divided by it, below -60 dB: the stored model
// (and blendDenominator) stop at -60 dB, the envelope asked for does not, and on a slow, curved rise (a riser of 4 s,
// a kettle coming to the boil) the ratio of the two fell to nothing: the note was silent for its first 0.3 to 0.5 s
// (-120 dB where the sample is at -56). With nothing flattened, under the floor the model's own value is used, so that
// the envelope the sample has divided by its own model is 1 there too. Above the floor nothing changes.
// Only while the key is held: after it, the release's fall is in the envelope asked for, and divided by a model
// under the floor it would come back to 1 (the tail would not fade).
inline float modelBelowFloor (const SampleData& s, double pos, float den, float flatten, bool released) noexcept
{
    if (released || flatten > 0.001f || den > 1.0001e-3f)
        return den;
    return std::max (1.0e-9f, std::min (den, s.modelAt (pos)));
}

inline float blendDenominator (float model, float env, float flatten) noexcept
{
    model = std::max (1.0e-3f, model);
    if (flatten <= 0.001f)
        return model;
    env = std::max (1.0e-3f, env);
    if (flatten >= 0.999f)
        return env;
    return std::exp ((1.0f - flatten) * std::log (model) + flatten * std::log (env));
}

//==============================================================================
// Overlapping Hann grains (4x) over the sample. When the spawn position advances
// at the read speed the grains are phase-coherent and the output equals the source.
// When stretching a pitched sample, new grains are placed a whole number of periods
// from the youngest grain (pitch-synchronous), so they stay in phase.
// Alongside the audio it returns the window-weighted envelope (model and measured)
// at the positions the grains are actually reading; dividing by it reshapes the
// sound exactly.
struct GrainCloud
{
    struct Grain { double pos = 0; int age = 0, len = 1; bool active = false; };

    std::array<Grain, 12> grains {};
    int countdown = 0;
    alignas (16) float coefs[SincTable::maxTaps] {};

    // Running power of the grain sum vs. the power it would have if the grains were
    // phase-coherent; coherenceGain() restores the level of incoherent sums.
    float powerOut = 0.0f, powerRef = 0.0f;

    float coherenceGain() const noexcept
    {
        return juce::jlimit (1.0f, 2.0f, std::sqrt ((powerRef + 1.0e-12f) / (powerOut + 1.0e-12f)));
    }

    // Starts mid-stream (grains of different ages, all reading `pos`) so the
    // window sum is already constant at the first sample.
    void prime (double pos, int len) noexcept
    {
        for (auto& g : grains)
            g.active = false;
        powerOut = powerRef = 0.0f;

        const int hop = std::max (1, len / 4);
        for (int k = 0; k < 4; ++k)
            grains[(size_t) k] = { pos, k * hop, len, true };
        countdown = hop;
    }

    void spawn (double pos, int len) noexcept
    {
        for (auto& g : grains)
        {
            if (! g.active)
            {
                g = { pos, 0, len, true };
                return;
            }
        }
    }
};

struct GrainReader : GrainCloud
{
    double alignedSpawn (double spawnPos, double period) const noexcept
    {
        const Grain* youngest = nullptr;
        for (const auto& g : grains)
            if (g.active && (youngest == nullptr || g.age < youngest->age))
                youngest = &g;
        if (youngest == nullptr)
            return spawnPos;
        return youngest->pos + std::round ((spawnPos - youngest->pos) / period) * period;
    }

    void tick (const SampleData& s, double spawnPos, double step, int len, double period, bool wantEnv, float powerCoef,
               float& outL, float& outR, float& denModel, float& denEnv) noexcept
    {
        if (--countdown <= 0)
        {
            spawn (period > 0.0 ? alignedSpawn (spawnPos, period) : spawnPos, len);
            countdown += std::max (1, len / 4);
        }

        const float* a = s.audio.getReadPointer (0);
        const float* b = s.audio.getReadPointer (1);
        const auto& win = hann();
        float l = 0.0f, r = 0.0f, wSum = 0.0f, mSum = 0.0f, eSum = 0.0f, absSum = 0.0f;

        for (auto& g : grains)
        {
            if (! g.active)
                continue;

            const float w = win.at ((float) g.age / (float) g.len);
            float xl, xr;
            readBandLimited (a, b, g.pos, s.length, step, coefs, xl, xr);
            l += w * xl;
            r += w * xr;
            absSum += w * std::abs (xl + xr);
            wSum += w;
            mSum += w * s.modelFast (g.pos);
            if (wantEnv)
                eSum += w * std::max (1.0e-3f, s.envAt (g.pos));
            g.pos += step;

            if (++g.age >= g.len)
                g.active = false;
        }

        outL = l * 0.5f;    // 4 Hann windows at 1/4 hop sum to 2
        outR = r * 0.5f;

        const float mono = l + r;
        powerOut += (mono * mono - powerOut) * powerCoef;
        powerRef += (absSum * absSum - powerRef) * powerCoef;

        if (wSum > 1.0e-4f)
        {
            denModel = mSum / wSum;
            denEnv = wantEnv ? eSum / wSum : 1.0f;
        }
        else
        {
            denModel = s.modelFast (spawnPos);
            denEnv = wantEnv ? std::max (1.0e-3f, s.envAt (spawnPos)) : 1.0f;
        }
    }
};

// Grains over a seamless loop buffer (Granular sustain). Jitter is quantised to
// whole periods for pitched sounds so the grains stay in phase.
struct LoopGrains : GrainCloud
{
    void tick (const juce::AudioBuffer<float>& buf, double centre, double step, int len, double spray, double period,
               float powerCoef, juce::Random& rng, float& outL, float& outR) noexcept
    {
        const int bufLen = buf.getNumSamples();
        if (--countdown <= 0)
        {
            double jitter = spray > 0.0 ? (rng.nextDouble() * 2.0 - 1.0) * spray : 0.0;
            if (period > 0.0)
                jitter = std::round (jitter / period) * period;
            spawn (centre + jitter, len);
            countdown += std::max (1, len / 4);
        }

        const float* a = buf.getReadPointer (0);
        const float* b = buf.getReadPointer (1);
        const auto& win = hann();
        float l = 0.0f, r = 0.0f, absSum = 0.0f;

        for (auto& g : grains)
        {
            if (! g.active)
                continue;
            const float w = win.at ((float) g.age / (float) g.len);
            float xl, xr;
            readBandLimitedWrap (a, b, g.pos, bufLen, step, coefs, xl, xr);
            l += w * xl;
            r += w * xr;
            absSum += w * std::abs (xl + xr);
            g.pos += step;
            if (++g.age >= g.len)
                g.active = false;
        }

        outL = l * 0.5f;
        outR = r * 0.5f;
        const float mono = l + r;
        powerOut += (mono * mono - powerOut) * powerCoef;
        powerRef += (absSum * absSum - powerRef) * powerCoef;
    }
};

//==============================================================================
// Warp: the pitch without the speed. The source is read in time at its own pace and the pitch
// is made by how grains of it are laid out.
//  - Tonos (TD-PSOLA): each grain is two cycles of the source, centred on the start of a cycle and
//    read at the speed it was recorded, so its formants stay where they were; grains are laid one
//    new period apart. Consecutive grains come from consecutive cycles, so they meet in phase.
//  - Textura, and sounds without a steady period: longer grains read at the new speed, laid a
//    quarter of their length apart around the time position: the spectrum moves, the timing stays.
// Nothing is computed ahead: no latency, and the cost of reading the source a few times over.
struct WarpReader
{
    struct Grain { double src = 0, step = 1; int age = 0, len = 1; bool active = false; };
    std::array<Grain, 24> grains {};
    double untilNext = 0.0;
    static constexpr int maxGroups = 3;
    std::array<float, maxGroups> powerOut {}, powerRef {}, match { 1.0f, 1.0f, 1.0f };
    double head = -1.0;          // where the attack is read at its pitch, as Re-Pitch would
    int newest = -1;             // the grain started last
    float measured = 0.0f;       // how long both powers have been followed, in time constants
    int headHeld = 0;            // samples the attack has been held past its end
    float headMix = 1.0f;

    // Up to six channels read by the same grains: the original and the beds of engine A, say,
    // so that what they share stays in phase where one hands over to the other. Each channel has
    // its own length, wraps or not, an offset from the grain's position in the time line, and a
    // group: each group's level is matched to its own source (an original fading under a bed at
    // full level must not be judged by the bed).
    struct Source
    {
        static constexpr int maxChannels = 6;
        const float* ch[maxChannels] {};
        int length[maxChannels] {};
        bool wrap[maxChannels] {};
        double offset[maxChannels] {};
        int group[maxChannels] {};
        int channels = 2;
        // The attack, up to `attackEnd` in the source, is read whole at the new pitch, the way
        // Re-Pitch reads it (grains would smear or double the hit), then handed to the grains over
        // `1 / fadeStep` samples. 0: no attack of its own (a loop, a frozen cycle).
        double attackEnd = 0.0;
        float fadeStep = 1.0f;
        // Optionally, channels quadFirst..quadFirst+3 also side by side, frame by frame (the same
        // values): read from here when all four are wanted.
        const float* quad = nullptr;
        int quadFirst = -1;
    };

#if defined (__SSE2__) || defined (_M_X64)
    // hermite() on four lanes at once, step by step the same arithmetic: the same results.
    static __m128 hermite4 (__m128 xm1, __m128 x0, __m128 x1, __m128 x2, float f) noexcept
    {
        const __m128 ff = _mm_set1_ps (f);
        const __m128 c1 = _mm_mul_ps (_mm_set1_ps (0.5f), _mm_sub_ps (x1, xm1));
        const __m128 c2 = _mm_sub_ps (_mm_add_ps (_mm_sub_ps (xm1, _mm_mul_ps (_mm_set1_ps (2.5f), x0)), _mm_mul_ps (_mm_set1_ps (2.0f), x1)),
                                      _mm_mul_ps (_mm_set1_ps (0.5f), x2));
        const __m128 c3 = _mm_add_ps (_mm_mul_ps (_mm_set1_ps (0.5f), _mm_sub_ps (x2, xm1)), _mm_mul_ps (_mm_set1_ps (1.5f), _mm_sub_ps (x0, x1)));
        return _mm_add_ps (_mm_mul_ps (_mm_add_ps (_mm_mul_ps (_mm_add_ps (_mm_mul_ps (c3, ff), c2), ff), c1), ff), x0);
    }
#endif

#if defined (__SSE2__) || defined (_M_X64)
    // readSet() of a set of up to four channels into one register, a lane per channel (0 where a
    // channel has no data).
    static __m128 readSet4 (const Source& src, int first, int end, double pos) noexcept
    {
        const double at = pos + src.offset[first];
        int i = (int) at;
        if ((double) i > at)
            --i;
        const float f = (float) (at - (double) i);
        const int len = src.length[first];
        const bool inside = i >= 1 && i + 2 < len;
        if (src.quad != nullptr && first == src.quadFirst && end - first == 4 && src.ch[first] != nullptr && src.ch[first + 1] != nullptr
            && src.ch[first + 2] != nullptr && src.ch[first + 3] != nullptr && (inside || src.wrap[first]))
        {
            int k0 = i - 1, k1 = i, k2 = i + 1, k3 = i + 2;
            if (! inside)
            {
                const auto w = [len] (int k) { k %= len; return k < 0 ? k + len : k; };
                k0 = w (k0); k1 = w (k1); k2 = w (k2); k3 = w (k3);
            }
            return hermite4 (_mm_loadu_ps (src.quad + 4 * k0), _mm_loadu_ps (src.quad + 4 * k1),
                             _mm_loadu_ps (src.quad + 4 * k2), _mm_loadu_ps (src.quad + 4 * k3), f);
        }
        if (inside)
        {
            __m128 t[4];
            for (int k = 0; k < 4; ++k)
            {
                const int c = first + k;
                t[k] = c < end && src.ch[c] != nullptr ? _mm_loadu_ps (src.ch[c] + i - 1) : _mm_setzero_ps();
            }
            _MM_TRANSPOSE4_PS (t[0], t[1], t[2], t[3]);
            return hermite4 (t[0], t[1], t[2], t[3], f);
        }
        float x[Source::maxChannels] {};
        readSet (src, first, end, pos, x);
        alignas (16) float r[4] {};
        for (int c = first; c < end; ++c)
            r[c - first] = src.ch[c] != nullptr ? x[c] : 0.0f;
        return _mm_load_ps (r);
    }
#endif

    // Cubic reads (readCubic / readCubicWrap) of channels first..end-1 of src, which share offset,
    // length and wrapping, at one position. Channels without data are left as they were.
    static void readSet (const Source& src, int first, int end, double pos, float* x) noexcept
    {
        const double at = pos + src.offset[first];
        // floor() without the library call: the same whole number for any position a buffer can have.
        int i = (int) at;
        if ((double) i > at)
            --i;
        const float f = (float) (at - (double) i);
        const int len = src.length[first];
#if defined (__SSE2__) || defined (_M_X64)
        if (src.quad != nullptr && first == src.quadFirst && end - first == 4 && src.ch[first] != nullptr && src.ch[first + 1] != nullptr
            && src.ch[first + 2] != nullptr && src.ch[first + 3] != nullptr && ((i >= 1 && i + 2 < len) || src.wrap[first]))
        {
            int k0 = i - 1, k1 = i, k2 = i + 1, k3 = i + 2;
            if (! (i >= 1 && i + 2 < len))
            {
                const auto w = [len] (int k) { k %= len; return k < 0 ? k + len : k; };
                k0 = w (k0); k1 = w (k1); k2 = w (k2); k3 = w (k3);
            }
            _mm_storeu_ps (x + first, hermite4 (_mm_loadu_ps (src.quad + 4 * k0), _mm_loadu_ps (src.quad + 4 * k1),
                                                _mm_loadu_ps (src.quad + 4 * k2), _mm_loadu_ps (src.quad + 4 * k3), f));
            return;
        }
        // Up to four channels at once: their four samples each, turned so that every register holds
        // one tap of all the channels, through the same arithmetic as hermite(), step by step.
        if (i >= 1 && i + 2 < len && end - first <= 4)
        {
            __m128 t[4];
            for (int k = 0; k < 4; ++k)
            {
                const int c = first + k;
                t[k] = c < end && src.ch[c] != nullptr ? _mm_loadu_ps (src.ch[c] + i - 1) : _mm_setzero_ps();
            }
            _MM_TRANSPOSE4_PS (t[0], t[1], t[2], t[3]);
            const __m128 h = hermite4 (t[0], t[1], t[2], t[3], f);
            alignas (16) float r[4];
            _mm_store_ps (r, h);
            for (int c = first; c < end; ++c)
                if (src.ch[c] != nullptr)
                    x[c] = r[c - first];
            return;
        }
#endif
        if (i >= 1 && i + 2 < len)
        {
            for (int c = first; c < end; ++c)
                if (const float* d = src.ch[c])
                    x[c] = hermite (d[i - 1], d[i], d[i + 1], d[i + 2], f);
            return;
        }
        if (src.wrap[first])
        {
            const auto w = [len] (int k) { k %= len; return k < 0 ? k + len : k; };
            const int i0 = w (i - 1), i1 = w (i), i2 = w (i + 1), i3 = w (i + 2);
            for (int c = first; c < end; ++c)
                if (const float* d = src.ch[c])
                    x[c] = hermite (d[i0], d[i1], d[i2], d[i3], f);
            return;
        }
        if (i < -2 || i > len)
        {
            for (int c = first; c < end; ++c)
                x[c] = 0.0f;
            return;
        }
        for (int c = first; c < end; ++c)
            if (const float* d = src.ch[c])
                x[c] = hermite (sampleAt (d, i - 1, len), sampleAt (d, i, len), sampleAt (d, i + 1, len), sampleAt (d, i + 2, len), f);
    }

    void reset() noexcept
    {
        for (auto& g : grains)
            g.active = false;
        untilNext = 0.0;
        powerOut.fill (0.0f);
        powerRef.fill (0.0f);
        match.fill (1.0f);
        head = -1.0;
        headMix = 1.0f;
        newest = -1;
        measured = 0.0f;
        headHeld = 0;
    }

    // time: where the note is in the source; timeStep: source samples per output sample;
    // ratio: the pitch; marks: cycle starts (nullptr: cycles every `period` from 0, as in a loop).
    void tick (const Source& src, double time, double timeStep, double ratio, bool tonal, const std::vector<double>* marks,
               double period, int textureLen, float powerCoef, float* out) noexcept
    {
        const bool cycles = tonal && period > 1.0 && (marks == nullptr || marks->size() > 2);
        if (untilNext <= 0.0)
        {
            Grain g;
            double hop;
            if (cycles)
            {
                double centre = 0.0, local = period;
                if (marks != nullptr)
                {
                    const auto& m = *marks;
                    auto it = std::lower_bound (m.begin(), m.end(), time);
                    size_t i = (size_t) std::distance (m.begin(), it);
                    if (i >= m.size())
                        i = m.size() - 1;
                    if (i > 0 && time - m[i - 1] < m[i] - time)
                        --i;
                    centre = m[i];
                    local = i + 1 < m.size() ? m[i + 1] - m[i] : m[i] - m[i - 1];
                }
                else
                {
                    centre = std::round (time / period) * period;
                }
                g.step = timeStep;
                g.len = std::max (4, (int) std::lround (2.0 * local / timeStep));
                g.src = centre - local;
                hop = local / timeStep / std::max (0.05, ratio);
            }
            else
            {
                g.step = timeStep * ratio;
                g.len = std::max (16, textureLen);
                // The middle of the grain reads the source where the time line is when the
                // middle sounds: no lag, and going up it reaches back half as far before the onset.
                g.src = time + 0.5 * g.len * (timeStep - g.step);
                hop = g.len * 0.25;
                // A sound with a pitch: the new grain starts a whole number of cycles from where
                // the last one is reading, so that the two add up in phase instead of beating
                // (the way WSOLA keeps a tone whole). Without a pitch the grains fall where they fall.
                if (period > 1.0 && newest >= 0 && grains[(size_t) newest].active)
                {
                    const double carry = grains[(size_t) newest].src;
                    g.src = carry + std::round ((g.src - carry) / period) * period;
                }
            }
            g.active = true;
            for (size_t slot = 0; slot < grains.size(); ++slot)
                if (! grains[slot].active)
                {
                    grains[slot] = g;
                    newest = (int) slot;
                    break;
                }
            untilNext += hop;
        }
        untilNext -= 1.0;

        // Channels that lie the same way in the time line (same offset, length and wrapping: the two
        // sides of the original, the four of the beds) are read at one position: where it falls and
        // its fraction are worked out once for all of them. The same reads as one by one, bit for bit.
        int setFirst[Source::maxChannels], setEnd[Source::maxChannels], sets = 0;
        int usedGroups = 0;      // groups with a channel to read: the others are silent and need no level
        for (int c = 0; c < src.channels;)
        {
            int e = c + 1;
            while (e < src.channels && src.offset[e] == src.offset[c] && src.length[e] == src.length[c] && src.wrap[e] == src.wrap[c])
                ++e;
            bool any = false;
            for (int k = c; k < e; ++k)
                if (src.ch[k] != nullptr)
                {
                    any = true;
                    usedGroups |= 1 << juce::jlimit (0, maxGroups - 1, src.group[k]);
                }
            if (any)
            {
                setFirst[sets] = c;
                setEnd[sets++] = e;
            }
            c = e;
        }

        const auto& win = hann();
        float acc[Source::maxChannels] {}, absSum = 0.0f, wSum = 0.0f;
        float x[Source::maxChannels] {};
#if defined (__SSE2__) || defined (_M_X64)
        bool narrow = true;      // every set fits in one register
        for (int s = 0; s < sets; ++s)
            narrow = narrow && setEnd[s] - setFirst[s] <= 4;
        if (narrow)
        {
            // Each set of channels in one register: the reads, the windowing and the sums lane by lane,
            // the same operations as below on each channel.
            __m128 sumV[Source::maxChannels];
            for (int s = 0; s < sets; ++s)
                sumV[s] = _mm_setzero_ps();
            for (auto& g : grains)
            {
                if (! g.active)
                    continue;
                const __m128 wv = _mm_set1_ps (win.at ((float) g.age / (float) g.len));
                for (int s = 0; s < sets; ++s)
                    sumV[s] = _mm_add_ps (sumV[s], _mm_mul_ps (wv, readSet4 (src, setFirst[s], setEnd[s], g.src)));
                g.src += g.step;
                if (++g.age >= g.len)
                    g.active = false;
            }
            for (int s = 0; s < sets; ++s)
            {
                alignas (16) float r[4];
                _mm_store_ps (r, sumV[s]);
                for (int c = setFirst[s]; c < setEnd[s]; ++c)
                    acc[c] = r[c - setFirst[s]];
            }
        }
        else
#endif
        for (auto& g : grains)
        {
            if (! g.active)
                continue;
            const float w = win.at ((float) g.age / (float) g.len);
            float mono = 0.0f;
            for (int s = 0; s < sets; ++s)
            {
                readSet (src, setFirst[s], setEnd[s], g.src, x);
                for (int c = setFirst[s]; c < setEnd[s]; ++c)
                    if (src.ch[c] != nullptr)
                    {
                        acc[c] += w * x[c];
                        mono += x[c];
                    }
            }
            absSum += w * std::abs (mono);
            wSum += w;
            g.src += g.step;
            if (++g.age >= g.len)
                g.active = false;
        }

        // The windows add up `ratio` deep in Tonos and four deep in Textura. Beyond that, the level
        // is the source's own at this moment: keeping the formants moves the new fundamental to
        // where the source had its second harmonic, say (a flute an octave up loses 9 dB), and
        // cycles not quite alike meet less in phase. Both powers are followed over 20 ms and the
        // output is brought to the source's, within 18 dB either way. The correction itself moves
        // over some 40 ms from no correction at all: at an onset the source rises before the grains
        // that straddle it, and a correction that followed at once would hit the attack up to 4x.
        // Cycles laid closer than they were overlap and add up (1 / ratio); laid further apart than
        // their two periods they stand alone with gaps between, and only their power adds up
        // (a Hann grain carries 3/8 of its length): the smaller of the two.
        const float r = (float) std::max (0.05, ratio);
        const float base = cycles ? std::min (1.0f / r, std::sqrt (4.0f / (3.0f * r))) : 0.5f;
        std::array<float, maxGroups> sum {}, reference {};
        for (int s = 0; s < sets; ++s)
        {
            readSet (src, setFirst[s], setEnd[s], time, x);
            for (int c = setFirst[s]; c < setEnd[s]; ++c)
            {
                if (src.ch[c] == nullptr)
                    continue;
                const auto group = (size_t) juce::jlimit (0, maxGroups - 1, src.group[c]);
                sum[group] += acc[c] * base;
                reference[group] += x[c];
            }
        }
        // Until the grains have left the attack behind they hold part of it and part of what came
        // before, and their power says nothing of the source's: the correction waits.
        const double reach = cycles ? 2.0 * period : 0.5 * std::max (16, textureLen) * std::max (timeStep, timeStep * ratio);
        const bool settled = time > src.attackEnd + reach;
        if (settled)
            measured += powerCoef;
        std::array<float, maxGroups> gain {};
        for (size_t g = 0; g < (size_t) maxGroups; ++g)
        {
            if (((usedGroups >> g) & 1) == 0)
                continue;
            if (settled)
            {
                powerOut[g] += (sum[g] * sum[g] - powerOut[g]) * powerCoef;
                powerRef[g] += (reference[g] * reference[g] - powerRef[g]) * powerCoef;
                const float target = juce::jlimit (0.125f, 8.0f, std::sqrt ((powerRef[g] + 1.0e-12f) / (powerOut[g] + 1.0e-12f)));
                if (measured > 0.75f)          // both powers have some 15 ms behind them
                    match[g] += (target - match[g]) * powerCoef * (headMix >= 1.0f ? 3.0f : 0.5f);
            }
            gain[g] = base * match[g];
        }
        juce::ignoreUnused (wSum, absSum);
        for (int c = 0; c < src.channels; ++c)
            out[c] = acc[c] * gain[(size_t) juce::jlimit (0, maxGroups - 1, src.group[c])];

        if (headMix > 0.0f)
        {
            if (head < 0.0)
            {
                head = time;
                if (src.attackEnd <= time)      // a note that starts past the attack has none
                    headMix = 0.0f;
            }
            if (headMix > 0.0f)
            {
                // Equal power: the attack and the grains read different places and do not add up in phase.
                const float toHead = std::sqrt (headMix), toGrains = std::sqrt (1.0f - headMix);
                for (int s = 0; s < sets; ++s)
                {
                    readSet (src, setFirst[s], setEnd[s], head, x);
                    for (int c = setFirst[s]; c < setEnd[s]; ++c)
                        if (src.ch[c] != nullptr)
                            out[c] = out[c] * toGrains + x[c] * toHead;
                }
                head += timeStep * ratio;
                // Past the attack, the grains take over once they have left it behind and their
                // level is known (the attack covers them meanwhile), within 50 ms.
                if (head >= src.attackEnd)
                {
                    const bool ready = settled && measured > 2.0f;
                    if (ready || ++headHeld > (int) (5.0f / src.fadeStep) || headMix < 1.0f)
                        headMix = std::max (0.0f, headMix - src.fadeStep);
                }
            }
        }
    }
};

// Where the attack of the sample ends for Warp: its peak, no later than 30 ms past the onset.
double warpAttackEndOf (const SampleData& s) noexcept
{
    const double longest = 0.03 * s.sampleRate;
    return s.onset + juce::jlimit (0.005 * s.sampleRate, longest, s.peak - s.onset);
}

// The test of SynthEngine::checkTones: the strongest partials of a sound, and how much of a sound's
// energy falls where another has partials.
struct Partial { double hz; float power; };

// Power spectrum of x through a Hann window, zero padded to at least twice its length.
std::vector<float> powerSpectrum (const std::vector<float>& x, double sampleRate, double& binHz)
{
    int order = 10;
    while ((1 << order) < 2 * (int) x.size() && order < 19)
        ++order;
    const int size = 1 << order, n = std::min ((int) x.size(), size);
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) 2 * size, 0.0f);
    for (int i = 0; i < n; ++i)
        buf[(size_t) i] = x[(size_t) i] * (0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) std::max (1, n - 1)));
    fft.performFrequencyOnlyForwardTransform (buf.data(), true);
    std::vector<float> power ((size_t) size / 2 + 1);
    for (size_t k = 0; k < power.size(); ++k)
        power[k] = buf[k] * buf[k];
    binHz = sampleRate / size;
    return power;
}

// The `top` strongest peaks between lo and hi Hz, each the largest within 2 Hz either side and no
// more than `rangeDb` under the strongest of the band.
std::vector<Partial> strongestPartials (const std::vector<float>& power, double binHz, double lo, double hi, int top, float rangeDb)
{
    const int from = std::max (1, (int) std::ceil (lo / binHz)), to = std::min ((int) power.size() - 1, (int) (hi / binHz));
    const int reach = std::max (1, (int) std::round (2.0 / binHz));
    float strongest = 0.0f;
    for (int k = from; k < to; ++k)
        strongest = std::max (strongest, power[(size_t) k]);
    const float floor = strongest * std::pow (10.0f, -rangeDb / 10.0f);
    std::vector<Partial> found;
    for (int k = from; k < to; ++k)
    {
        const float v = power[(size_t) k];
        if (v <= floor)
            continue;
        bool largest = true;
        for (int j = std::max (0, k - reach); j <= std::min ((int) power.size() - 1, k + reach) && largest; ++j)
            largest = power[(size_t) j] < v || (power[(size_t) j] == v && j >= k);
        if (largest)
            found.push_back ({ k * binHz, v });
    }
    std::sort (found.begin(), found.end(), [] (const Partial& a, const Partial& b) { return a.power > b.power; });
    if ((int) found.size() > top)
        found.resize ((size_t) top);
    return found;
}

// Share of the energy of `played` (its strongest partials) that lies on a partial of `wanted`:
// within 2.7 Hz or 1.2 %, as in the audit of the library.
float shareOnGrid (const std::vector<Partial>& played, const std::vector<Partial>& wanted)
{
    double on = 0.0, all = 0.0;
    for (const auto& p : played)
    {
        all += p.power;
        for (const auto& w : wanted)
            if (std::abs (w.hz - p.hz) <= std::max (2.7, 0.012 * p.hz))
            {
                on += p.power;
                break;
            }
    }
    return all > 0.0 ? (float) (on / all) : 1.0f;
}

// Plain 12 dB lowpass (TPT state variable) used to darken the sampled engines.
struct TiltFilter
{
    float ic1[2] {}, ic2[2] {};
    float a1 = 1, a2 = 0, a3 = 0;
    bool active = false;

    void reset() noexcept { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f; }

    void set (float cutoff, double sr) noexcept
    {
        active = cutoff < 18000.0f;
        const float g = std::tan (pi * juce::jlimit (40.0f, (float) sr * 0.45f, cutoff) / (float) sr);
        const float k = 1.4142f;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    float process (int ch, float x) noexcept
    {
        const float v3 = x - ic2[ch];
        const float v1 = a1 * ic1[ch] + a2 * v3;
        const float v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3;
        ic1[ch] = 2.0f * v1 - ic1[ch];
        ic2[ch] = 2.0f * v2 - ic2[ch];
        return v2;
    }
};

//==============================================================================
// A cloud of short windowed grains read from the sample: the voice of engine E, and the
// granular texture that the other engines can mix in.
struct CloudSettings
{
    int size = 1;                   // grain length, in output samples
    double interval = 1.0;          // output samples between grain births, on average
    double spray = 0.0;             // sample positions, either side of the centre
    float pitchSpread = 0.0f;       // semitones, either side
    float reverseChance = 0.0f;
    float width = 0.0f;
    double period = 0.0;            // Coherente (engine E): the note's period in samples of the sample; 0 off
};

struct GranularCloud
{
    struct Grain { double pos = 0, step = 1; int age = 0, len = 1; float gainL = 1, gainR = 1; bool active = false; };
    std::array<Grain, 64> grains {};
    std::uint64_t live = 0;         // bit i set: grains[i] sounds (visited in slot order, the free ones skipped)
    double untilNext = 0.0;
    double reading = -1.0;          // Coherente: a reading of the sample that never jumps; grains start in phase with it
    alignas (16) float coefs[SincTable::maxTaps] {};

    static int lowestBit (std::uint64_t m) noexcept
    {
       #if defined (_MSC_VER)
        unsigned long i;
        _BitScanForward64 (&i, m);
        return (int) i;
       #else
        return __builtin_ctzll (m);
       #endif
    }

    void reset() noexcept
    {
        for (auto& g : grains)
            g.active = false;
        live = 0;
        untilNext = 0.0;
        reading = -1.0;
    }

    void spawn (double pos, double step, int len, float pan) noexcept
    {
        if (live == ~std::uint64_t (0))
            return;
        const int i = lowestBit (~live);        // the first free slot
        auto& g = grains[(size_t) i];
        g = { pos, step, 0, len, 1.0f, 1.0f, true };
        Dsp::panGains (pan, g.gainL, g.gainR);
        live |= std::uint64_t (1) << i;
    }

    // Adds num samples of cloud around `centre`, each scaled by gain[i].
    void render (const SampleData& s, double centre, double step, const CloudSettings& c, juce::Random& rng,
                 const float* gain, float* outL, float* outR, int num) noexcept
    {
        const float* a = s.audio.getReadPointer (0);
        const float* b = s.audio.getReadPointer (1);
        const auto& win = hann();

        // Unrelated grains add up in power; a Hann window keeps 3/8 of it.
        float norm = 1.0f / std::sqrt (0.375f * (float) std::max (1.0, c.size / c.interval));
        // Coherente: grains a whole number of periods from a reading that never jumps, born at even intervals (a
        // whole number of them in a grain): they add up in phase, their windows to a steady 1/2 per grain. Placed
        // anywhere, grains of a note cancel each other by chance, and the more the lower it is: a bass of 55 Hz
        // came out 9 dB over its own noise, and its pitch wandered by a semitone.
        const bool coherent = c.period > 1.0;
        double interval = c.interval;
        if (coherent)
        {
            const double perGrain = std::max (2.0, std::round (c.size / c.interval));
            interval = c.size / perGrain;
            norm = (float) (2.0 / perGrain);
            if (reading < 0.0)
                reading = centre;
        }

        for (int i = 0; i < num; ++i)
        {
            if ((untilNext -= 1.0) <= 0.0)
            {
                double pos = centre + c.spray * (rng.nextDouble() * 2.0 - 1.0);
                double grainStep = step * std::exp2 (c.pitchSpread * (rng.nextFloat() * 2.0f - 1.0f) / 12.0f);
                const bool backwards = rng.nextFloat() < c.reverseChance;
                if (coherent && ! backwards && c.pitchSpread <= 0.0f)
                    pos = reading + std::round ((pos - reading) / c.period) * c.period;
                spawn (backwards ? pos + grainStep * c.size : pos, backwards ? -grainStep : grainStep, c.size,
                       c.width * (rng.nextFloat() * 2.0f - 1.0f));
                untilNext += coherent ? interval : interval * (0.75 + 0.5 * rng.nextDouble());
            }
            if (coherent)
                reading += step;

            float l = 0.0f, r = 0.0f;
            for (auto m = live; m != 0; m &= m - 1)
            {
                const int gi = lowestBit (m);
                auto& g = grains[(size_t) gi];
                const float w = win.at ((float) g.age / (float) g.len);
                float xl, xr;
                readBandLimited (a, b, g.pos, s.length, g.step, coefs, xl, xr);
                l += w * g.gainL * xl;
                r += w * g.gainR * xr;
                g.pos += g.step;
                if (++g.age >= g.len)
                {
                    g.active = false;
                    live &= ~(std::uint64_t (1) << gi);
                }
            }
            outL[i] += l * norm * gain[i];
            outR[i] += r * norm * gain[i];
        }
    }
};

} // namespace

//==============================================================================
class Voice
{
public:
    // Fixed seeds: the same notes and settings always render the same audio.
    Voice (SynthEngine& e, int index) : engine (e), rng (0x5eed + index), noise (0xfeed + index)
    {
        mech.seed (0x3c4a + index);
        windowCuts.reserve ((size_t) SynthEngine::maxBeatCuts + 1);
    }

    void prepare (double sr)
    {
        hostRate = sr;
        filter.prepare (sr);
        mech.prepare (sr);
        motion.prepare (sr);
    }

    bool isActive() const noexcept { return active; }
    // The modulation of this note's last block, and when it started (the newest note's is shown on the knobs).
    float modulationOf (int dst) const noexcept { return mod[(size_t) dst]; }
    juce::uint32 startOrder() const noexcept { return order; }
    bool isReleased() const noexcept { return released; }
    bool isFading() const noexcept { return fading; }
    int getNote() const noexcept { return note; }
    juce::uint32 getOrder() const noexcept { return order; }
    float getPitch() const noexcept { return pitch; }
    void stopImmediately() noexcept
    {
        active = false;
        releaseDrone();
        releaseStretcher();
    }

    float getPlayhead() const noexcept
    {
        if (! active)
            return -1.0f;
        // LOOP: where it really reads (a slice: inside its slice; Normal: the loop's time line). The sampler's
        // own reading position stays at its start there: the playhead used to sit on the first slice.
        if (looping)
            return slicing ? (loopRead < sliceTo ? (float) loopRead : -1.0f) : (float) (loopTime + loopWinStart);
        if (drone != nullptr)
            return (float) drone->position();
        const auto& ln = lanes[0];
        if (ln.srcMix >= 1.0f && sustainSource != nullptr && sustainSource->loop.getNumSamples() > 0)
            return (float) (sustainSource->start + std::fmod (ln.srcPos, (double) sustainSource->loop.getNumSamples()));
        return (float) ln.pos;
    }

    int getSlice() const noexcept { return active && slicing && ! fading ? sliceIndex : -1; }
    bool transport = false;         // the voice Play con el host plays

    bool pedalHeld = false;
    float pressure = 0.0f;      // polyphonic aftertouch of this voice's key
    bool onPrevious = false;    // started before the sample's analysis was extended: plays that one to its end
    int channel = 1;            // the MIDI channel it was played on (MPE: its own)

    // MPE of this note: bend in semitones, pressure 0-1, and how much brighter its timbre makes it (octaves).
    float mpeBend() const noexcept
    {
        return engine.mpe && channel != 1 ? engine.chanBend[(size_t) channel] * engine.chanBendRange[(size_t) channel] : 0.0f;
    }
    float mpePressure() const noexcept { return engine.mpe && channel != 1 ? engine.chanPressure[(size_t) channel] : 0.0f; }
    float timbreOct() const noexcept
    {
        return engine.timbreSeen[(size_t) channel] ? (engine.chanTimbre[(size_t) channel] - 0.5f) * 3.0f : 0.0f;
    }

    void start (const SampleData& s, const ParamSnapshot& p, int newNote, float vel, float fromPitch, juce::uint32 newOrder)
    {
        // Each note's chance (grains, noise, Drone's phases, Mecanismo) from a seed of its own: drawn from the
        // engine's sequence (a different one for every note), or in the tests from its key alone, so that a note
        // sounds the same alone or among others. Before, each voice went on with its own sequence from note to
        // note: what a note got depended on what that voice had played before, and a voice disturbed by another
        // could not be told from chance (ReanimaAudit estres).
        const juce::int64 seed = engine.keySeeds.load (std::memory_order_relaxed) ? (juce::int64) newNote * 0x9e3779b9LL + 17 : engine.rng.nextInt64();
        rng.setSeed (seed);
        noise.seed (seed ^ 0x6a09e667LL);
        mech.seed (seed ^ 0x3c6ef372LL);
        active = true;
        onPrevious = false;
        released = pedalHeld = fading = false;
        pressure = 0.0f;
        vibPhase = 0.0;
        fadeGain = 1.0f;
        note = newNote;
        velocity = vel;
        order = newOrder;
        targetPitch = (float) newNote;
        pitch = (p[P::Glide] > 0.0f && fromPitch >= 0.0f) ? fromPitch : targetPitch;
        t = tRel = 0.0;
        relLevel = relGain = ampLevel = envelopeLevel = 0.0f;
        e2Stage = 0;
        e2Level = 0.0f;
        follower = 0.0f;
        holdCount = 0.0f;
        holdL = holdR = 0.0f;
        keyUpPending = false;
        randomValue = rng.nextFloat() * 2.0f - 1.0f;

        // Each note differs a little in tuning, level and brightness. It never moves the start
        // point: that would cut the attack and shift the envelope of the note.
        variationCents = p[P::Variation] * 8.0f * (rng.nextFloat() * 2.0f - 1.0f);
        variationGain = dbToGain (p[P::Variation] * 1.5f * (rng.nextFloat() * 2.0f - 1.0f));
        variationDark = p[P::Variation] * 0.8f * rng.nextFloat();

        sustainSource = nullptr;
        heldOriginal = 1.0f;
        heldBright = heldDark = 0.0f;
        tiltMix = 0.0f;
        gainsReady = false;

        // Retrig starts each note from zero; otherwise the note joins the free-running LFO.
        for (int l = 0; l < P::numLfos; ++l)
        {
            const bool retrig = p[(size_t) (P::Lfo1Rate + l * P::lfoStride + 4)] > 0.5f;
            lfoPhase[(size_t) l] = retrig ? 0.0 : engine.globalLfoPhase[(size_t) l];
            lfoHold[(size_t) l] = retrig ? rng.nextFloat() * 2.0f - 1.0f : engine.globalSampleHold[(size_t) l];
            drift[(size_t) l].start (rng.getSeed() * 31 + l * 7919 + 1);       // reads the seed, draws nothing
            lfoFresh[(size_t) l] = true;
        }

        mod.fill (0.0f);
        filter.reset();
        motion.reset();
        tilt.reset();
        noise.reset();
        mech.reset();
        updateModulation (p, 0.0);

        // Per-partial constants for engine C: brightness weight and stereo position.
        const auto& pm = s.partials;
        const bool usesPartials = (int) p[P::Engine] == P::EngineResynth && pm.valid();
        if (usesPartials)
        {
            const int ref = juce::jlimit (0, pm.numFrames - 1, (int) std::round ((s.peak + 0.03 * s.sampleRate) / pm.hop));
            for (int k = 0; k < std::min (maxPartials, pm.numPartials); ++k)
            {
                const float f = std::max (1.0f, pm.freq[(size_t) ref * pm.numPartials + k]);
                brightness[(size_t) k] = juce::jlimit (0.0f, 5.0f, std::log2 (f / pm.refFreq));
                pm.gains (k, partialPanL[(size_t) k], partialPanR[(size_t) k]);
            }
            for (int b = 0; b < SampleData::PartialModel::numBands; ++b)
            {
                const float centre = std::sqrt (pm.bandEdges[(size_t) b] * pm.bandEdges[(size_t) b + 1]);
                bandBrightness[(size_t) b] = juce::jlimit (0.0f, 5.0f, std::log2 (centre / pm.refFreq));
            }
        }

        numLanes = juce::jlimit (1, (int) lanes.size(), juce::roundToInt (p[P::Unison]));
        const double startPos = s.startPosition (p[P::StartPos] + mod[P::DstStart]);
        const int grainLen = grainLength (p);

        // Starting anywhere but the detected onset could land on a loud sample: fade in quickly.
        skippedSeconds = std::max (0.0, (startPos - s.onset) / s.sampleRate);
        startFade = startPos == s.onset ? 1.0f : 0.0f;
        startFadeStep = 1.0f / (0.002f * (float) hostRate);

        const double bedLength = std::max (1, s.bedBright.length);
        const bool granular = playingEngine (s, p) == P::EngineGranular;

        // Detuned lanes also start apart. In tune that would only comb-filter the sound.
        const double laneSpread = numLanes > 1 ? 0.002 * s.sampleRate * laneIndependence (p) : 0.0;

        for (int i = 0; i < numLanes; ++i)
        {
            auto& ln = lanes[(size_t) i];
            ln.offset = numLanes == 1 ? 0.0f : (float) i / (float) (numLanes - 1) * 2.0f - 1.0f;
            ln.pos = startPos + rng.nextDouble() * laneSpread;
            ln.transPos = ln.pos;
            ln.seg = ln.pos < s.peak ? SegAttack : (ln.pos < s.knee ? SegDecay : SegBody);
            ln.done = false;
            ln.srcMix = 0.0f;
            ln.grainCoh = ln.srcCoh = -1.0f;
            ln.srcPos = 0.0;
            ln.ratio = 1.0f;
            ln.step = laneStep (s, p, ln.offset);
            ln.grains.prime (ln.pos, grainLen);
            ln.warp.reset();
            ln.warpSrc.reset();
            ln.warpTrans.reset();
            ln.bedPos = i > 0 && laneSpread > 0.0 ? std::floor (rng.nextDouble() * bedLength)
                                                  : std::fmod (s.bedBright.origin + startPos, bedLength);
            ln.synced = false;
            ln.punchLow = {};
            ln.cloud.reset();
            if (granular)
            {
                ln.pos = juce::jlimit (0.0, (double) s.length - 1.0, s.readingPosition (p[P::GrainPos] + mod[P::DstStart]));
                ln.levelGain = levelFor (s, ln.pos);
            }
            ln.amp.fill (0.0f);
            if (usesPartials)
                syncPartialPhases (s, ln);
        }

        mechStart = startPos;
        mech.keyDown ((int) p[P::MechType], SynthEngine::mechanismAmount (p), s, mechNote (s, p, startPos));

        // Engine F: a stream of long frames, from the Posición point. It needs a few blocks
        // before its first frame is ready.
        releaseDrone();
        loopTime = loopRead = loopFadeRead = 0.0;
        loopWinStart = loopWinLength = 0;
        windowOut = 0.0f;
        stretcherPrimed = false;
        sliceFrom = sliceTo = 0.0;
        looping = (int) p[P::PlayMode] == P::PlayLoop;
        sliceIndex = -1;
        transport = transportTuned = false;
        rubberMix = 0.0f;
        lastRendered = nullptr;
        rubberStill = 0;
        sliceGate = p[P::LoopHitEnv] > 0.5f;
        hitT = 0.0;
        hitFrom = 0.0f;
        hitStrength = 1.0f;
        hitStrikePending = false;
        perHitTimbre = false;
        lastTimeStep = 0.0;
        stretchMix = 0.0f;
        plainPos = 0.0;
        jumpFade = 1.0f;
        slicing = looping && (int) p[P::LoopKind] == P::LoopSlice;
        if (slicing)
        {
            const int count = engine.sliceCount(), index = newNote - juce::roundToInt (p[P::SliceStart]);
            if (index >= 0 && index < count)
            {
                const int len = std::max (1, s.loopInfo.length > 0 ? std::min (s.loopInfo.length, s.length) : s.length);
                sliceFrom = engine.sliceStarts[(size_t) index].load (std::memory_order_relaxed);
                const int end = engine.sliceEnds[(size_t) index].load (std::memory_order_relaxed);
                sliceTo = end >= 0 && p[P::SliceThru] < 0.5f ? end : len;
                loopRead = sliceFrom;
                sliceIndex = engine.sliceOriginal[(size_t) index].load (std::memory_order_relaxed);
            }
            engine.lastSliceIndex.store (index >= 0 && index < count ? index : -1, std::memory_order_relaxed);
            engine.lastSliceFrom.store ((int) sliceFrom, std::memory_order_relaxed);
            engine.lastSliceTo.store ((int) sliceTo, std::memory_order_relaxed);
            engine.lastSliceNote.store (newNote, std::memory_order_release);
        }
        loopSlice = -1;
        loopFadeGain = 0.0f;
        loopWarp.reset();
        if (playingEngine (s, p) == P::EngineDrone && (int) p[P::PlayMode] != P::PlayLoop)
        {
            drone = engine.acquireDrone();
            if (drone != nullptr)
            {
                const auto d = droneSettings (s, p);
                const double from = s.readingPosition (p[P::GrainPos] + mod[P::DstStart]);
                drone->start (juce::jlimit (d.regionStart, d.regionEnd - 1.0, from), d.order,
                              std::max ((1 << d.order) / 4, 2 * engine.lastBlock), rng.nextInt64());
            }
        }
    }

    // Engine F: how many blocks this note's next frame can wait, and one stage of it.
    int droneSlack (const ParamSnapshot& p, int blockSize) const noexcept
    {
        return drone != nullptr && (int) p[P::Engine] == P::EngineDrone ? drone->slack (blockSize) : std::numeric_limits<int>::max();
    }

    // Engine F: when this note's frame is due, then how far along it is (for the order of the work).
    std::pair<int, int> droneOrder (const ParamSnapshot& p) const noexcept
    {
        if (drone == nullptr || (int) p[P::Engine] != P::EngineDrone)
            return { std::numeric_limits<int>::max(), 0 };
        return { drone->deadline(), -drone->progress() };
    }

    double droneDemand (const ParamSnapshot& p, int blockSize) const noexcept
    {
        return drone != nullptr && (int) p[P::Engine] == P::EngineDrone ? drone->demand (blockSize) : 0.0;
    }

    void droneStep (const SampleData& s, const ParamSnapshot& p)
    {
        drone->step (s, droneSettings (s, p), *engine.droneShared);
    }

    void glideTo (int newNote, const ParamSnapshot& p)
    {
        note = newNote;
        targetPitch = (float) newNote;
        if (transport)
            transportTuned = true;      // from now on through the stretch (its pitch can move)
        else if (p[P::Glide] <= 0.0f)
            pitch = targetPitch;
    }

    void release()
    {
        if (released || (slicing && ! sliceGate))      // a slice plays to its end (trigger), unless Por golpe
            return;
        released = true;
        pedalHeld = false;
        relLevel = ampLevel;
        relGain = 1.0f;
        keyUpPending = true;
        relSlope = 0.0f;
        offOriginal = heldOriginal;
        offBright = heldBright;
        offDark = heldDark;
        offEnding = heldEnding;
        tRel = 0.0;
        e2Stage = 3;
    }

    void fadeOut()
    {
        fading = true;
        fadeStep = 1.0f / (0.004f * (float) hostRate);
    }

    void render (float* outL, float* outR, int num, const SampleData& s, const ParamSnapshot& p)
    {
        const double dt = 1.0 / hostRate;
        updateModulation (p, num * dt);

        if (pitch != targetPitch)
        {
            // The loop of Play con el host moves at least over 15 ms: a jump of pitch mid-sound would click.
            const float glide = transport ? std::max (15.0f, p[P::Glide]) : p[P::Glide];
            if (glide <= 0.0f)
                pitch = targetPitch;
            else
            {
                pitch += (targetPitch - pitch) * (1.0f - std::exp (-3.0f * (float) (num * dt) / (glide * 0.001f)));
                if (std::abs (targetPitch - pitch) < 1.0e-3f)
                    pitch = targetPitch;
            }
        }

        // Expression: pressing the key (aftertouch, breath) and the mod wheel, as a player does.
        pressing = std::max ({ engine.aftertouch, engine.breath, pressure, mpePressure() }) * p[P::AtAmount];
        const float vibratoDepth = 0.5f * p[P::VibDepth] * std::min (1.0f, engine.modWheel + 0.6f * pressing);
        vibrato = vibratoDepth > 0.0f ? vibratoDepth * (float) std::sin (vibPhase) : 0.0f;
        vibPhase = std::fmod (vibPhase + Dsp::twoPi * p[P::VibRate] * num * dt, (double) Dsp::twoPi);

        // Pitch envelope: starts Pitch env. semitones away and comes back over Tiempo pitch.
        pitchEnv = p[P::PitchEnvAmt] != 0.0f ? p[P::PitchEnvAmt] * std::exp (-(float) t * 5.0f / std::max (0.001f, p[P::PitchEnvTime] * 0.001f)) : 0.0f;

        const bool hold = (int) p[P::SustainType] == P::SustainHold;
        const AdsrShape user { std::max (1.0e-4f, p[P::AmpA] * 0.001f), p[P::AmpCurve], std::max (1.0e-3f, p[P::AmpD] * 0.001f),
                               p[P::AmpS], hold ? 0.0f : p[P::AmpFade], std::max (1.0e-3f, p[P::AmpR] * 0.001f) };
        // LOOP: the loop player instead of the engine; the envelope is a gate (Ataque and Release its fades).
        const bool looping = (int) p[P::PlayMode] == P::PlayLoop && s.length > 0;
        const int engineMode = playingEngine (s, p);
        const bool rebuild = ! looping && engineMode == P::EngineRebuild && s.bedBright.length > 0;
        // Sintetizador: the sample is flattened into a steady tone and the ADSR shapes it from zero.
        const bool synth = ! looping && (int) p[P::EnvMode] == P::EnvSynth && (int) p[P::Engine] <= P::EngineHybrid;
        const float flatten = synth ? 1.0f : p[P::Flatten];
        const bool untouched = rebuild && ! synth && matchesSample (s, user, hold);
        const bool resynth = ! looping && engineMode == P::EngineResynth && s.partials.valid();
        const bool withTransient = ! looping && (engineMode == P::EngineHybrid || engineMode == P::EngineResynth);
        const float transLen = juce::jlimit (0.002f, 0.4f, p[P::TransientMs] * 0.001f);

        // Envelopes for this sub-block.
        const float e2A = std::max (1.0e-4f, p[P::E2A] * 0.001f);
        const float e2Dcoef = std::exp (-5.0f * (float) dt / std::max (1.0e-3f, p[P::E2D] * 0.001f));
        const float e2Rcoef = std::exp (-6.9f * (float) dt / std::max (1.0e-3f, p[P::E2R] * 0.001f));
        const float e2S = p[P::E2S];
        const bool perHit = looping && p[P::LoopHitEnv] > 0.5f;
        perHitTimbre = false;
        if (! perHit)
        {
            std::fill (hitArr.begin(), hitArr.begin() + num, false);
            hitStrikePending = false;
        }

        // A reshaped note may be sustaining what the sample never did, so it gets a real fade
        // even when the detected release is that of a short hit.
        const float fall = rebuild && ! untouched ? std::max (user.release, SynthEngine::minReshapedRelease) : user.release;

        if (released)
        {
            // The fall starts gently: a level that breaks away at full speed is heard as a knock.
            const float relStep = 1.0f - std::exp (-6.9f * (float) dt / fall);
            const float slopeStep = (float) dt / juce::jlimit (0.004f, 0.03f, fall * 0.5f);

            for (int i = 0; i < num; ++i)
            {
                relSlope = std::min (1.0f, relSlope + slopeStep);
                relGain *= 1.0f - relStep * relSlope;
                uArr[(size_t) i] = relLevel * relGain;
                relArr[(size_t) i] = relGain;
            }
        }
        else if (looping && perHit)
        {
            // Envolvente at each hit: from where the level is (no drop to zero: no click), up over Ataque, then
            // Decay to Sustain as on a note. Normal: the hits are the cuts the loop's time line crosses in this
            // block; Slice: the slice is the hit, its note the envelope.
            const int cuts = slicing ? 0 : engine.sliceCount();
            // The hits of the part that plays (Ventana): its cuts, counted from its start.
            const auto window = SynthEngine::loopWindow (s, p);
            const int length = window.length;
            const auto cutAt = [&] (int k)
            {
                const int c = engine.cutPoints[(size_t) k].load (std::memory_order_relaxed) - window.start;
                return c > 0 && c < length ? c : -1;
            };
            double pos = loopTime;
            double next = -1.0;
            if (cuts > 1 && lastTimeStep > 0.0)
            {
                next = length;      // the next round's first cut (0)
                for (int k = 0; k < cuts; ++k)
                {
                    const int c = cutAt (k);
                    if (c > pos)
                    {
                        next = c;
                        break;
                    }
                }
            }
            for (int i = 0; i < num; ++i)
            {
                bool hit = false;
                if (next >= 0.0)
                {
                    pos += lastTimeStep;
                    if (pos >= next)
                    {
                        hit = true;
                        hitStrength = strengthOfHit (s, window.start + (next >= length ? 0 : (int) next));
                        hitStrikePending = true;
                        if (next >= length)
                            pos -= length;
                        next = length;
                        for (int k = 0; k < cuts; ++k)
                        {
                            const int c = cutAt (k);
                            if (c > pos)
                            {
                                next = c;
                                break;
                            }
                        }
                    }
                }
                if (hit)
                {
                    hitT = 0.0;
                    hitFrom = envelopeLevel;
                }
                hitArr[(size_t) i] = hit;
                hitT += dt;
                const float level = hitT < user.attack ? hitFrom + (1.0f - hitFrom) * (float) (hitT / user.attack) : user.heldLevel (hitT);
                uArr[(size_t) i] = level;
                relArr[(size_t) i] = 1.0f;
                envelopeLevel = level;
            }
            // TIMBRE follows the same hits: where this hit's envelope is (rising through Ataque, or fallen towards
            // Sustain) and how strong it was.
            perHitTimbre = true;
            hitAttackLevel = hitT < user.attack ? std::max (1.0e-3f, (float) (hitT / user.attack)) : 1.0f;     // how far through Ataque
            hitFall = hitT >= user.attack && user.sustain < 0.999f ? juce::jlimit (0.0f, 1.0f, (1.0f - envelopeLevel) / (1.0f - user.sustain)) : 0.0f;
        }
        else if (looping)
        {
            for (int i = 0; i < num; ++i)
            {
                uArr[(size_t) i] = std::min (1.0f, (float) ((t + (i + 1) * dt) / user.attack));
                relArr[(size_t) i] = 1.0f;
            }
            envelopeLevel = uArr[(size_t) num - 1];
        }
        else
        {
            // Engine F with the envelope the sample was found to have: a drone holds while the key is down, at the
            // level of the part of the sample that sounds (no lower than its sustain). A hit's own decay closed
            // before the drone's first frame sounded (0.17 s in), and F was silent on kicks, snares and claps.
            // Any other envelope shapes it as set.
            AdsrShape shape = user;
            if (engineMode == P::EngineDrone && matchesSample (s, user, hold))
            {
                shape.sustain = std::max (user.sustain, s.meanLevel);
                shape.fadeDbPerSec = 0.0f;
            }
            for (int start = 0; start < num; start += controlBlock)
            {
                const int count = std::min (controlBlock, num - start);
                const float next = shape.heldLevel (t + (start + count - 1) * dt);
                for (int i = 0; i < count; ++i)
                {
                    uArr[(size_t) (start + i)] = envelopeLevel + (next - envelopeLevel) * (float) (i + 1) / (float) count;
                    relArr[(size_t) (start + i)] = 1.0f;
                }
                envelopeLevel = next;
            }
        }

        for (int i = 0; i < num; ++i)
        {
            const float tf = (float) (t + i * dt);
            wTrans[(size_t) i] = ! withTransient ? 0.0f
                               : tf < transLen * 0.5f ? 1.0f
                               : tf < transLen ? 0.5f + 0.5f * std::cos (pi * (tf - transLen * 0.5f) / (transLen * 0.5f))
                               : 0.0f;

            if (perHit && ! released && hitArr[(size_t) i])
                e2Stage = 0;        // the filter's envelope again from where it is
            switch (e2Stage)
            {
                case 0: e2Level += (float) dt / e2A; if (e2Level >= 1.0f) { e2Level = 1.0f; e2Stage = 1; } break;
                case 1: e2Level = e2S + (e2Level - e2S) * e2Dcoef; break;
                default: e2Level *= e2Rcoef; break;
            }
        }
        ampLevel = uArr[(size_t) num - 1];

        std::fill (bufL.begin(), bufL.begin() + num, 0.0f);
        std::fill (bufR.begin(), bufR.begin() + num, 0.0f);

        computeLaneGains (p);
        for (int i = 0; i < numLanes; ++i)
            lanes[(size_t) i].step = laneStep (s, p, lanes[(size_t) i].offset);

        const float bow = juce::jlimit (0.0f, 1.0f, p[P::Bow] + mod[P::DstBow]);
        const float breath = juce::jlimit (0.0f, 1.0f, p[P::Breath] + mod[P::DstBreath]);
        const float relPhase = released ? (float) std::min (1.0, tRel / fall) : 0.0f;

        bool alive;
        if (looping)
        {
            synthetic = 0.0f;
            alive = renderLoop (s, p, num);
        }
        else if (engineMode == P::EngineGranular)
        {
            synthetic = 1.0f;
            alive = renderGranular (s, p, num);
        }
        else if (engineMode == P::EngineDrone)
        {
            synthetic = 1.0f;
            if (drone != nullptr)
                drone->render (s, droneSettings (s, p), *engine.droneShared, uArr.data(), bufL.data(), bufR.data(), num);
            alive = true;
        }
        else if (rebuild)
            alive = renderRebuild (s, p, num, user, untouched, synth, hold, breath);
        else if (resynth)
        {
            synthetic = 1.0f;
            alive = renderResynth (s, p, num, user, hold || synth, flatten, bow, breath, relPhase);
        }
        else
        {
            alive = renderSampled (s, p, num, user, hold || synth, flatten, breath);
            synthetic = lanes[0].srcMix;
        }

        // The granular texture: a cloud that follows the note, at the level of the envelope.
        if (! looping && engineMode != P::EngineGranular && engineMode != P::EngineDrone && p[P::GrainMix] > 0.0f)
        {
            for (int i = 0; i < num; ++i)
                texGain[(size_t) i] = p[P::GrainMix] * uArr[(size_t) i];
            lanes[0].cloud.render (s, lanes[0].pos, lanes[0].step, textureCloud (s), rng, texGain.data(), bufL.data(), bufR.data(), num);
        }

        // A soft ceiling at the sample's own peak. In E and F, grains that overlap and phases drawn
        // at random pile up now and then into peaks well over it: rounding starts at 80 % of it.
        // The other engines can be asked for more than the sample has (a snare body held at 70 %
        // is noise raised to the level of the hit, and noise has high peaks): there only what goes
        // over the sample's peak is rounded off, 2.5 dB over it at most, and anything under it
        // passes exactly as it was.
        {
            const bool piles = engineMode == P::EngineGranular || engineMode == P::EngineDrone;
            const float knee = (piles ? 0.8f : 1.0f) * s.peakLevel, room = (piles ? 0.2f : 0.333f) * s.peakLevel;
            for (auto* buf : { bufL.data(), bufR.data() })
                for (int i = 0; i < num; ++i)
                {
                    const float a = std::abs (buf[i]);
                    if (a > knee)
                        buf[i] = std::copysign (knee + room * std::tanh ((a - knee) / room), buf[i]);
                }
        }

        postProcess (outL, outR, num, s, p, resynth, bow, relPhase);

        t += num * dt;
        if (released)
            tRel += num * dt;

        if ((fading && fadeGain <= 0.0f) || (released && relGain < 1.0e-5f && ! mech.busy()) || ! alive)
            stopImmediately();
    }

private:
    enum Seg { SegAttack, SegDecay, SegBody, SegSustain };

    struct Lane
    {
        double pos = 0, transPos = 0, step = 1;
        int seg = SegAttack;
        bool done = false;
        float offset = 0, gainL = 1, gainR = 1;
        float ratio = 1;                        // last envelope ratio u / den (drives the tilt)
        GrainReader grains;

        // Sustain source (engines B and D).
        float srcMix = 0.0f;
        float grainCoh = -1.0f, srcCoh = -1.0f;      // the grains' level correction, as applied (moves per sample)
        double srcPos = 0.0;
        LoopGrains srcGrains;
        Dsp::Drift breathAmp, breathPitch;

        // Engine E, and the granular texture of the others.
        GranularCloud cloud;
        float levelGain = 1.0f;

        // Engine A: read position in the beds, and the low part removed from the punch.
        double bedPos = 0.0;

        // Warp: the original and the beds read by the same grains, the pitch apart from the speed;
        // the sustain source and the transient of engines B, C, D by their own.
        WarpReader warp, warpSrc, warpTrans;
        std::array<float, 2> punchLow {};

        // Engine C oscillators.
        std::array<double, maxPartials> phase {};
        std::array<float, maxPartials> amp {};
        bool synced = false;
        std::array<Dsp::Drift, 8> drifts;
    };

    //--------------------------------------------------------------------------
    void releaseDrone() noexcept
    {
        if (drone != nullptr)
            drone->inUse = false;
        drone = nullptr;
    }

    // Engine F at this moment: the reading speed, the spectral treatment, and the copies of the
    // spectrum that make the pitch, the intervals and the unison voices.
    Drone::Settings droneSettings (const SampleData& s, const ParamSnapshot& p) const
    {
        Drone::Settings d;
        d.order = Drone::orderFor ((int) p[P::DroneSize], hostRate);
        d.hostRate = hostRate;
        d.rateRatio = s.sampleRate / hostRate;
        d.advance = p[P::DroneFreeze] > 0.5f ? 0.0 : d.rateRatio / std::max (1.0f, p[P::DroneStretch]);
        d.regionStart = s.onset;
        d.regionEnd = std::max (s.onset + 2.0, s.usableEnd);
        d.fundamentalHz = s.pitched ? 440.0f * std::exp2 ((s.rootMidi - 69.0f) / 12.0f) : 0.0f;
        d.harmonics = p[P::DroneHarm];
        d.harmonicWidthCents = p[P::DroneHarmWidth];
        d.tonal = p[P::DroneTonal];
        d.spread = juce::jlimit (0.0f, 1.0f, p[P::DroneSpread] + mod[P::DstSpray]);
        d.shiftHz = p[P::DroneShift];
        d.width = p[P::DroneWidth];
        d.curve = p[P::DroneCurveOn] > 0.5f ? engine.droneCurve.data() : nullptr;
        d.keepFormants = (int) p[P::WarpMode] == P::WarpTones && s.pitched;   // as in the other engines

        const float ratio = (float) (laneStep (s, p, 0.0f) / d.rateRatio);
        int n = 0;
        d.copyRatio[(size_t) n] = ratio;
        d.copyLevel[(size_t) n++] = 1.0f;
        const float levels[Drone::numIntervals] = { p[P::DroneSub2], p[P::DroneSub1], p[P::DroneFifth], p[P::DroneUp1], p[P::DroneUp2] };
        for (int i = 0; i < Drone::numIntervals; ++i)
        {
            if (levels[i] <= 0.0f)
                continue;
            d.copyRatio[(size_t) n] = ratio * Drone::intervalRatios[i];
            d.copyLevel[(size_t) n++] = levels[i];
        }
        const float cents = detuneCents (p, mod[P::DstDetune]);
        for (int i = 0; i < numLanes && cents > 0.0f; ++i)
        {
            if (lanes[(size_t) i].offset == 0.0f)
                continue;
            d.copyRatio[(size_t) n] = ratio * std::exp2 (cents * lanes[(size_t) i].offset / 1200.0f);
            d.copyLevel[(size_t) n++] = p[P::UniBlend];
        }
        d.numCopies = n;
        return d;
    }

    // 0 when the unison lanes are in tune and sum as one sound, 1 when they are independent.
    static float laneIndependence (const ParamSnapshot& p) noexcept { return juce::jlimit (0.0f, 1.0f, detuneCents (p, 0.0f) * 0.1f); }

    // Detune in %, with a curve that is fine near zero and wide at the top: at 100 % the
    // outer unison voices sit ±50 cents away.
    static float detuneCents (const ParamSnapshot& p, float modulation) noexcept
    {
        const float amount = juce::jlimit (0.0f, 1.0f, (p[P::UniDetune] + modulation * 100.0f) * 0.01f);
        return 50.0f * std::pow (amount, 1.3f);
    }

    int grainLength (const ParamSnapshot& p) const
    {
        const float ms = juce::jlimit (5.0f, 1000.0f, p[P::GrainMs] * std::exp2 (2.0f * mod[P::DstGrain]));
        return std::max (8, (int) (ms * 0.001f * (float) hostRate));
    }

    // Do3 plays the sample as it is; with Snap a C, moved to the C nearest its note.
    static float rootPitch (const SampleData& s, const ParamSnapshot& p)
    {
        return (float) SynthEngine::rootKey - SynthEngine::snapShift (s, p[P::SnapC] > 0.5f);
    }

    // The note that sounds, in MIDI: the sample's own note moved as the voice moves it, or the
    // key played when the sample has no note.
    float soundingNote (const SampleData& s, const ParamSnapshot& p) const noexcept
    {
        const float shift = pitch + p[P::Transpose] + p[P::Fine] * 0.01f - rootPitch (s, p);
        return s.pitched ? s.rootMidi + shift : pitch + p[P::Transpose];
    }

    // How this note plays, for Mecanismo to join it.
    Mechanism::Note mechNote (const SampleData& s, const ParamSnapshot& p, double startPos) const noexcept
    {
        Mechanism::Note n;
        n.velocity = velocity;
        n.sounding = soundingNote (s, p);
        n.speed = std::exp2 ((pitch + p[P::Transpose] + p[P::Fine] * 0.01f - rootPitch (s, p)) / 12.0f);
        n.attackMs = p[P::AmpA];
        n.adapt = p[P::MechAdapt];
        n.downGain = dbToGain (p[P::MechDown]);
        n.upGain = dbToGain (p[P::MechUp]);
        n.startPos = startPos;
        return n;
    }

    double laneStep (const SampleData& s, const ParamSnapshot& p, float offset) const
    {
        const float detune = detuneCents (p, mod[P::DstDetune]) * offset;
        const float semis = pitch + p[P::Transpose] + (p[P::Fine] + detune + variationCents) * 0.01f
                          + engine.pitchBend * p[P::BendRange] + mpeBend() + vibrato + pitchEnv + (mod[P::DstPitch] * 24.0f + mod[P::DstFine]) - rootPitch (s, p);
        return std::exp2 (semis / 12.0) * s.sampleRate / hostRate;
    }

    // Sets the oscillator phases to the analysed phases at the lane's position.
    void syncPartialPhases (const SampleData& s, Lane& ln) noexcept
    {
        const auto& pm = s.partials;
        if (! pm.valid())
            return;
        const int f = juce::jlimit (0, pm.numFrames - 1, (int) std::round (ln.pos / pm.hop));
        const double offset = (ln.pos - (double) f * pm.hop) / s.sampleRate;
        for (int k = 0; k < std::min (maxPartials, pm.numPartials); ++k)
        {
            const size_t idx = (size_t) f * pm.numPartials + k;
            ln.phase[(size_t) k] = pm.phase[idx] + pm.freq[idx] * offset;
        }
    }

    // Warp for this block: on or not, Tonos with a period or not, and how fast the source's own
    // time runs against the output.
    // tonal: Tonos on a sample with a note; cycles: the reader lays it cycle by cycle, unless the
    // sample failed its test on loading (then Tonos plays as Textura).
    struct WarpPlan { bool on = false, tonal = false, cycles = false; double timeStep = 1.0; int textureLen = 16; float power = 0.0f; };

    WarpPlan warpPlan (const SampleData& s, const ParamSnapshot& p) const noexcept
    {
        WarpPlan w;
        const int mode = (int) p[P::WarpMode];
        w.on = mode != P::WarpRepitch;
        w.tonal = mode == P::WarpTones && s.pitched;
        w.cycles = w.tonal && s.tonesHold;
        w.timeStep = s.sampleRate / hostRate;
        w.textureLen = std::max (16, (int) (0.06 * hostRate));
        w.power = 1.0f - std::exp (-1.0f / (0.02f * (float) hostRate));
        return w;
    }

    // Where the attack of the sample ends for Warp: its peak, no later than 30 ms past the onset.
    double warpAttackEnd (const SampleData& s) const noexcept { return warpAttackEndOf (s); }

    static WarpReader::Source warpSourceOf (const juce::AudioBuffer<float>& buffer, int length, bool wrap,
                                            double attackEnd = 0.0, float fadeStep = 1.0f) noexcept
    {
        WarpReader::Source src;
        src.channels = 2;
        src.attackEnd = attackEnd;
        src.fadeStep = fadeStep;
        for (int c = 0; c < 2; ++c)
        {
            src.ch[c] = buffer.getReadPointer (std::min (c, buffer.getNumChannels() - 1));
            src.length[c] = length;
            src.wrap[c] = wrap;
        }
        return src;
    }

    // The position moves at the source's own pace under Warp, at the pitch's otherwise.
    static void advanceWarped (Lane& ln, const SampleData& s, double kA, double kD, const WarpPlan& w) noexcept
    {
        if (! w.on)
        {
            advancePosition (ln, s, kA, kD);
            return;
        }
        const double pitchStep = ln.step;
        ln.step = w.timeStep;
        advancePosition (ln, s, kA, kD);
        ln.step = pitchStep;
    }

    // Warped position through attack / decay / body (shared by all engines).
    static void advancePosition (Lane& ln, const SampleData& s, double kA, double kD) noexcept
    {
        switch (ln.seg)
        {
            case SegAttack: ln.pos += ln.step * kA; if (ln.pos >= s.peak) ln.seg = SegDecay; break;
            case SegDecay:  ln.pos += ln.step * kD; if (ln.pos >= s.knee) ln.seg = SegBody; break;
            default:        ln.pos += ln.step; break;
        }
    }

    //--------------------------------------------------------------------------
    // Engine E: a cloud of grains around a point that travels at the Deriva speed. The ADSR
    // shapes it like any other voice, and it holds as long as the key does.
    CloudSettings granularCloud (const SampleData& s, const ParamSnapshot& p) const
    {
        CloudSettings c;
        c.size = std::max (16, (int) (p[P::GrainMs] * 0.001f * (float) hostRate));
        c.interval = hostRate / std::max (1.0f, p[P::GrainDensity]);
        c.spray = juce::jlimit (0.0f, 1.0f, p[P::Spray] + mod[P::DstSpray]) * 0.25 * s.sampleRate;
        c.pitchSpread = p[P::GrainPitch];
        c.reverseChance = p[P::GrainReverse];
        c.width = p[P::GrainWidth];
        // Coherente, and by itself on a low note (under 150 Hz): placed at random, grains of a sub cancel each
        // other and it sounds rough (heard on a sub-bass loop).
        const bool low = s.pitched && s.period > 1.0 && s.sampleRate / s.period < 150.0;
        c.period = (p[P::GrainCoherent] > 0.5f || low) && s.pitched && s.period > 1.0 ? s.period : 0.0;
        return c;
    }

    // The texture of the other engines: small grains close to where the note is playing.
    CloudSettings textureCloud (const SampleData& s) const
    {
        CloudSettings c;
        c.size = std::max (16, (int) (0.06 * hostRate));
        c.interval = hostRate / 40.0;
        c.spray = 0.025 * s.sampleRate;
        c.pitchSpread = 0.1f;
        c.width = 0.6f;
        return c;
    }

    // The engine a note plays. E and F make a texture out of the sample; out of a sound shorter than a grain (a
    // hi-hat of 25 ms against grains of 80 ms) or a low hit with nothing held after it (a kick), what came out was
    // silence, or, made to sound, a stutter and a smeared rumble (judged by ear on 2026-11-22). Those play the hit
    // as engine A does.
    static int playingEngine (const SampleData& s, const ParamSnapshot& p) noexcept
    {
        const int chosen = (int) p[P::Engine];
        if (chosen != P::EngineGranular && chosen != P::EngineDrone)
            return chosen;
        const bool lowHit = s.excitation.sustain < 0.3f && s.excitation.lowShare > 0.5f;
        const bool shorterThanGrain = chosen == P::EngineGranular && (s.usableEnd - s.onset) / s.sampleRate < p[P::GrainMs] * 0.001f;
        return lowHit || shorterThanGrain ? (int) P::EngineRebuild : chosen;
    }

    static float levelFor (const SampleData& s, double pos) noexcept
    {
        return juce::jlimit (1.0f, 4.0f, 1.0f / std::max (0.25f, s.envAt (pos)));
    }

    bool renderGranular (const SampleData& s, const ParamSnapshot& p, int num)
    {
        const auto cloud = granularCloud (s, p);
        const double speed = juce::jlimit (-1.0f, 1.0f, p[P::GranSpeed]);

        for (int k = 0; k < numLanes; ++k)
        {
            auto& ln = lanes[(size_t) k];
            std::fill (laneL.begin(), laneL.begin() + num, 0.0f);
            std::fill (laneR.begin(), laneR.begin() + num, 0.0f);
            ln.cloud.render (s, ln.pos, ln.step, cloud, rng, uArr.data(), laneL.data(), laneR.data(), num);

            // The cloud sounds at the level of the sample's loudest part wherever it reads, up
            // to +12 dB: Posición chooses the colour, not the volume.
            const float target = levelFor (s, ln.pos);
            const float step = (target - ln.levelGain) / (float) num;
            for (int i = 0; i < num; ++i)
            {
                ln.levelGain += step;
                bufL[(size_t) i] += laneL[(size_t) i] * ln.gainL * ln.levelGain;
                bufR[(size_t) i] += laneR[(size_t) i] * ln.gainR * ln.levelGain;
            }

            // Under Warp the cloud moves through the sample at its own pace whatever the note.
            const double pace = (int) p[P::WarpMode] != P::WarpRepitch ? s.sampleRate / hostRate : ln.step;
            ln.pos = std::fmod (ln.pos + pace * speed * num, (double) s.length);
            if (ln.pos < 0.0)
                ln.pos += s.length;
        }
        return true;
    }

    //--------------------------------------------------------------------------
    // Engine A: the original sample and its reconstructed beds, played as plain audio.
    // Your envelope divided by the detected one says how much the original has to change.
    // While that is a cut or a small boost the original does the job; when it would need
    // more than the sample can give (a higher sustain, a longer decay, a slow attack), the
    // bed takes over. Quieter passages use the darker bed, as real sounds do.
    // When the note starts further into the sample, its level is already below what the model
    // says for this moment: the difference between both curves is applied to the real level.
    static void splitBed (float level, float weight, float& bright, float& dark) noexcept
    {
        const float late = juce::jlimit (0.0f, 1.0f, (-6.0f - 6.02f * std::log2 (std::max (level, 1.0e-6f))) / 18.0f);
        bright = level * weight * std::cos (late * pi * 0.5f);
        dark = level * weight * std::sin (late * pi * 0.5f);
    }

    // Equal power for sounds that are unrelated; the more they coincide, the less they need.
    static void crossfade (float position, float coherence, float& sampleWeight, float& bedWeight) noexcept
    {
        const float smooth = position * position * (3.0f - 2.0f * position);
        const float a = std::cos (smooth * pi * 0.5f), b = std::sin (smooth * pi * 0.5f);
        const float norm = 1.0f / std::sqrt (1.0f + 2.0f * coherence * a * b);
        sampleWeight = a * norm;
        bedWeight = b * norm;
    }

    // Synthesizer mode: the sample's own level is divided out, so the note follows the ADSR
    // exactly. Where the sample is too quiet for that, or runs out, the bed takes over.
    static void synthGains (float u, float measured, float ending, float coherence, float& original, float& bright, float& dark) noexcept
    {
        const float d = std::max (1.0e-4f, measured);
        const float wanted = std::max (u, 1.0e-6f);
        float wOriginal, wBed;
        crossfade (std::max (ending, juce::jlimit (0.0f, 1.0f, (std::log2 (wanted / d) - 0.3f) / 1.3f)), coherence, wOriginal, wBed);
        original = std::min (4.0f, wanted / d) * wOriginal;
        splitBed (wanted, wBed, bright, dark);
    }

    static void rebuildGains (float u, double when, double skipped, const AdsrShape& detected, float measured, float followMeasured,
                              float quicker, float ending, float coherence, float& original, float& bright, float& dark) noexcept
    {
        float already = 1.0f;
        if (skipped > 0.0)
        {
            const double settled = std::max (when, (double) detected.attack);
            already = std::min (1.0f, detected.heldLevel (settled + skipped) / std::max (1.0e-4f, detected.heldLevel (settled)));
        }

        // During a slowed attack the real level of the sample is used instead of the model,
        // so that the swell rises as drawn, whatever bumps the sample has.
        const float model = detected.heldLevel (when) * already;
        const float d = std::max (1.0e-4f, model + followMeasured * (measured - model));
        const float wanted = std::max (u * already, 1.0e-6f);
        // The attack itself does not call for the bed; past it, the model is the reference. The change is
        // eased over 20 ms: a sudden switch made the bed jump in at the end of the attack (a click).
        // An attack quicker than the sample's own (`quicker`) is the exception: the original cannot rise
        // fast enough (a clarinet is still 40 dB down a tenth of a second in), so the bed gives the note the
        // level asked for while the sample comes up, and hands it back when the sample gets there.
        const float past = std::max (quicker, juce::jlimit (0.0f, 1.0f, (float) ((when - (double) detected.attack) / 0.02)));
        const float reference = already * std::pow (d / std::max (1.0e-6f, already), past);
        const float excess = std::log2 (wanted / reference);

        // A boost is only for what the sample lacks: where it is louder than the model says,
        // as in a transient, it must not rise over the curve.
        const float boostLimit = std::max (1.0f, wanted / std::max (1.0e-4f, measured));
        const float gain = std::min ({ 4.0f, wanted / d, boostLimit });

        // The bed also takes over where the sample really falls short of the curve, even when
        // the model says it should not (a tail that dies faster than its fit, the last of the
        // file): as the original fades out the bed is already there, and the note never dips
        // and climbs again. And it takes over when the sample is about to run out.
        const float shortfall = past * std::log2 (wanted / std::max (1.0e-6f, gain * measured));
        const float takeover = std::max ({ ending, juce::jlimit (0.0f, 1.0f, (excess - 0.3f) / 1.3f),
                                           juce::jlimit (0.0f, 1.0f, (shortfall - 0.15f) / 1.0f) });
        float wOriginal, wBed;
        crossfade (takeover, coherence, wOriginal, wBed);
        original = gain * wOriginal;
        splitBed (wanted, wBed, bright, dark);
    }

    // With the knobs on the detected values engine A plays the file as it is.
    static bool matchesSample (const SampleData& s, const AdsrShape& user, bool hold) noexcept
    {
        const auto& det = s.detected;
        auto same = [] (float a, float b) { return std::abs (a - b) <= 1.0e-3f * std::max (std::abs (a), std::abs (b)) + 1.0e-4f; };
        return ! hold && same (user.attack, det.attack) && same (user.curve, det.curve) && same (user.decay, det.decay)
            && same (user.sustain, det.sustain) && same (user.fadeDbPerSec, det.fadeDbPerSec);
    }

    bool renderRebuild (const SampleData& s, const ParamSnapshot& p, int num, const AdsrShape& user, bool untouched, bool synth,
                        bool hold, float breath)
    {
        const auto& det = s.detected;
        const double dt = 1.0 / hostRate;

        const bool slowed = ! untouched && user.attack > 3.0f * det.attack;
        // A quicker attack on a sound that rises slowly is filled by the bed (see rebuildGains); a hit that is
        // already quick (a kick asked for 1 ms instead of 2) keeps its own strike.
        const float quicker = ! untouched && user.attack < det.attack ? juce::jlimit (0.0f, 1.0f, (det.attack - 0.01f) / 0.03f) : 0.0f;

        // The part of the file in play: Fin, and the loop that repeats while the note sounds.
        // The file played as it is goes on to its own end: cut where its sound is over (40 dB under its
        // loudest), a recording with a noisy tail (a violin of the Iowa library) ended in a tick. A Fin of the
        // user's own still ends it where it is put.
        const double fin = std::max (s.onset + 1.0, (double) p[P::EndPos] * s.length);
        const double end = untouched && fin >= s.usableEnd ? std::min (fin, (double) s.length) : std::min (s.usableEnd, fin);
        const double loopStart = std::min ((double) p[P::LoopStart], (double) p[P::LoopEnd]) * s.length;
        const double loopEnd = std::min (end, std::max ((double) p[P::LoopStart], (double) p[P::LoopEnd]) * s.length);
        const bool looping = p[P::LoopOn] > 0.5f && loopEnd - loopStart > 0.005 * s.sampleRate;
        const double loopLength = loopEnd - loopStart;
        const double loopFade = std::min ({ (double) p[P::LoopXfade] * 0.001 * s.sampleRate, loopLength * 0.5, loopStart });
        const double fadeIn = p[P::FadeIn] * 0.001 * s.sampleRate, fadeOut = p[P::FadeOut] * 0.001 * s.sampleRate;
        const double endingLength = std::max (1.0, std::min (0.15 * s.sampleRate, (end - s.onset) / 3.0));

        // Gains at control rate. After the note-off they stay as they were and the release
        // scales them, so the original keeps its own decay. If the sample runs out, the bed
        // takes over at the level the original has by then: a released note never climbs.
        float prevO = heldOriginal, prevB = heldBright, prevD = heldDark;
        for (int start = 0; start < num; start += controlBlock)
        {
            const int count = std::min (controlBlock, num - start);
            const float ending = looping ? 0.0f : juce::jlimit (0.0f, 1.0f, (float) ((lanes[0].pos - (end - endingLength)) / endingLength));
            float o = 1.0f, b = 0.0f, d = 0.0f;
            if (released && synth)
            {
                synthGains (relLevel, s.envAt (lanes[0].pos), ending, s.bedBright.coherence, o, b, d);
            }
            else if (released)
            {
                o = offOriginal;
                b = offBright;
                d = offDark;
                if (! untouched && ending > offEnding)
                {
                    float wOriginal, wBed, handBright, handDark;
                    crossfade ((ending - offEnding) / (1.0f - offEnding), s.bedBright.coherence, wOriginal, wBed);
                    splitBed (offOriginal * s.envAt (lanes[0].pos), wBed, handBright, handDark);
                    o *= wOriginal;
                    b += handBright;
                    d += handDark;
                }
            }
            else
            {
                heldEnding = ending;
                if (synth)
                {
                    synthGains (uArr[(size_t) (start + count - 1)], s.envAt (lanes[0].pos), ending, s.bedBright.coherence, o, b, d);
                }
                else if (! untouched)
                {
                    const double when = t + (start + count - 1) * dt;
                    const float follow = slowed ? juce::jlimit (0.0f, 1.0f, (float) (((double) user.attack + 0.15 - when) / 0.15)) : 0.0f;
                    rebuildGains (uArr[(size_t) (start + count - 1)], when, skippedSeconds, det, s.envAt (lanes[0].pos), follow, quicker, ending,
                                  s.bedBright.coherence, o, b, d);
                }
            }

            for (int i = 0; i < count; ++i)
            {
                const float w = (float) (i + 1) / (float) count;
                const float rel = relArr[(size_t) (start + i)];
                gOriginal[(size_t) (start + i)] = (prevO + w * (o - prevO)) * rel;
                gBright[(size_t) (start + i)] = (prevB + w * (b - prevB)) * rel;
                gDark[(size_t) (start + i)] = (prevD + w * (d - prevD)) * rel;
            }
            prevO = o;
            prevB = b;
            prevD = d;
        }
        heldOriginal = prevO;
        heldBright = prevB;
        heldDark = prevD;
        synthetic = (prevB + prevD) / std::max (1.0e-6f, prevO * s.envAt (lanes[0].pos) + prevB + prevD);

        const bool slowAttack = slowed && t < (double) user.attack;

        // Golpe: an attack faster than the sample's own needs a hit that the sample does not have.
        // It is made from the high part of the sample's own bright bed, and dies in a few ms.
        const float gap = juce::jlimit (0.0f, 1.0f, std::log2 (det.attack / user.attack) * 0.25f);
        const float punch = p[P::Punch] * gap;
        const bool punching = punch > 0.0f && t < 8.0 * punchTime && ! released;
        if (punching)
            for (int i = 0; i < num; ++i)
                punchArr[(size_t) i] = 2.0f * punch * uArr[(size_t) i] * std::exp (-(float) (t + i * dt) / punchTime);
        const float punchCoef = 1.0f - std::exp (-Dsp::twoPi * 1500.0f / (float) hostRate);
        const float* rawL = s.audio.getReadPointer (0);
        const float* rawR = s.audio.getReadPointer (1);
        const int rawLen = s.audio.getNumSamples();
        const int bedLen = s.bedBright.length;
        const int darkLen = s.bedDark.length;
        const int darkShift = darkLen > 0 ? ((s.bedDark.origin - s.bedBright.origin) % darkLen + darkLen) % darkLen : 0;
        const float* brightL = s.bedBright.audio.getReadPointer (0) + SampleData::Bed::pad;
        const float* brightR = s.bedBright.audio.getReadPointer (1) + SampleData::Bed::pad;
        const float* darkL = darkLen > 0 ? s.bedDark.audio.getReadPointer (0) + SampleData::Bed::pad : brightL;
        const float* darkR = darkLen > 0 ? s.bedDark.audio.getReadPointer (1) + SampleData::Bed::pad : brightR;
        const auto& interpolator = sinc();
        alignas (16) float coefficients[SincTable::maxTaps];
        bool anyAlive = false;

        // Warp: time at the source's own pace, the pitch made by the reader.
        const int warpMode = (int) p[P::WarpMode];
        const bool warping = warpMode != P::WarpRepitch;
        const bool tonal = warpMode == P::WarpTones && s.pitched && s.tonesHold;
        const double timeStep = s.sampleRate / hostRate;
        const int textureLen = std::max (16, (int) (0.06 * hostRate));
        const float warpPower = 1.0f - std::exp (-1.0f / (0.02f * (float) hostRate));
        WarpReader::Source warpSource;
        warpSource.channels = 6;
        const float* warpChannels[] { rawL, rawR, brightL, brightR, darkL, darkR };
        for (int c = 0; c < 6; ++c)
        {
            warpSource.ch[c] = warpChannels[c];
            warpSource.length[c] = c < 2 ? rawLen : (c < 4 ? bedLen : darkLen);
            warpSource.wrap[c] = c >= 2;
            warpSource.group[c] = c / 2;           // the original, the bright bed, the dark bed
        }
        if (darkLen != bedLen || darkLen <= 0)
            warpSource.ch[4] = warpSource.ch[5] = nullptr;
        else if (s.bedQuad.size() == (size_t) bedLen * 4)
        {
            warpSource.quad = s.bedQuad.data();
            warpSource.quadFirst = 2;
        }
        warpSource.attackEnd = warpAttackEnd (s);
        warpSource.fadeStep = 1.0f / (0.01f * (float) hostRate);

        for (int k = 0; k < numLanes; ++k)
        {
            auto& ln = lanes[(size_t) k];
            if (ln.done)
                continue;
            const double ratio = ln.step / timeStep;
            float warped[WarpReader::Source::maxChannels] {};
            // Past its end the original is never heard again (without a loop the time line only goes
            // forward): the grains stop reading it. Its level match is only ever used while it sounds.
            const bool originalOver = warping && ! looping && ln.pos >= end;
            warpSource.ch[0] = originalOver ? nullptr : rawL;
            warpSource.ch[1] = originalOver ? nullptr : rawR;

            if (hold)
            {
                ln.breathAmp.tick (rng, num, 0.6f, hostRate);
                ln.breathPitch.tick (rng, num, 0.35f, hostRate);
            }
            const float breathGain = hold ? 1.0f + breath * 0.1f * ln.breathAmp.value : 1.0f;
            const double bedStep = ln.step * (hold ? 1.0 + breath * 0.0012 * ln.breathPitch.value : 1.0);
            const int cut = SincTable::cutFor (ln.step);
            const int taps = interpolator.taps[(size_t) cut];
            const int back = taps / 2 - 1;

            // The original at any position: exact on whole samples at the original pitch,
            // band-limited otherwise, silent outside the file.
            const auto readOriginal = [&] (double pos, float& a, float& b)
            {
                const double fl = Dsp::floorFast (pos);
                const int at = (int) fl;
                const float frac = (float) (pos - fl);
                if (frac == 0.0f && cut == 0)
                {
                    a = at >= 0 && at < rawLen ? rawL[at] : 0.0f;
                    b = at >= 0 && at < rawLen ? rawR[at] : 0.0f;
                    return;
                }
                interpolator.kernel (frac, cut, coefficients);
                if (at >= back && at - back + taps <= rawLen)
                {
                    dotStereo (coefficients, rawL + at - back, rawR + at - back, taps, a, b);
                    return;
                }
                a = b = 0.0f;
                for (int m = 0; m < taps; ++m)
                {
                    const int index = at - back + m;
                    if (index >= 0 && index < rawLen)
                    {
                        a += coefficients[m] * rawL[index];
                        b += coefficients[m] * rawR[index];
                    }
                }
            };
            const double from = s.startPosition (p[P::StartPos]);

            for (int i = 0; i < num; ++i)
            {
                float l = 0.0f, r = 0.0f, a, b;
                const float go = gOriginal[(size_t) i], gb = gBright[(size_t) i] * breathGain, gd = gDark[(size_t) i] * breathGain;

                // The beds lie in the time line `bedPos - pos` away from the original, lined up with it.
                if (warping && (go > 1.0e-6f || gb > 1.0e-6f || gd > 1.0e-6f))
                {
                    for (int c = 2; c < 6; ++c)
                        warpSource.offset[c] = ln.bedPos - ln.pos;
                    ln.warp.tick (warpSource, ln.pos, timeStep, ratio, tonal, &s.marks, (s.pitched ? s.cycle : 0.0), textureLen, warpPower, warped);
                }

                if (go > 1.0e-6f && ln.pos < end)
                {
                    if (warping)
                    {
                        a = warped[0];
                        b = warped[1];
                    }
                    else
                        readOriginal (ln.pos, a, b);

                    // Before the loop end, what follows it blends with what precedes the loop
                    // start: at the jump both are the same sound.
                    if (! warping && looping && loopFade > 0.0 && ln.pos > loopEnd - loopFade)
                    {
                        float c, d;
                        readOriginal (ln.pos - loopLength, c, d);
                        const float w = (float) ((ln.pos - (loopEnd - loopFade)) / loopFade) * pi * 0.5f;
                        a = a * std::cos (w) + c * std::sin (w);
                        b = b * std::cos (w) + d * std::sin (w);
                    }

                    float fade = 1.0f;
                    if (fadeIn > 0.0 && ln.pos < from + fadeIn)
                        fade *= (float) std::max (0.0, (ln.pos - from) / fadeIn);
                    if (fadeOut > 0.0 && ! looping && ln.pos > end - fadeOut)
                        fade *= (float) std::max (0.0, (end - ln.pos) / fadeOut);

                    l += a * go * fade;
                    r += b * go * fade;
                }

                if (punching)
                {
                    const int bi = (int) ln.bedPos;
                    const float xl = brightL[bi], xr = brightR[bi];
                    ln.punchLow[0] += punchCoef * (xl - ln.punchLow[0]);
                    ln.punchLow[1] += punchCoef * (xr - ln.punchLow[1]);
                    l += (xl - ln.punchLow[0]) * punchArr[(size_t) i];
                    r += (xr - ln.punchLow[1]) * punchArr[(size_t) i];
                }

                if (warping && (gb > 1.0e-6f || gd > 1.0e-6f))
                {
                    const bool dark = warpSource.ch[4] != nullptr;
                    l += warped[2] * gb + (dark ? warped[4] : warped[2]) * gd;
                    r += warped[3] * gb + (dark ? warped[5] : warped[3]) * gd;
                }
                else if (gb > 1.0e-6f || gd > 1.0e-6f)
                {
                    const double bf = Dsp::floorFast (ln.bedPos);
                    const int bi = (int) bf;
                    const int di = darkLen > 0 ? (bi + darkShift) % darkLen : bi;
                    const float bfrac = (float) (ln.bedPos - bf);

                    if (bfrac == 0.0f && cut == 0)
                    {
                        l += brightL[bi] * gb + darkL[di] * gd;
                        r += brightR[bi] * gb + darkR[di] * gd;
                    }
                    else
                    {
                        interpolator.kernel (bfrac, cut, coefficients);
                        if (gb > 1.0e-6f)
                        {
                            dotStereo (coefficients, brightL + bi - back, brightR + bi - back, taps, a, b);
                            l += a * gb;
                            r += b * gb;
                        }
                        if (gd > 1.0e-6f)
                        {
                            dotStereo (coefficients, darkL + di - back, darkR + di - back, taps, a, b);
                            l += a * gd;
                            r += b * gd;
                        }
                    }
                }

                ln.pos += warping ? timeStep : ln.step;
                if (looping && ln.pos >= loopEnd)
                    ln.pos -= loopLength;
                ln.bedPos += warping ? timeStep : bedStep;
                if (ln.bedPos >= (double) bedLen)
                    ln.bedPos -= (double) bedLen;

                bufL[(size_t) i] += l * ln.gainL;
                bufR[(size_t) i] += r * ln.gainR;
            }

            // A slowed attack darkens like a bow starting, following the swell. Let go during it, the note
            // keeps the colour it had: following the release made it ever darker (-35 dB in 0.3 s) and then
            // bright at once when the attack time came (a +40 dB jump).
            if (! released)
                ln.ratio = slowAttack ? juce::jlimit (0.0f, 1.0f, uArr[(size_t) num - 1]) : 1.0f;
            ln.done = ! looping && ln.pos >= end && gBright[(size_t) num - 1] + gDark[(size_t) num - 1] < 1.0e-6f;
            anyAlive = anyAlive || ! ln.done;
        }

        return anyAlive;
    }

    //--------------------------------------------------------------------------
    // Engines B and D: the sample read by grains (stretched by the envelope), then a
    // sustain source (loop / granular / freeze) once the sustain region is reached.
    bool renderSampled (const SampleData& s, const ParamSnapshot& p, int num, const AdsrShape& user, bool hold, float flatten, float breath)
    {
        // With the envelope the sample was found to have (nothing flattened, nothing held): past the attack, where the
        // fitted model has died away (under -60 dB) but the file goes on (a stem whose next hits come after the first
        // one's decay, a tail longer than its fit), the file plays as it is. The envelope over the model was 0 over 0
        // there, and the note fell silent after its first hit: a clap stem lost every clap but the first.
        const bool asRecorded = flatten <= 0.001f && matchesSample (s, user, hold);
        const bool stretch = p[P::Stretch] > 0.5f;
        const float userA = std::max (1.0e-4f, p[P::AmpA] * 0.001f), userD = std::max (1.0e-3f, p[P::AmpD] * 0.001f);
        const double kA = stretch ? (double) s.detected.attack / userA : 1.0;
        const double kD = stretch ? (double) s.detected.decay / userD : 1.0;
        const int mode = (int) p[P::SustainMode];
        const auto& src = hold ? s.early : s.late;
        sustainSource = &src;
        const bool hasSource = src.loop.getNumSamples() > 0 && src.freeze.getNumSamples() > 0;
        const int loopLen = src.loop.getNumSamples();
        const int freezeLen = src.freeze.getNumSamples();

        const double spray = juce::jlimit (0.0f, 1.0f, p[P::Spray] + mod[P::DstSpray]) * 0.5 * loopLen;
        const int grainLen = grainLength (p);
        const float transGain = dbToGain (p[P::TransientGain] + 24.0f * mod[P::DstTransient]);
        const bool wantEnv = true;      // the measured level limits the boost, whatever Aplanar says
        const float denSource = blendDenominator (src.model, src.env, flatten);
        const float powerCoef = 1.0f - std::exp (-1.0f / (0.02f * (float) hostRate));
        // The hand-over to the sustain source: its time and how far it goes (Fundido gran., Mezcla gran.),
        // with equal power on both sides (the grains and the original are not in phase with each other).
        const float srcRamp = (float) (1.0 / (std::max (0.01f, p[P::GrainFade] * 0.001f) * hostRate));
        const float srcTarget = juce::jlimit (0.0f, 1.0f, p[P::GrainBlend]);
        const float granSpeed = p[P::GranSpeed];
        // Movimiento: the source's part kept apart to be coloured (applyMotion), also while it fades back to flat.
        const float motionAmount = juce::jlimit (0.0f, 1.0f, p[P::SustainMotion]);
        const bool moving = hasSource && s.colour.valid() && (motionAmount > 0.0f || motion.engaged());
        if (moving)
        {
            std::fill (srcL.begin(), srcL.begin() + num, 0.0f);
            std::fill (srcR.begin(), srcR.begin() + num, 0.0f);
        }

        // Sostenido enters the source at the sustain point; Natural lets the original
        // play to the end of its region and hands over during the last 150 ms.
        const double entry = hold ? src.start : std::max (src.start, src.end - 0.15 * s.sampleRate);
        const float* rawL = s.audio.getReadPointer (0);
        const float* rawR = s.audio.getReadPointer (1);
        bool anyAlive = false;
        const auto wp = warpPlan (s, p);
        const auto rawSource = warpSourceOf (s.audio, s.length, false, warpAttackEnd (s), 1.0f / (0.01f * (float) hostRate));
        const auto loopSource = warpSourceOf (src.loop, std::max (1, loopLen), true);
        const auto freezeSource = warpSourceOf (src.freeze, std::max (1, freezeLen), true);
        // As recorded, past the end of a sound that ends inside the file: falling at the rate it ended at (the
        // sustain source went on for a second, at -12 dB on a kick, where the file and engine A are silent). From the
        // moment the file is 60 dB under its loudest: before that, what was there is untouched.
        const float endFall = asRecorded ? s.endFallDbPerSec : 0.0f;

        for (int k = 0; k < numLanes; ++k)
        {
            auto& ln = lanes[(size_t) k];
            if (ln.done)
                continue;
            anyAlive = true;
            const double warpRatio = ln.step / wp.timeStep;
            float warped[WarpReader::Source::maxChannels] {};

            // Breathing (Sostenido only): slow level and pitch drift of the sustain source.
            ln.breathAmp.tick (rng, num, 0.6f, hostRate);
            ln.breathPitch.tick (rng, num, 0.35f, hostRate);
            const float breathGain = hold ? 1.0f + breath * 0.15f * ln.breathAmp.value : 1.0f;
            const double srcStep = ln.step * (hold ? 1.0 + breath * 0.0025 * ln.breathPitch.value : 1.0);
            const bool stretching = (ln.seg == SegAttack && std::abs (kA - 1.0) > 1.0e-3) || (ln.seg == SegDecay && std::abs (kD - 1.0) > 1.0e-3);
            const double period = (s.period > 0.0 && stretching) ? s.period : 0.0;
            // The level correction of the grains is measured once per block; it moves to it a sample at a
            // time (a step every 32 samples was a click in long attacks).
            const float grainCohTarget = ln.grains.coherenceGain(), srcCohTarget = ln.srcGrains.coherenceGain();
            if (ln.grainCoh < 0.0f) ln.grainCoh = grainCohTarget;
            if (ln.srcCoh < 0.0f) ln.srcCoh = srcCohTarget;
            const float grainCohStep = (grainCohTarget - ln.grainCoh) / (float) num, srcCohStep = (srcCohTarget - ln.srcCoh) / (float) num;

            for (int i = 0; i < num; ++i)
            {
                const float u = uArr[(size_t) i];
                float l = 0.0f, r = 0.0f;
                float ml = 0.0f, mr = 0.0f;         // the source's part, under Movimiento
                ln.grainCoh += grainCohStep;
                ln.srcCoh += srcCohStep;
                const float grainCoherence = ln.grainCoh, sourceCoherence = ln.srcCoh;

                if (ln.srcMix < 1.0f)
                {
                    float bl, br, dm, de;
                    if (wp.on)
                    {
                        ln.warp.tick (rawSource, ln.pos, wp.timeStep, warpRatio, wp.cycles, &s.marks, (s.pitched ? s.cycle : 0.0), wp.textureLen, wp.power, warped);
                        bl = warped[0];
                        br = warped[1];
                        dm = s.modelFast (ln.pos);
                        de = wantEnv ? std::max (1.0e-3f, s.envAt (ln.pos)) : 1.0f;
                    }
                    else
                        ln.grains.tick (s, ln.pos, ln.step, grainLen, period, wantEnv, powerCoef, bl, br, dm, de);
                    const float den = modelBelowFloor (s, ln.pos, blendDenominator (dm, de, flatten), flatten, released);
                    ln.ratio = asRecorded && ln.seg != SegAttack && dm <= 1.0001e-3f ? relArr[(size_t) i] : u / den;
                    // Where the sample is louder than its model (a hit, a bump) it is not raised
                    // over the curve: the boost is only for what the sample lacks, as in engine A.
                    const float limit = std::max (1.0f, u / de);
                    const float g = std::min ({ maxBoost, ln.ratio, limit }) * (wp.on ? 1.0f : grainCoherence) * std::cos (ln.srcMix * pi * 0.5f);
                    l = bl * g;
                    r = br * g;
                }

                if (ln.srcMix > 0.0f)
                {
                    float xl = 0.0f, xr = 0.0f;
                    if (wp.on && mode != P::SustainGranular)
                    {
                        const bool frozen = mode == P::SustainFreeze;
                        const int length = frozen ? freezeLen : loopLen;
                        ln.warpSrc.tick (frozen ? freezeSource : loopSource, ln.srcPos, wp.timeStep, warpRatio * (srcStep / ln.step),
                                         wp.cycles, nullptr, (s.pitched ? s.cycle : 0.0), wp.textureLen, wp.power, warped);
                        xl = warped[0];
                        xr = warped[1];
                        ln.srcPos += wp.timeStep;
                        if (length > 0 && ln.srcPos >= length) ln.srcPos -= length;
                    }
                    else if (mode == P::SustainFreeze)
                    {
                        readBandLimitedWrap (src.freeze.getReadPointer (0), src.freeze.getReadPointer (1), ln.srcPos, freezeLen, srcStep, sincCoefs, xl, xr);
                        ln.srcPos += srcStep;
                        if (ln.srcPos >= freezeLen) ln.srcPos -= freezeLen;
                    }
                    else if (mode == P::SustainGranular)
                    {
                        ln.srcGrains.tick (src.loop, ln.srcPos, srcStep, grainLen, spray, s.period, powerCoef, rng, xl, xr);
                        xl *= sourceCoherence;
                        xr *= sourceCoherence;
                        ln.srcPos += srcStep * granSpeed * 0.5;
                        if (ln.srcPos >= loopLen) ln.srcPos -= loopLen;
                        if (ln.srcPos < 0.0) ln.srcPos += loopLen;
                    }
                    else
                    {
                        readBandLimitedWrap (src.loop.getReadPointer (0), src.loop.getReadPointer (1), ln.srcPos, loopLen, srcStep, sincCoefs, xl, xr);
                        ln.srcPos += srcStep;
                        if (ln.srcPos >= loopLen) ln.srcPos -= loopLen;
                    }

                    const float ratio = u / denSource;
                    if (ln.srcMix >= 1.0f)
                        ln.ratio = ratio;
                    // Never above the curve: the loop's own level says how far it can be raised.
                    const float g = std::min ({ maxBoost, ratio, std::max (1.0f, u / src.level) }) * breathGain * std::sin (ln.srcMix * pi * 0.5f);
                    if (moving)
                    {
                        ml = xl * g;
                        mr = xr * g;
                    }
                    else
                    {
                        l += xl * g;
                        r += xr * g;
                    }
                }

                const float w = wTrans[(size_t) i];
                if (w > 0.0f)
                {
                    const float tg = w * transGain * relArr[(size_t) i];
                    ml *= 1.0f - w;
                    mr *= 1.0f - w;
                    if (wp.on)
                    {
                        ln.warpTrans.tick (rawSource, ln.transPos, wp.timeStep, warpRatio, wp.cycles, &s.marks, (s.pitched ? s.cycle : 0.0), wp.textureLen, wp.power, warped);
                        l = l * (1.0f - w) + warped[0] * tg;
                        r = r * (1.0f - w) + warped[1] * tg;
                    }
                    else
                    {
                        float tl, tr;
                        readBandLimited (rawL, rawR, ln.transPos, s.length, ln.step, sincCoefs, tl, tr);
                        l = l * (1.0f - w) + tl * tg;
                        r = r * (1.0f - w) + tr * tg;
                    }
                }
                ln.transPos += wp.on ? wp.timeStep : ln.step;

                // Position and entry into the sustain source.
                advanceWarped (ln, s, kA, kD, wp);
                if (ln.seg != SegSustain && ln.seg != SegAttack && hasSource && ln.pos >= entry)
                {
                    ln.seg = SegSustain;
                    ln.srcPos = mode == P::SustainFreeze ? rng.nextDouble() * freezeLen
                                                         : std::fmod (std::max (0.0, ln.pos - src.start), (double) loopLen);
                    ln.srcGrains.prime (ln.srcPos, grainLen);
                }
                if (ln.seg == SegSustain)
                    ln.srcMix += juce::jlimit (-srcRamp, srcRamp, srcTarget - ln.srcMix);

                if (endFall > 0.0f && ln.pos > s.soundEnd)
                {
                    const float past = dbToGain (-endFall * (float) ((ln.pos - s.soundEnd) / s.sampleRate));
                    l *= past;
                    r *= past;
                    ml *= past;
                    mr *= past;
                    if (past < 1.0e-5f)
                        ln.done = true;
                }

                bufL[(size_t) i] += l * ln.gainL;
                bufR[(size_t) i] += r * ln.gainR;
                if (moving)
                {
                    srcL[(size_t) i] += ml * ln.gainL;
                    srcR[(size_t) i] += mr * ln.gainR;
                }
            }

            if (ln.seg != SegSustain && ln.pos > s.length + grainLen * ln.step && ln.transPos > s.length)
                ln.done = true;
        }

        if (moving)
            applyMotion (s, p, num, hold, motionAmount);
        return anyAlive;
    }

    // Movimiento: the colour the sample had where the note is now (the first lane's place in it, which goes on moving
    // through the sustain as if the original were still playing; held on its last clear frame past it), against the
    // colour of the source that holds the note, as gains of MotionEq on the source's part, which is then added in.
    void applyMotion (const SampleData& s, const ParamSnapshot& p, int num, bool hold, float amount) noexcept
    {
        using C = SampleData::Colour;
        const auto& c = s.colour;
        const auto& ln = lanes[0];
        const auto& source = hold ? c.early : c.late;
        const double frame = juce::jlimit (0.0, (double) c.lastGood, ln.pos / (double) c.hop);
        const int f0 = (int) frame, f1 = std::min (c.lastGood, f0 + 1);
        const float fr = (float) (frame - f0);
        // In with the hand-over: nothing before the source is heard.
        const float handover = juce::jlimit (0.0f, 1.0f, ln.srcMix / std::max (0.05f, p[P::GrainBlend]));

        std::array<float, C::numBands> diff {};
        std::array<bool, C::numBands> valid {};
        float topOrig = -300.0f, topSource = -300.0f;
        for (int b = 0; b < motion.bands; ++b)
        {
            const float o = c.db[(size_t) f0 * C::numBands + (size_t) b]
                          + fr * (c.db[(size_t) f1 * C::numBands + (size_t) b] - c.db[(size_t) f0 * C::numBands + (size_t) b]);
            diff[(size_t) b] = o - source[(size_t) b];
            topOrig = std::max (topOrig, o);
            topSource = std::max (topSource, source[(size_t) b]);
        }
        // Only bands both have (within 60 dB of their loudest): the rest is noise, or nothing to colour.
        double weighted = 0.0, weights = 0.0;
        for (int b = 0; b < motion.bands; ++b)
        {
            const float o = diff[(size_t) b] + source[(size_t) b];
            valid[(size_t) b] = o > topOrig - 60.0f && source[(size_t) b] > topSource - 60.0f;
            if (valid[(size_t) b])
            {
                const double w = std::pow (10.0, (source[(size_t) b] - topSource) / 10.0);
                weights += w;
                weighted += w * std::pow (10.0, diff[(size_t) b] / 10.0);
            }
        }
        const float level = weights > 0.0 ? (float) (10.0 * std::log10 (weighted / weights)) : 0.0f;

        const float follow = 1.0f - std::exp (-(float) num / (0.08f * (float) hostRate));
        for (int b = 0; b < motion.bands; ++b)
        {
            const float target = valid[(size_t) b] ? juce::jlimit (-12.0f, 12.0f, diff[(size_t) b] - level) * amount * handover * motion.scale
                                                   : 0.0f;
            auto& d = motion.db[(size_t) b];
            d += (target - d) * follow;
            if (std::abs (d) < 1.0e-3f && std::abs (target) < 1.0e-3f)
                d = 0.0f;
            motion.set (b, d);
        }
        double in = 0.0, out = 0.0;
        for (int i = 0; i < num; ++i)
        {
            const float l = srcL[(size_t) i], r = srcR[(size_t) i];
            in += (double) l * l + (double) r * r;
            srcL[(size_t) i] = motion.tick (0, l);
            srcR[(size_t) i] = motion.tick (1, r);
            out += (double) srcL[(size_t) i] * srcL[(size_t) i] + (double) srcR[(size_t) i] * srcR[(size_t) i];
        }
        const float powerCoef = 1.0f - std::exp (-(float) num / (0.3f * (float) hostRate));
        motion.powerIn += ((float) (in / num) - motion.powerIn) * powerCoef;
        motion.powerOut += ((float) (out / num) - motion.powerOut) * powerCoef;
        const float target = motion.engaged() && motion.powerOut > 1.0e-14f
                           ? juce::jlimit (0.25f, 4.0f, std::sqrt ((motion.powerIn + 1.0e-20f) / (motion.powerOut + 1.0e-20f)))
                           : 1.0f;
        const float from = motion.keep, step = (target - from) / (float) num;
        for (int i = 0; i < num; ++i)
        {
            const float g = from + step * (float) (i + 1);
            bufL[(size_t) i] += srcL[(size_t) i] * g;
            bufR[(size_t) i] += srcR[(size_t) i] * g;
        }
        motion.keep = target;
    }

    //--------------------------------------------------------------------------
    // Engine C: sines (each partial with its own envelope) + shaped noise + the
    // original transient. Timbre follows the envelope per partial.
    bool renderResynth (const SampleData& s, const ParamSnapshot& p, int num, const AdsrShape& user, bool hold,
                        float flatten, float bow, float breath, float relPhase)
    {
        const auto& pm = s.partials;
        // (Parciales counts a side's: a model of the two sides apart has twice as many to play)
        const int K = std::min ({ maxPartials, pm.numPartials, juce::roundToInt (p[P::Partials]) * (pm.split ? 2 : 1) });
        const bool stretch = p[P::Stretch] > 0.5f;
        const double kA = stretch ? (double) s.detected.attack / user.attack : 1.0;
        const double kD = stretch ? (double) s.detected.decay / user.decay : 1.0;
        const float transGain = dbToGain (p[P::TransientGain] + 24.0f * mod[P::DstTransient]);
        const float noiseLevel = p[P::NoiseLevel];
        const float velBright = p[P::VelBright], relDark = p[P::RelDark];
        const float* rawL = s.audio.getReadPointer (0);
        const float* rawR = s.audio.getReadPointer (1);
        const float uEnd = uArr[(size_t) num - 1];
        const float nyquistGuard = 0.45f * (float) hostRate;
        const auto wp = warpPlan (s, p);
        const auto rawSource = warpSourceOf (s.audio, s.length, false, warpAttackEnd (s), 1.0f / (0.01f * (float) hostRate));
        const bool asRecorded = flatten <= 0.001f && matchesSample (s, user, hold);
        // As recorded, past the end of a sound that ends inside the file, the partials and the noise also fall at the
        // rate it ended at (they went on at their own slow decay, at -8 dB on a kick 20 ms after the file falls
        // silent). From where the file's sound ends, not from the model's last frame: that can come first, and a
        // bass then fell silent where the file still sounds at -33 dB.
        const float endFall = asRecorded ? s.endFallDbPerSec : 0.0f;

        for (int k = 0; k < numLanes; ++k)
        {
            auto& ln = lanes[(size_t) k];
            const double ratioOfPitch = ln.step / wp.timeStep;
            float warped[WarpReader::Source::maxChannels] {};
            std::fill (laneL.begin(), laneL.begin() + num, 0.0f);
            std::fill (laneR.begin(), laneR.begin() + num, 0.0f);

            // Advance the position over the block (holds at the sustain point in Sostenido).
            const bool atHold = hold && ln.pos >= s.sustainPos && ln.seg != SegAttack;
            if (atHold)
            {
                ln.pos = std::max (ln.pos, s.sustainPos);
                ln.seg = SegSustain;
            }
            else
            {
                for (int i = 0; i < num; ++i)
                    advanceWarped (ln, s, kA, kD, wp);
            }

            // Re-align the oscillators with the original once the transient fades.
            if (! ln.synced && t >= p[P::TransientMs] * 0.0005f)
            {
                syncPartialPhases (s, ln);
                ln.synced = true;
            }

            // Frame at the end of the block (or extrapolated past the end of the sample).
            const double fp = ln.pos / pm.hop;
            const bool beyond = fp >= pm.lastFrame;
            const int f0 = juce::jlimit (0, pm.numFrames - 1, (int) fp);
            const int f1 = std::min (pm.numFrames - 1, f0 + 1);
            const float frac = beyond ? 0.0f : (float) (fp - f0);
            const float beyondSeconds = beyond ? (float) ((ln.pos - (double) pm.lastFrame * pm.hop) / s.sampleRate) : 0.0f;
            const int fBase = beyond ? pm.lastFrame : f0;
            const float pastEndDb = beyond && endFall > 0.0f ? endFall * (float) std::max (0.0, (ln.pos - s.soundEnd) / s.sampleRate) : 0.0f;

            const float measured = std::max (1.0e-3f, s.envAt (ln.pos));
            const float den = blendDenominator (s.modelFast (ln.pos), measured, flatten);
            // With the envelope the sample was found to have (and nothing flattened, nothing held), the partials and
            // the noise play at the levels they were measured at, as engine A plays the sample untouched: dividing the
            // envelope by its own model is 1, but the model is floored at -60 dB and the envelope is not, and on a
            // slow, curved attack (a kettle coming to the boil: 5.5 s) the note was silent for its first half second
            // (-113 dB where the sample is at -55). After the key is let go, the release's own fall.
            // (Measured against the floor-only fix of engines B and D, modelBelowFloor: here this one keeps the
            // transposed notes' tails and levels better, 35 tails longer otherwise; approved by ear on 2026-11-22.)
            const float ratio = asRecorded ? (released ? relArr[(size_t) num - 1] : 1.0f)
                                           : std::min ({ maxBoost, uEnd / den, std::max (1.0f, uEnd / measured) });
            ln.ratio = ratio;
            const float pitchRatio = (float) (ln.step * hostRate / s.sampleRate);

            // Timbre: darker when the envelope is below the original (slow bowed attack),
            // during the release, and for soft notes. One exponent per partial.
            const float darkExp = bow * 0.5f * std::log (std::max (1.0e-3f, std::min (1.0f, ratio)))
                                + velBright * 0.5f * std::log (std::max (0.05f, std::min (1.0f, velocity / 0.8f)))
                                - relDark * 1.5f * relPhase + 0.2f * timbreOct();

            for (int d = 0; d < 8; ++d)
                ln.drifts[(size_t) d].tick (rng, num, 0.2f + 0.15f * (float) d, hostRate);

            // The partials of this frame. Under Tonos each one takes the level the frame has at the
            // frequency it now sounds at, so the formants stay where they were.
            std::array<float, maxPartials> frameAmp {}, frameFreq {};
            double holdPower = 0.0, nowPower = 0.0;         // past the end: the partials now against the last whole frame
            for (int q = 0; q < K; ++q)
            {
                const size_t i0 = (size_t) fBase * pm.numPartials + q;
                const size_t i1 = (size_t) f1 * pm.numPartials + q;
                frameAmp[(size_t) q] = beyond ? pm.amp[i0] * dbToGain (-pm.decayDbPerSec[(size_t) q] * beyondSeconds - pastEndDb)
                                              : pm.amp[i0] + frac * (pm.amp[i1] - pm.amp[i0]);
                frameFreq[(size_t) q] = beyond ? pm.freq[i0] : pm.freq[i0] + frac * (pm.freq[i1] - pm.freq[i0]);
                const float held = pm.amp[(size_t) pm.holdFrame * pm.numPartials + q];
                holdPower += (double) held * held;
                nowPower += (double) frameAmp[(size_t) q] * frameAmp[(size_t) q];
            }
            // A file cut while it still sounds, held past its end (a note longer than it, or played higher so that
            // it runs out sooner): the noise is the last whole frame's, falling with the partials. The last frames'
            // windows hang over the end, and what the partials there leave out was taken for noise: a steady tone
            // played an octave up turned into a breath 18 dB under its harmonics (the sample: 80 dB under).
            const bool cutEnd = beyond && pm.holdFrame < pm.lastFrame;
            const float noiseFall = cutEnd && holdPower > 1.0e-20 ? (float) std::sqrt (std::min (1.0, nowPower / holdPower)) : 1.0f;
            // The spectral envelope of this frame, drawn through its harmonics only (in log level):
            // a weak partial between them, or below the fundamental, must not take the level of the
            // strong one it lands next to.
            std::array<float, maxPartials> envFreq {}, envLog {};
            int envCount = 0;
            if (wp.on && wp.tonal && s.period > 1.0)
            {
                const float fundamental = (float) (s.sampleRate / s.period);
                for (int q = 0; q < K; ++q)
                {
                    const float f = frameFreq[(size_t) q], harmonic = std::round (f / fundamental);
                    if (harmonic < 1.0f || std::abs (f - harmonic * fundamental) > 0.2f * fundamental || frameAmp[(size_t) q] <= 1.0e-9f)
                        continue;
                    if (envCount > 0 && f <= envFreq[(size_t) envCount - 1])
                        continue;
                    envFreq[(size_t) envCount] = f;
                    envLog[(size_t) envCount] = std::log (frameAmp[(size_t) q]);
                    ++envCount;
                }
            }
            const bool keepFormants = envCount > 1;
            const auto envelopeAt = [&] (float hz)
            {
                if (hz <= envFreq[0])
                    return envLog[0];
                const int last = envCount - 1;
                if (hz >= envFreq[(size_t) last])     // past the last harmonic the sample has nothing: 36 dB an octave down
                    return envLog[(size_t) last] - 6.0f * std::log2 (hz / envFreq[(size_t) last]) * 0.6931472f;
                const int hi = (int) (std::upper_bound (envFreq.begin(), envFreq.begin() + envCount, hz) - envFreq.begin());
                const float x = (hz - envFreq[(size_t) hi - 1]) / (envFreq[(size_t) hi] - envFreq[(size_t) hi - 1]);
                return envLog[(size_t) hi - 1] + x * (envLog[(size_t) hi] - envLog[(size_t) hi - 1]);
            };

            // Each partial moves by what the envelope says between where it was and where it now
            // sounds; the frame then keeps its own power (moving the fundamental to a weaker place
            // must not leave the note quieter), as the PSOLA reader of engines A, B, D does.
            std::array<float, maxPartials> partialAmp = frameAmp;
            if (keepFormants)
            {
                float before = 0.0f, after = 0.0f;
                for (int q = 0; q < K; ++q)
                {
                    const float f = frameFreq[(size_t) q];
                    if (f * (float) pitchRatio >= nyquistGuard)
                        continue;
                    const float change = juce::jlimit (-4.0f, 4.0f, envelopeAt (f * (float) ratioOfPitch) - envelopeAt (f));
                    partialAmp[(size_t) q] = frameAmp[(size_t) q] * std::exp (change);
                    before += frameAmp[(size_t) q] * frameAmp[(size_t) q];
                    after += partialAmp[(size_t) q] * partialAmp[(size_t) q];
                }
                const float powerGain = juce::jlimit (0.25f, 4.0f, std::sqrt ((before + 1.0e-12f) / (after + 1.0e-12f)));
                for (int q = 0; q < K; ++q)
                    partialAmp[(size_t) q] *= powerGain;
            }

            float energy = 0.0f;
            for (int q = 0; q < K; ++q)
            {
                float a = partialAmp[(size_t) q];
                float f = frameFreq[(size_t) q];

                if (atHold)
                {
                    a *= 1.0f + breath * 0.15f * ln.drifts[(size_t) q % 8].value;
                    f *= 1.0f + breath * 0.002f * ln.drifts[(size_t) (q + 3) % 8].value;
                }

                const float target = a * ratio * std::exp (brightness[(size_t) q] * darkExp);
                const float outFreq = f * pitchRatio;
                float current = ln.amp[(size_t) q];
                const float tgt = outFreq < nyquistGuard ? target : 0.0f;
                energy += tgt;

                if (current < 1.0e-6f && tgt < 1.0e-6f)
                {
                    ln.phase[(size_t) q] += f * pitchRatio / hostRate * num;
                    ln.amp[(size_t) q] = 0.0f;
                    continue;
                }

                // A phase left running for many hours while silent comes back into one cycle.
                if (std::abs (ln.phase[(size_t) q]) >= 1073741824.0)
                    ln.phase[(size_t) q] -= Dsp::floorFast (ln.phase[(size_t) q]);
                bank.q[bank.count] = q;
                bank.phase[bank.count] = ln.phase[(size_t) q];
                bank.inc[bank.count] = outFreq / hostRate;
                bank.amp[bank.count] = current;
                bank.da[bank.count] = (tgt - current) / (float) num;
                bank.gl[bank.count] = partialPanL[(size_t) q];
                bank.gr[bank.count] = partialPanR[(size_t) q];
                ++bank.count;
                ln.amp[(size_t) q] = tgt;
            }
            bank.render (num, laneL.data(), laneR.data());
            for (int a = 0; a < bank.count; ++a)
                ln.phase[(size_t) bank.q[a]] = bank.phase[a] - Dsp::floorFast (bank.phase[a]);
            bank.count = 0;

            // Shaped noise (first lane only, shared by the voice).
            if (k == 0 && noiseLevel > 0.0f)
            {
                const int nb = SampleData::PartialModel::numBands;
                const float air = (! released && ratio < 1.0f) ? 1.0f + bow * 2.0f * (1.0f - ratio) : 1.0f;
                const float rateFix = std::sqrt ((float) (hostRate / s.sampleRate));
                for (int b = 0; b < nb; ++b)
                {
                    const float a0 = pm.noise[(size_t) fBase * nb + b];
                    const float a1 = pm.noise[(size_t) f1 * nb + b];
                    const float base = cutEnd ? pm.noise[(size_t) pm.holdFrame * nb + b] * noiseFall
                                     : beyond ? a0 * dbToGain (-std::max (3.0f, s.detected.fadeDbPerSec) * beyondSeconds - pastEndDb)
                                              : a0 + frac * (a1 - a0);
                    bandAmps[(size_t) b] = base * ratio * noiseLevel * air * rateFix * std::exp (bandBrightness[(size_t) b] * darkExp);
                    energy += bandAmps[(size_t) b];
                }
                noise.process (laneL.data(), laneR.data(), num, bandAmps.data(), pm.bandEdges.data(), keepFormants ? 1.0f : pitchRatio, hostRate);
            }

            // Original transient, crossfaded into the resynthesis.
            for (int i = 0; i < num; ++i)
            {
                const float w = wTrans[(size_t) i];
                if (w > 0.0f)
                {
                    const float tg = w * transGain * relArr[(size_t) i];
                    float tl, tr;
                    if (wp.on)
                    {
                        ln.warpTrans.tick (rawSource, ln.transPos, wp.timeStep, ratioOfPitch, wp.cycles, &s.marks, (s.pitched ? s.cycle : 0.0), wp.textureLen, wp.power, warped);
                        tl = warped[0];
                        tr = warped[1];
                    }
                    else
                    {
                        readBandLimited (rawL, rawR, ln.transPos, s.length, ln.step, sincCoefs, tl, tr);
                    }
                    laneL[(size_t) i] = laneL[(size_t) i] * (1.0f - w) + tl * tg;
                    laneR[(size_t) i] = laneR[(size_t) i] * (1.0f - w) + tr * tg;
                }
                ln.transPos += wp.on ? wp.timeStep : ln.step;
                bufL[(size_t) i] += laneL[(size_t) i] * ln.gainL;
                bufR[(size_t) i] += laneR[(size_t) i] * ln.gainR;
            }

            ln.done = beyond && energy < 1.0e-6f && ! atHold;
        }

        for (int k = 0; k < numLanes; ++k)
            if (! lanes[(size_t) k].done)
                return true;
        return false;
    }

    //--------------------------------------------------------------------------
    // LOOP: the sample read round and round from its start, without seams (the file as it is: its ends
    // were not faded). With Warp the time follows the tempo and the key only the pitch:
    //  - Beats: the loop is cut at its hits; each slice starts where its hit falls in the warped time and is
    //    read at its own pace (and pitch), so the hits stay whole. A slice that would run into the next hit
    //    fades out over 4 ms before it; the next one starts with a 1 ms rise.
    //  - Tonal: the Warp reader of the engines in Textura (overlapping grains matched in level), wrapping.
    // At its own tempo and pitch it is the file as it is, whatever the algorithm. Without Warp it is a
    // classic sampler: a key up is faster.
    // The pitch of a loop: C3 plays it as recorded, a key moves it by its semitones, Transponer and Fino too.
    // Snap a C never applies (a loop's "note" is not what it is tuned to: on a drum loop it moved C3 up to
    // 6 semitones away from the recording, the chipmunk heard at the root key).
    double loopStep (const SampleData& s, const ParamSnapshot& p) const
    {
        const float semis = pitch - (float) SynthEngine::rootKey + p[P::Transpose] + p[P::Fine] * 0.01f
                          + engine.pitchBend * p[P::BendRange] + mpeBend() + vibrato + pitchEnv + (mod[P::DstPitch] * 24.0f + mod[P::DstFine])
                          + SynthEngine::snapShift (s, p[P::SnapC] > 0.5f);
        return std::exp2 (semis / 12.0) * s.sampleRate / hostRate;
    }

    // The loop's own tempo: BPM original if set, else the length chosen in beats, else what was detected.
    static double loopTempo (const SampleData& s, const ParamSnapshot& p, int length) noexcept
    {
        if (p[P::LoopBpm] > 0.0f)
            return p[P::LoopBpm];
        const int chosen = P::loopBeatChoices[(size_t) juce::jlimit (0, 12, (int) p[P::LoopBeats])];
        const int beats = chosen > 0 ? chosen : s.loopInfo.beats;
        return beats > 0 ? beats * 60.0 / (length / s.sampleRate) : 0.0;
    }

    void releaseStretcher() noexcept
    {
        if (stretcher != nullptr)
            stretcher->inUse = false;
        stretcher = nullptr;
        stretcherPrimed = false;
    }

    // SLICE: the slice of this key, read as it is (Transponer and Fino move it), from just before its hit to
    // just before the next one: a 0.3 ms rise, 2 ms of fade before the next slice's hit. It plays to its end
    // whatever the key does (as a sampler's trigger mode), so a short tap still plays the whole hit.
    bool renderSlice (const SampleData& s, const ParamSnapshot& p, int num)
    {
        if (sliceTo <= sliceFrom)
            return false;
        const auto& buf = s.loopAudio.getNumSamples() >= s.length ? s.loopAudio : s.audio;
        const float* a = buf.getReadPointer (0);
        const float* b = buf.getReadPointer (std::min (1, buf.getNumChannels() - 1));
        const float semis = p[P::Transpose] + p[P::Fine] * 0.01f + engine.pitchBend * p[P::BendRange] + mpeBend();
        const double speed = std::exp2 (semis / 12.0) * s.sampleRate / hostRate;
        // Fundido: the fade before the slice's end (Thru: before the sample's end); 0 ms: none.
        const double fadeIn = std::max (1.0, 0.0003 * s.sampleRate), fadeOut = std::max (1.0, p[P::SliceFade] * 0.001 * s.sampleRate);
        for (int i = 0; i < num; ++i)
        {
            if (loopRead >= sliceTo)
                return false;
            const float g = (float) std::min ({ 1.0, (loopRead - sliceFrom) / fadeIn + 1.0 / fadeIn, (sliceTo - loopRead) / fadeOut });
            float l, r;
            readBandLimited (a, b, loopRead, s.length, speed, loopCoefs, l, r);
            bufL[(size_t) i] += l * g * uArr[(size_t) i];
            bufR[(size_t) i] += r * g * uArr[(size_t) i];
            loopRead += speed;
        }
        return true;
    }

    // LOOP NORMAL: the sample read round and round from its start, without seams (the file as it is: its ends
    // were not faded). With Warp the time follows the tempo and the key only the pitch:
    //  - Beats: the loop is cut at its hits; each slice starts where its hit falls in the warped time and is
    //    read at its own pace (and pitch), so the hits stay whole.
    //  - Complex / Complex Pro: Signalsmith Stretch (a phase vocoder that keeps the channels coherent and,
    //    in Pro, the formants), fed ahead of the note by its latency so that its output lines up with the
    //    loop; its first few tens of milliseconds, before it has anything, come from the Textura reader and
    //    hand over in 10 ms.
    //  - (Tonal, the overlapping grains of earlier versions, is gone: it plays Complex. It stays only as the
    //    first milliseconds of Complex, and for a note that starts before LOOP's stretchers exist: the moment after
    //    LOOP is chosen. There is one per voice.)
    // At its own tempo and pitch it is the file as it is, whatever the algorithm. Without Warp it is a
    // classic sampler: a key up is faster.
    bool renderLoop (const SampleData& s, const ParamSnapshot& p, int num)
    {
        const auto& info = s.loopInfo;
        const auto& buf = s.loopAudio.getNumSamples() >= s.length ? s.loopAudio : s.audio;
        const float* a = buf.getReadPointer (0);
        const float* b = buf.getReadPointer (std::min (1, buf.getNumChannels() - 1));
        if ((int) p[P::LoopKind] == P::LoopSlice)
            return renderSlice (s, p, num);
        const int fullLength = std::max (1, info.length > 0 ? std::min (info.length, s.length) : s.length);
        const double loopBpm = loopTempo (s, p, fullLength);
        // Ventana: the part of the loop that plays is the loop now (its start the reading's 0, its cuts those in
        // it). Moved or resized while playing: the old part fades out over 3 ms, then the new one is read from
        // there, 3 ms faded in, the stretch started afresh (cut at once, a click: measured).
        auto window = SynthEngine::loopWindow (s, p);
        if (loopWinLength > 0 && (window.start != loopWinStart || window.length != loopWinLength) && windowOut < 1.0f)
        {
            window = { loopWinStart, loopWinLength, loopWinLength != fullLength };       // still the old part
            const float stepOut = 1.0f / (float) std::max (1.0, 0.003 * hostRate);
            for (int i = 0; i < num; ++i)
                uArr[(size_t) i] *= std::max (0.0f, 1.0f - (windowOut + (float) (i + 1) * stepOut));
            windowOut += (float) num * stepOut;
        }
        const int length = window.length;
        if (window.on)
        {
            a += window.start;
            b += window.start;
        }
        if (window.start != loopWinStart || window.length != loopWinLength)
        {
            windowOut = 0.0f;
            if (loopWinLength > 0)
            {
                loopTime = std::fmod (loopTime, (double) length);
                loopRead = loopTime;
                plainPos = loopTime;
                loopSlice = -1;
                stretcherPrimed = false;
                jumpFade = 0.0f;
            }
            loopWinStart = window.start;
            loopWinLength = window.length;
        }
        const auto& allCuts = engine.beatSlices (s);
        if (window.on)
        {
            windowCuts.clear();
            windowCuts.push_back (0);
            for (const int c : allCuts)
                if (c > window.start && c < window.start + length && windowCuts.size() < windowCuts.capacity())
                    windowCuts.push_back (c - window.start);
        }
        const auto& loopCuts = window.on ? windowCuts : allCuts;
        const bool warp = p[P::LoopWarp] > 0.5f;
        const bool follow = p[P::LoopSync] > 0.5f && engine.tempoKnown && loopBpm > 0.0;
        const double tempo = warp && follow ? engine.bpm / loopBpm : 1.0;
        const double natural = s.sampleRate / hostRate;          // source samples per output sample at the file's pace
        const double step = loopStep (s, p);                     // ... at the key's pitch
        const double pitchRatio = step / natural;
        const double timeStep = natural * tempo;
        int algo = (int) p[P::LoopAlgo];
        if (algo == P::LoopAuto)
            algo = info.rhythmic ? P::LoopBeatsAlgo : P::LoopComplex;      // Complex keeps the level; Pro is a choice

        const auto wrap = [length] (double& x) { if (x >= length) x -= length * std::floor (x / length); };
        lastTimeStep = ! warp ? step : timeStep;
        if (transport && loopBpm > 0.0)
        {
            // Where the host is in the loop (its first beat on the host's beat 1). A jump (start, a locate, a
            // loop of the host) puts it there with 3 ms of fade in; a running loop is already there.
            const double beats = loopBpm * (length / s.sampleRate) / 60.0;
            double at = std::fmod (engine.transportPpqNow, beats);
            if (at < 0.0)
                at += beats;
            // At the loop's own tempo the reading lands on a whole sample (at most 11 us early or late): the
            // file itself, not a fraction of a sample away from it.
            const double exact = at / beats * length;
            const double target = std::abs (tempo - 1.0) * length < 1.0 ? std::round (exact) : exact;
            const auto apart = [length, target] (double x) { double d = target - x; return std::abs (d - length * std::round (d / length)); };
            // The file read as it is counts only at the loop's own tempo (stretched, it is not used and does
            // not keep the time line's pace: taken into account, it re-started the stretch every few ms).
            const bool ownTempo = std::abs (tempo - 1.0) * length < 1.0;
            if (! ownTempo)
                plainPos = loopTime;
            if (std::max (apart (loopTime), ownTempo ? apart (plainPos) : 0.0) > 0.004 * s.sampleRate)
            {
                loopTime = loopRead = plainPos = target;
                stretcherPrimed = false;
                const auto& beatCuts = loopCuts;
                const int* sl = beatCuts.empty() ? nullptr : beatCuts.data();
                if (sl != nullptr)
                {
                    const int ns = (int) beatCuts.size();
                    loopSlice = juce::jlimit (0, ns - 1, (int) (std::upper_bound (sl, sl + ns, (int) target) - sl) - 1);
                    loopEnd = loopSlice + 1 < ns ? sl[loopSlice + 1] : length;
                    loopRise = 1.0f;
                    loopFadeGain = 0.0f;
                }
                jumpFade = 0.0f;
            }
        }
        const bool plain = (! warp && ! transport) || (! transport && std::abs (tempo - 1.0) * length < 1.0 && std::abs (pitchRatio - 1.0) < 1.0e-9);
        // The transport voice at the loop's own tempo: the file as it is until a key tunes it, then the stretch.
        const bool blendPlain = transport && std::abs (tempo - 1.0) * length < 1.0;
        const float mixFrom = stretchMix, rampLen = (float) std::max (1.0, 0.01 * hostRate);
        if (! blendPlain || transportTuned || std::abs (pitchRatio - 1.0) > 1.0e-9)
            stretchMix = std::min (1.0f, blendPlain ? stretchMix + (float) num / (float) std::max (1.0, 0.01 * hostRate) : 1.0f);
        // The file as it is, read sample by sample from where the host put it (not along the time line, whose
        // tempo is the host's to the last digit: 122 against a loop worked out as 121.9998 drifted the reading
        // by fractions of a sample, -12 dB of difference against the file). The host's position re-locks it.
        const double plainBase = plainPos;
        if (transport)
        {
            plainPos += num * natural;
            wrap (plainPos);
        }
        const auto plainBlend = [&] (int i, double, float& l, float& r)
        {
            if (stretchMix >= 1.0f)
                return;
            float pl, pr;
            double x = plainBase + i * natural;
            wrap (x);
            readBandLimitedWrap (a, b, x, length, natural, loopCoefs, pl, pr);
            // Sample by sample from where the last block left it (a step of gain between the two would click).
            // Smoothstep: no corner at either end (a straight ramp's end left a tick at -50 dB, measured).
            const float lin = stretchMix > mixFrom ? std::min (1.0f, mixFrom + (float) (i + 1) / rampLen) : stretchMix;
            const float g = lin * lin * (3.0f - 2.0f * lin);
            l = l * g + pl * (1.0f - g);
            r = r * g + pr * (1.0f - g);
        };
        const auto jump = [&] (float& l, float& r)
        {
            if (jumpFade < 1.0f)
            {
                l *= jumpFade;
                r *= jumpFade;
                jumpFade = std::min (1.0f, jumpFade + 1.0f / (float) std::max (1.0, 0.003 * hostRate));
            }
        };
        // Beats slower than the loop fills what is left after each hit with the stretched loop (below).
        // Bungee transposes two octaves either way at most (its buffers are made for that: past it, measured, it
        // wrote out of them); further, Complex goes through Signalsmith and Beats keeps its die-away.
        const bool bungeeRange = pitchRatio > 0.2505 && pitchRatio < 3.99;
        const bool beatsTails = algo == P::LoopBeatsAlgo && warp && tempo < 1.0 - 1.0e-6 && bungeeRange;
        if (plain || (algo != P::LoopComplex && algo != P::LoopComplexPro && algo != P::LoopRubberBand && ! beatsTails))
            releaseStretcher();
        if (plain)
            rubberMix = 0.0f;
        if (plain)
        {
            // Without Warp (or at the loop's own tempo and pitch): one reading, at the pitch's speed.
            const double speed = warp ? natural : step;
            for (int i = 0; i < num; ++i)
            {
                float l, r;
                readBandLimitedWrap (a, b, loopRead, length, speed, loopCoefs, l, r);
                jump (l, r);            // after the Ventana moved: 3 ms in (1 otherwise: nothing)
                bufL[(size_t) i] += l * uArr[(size_t) i];
                bufR[(size_t) i] += r * uArr[(size_t) i];
                loopRead += speed;
                wrap (loopRead);
            }
            loopTime = loopRead;
            loopSlice = -1;
            return true;
        }

        if ((algo == P::LoopComplex || algo == P::LoopComplexPro) && stretcher == nullptr)
            stretcher = engine.acquireStretcher();      // none left: this note plays as Textura
        // Bungee for this block into bgL/bgR (see Complex below): the loop at the time line's place, stretched.
        // limit (in the loop's samples, or -1): Beats's tails, the reading held there (30 ms short of the next
        // hit: the sound just before it, drawn out over the gap; the hit itself, which the stretch would smear
        // into the tail, stays out of its window). Following the time line instead, a slice slowed to half
        // handed over to the middle of its own hit again: false hits in the gaps.
        const auto bungeeBlock = [&] (LoopStretcher& st, double limit, bool restart)
        {
                auto& bg = *st.bungee;
                if (! stretcherPrimed)
                {
                    st.request = {};
                    st.request.position = loopTime / natural;
                    st.request.speed = tempo;
                    st.request.pitch = pitchRatio;
                    st.request.reset = true;
                    bg.preroll (st.request);
                    st.fifoStart = st.fifoCount = 0;
                    st.cacheBegin = st.cacheEnd = 0;
                    st.started = false;
                    st.owed = 0;
                    st.held = false;
                    st.lead = 0.0;
                    st.lastOut = loopTime / natural;
                    st.startAt = loopTime / natural;
                    std::fill (std::begin (st.coefs), std::end (st.coefs), 0.0f);
                    stretcherPrimed = true;
                }
                st.request.speed = tempo;
                st.request.pitch = pitchRatio;
                const int fifoSize = (int) st.fifo[0].size(), ringSize = (int) st.cache[0].size(), ringMask = ringSize - 1;
                for (int guard = 0; st.fifoCount < num + (st.started ? 0 : (int) st.owed) && guard < 64; ++guard)
                {
                    // Starting takes a few grains at once (Bungee's preroll): a budget per block for all the notes
                    // starting together, so that a chord does not run over a block of 64 (one start alone fits).
                    if (! st.started)
                    {
                        if (engine.grainBudget <= 0)
                            break;
                        --engine.grainBudget;
                    }
                    const auto chunk = bg.specifyGrain (st.request);
                    const int frames = chunk.end - chunk.begin;
                    if (frames <= 0 || frames * 2 > (int) st.grain.size())
                        break;
                    if (st.cacheEnd <= st.cacheBegin || chunk.begin < st.cacheBegin || chunk.begin > st.cacheEnd || chunk.end - st.cacheBegin > ringSize)
                        st.cacheBegin = st.cacheEnd = chunk.begin;
                    for (; st.cacheEnd < chunk.end; ++st.cacheEnd)
                    {
                        double x = (double) st.cacheEnd * natural;
                        x -= length * std::floor (x / length);
                        float l, r;
                        if (natural == 1.0)
                        {
                            const int k = std::min (length - 1, (int) x);
                            l = a[k];
                            r = b[k];
                        }
                        else
                            readBandLimitedWrap (a, b, x, length, natural, st.coefs, l, r);
                        st.cache[0][(size_t) (st.cacheEnd & ringMask)] = l;
                        st.cache[1][(size_t) (st.cacheEnd & ringMask)] = r;
                    }
                    for (int i = 0; i < frames; ++i)
                    {
                        st.grain[(size_t) i] = st.cache[0][(size_t) ((chunk.begin + i) & ringMask)];
                        st.grain[(size_t) (frames + i)] = st.cache[1][(size_t) ((chunk.begin + i) & ringMask)];
                    }
                    bg.analyseGrain (st.grain.data(), frames);
                    Bungee::OutputChunk out {};
                    bg.synthesiseGrain (out);
                    bg.next (st.request);
                    st.request.reset = false;
                    if (limit >= 0.0)
                        st.request.position = limit / natural;      // held there: the tail before the next hit
                    const double from = out.request[0]->position, to = out.request[1]->position;
                    for (int f = 0; f < out.frameCount && st.fifoCount < fifoSize; ++f)
                    {
                        const double at = from + (to - from) * f / out.frameCount;
                        if (std::isnan (at))
                            continue;
                        if (! st.started)
                        {
                            if (at < st.startAt - 0.5)
                                continue;       // the preroll: before the note's place in the loop
                            st.started = true;
                        }
                        const int w = (st.fifoStart + st.fifoCount) % fifoSize;
                        st.fifo[0][(size_t) w] = out.data[f];
                        st.fifo[1][(size_t) w] = out.data[f + out.channelStride];
                        ++st.fifoCount;
                        st.lastOut = at;
                    }
                }
                // What the wait owes the time line is dropped, so the loop stays where the host and the key put it.
                const bool wasOwed = st.owed > 0;
                while (st.owed > 0 && st.fifoCount > 0)
                {
                    st.fifoStart = (st.fifoStart + 1) % fifoSize;
                    --st.fifoCount;
                    --st.owed;
                }
                if (wasOwed && st.owed == 0 && st.fifoCount > 0)
                    jumpFade = 0.0f;        // it comes in mid-sound: 3 ms of fade
                for (int i = 0; i < num; ++i)
                {
                    float l = 0.0f, r = 0.0f;
                    if (st.fifoCount == 0)
                        ++st.owed;          // nothing yet: silence now, owed later
                    if (st.fifoCount > 0)
                    {
                        l = st.fifo[0][(size_t) st.fifoStart];
                        r = st.fifo[1][(size_t) st.fifoStart];
                        st.fifoStart = (st.fifoStart + 1) % fifoSize;
                        --st.fifoCount;
                    }
                    bgL[(size_t) i] = l;
                    bgR[(size_t) i] = r;
                }
        };

        // "Rubber Band": the loop stretched by Rubber Band R3 offline, for this tempo and pitch, rendered in the
        // background (R3 in real time ran over blocks of 64: 146 % of a block while playing, 490 % at a start).
        // This voice asks for it and reads it; until it is there (a start, a new key or tempo) Complex plays,
        // and the two cross in 60 ms.
        const RubberRender* rendered = nullptr;
        if (algo == P::LoopRubberBand)
        {
            // Asked for only once the pitch and tempo hold still (8 blocks): while a key glides, or vibrato or a
            // bend moves it, each block's pitch would be a render of its own (offline, each waited for).
            const bool still = std::abs (pitchRatio - rubberAskPitch) < 1.0e-9 && std::abs (tempo - rubberAskTempo) < 1.0e-9;
            rubberStill = still ? rubberStill + 1 : 0;
            rubberAskPitch = pitchRatio;
            rubberAskTempo = tempo;
            if (rubberStill >= 8)
            {
                engine.rubberWantTempo.store (tempo, std::memory_order_relaxed);
                engine.rubberWantPitch.store (pitchRatio, std::memory_order_relaxed);
                engine.rubberWantStart.store (window.start, std::memory_order_relaxed);
                engine.rubberWantLength.store (length, std::memory_order_relaxed);
                engine.rubberWanted.store (true, std::memory_order_release);
            }
            const auto* rr = engine.rubberReady.load (std::memory_order_acquire);
            if (rr != nullptr && rr->sample == &s && rr->loopLength == length && rr->loopStart == window.start && std::abs (rr->tempo - tempo) < 1.0e-6
                && std::abs (rr->pitch - pitchRatio) < 1.0e-6)
                rendered = rr;
            algo = bungeeRange ? P::LoopComplex : P::LoopComplexPro;      // what plays until (or besides) it
        }
        // A new render while the previous one is still fading out waits for it to be out (switching under a
        // part of the gain was a click, measured in REAPER).
        if (rendered != nullptr && lastRendered != nullptr && rendered != lastRendered && rubberMix > 0.0f)
            rendered = nullptr;
        const float rubberFrom = rubberMix, rubberStep = (float) num / (float) std::max (1.0, 0.06 * hostRate);
        // Fading out (a new key or tempo: its render is not there yet), the render it was reading goes on to the
        // end of the fade (the processor frees a replaced render a second later): it does not drop out at once.
        const RubberRender* reading = rendered != nullptr ? rendered : (rubberFrom > 0.0f ? lastRendered : nullptr);
        lastRendered = rendered != nullptr ? rendered : (rubberFrom > 0.0f ? lastRendered : nullptr);
        rubberMix = rendered != nullptr ? std::min (1.0f, rubberMix + rubberStep) : std::max (0.0f, rubberMix - rubberStep);
        const auto rubberBlend = [&] (int i, float& l, float& r)
        {
            if (reading == nullptr)
                return;
            const float m = juce::jlimit (0.0f, 1.0f, rubberFrom + (rubberMix - rubberFrom) * (float) (i + 1) / (float) num);
            float rl = 0.0f, rr = 0.0f;
            {
                double x = loopTime / reading->tempo;
                x -= reading->length * std::floor (x / reading->length);
                readBandLimitedWrap (reading->l.data(), reading->r.data(), x, reading->length, natural, rubberCoefs, rl, rr);
            }
            // Equal power over 60 ms: the two are not in phase (a straight 20 ms crossing dipped in the middle).
            const float gIn = std::sin (m * juce::MathConstants<float>::halfPi), gOut = std::cos (m * juce::MathConstants<float>::halfPi);
            l = l * gOut + rl * gIn;
            r = r * gOut + rr * gIn;
        };
        if (rendered != nullptr && rubberFrom >= 1.0f && rubberMix >= 1.0f)
        {
            releaseStretcher();
            for (int i = 0; i < num; ++i)
            {
                float l = 0.0f, r = 0.0f;
                rubberBlend (i, l, r);
                plainBlend (i, loopTime, l, r);
                jump (l, r);
                bufL[(size_t) i] += l * uArr[(size_t) i];
                bufR[(size_t) i] += r * uArr[(size_t) i];
                loopTime += timeStep;
                wrap (loopTime);
            }
            loopRead = loopTime;
            return true;
        }
        if ((algo == P::LoopComplex || algo == P::LoopComplexPro) && stretcher == nullptr)
            stretcher = engine.acquireStretcher();

        // A stretcher changing hands (Bungee / Signalsmith) starts afresh.
        const bool useBungee = (algo == P::LoopComplex && bungeeRange) || beatsTails;
        if (stretcher != nullptr && useBungee != stretcherBungee)
            stretcherPrimed = false;
        stretcherBungee = useBungee;
        if (algo == P::LoopComplex && bungeeRange && stretcher != nullptr && stretcher->bungee != nullptr)
        {
            // Bungee (measured against Signalsmith, Rubber Band R2/R3 and a harmonic/percussive hybrid on drums,
            // bass, voice, guitar, piano, chords, pad and texture, slower and faster: the closest to the file after
            // stretching there and back, the least pre-echo before hits and timbre change, the least CPU).
            // Its input is the loop at the host's rate (virtual frame j = source sample j * natural, wrapped), so
            // its positions are the time line's: what comes out at frame k is the loop at loopTime.
            bungeeBlock (*stretcher, -1.0, false);
            for (int i = 0; i < num; ++i)
            {
                float l = bgL[(size_t) i], r = bgR[(size_t) i];
                rubberBlend (i, l, r);
                plainBlend (i, loopTime, l, r);
                jump (l, r);
                bufL[(size_t) i] += l * uArr[(size_t) i];
                bufR[(size_t) i] += r * uArr[(size_t) i];
                loopTime += timeStep;
                wrap (loopTime);
            }
            loopRead = loopTime;
            return true;
        }
        if ((algo == P::LoopComplex || algo == P::LoopComplexPro) && stretcher != nullptr)
        {
            auto& st = *stretcher;
            auto& ss = st.stretch;
            // The stretch is fed the loop at the host's rate (read band-limited from the file, as Bungee is), so
            // that it sees only the tempo's stretch and the key's transposition. Fed at the file's rate, a 44.1 kHz
            // loop in a 96 kHz project was a stretch of x2.18 to it at the loop's own tempo: over x2 Signalsmith
            // turns to its random-phase mode (a seed of the system's, so not two renders alike), more diffuse.
            // At the file's own rate it reads the file as before, sample for sample.
            ss.setTransposeFactor ((float) pitchRatio);
            // Pro keeps the formants where they were in Hz.
            ss.setFormantFactor (1.0f, algo == P::LoopComplexPro);
            // Its envelope smoothed over 400 Hz: the pitch guess of a chord is no use, and with it (or 100-200 Hz) a
            // chord moved 5 semitones up came out 5-10 dB quieter. Measured on chords, pad, bass and pluck at
            // +5/-7: the 1-4 kHz band within 3 dB of the original on average (as Complex), 1-3 dB lower level.
            ss.setFormantBase (400.0f / (float) hostRate);
            // from: in frames at the host's rate (frame j is the file's sample j * natural).
            const auto fill = [&] (double from, int count, size_t offset)
            {
                for (int i = 0; i < count; ++i)
                {
                    float l, r;
                    if (natural == 1.0)
                    {
                        int k = (int) std::fmod (from + i, (double) length);
                        if (k < 0)
                            k += length;
                        l = a[k];
                        r = b[k];
                    }
                    else
                    {
                        double x = std::fmod ((from + i) * natural, (double) length);
                        if (x < 0.0)
                            x += length;
                        readBandLimitedWrap (a, b, x, length, natural, st.coefs, l, r);
                    }
                    st.in[0][offset + (size_t) i] = l;
                    st.in[1][offset + (size_t) i] = r;
                }
            };
            const double framesPer = timeStep / natural;     // input frames per output sample: the tempo
            const double framesInLoop = length / natural;
            if (! stretcherPrimed)
            {
                // Fed ahead of the loop's time by what the stretch holds back, so that what comes out now is
                // the loop at loopTime.
                ss.reset();
                const int seekLength = ss.seekLength();
                stretcherFeed = loopTime / natural + ss.inputLatency() + ss.outputLatency() * framesPer;
                fill (stretcherFeed - seekLength, seekLength, 0);
                const float* in[2] = { st.in[0].data(), st.in[1].data() };
                ss.seek (in, seekLength, framesPer);
                stretcherPrimed = true;
                stretcherAge = 0;
                stretcherCarry = 0.0;
            }
            stretcherCarry += num * framesPer;
            const int count = std::min ((int) st.in[0].size(), (int) stretcherCarry);
            stretcherCarry -= count;
            fill (stretcherFeed, count, 0);
            stretcherFeed = std::fmod (stretcherFeed + count, framesInLoop);
            const float* in[2] = { st.in[0].data(), st.in[1].data() };
            float* out[2] = { st.out[0].data(), st.out[1].data() };
            ss.process (in, count, out, num);

            // Until the stretch has output, the Textura reader; then 10 ms of crossfade.
            const int ready = ss.outputLatency(), cross = std::max (1, (int) (0.01 * hostRate));
            const auto src = warpSourceOf (buf, length, true);
            const int textureLen = std::max (16, (int) (0.06 * hostRate));
            const float power = 1.0f - std::exp (-1.0f / (0.02f * (float) hostRate));
            for (int i = 0; i < num; ++i)
            {
                const int age = stretcherAge + i;
                const float mix = age < ready ? 0.0f : std::min (1.0f, (float) (age - ready) / (float) cross);
                float l = st.out[0][(size_t) i] * mix, r = st.out[1][(size_t) i] * mix;
                if (mix < 1.0f)
                {
                    float warped[WarpReader::Source::maxChannels] {};
                    loopWarp.tick (src, loopTime, timeStep, pitchRatio / tempo, false, nullptr, 0.0, textureLen, power, warped);
                    l += warped[0] * (1.0f - mix);
                    r += warped[1] * (1.0f - mix);
                }
                rubberBlend (i, l, r);
                plainBlend (i, loopTime, l, r);
                jump (l, r);
                bufL[(size_t) i] += l * uArr[(size_t) i];
                bufR[(size_t) i] += r * uArr[(size_t) i];
                loopTime += timeStep;
                wrap (loopTime);
            }
            stretcherAge += num;
            loopRead = loopTime;
            return true;
        }
        if (algo != P::LoopBeatsAlgo)       // Complex before its stretchers exist (the moment LOOP is chosen): grains
        {
            const auto src = warpSourceOf (buf, length, true);
            float warped[WarpReader::Source::maxChannels] {};
            // Grains of 60 ms; moved down, at least four cycles of a low note: a bass read in grains of two
            // cycles beat against itself (measured: 36 clicks in 12 s at -3 st, 0 with this). Moved up, the
            // longer grains did worse (62 against 1), so they stay at 60 ms.
            const double cycles4 = s.pitched && pitchRatio < 0.999 ? 4.0 * hostRate / (440.0 * std::exp2 ((s.rootMidi - 69.0) / 12.0)) : 0.0;
            const int textureLen = std::max (16, (int) std::max (0.06 * hostRate, std::min (0.2 * hostRate, cycles4)));
            const float power = 1.0f - std::exp (-1.0f / (0.02f * (float) hostRate));
            for (int i = 0; i < num; ++i)
            {
                loopWarp.tick (src, loopTime, timeStep, pitchRatio / tempo, false, nullptr, 0.0, textureLen, power, warped);
                plainBlend (i, loopTime, warped[0], warped[1]);
                jump (warped[0], warped[1]);
                bufL[(size_t) i] += warped[0] * uArr[(size_t) i];
                bufR[(size_t) i] += warped[1] * uArr[(size_t) i];
                loopTime += timeStep;
                wrap (loopTime);
            }
            loopRead = loopTime;
            return true;
        }

        // Beats. Each hit is the file as it is, from where it falls in the time line. Slower than the loop, a
        // hit's slice ends before the next hit comes: the gap was a die-away into silence, and the sample's own
        // reverb and room dipped between hits (heard as level jumps, measured: 9 % of the time 12 dB down at
        // x0.82). Now the slice hands over to the loop stretched by Bungee at that same place, which carries the
        // tail to the next hit, and steps aside 3 ms before it: hits untouched, tails continuous.
        bool tails = false;
        if (beatsTails)
        {
            if (stretcher == nullptr)
                stretcher = engine.acquireStretcher();
            if (stretcher != nullptr && stretcher->bungee != nullptr)
            {
                // Held 60 ms short of the next hit (Bungee's window reaches some 40 ms ahead; at 30 ms the tail came out
                // 7 dB over the file's, part of the hit in it): the tail is what
                // was there just before the hit, stretched over the gap.
                const auto& beatCuts = loopCuts;
                const int* cuts = beatCuts.empty() ? nullptr : beatCuts.data();
                const int count = (int) beatCuts.size();
                int k = 0;
                if (cuts != nullptr)
                    k = juce::jlimit (0, count - 1, (int) (std::upper_bound (cuts, cuts + count, (int) loopTime) - cuts) - 1);
                const double nextHit = cuts != nullptr && k + 1 < count ? cuts[k + 1] : length;
                const bool restart = k != tailSlice;
                tailSlice = k;
                bungeeBlock (*stretcher, std::max ((cuts != nullptr ? cuts[k] : 0) + 0.0, nextHit - 0.06 * s.sampleRate), restart);
                tails = true;
            }
        }
        static constexpr int whole[1] { 0 };       // a plain array: nothing made on the audio thread
        const auto& beatCuts = loopCuts;
        const int* slices = beatCuts.empty() ? whole : beatCuts.data();
        const int numSlices = beatCuts.empty() ? 1 : (int) beatCuts.size();
        const float fadeOut = (float) std::max (1.0, 0.004 * hostRate), rise = (float) std::max (1.0, 0.001 * hostRate);
        for (int i = 0; i < num; ++i)
        {
            const int k = juce::jlimit (0, numSlices - 1, (int) (std::upper_bound (slices, slices + numSlices, (int) loopTime) - slices) - 1);
            if (k != loopSlice)
            {
                loopFadeRead = loopRead;
                loopFadeEnd = loopEnd;
                loopFadeGain = loopSlice >= 0 ? loopSliceGain : 0.0f;
                loopSlice = k;
                loopRead = slices[k] - rise * step;
                loopRise = 0.0f;
            }
            const double end = k + 1 < numSlices ? slices[k + 1] : length;
            loopEnd = end;
            const float left = (float) ((end - loopRead) / step);
            loopRise = std::min (1.0f, loopRise + 1.0f / rise);
            // A slice that runs out before the next hit comes (slower than the loop, or a key up) dies away
            // over up to 15 ms, a third of the slice at most: over 4 ms it was heard as a tick.
            const float sliceLength = (float) ((end - slices[k]) / step);
            const float dieAway = std::max (fadeOut, std::min ((float) (0.015 * hostRate), sliceLength / 3.0f));
            loopSliceGain = std::min (loopRise, juce::jlimit (0.0f, 1.0f, left / dieAway));
            float l = 0.0f, r = 0.0f;
            if (loopSliceGain > 0.0f)
            {
                readBandLimitedWrap (a, b, loopRead, length, step, loopCoefs, l, r);
                l *= loopSliceGain;
                r *= loopSliceGain;
            }
            if (loopFadeGain > 0.0f)
            {
                // The slice cut fades out, and never reads into the next hit (already playing).
                const float g = std::min (loopFadeGain, juce::jlimit (0.0f, 1.0f, (float) ((loopFadeEnd - loopFadeRead) / step) / fadeOut));
                if (g > 0.0f)
                {
                    float fl, fr;
                    readBandLimitedWrap (a, b, loopFadeRead, length, step, loopCoefs, fl, fr);
                    l += fl * g;
                    r += fr * g;
                }
                loopFadeGain = std::max (0.0f, loopFadeGain - 1.0f / fadeOut);
                loopFadeRead += step;
            }
            if (tails)
            {
                // The stretched loop where the slice is not: its weight is what the slice (and the cut slice
                // fading out) leave, and it is gone 3 ms before the next hit (its smeared copy of the hit stays out).
                const double toHit = (end - loopTime) / timeStep;
                const float beforeHit = (float) juce::jlimit (0.0, 1.0, (toHit - 0.0005 * hostRate) / (0.003 * hostRate));
                const float held = std::min (1.0f, loopSliceGain + loopFadeGain);
                const float w = std::max (0.0f, 1.0f - held) * beforeHit;
                l += bgL[(size_t) i] * w;
                r += bgR[(size_t) i] * w;
            }
            plainBlend (i, loopTime, l, r);
            jump (l, r);
            bufL[(size_t) i] += l * uArr[(size_t) i];
            bufR[(size_t) i] += r * uArr[(size_t) i];
            loopRead += step;           // not wrapped: past its slice's end a slice is silent (the read wraps)
            loopTime += timeStep;
            wrap (loopTime);
        }
        return true;
    }

    //--------------------------------------------------------------------------
    void postProcess (float* outL, float* outR, int num, const SampleData& s, const ParamSnapshot& p, bool resynth,
                      float bow, float relPhase)
    {
        // Darkening for the sampled engines (engine C does it per partial).
        if (! resynth)
        {
            // What a real instrument does to its highs. The held part only darkens where Reanima
            // makes the sustain itself: a recorded sample already has its own.
            const float ratio = std::max (1.0e-3f, std::min (1.0f, lanes[0].ratio));
            const float darkOct = variationDark
                                + bow * 3.0f * -std::log2 (ratio)
                                + p[P::RelDark] * (2.5f * synthetic * (1.0f - std::exp (-(float) t / 2.0f)) + 6.0f * relPhase)
                                + p[P::VelBright] * 5.0f * (1.0f - std::min (1.0f, velocity / 0.8f))
                                - 2.5f * pressing + std::max (0.0f, -timbreOct()) * 2.7f;    // MPE timbre below its middle: up to 4 octaves darker
            // Por golpe in a loop: the same, hit by hit. Arco while each hit rises through Ataque (as a slowed
            // attack does to a note), Apagado as each falls towards Sustain (up to 2.5 octaves at Sustain, as the
            // held part of a note), Brillo by each hit's strength (a weak one as a soft key).
            const float perHitDark = perHitTimbre ? bow * 3.0f * -std::log2 (hitAttackLevel) + p[P::RelDark] * 2.5f * hitFall
                                                        + p[P::VelBright] * 5.0f * (1.0f - std::min (1.0f, hitStrength / 0.8f))
                                                  : 0.0f;
            tilt.set (20000.0f * std::exp2 (-std::max (0.0f, darkOct + perHitDark)), hostRate);
        }
        else
            tilt.active = false;

        const bool filterOn = p[P::FltOn] > 0.5f;
        if (filterOn)
        {
            const float semis = p[P::FltKey] * (pitch - 60.0f)
                              + p[P::FltEnvAmt] * 60.0f * e2Level
                              + p[P::FltFollow] * 48.0f * std::min (1.0f, follower * 2.0f)
                              + mod[P::DstCutoff] * 60.0f + 8.0f * timbreOct();
            filter.set ((int) p[P::FltType], (int) p[P::FltSlope], (int) p[P::FltCircuit],
                        p[P::FltFreq] * std::exp2 (semis / 12.0f),
                        p[P::FltRes] + mod[P::DstRes], p[P::FltDrive] + 24.0f * mod[P::DstDrive],
                        p[P::FltMorph] + mod[P::DstMorph], p[P::FltMix]);
            // Armónico: on the harmonics of the note that sounds now (bend, vibrato, glide and pitch modulation
            // included), so that they stay on the sound's own as it moves.
            if ((int) p[P::FltType] == P::Harmonic)
            {
                const float midi = soundingNote (s, p) + engine.pitchBend * p[P::BendRange] + mpeBend() + vibrato + pitchEnv
                                 + mod[P::DstPitch] * 24.0f + mod[P::DstFine];
                filter.setHarmonic (440.0f * std::exp2 ((midi - 69.0f) / 12.0f), p[P::FltHarmGain] + 24.0f * mod[P::DstDrive],
                                    p[P::FltHarmRegion]);
            }
        }

        const float folAtk = std::exp (-1.0f / (p[P::FltFolAtk] * 0.001f * (float) hostRate));
        const float folRel = std::exp (-1.0f / (p[P::FltFolRel] * 0.001f * (float) hostRate));
        const float downsample = p[P::Downsample] * (float) (hostRate / 44100.0);    // same grit at any rate
        const bool crushRate = p[P::Downsample] > 1.001f;
        const int bits = juce::roundToInt (p[P::Bits]);
        const float quant = std::exp2 ((float) bits - 1.0f) / std::max (1.0e-3f, s.peakRms);   // relative to the sample: a quiet one is not erased

        const float velGain = 1.0f - p[P::VelAmp] * (1.0f - velocity);
        const float sampleGain = dbToGain (p[P::SampleGain]) / (p[P::Normalize] > 0.5f ? s.peakLevel : 1.0f);
        const float voiceGain = velGain * variationGain * sampleGain * dbToGain (24.0f * mod[P::DstVolume] + 4.0f * pressing);
        float targetL, targetR;
        Dsp::panGains (juce::jlimit (-1.0f, 1.0f, mod[P::DstPan]), targetL, targetR);
        targetL *= voiceGain;
        targetR *= voiceGain;
        const float tiltTarget = tilt.active ? 1.0f : 0.0f;
        if (! gainsReady)
        {
            gainL = targetL;
            gainR = targetR;
            tiltMix = tiltTarget;
            gainsReady = true;
        }
        // The voice's gain follows its target over some 3 ms (volume modulation, a gate, pressure): a change
        // made at once, or over one 32-sample block, clicks.
        const float gainCoef = 1.0f - std::exp (-1.0f / (0.003f * (float) hostRate));
        const float tiltCoef = 1.0f - std::exp (-1.0f / (0.005f * (float) hostRate));

        // The mechanism of the instrument: at the key going down (in start) and coming back up.
        if (keyUpPending)
        {
            mech.keyUp ((int) p[P::MechType], SynthEngine::mechanismAmount (p), s, mechNote (s, p, mechStart));
            keyUpPending = false;
        }
        // Mecanismo por golpe: a key strike at each hit of the loop (Por golpe), as strong as the hit.
        if (hitStrikePending)
        {
            hitStrikePending = false;
            if (perHitTimbre && p[P::MechPerHit] > 0.5f && SynthEngine::mechanismAmount (p) > 0.0f)
                mech.keyDown ((int) p[P::MechType], SynthEngine::mechanismAmount (p) * std::max (0.25f, hitStrength), s, mechNote (s, p, loopRead + loopWinStart));
        }
        float toneEnergy = 0.0f;
        for (int i = 0; i < num; ++i)
            toneEnergy += bufL[(size_t) i] * bufL[(size_t) i] + bufR[(size_t) i] * bufR[(size_t) i];
        // Listening apart (SynthEngine::listen): Mecanismo goes on in time either way, so that switching
        // back joins it where it is; what is not to be heard is taken out after it.
        const int listen = engine.listen.load (std::memory_order_relaxed);
        if (listen == SynthEngine::ListenSample)
        {
            std::copy (bufL.begin(), bufL.begin() + num, keepL.begin());
            std::copy (bufR.begin(), bufR.begin() + num, keepR.begin());
        }
        else if (listen == SynthEngine::ListenMechanism)
        {
            std::fill (keepL.begin(), keepL.begin() + num, 0.0f);
            std::fill (keepR.begin(), keepR.begin() + num, 0.0f);
            std::swap (bufL, keepL);
            std::swap (bufR, keepR);
        }
        mech.process (bufL.data(), bufR.data(), num, s, std::sqrt (toneEnergy / (float) (2 * std::max (1, num))));
        if (listen == SynthEngine::ListenSample)
        {
            std::copy (keepL.begin(), keepL.begin() + num, bufL.begin());
            std::copy (keepR.begin(), keepR.begin() + num, bufR.begin());
        }

        for (int i = 0; i < num; ++i)
        {
            float l = bufL[(size_t) i], r = bufR[(size_t) i];

            if (crushRate)
            {
                if (holdCount <= 0.0f)
                {
                    holdL = l;
                    holdR = r;
                    holdCount += std::max (1.0f, downsample);
                }
                holdCount -= 1.0f;
                l = holdL;
                r = holdR;
            }

            if (bits < 24)
            {
                l = std::round (l * quant) / quant;
                r = std::round (r * quant) / quant;
            }

            // Keeps running while bypassed and blends in and out, so that it never clicks.
            {
                const float tl = tilt.process (0, l), tr = tilt.process (1, r);
                tiltMix += (tiltTarget - tiltMix) * tiltCoef;
                if (tiltMix < 1.0e-4f && tiltTarget == 0.0f)
                    tiltMix = 0.0f;
                l += (tl - l) * tiltMix;
                r += (tr - r) * tiltMix;
            }

            const float level = (std::abs (l) + std::abs (r)) * 0.5f;
            follower = level + (follower - level) * (level > follower ? folAtk : folRel);

            if (filterOn)
                filter.process (l, r);

            if (fading)
                fadeGain = std::max (0.0f, fadeGain - fadeStep);

            float g = fadeGain;
            if (startFade < 1.0f)
            {
                startFade = std::min (1.0f, startFade + startFadeStep);
                g *= startFade * startFade * (3.0f - 2.0f * startFade);
            }

            gainL += (targetL - gainL) * gainCoef;
            gainR += (targetR - gainR) * gainCoef;
            outL[i] += l * g * gainL;
            outR[i] += r * g * gainR;
        }
    }

    void computeLaneGains (const ParamSnapshot& p)
    {
        const float width = p[P::UniWidth];
        const float blend = p[P::UniBlend];
        const float centreSpan = numLanes > 1 ? 1.0f / (float) (numLanes - 1) + 1.0e-4f : 2.0f;
        float sum = 0.0f, sumSq = 0.0f;

        for (int i = 0; i < numLanes; ++i)
        {
            const float w = std::abs (lanes[(size_t) i].offset) <= centreSpan ? 1.0f : blend * 1.5f;
            sum += w;
            sumSq += w * w;
        }

        // Independent lanes add up in power, lanes in tune in amplitude: Unison keeps the level.
        const float apart = laneIndependence (p);
        const float norm = (1.0f - apart) / sum + apart / std::sqrt (sumSq);

        for (int i = 0; i < numLanes; ++i)
        {
            auto& ln = lanes[(size_t) i];
            const float w = (std::abs (ln.offset) <= centreSpan ? 1.0f : blend * 1.5f) * norm;
            Dsp::panGains (ln.offset * width, ln.gainL, ln.gainR);
            ln.gainL *= w;
            ln.gainR *= w;
        }
    }

    void updateModulation (const ParamSnapshot& p, double blockSeconds)
    {
        for (int l = 0; l < P::numLfos; ++l)
        {
            const int base = P::Lfo1Rate + l * P::lfoStride;
            const int shape = (int) p[(size_t) base + 3];

            const float rateMod = l == 0 ? mod[P::DstLfo1Rate] : (l == 1 ? mod[P::DstLfo2Rate] : 0.0f);
            const double rate = lfoRateHz (p, l, engine.bpm) * std::exp2 (4.0f * rateMod);
            const float smooth = p[(size_t) base + 5];
            auto& dr = drift[(size_t) l];
            const float raw = shape == P::Drift ? dr.value (lfoPhase[(size_t) l], smooth)
                                                : lfoValue (shape, lfoPhase[(size_t) l], lfoHold[(size_t) l]);
            // Suavidad on the other shapes: a glide towards the wave, up to half a cycle long (corners and steps
            // rounded off). At 0 the wave is as it always was.
            if (shape != P::Drift && smooth > 0.0f && ! lfoFresh[(size_t) l] && blockSeconds > 0.0)
            {
                const double tau = (double) (smooth * smooth) * 0.5 / std::max (1.0e-3, rate);
                const float a = (float) (1.0 - std::exp (-blockSeconds / tau));
                lfoOut[(size_t) l] += (raw - lfoOut[(size_t) l]) * a;
            }
            else
                lfoOut[(size_t) l] = raw;
            lfoFresh[(size_t) l] = false;
            lfoPhase[(size_t) l] += rate * blockSeconds;
            if (lfoPhase[(size_t) l] >= 1.0)
            {
                lfoPhase[(size_t) l] -= Dsp::floorFast (lfoPhase[(size_t) l]);
                lfoHold[(size_t) l] = rng.nextFloat() * 2.0f - 1.0f;
                if (shape == P::Drift)
                    dr.nextCycle();
            }
            if (shape == P::Drift)
                dr.advance (rate * blockSeconds);
        }

        const float sources[P::NumSources] = {
            0.0f, lfoOut[0], lfoOut[1], lfoOut[2], e2Level, ampLevel, velocity, ((float) note - 60.0f) / 60.0f,
            engine.modWheel, std::max ({ engine.aftertouch, pressure, mpePressure() }), p[P::Macro1], p[P::Macro2], p[P::Macro3], p[P::Macro4], randomValue,
            engine.sideValue };

        mod.fill (0.0f);
        for (int slot = 0; slot < P::numSlots; ++slot)
        {
            const int base = P::M1Src + slot * P::slotStride;
            const int src = (int) p[(size_t) base];
            const int dst = (int) p[(size_t) base + 1];
            if (src > 0 && src < P::NumSources && dst > 0 && dst < P::NumDests)
                mod[(size_t) dst] += sources[src] * p[(size_t) base + 2];
        }
    }

    SynthEngine& engine;
    juce::Random rng;
    double hostRate = 48000.0;

    bool active = false, released = false, fading = false;
    int note = 60;
    float velocity = 1.0f;
    juce::uint32 order = 0;
    float pitch = 60.0f, targetPitch = 60.0f;
    double t = 0.0, tRel = 0.0;
    double skippedSeconds = 0.0;            // how far past the onset the note started
    float relLevel = 0, relGain = 0, relSlope = 0, ampLevel = 0, envelopeLevel = 0;
    float offOriginal = 1, offBright = 0, offDark = 0, offEnding = 0, heldEnding = 0;   // the mix at the note-off
    float fadeGain = 1, fadeStep = 0, startFade = 1, startFadeStep = 0;
    float gainL = 1, gainR = 1;
    bool gainsReady = false;
    int e2Stage = 0;
    float e2Level = 0;
    std::array<double, P::numLfos> lfoPhase {};
    std::array<float, P::numLfos> lfoHold {}, lfoOut {};
    std::array<DriftLfo, P::numLfos> drift {};
    std::array<bool, P::numLfos> lfoFresh {};
    float randomValue = 0, variationCents = 0, variationGain = 1, variationDark = 0;
    std::array<float, P::NumDests> mod {};

    std::array<Lane, 16> lanes;
    int numLanes = 1;
    const SampleData::SustainSource* sustainSource = nullptr;

    std::array<float, subBlock> uArr {}, relArr {}, punchArr {}, texGain {}, wTrans {}, bufL {}, bufR {}, laneL {}, laneR {};
    std::array<float, subBlock> keepL {}, keepR {};         // listening apart (SynthEngine::listen)
    std::array<float, subBlock> srcL {}, srcR {};           // Movimiento: the sustain source's part (B and D)
    MotionEq motion;
    std::array<float, subBlock> gOriginal {}, gBright {}, gDark {};
    alignas (16) float sincCoefs[SincTable::maxTaps] {};
    float heldOriginal = 1.0f, heldBright = 0.0f, heldDark = 0.0f;
    std::array<float, maxPartials> brightness {}, partialPanL {}, partialPanR {};
    std::array<float, SampleData::PartialModel::numBands> bandAmps {}, bandBrightness {};
    NoiseSynth noise;
    PartialBank bank;                           // engine C: the partials sounding in this block

    // LOOP: the time along the loop (warped), the slice playing and the one cut, fading (Beats).
    double loopTime = 0.0, loopRead = 0.0, loopFadeRead = 0.0, loopEnd = 0.0, loopFadeEnd = 0.0;
    LoopStretcher* stretcher = nullptr;
    bool stretcherPrimed = false, slicing = false, looping = false, transportTuned = false;
    float stretchMix = 0.0f, jumpFade = 1.0f;
    double plainPos = 0.0;
    // Por golpe: the time since the last hit, the level the envelope started that hit from, the loop's reading
    // speed (to find the hits ahead in the block), and where in the block they fall.
    bool sliceGate = false;
    double hitT = 0.0, lastTimeStep = 0.0;
    float hitFrom = 0.0f;
    // Por golpe in TIMBRE: this block's hit (its strength, 0..1), where its envelope is, and Mecanismo's strike due.
    bool perHitTimbre = false, hitStrikePending = false;
    // Ventana: where the part that plays starts and how long it is (0 before the first block of a note), and its
    // cuts counted from its start (room made once).
    int loopWinStart = 0, loopWinLength = 0;
    float windowOut = 0.0f;         // how far the old part has faded out (1: out)
    std::vector<int> windowCuts;
    float hitStrength = 1.0f, hitAttackLevel = 1.0f, hitFall = 0.0f;

    // How strong the hit at `at` (samples of the loop) is, as a key's velocity: its peak over its first 15 ms
    // against that of the loop's strong hits, 0..1 (ReanimaProcessor::hitPeak, the same measure).
    float strengthOfHit (const SampleData& s, int at) const noexcept
    {
        const float ref = std::max (1.0e-6f, engine.hitStrengthRef.load (std::memory_order_relaxed));
        return juce::jlimit (0.0f, 1.0f, SynthEngine::hitPeak (s, at) / ref);
    }
    std::array<bool, subBlock> hitArr {};
    std::array<float, subBlock> bgL {}, bgR {};      // Bungee's block (Complex, and Beats's tails)
    int tailSlice = -1;
    float rubberMix = 0.0f;
    const RubberRender* lastRendered = nullptr;
    double rubberAskPitch = 0.0, rubberAskTempo = 0.0;
    int rubberStill = 0;
    alignas (16) float rubberCoefs[128] {};
    bool stretcherBungee = false;
    int sliceIndex = -1;
    double stretcherFeed = 0.0, stretcherCarry = 0.0, sliceFrom = 0.0, sliceTo = 0.0;
    int stretcherAge = 0;
    float loopFadeGain = 0.0f, loopSliceGain = 1.0f, loopRise = 1.0f;
    int loopSlice = -1;
    WarpReader loopWarp;
    alignas (16) float loopCoefs[SincTable::maxTaps] {};

    VoiceFilter filter;
    TiltFilter tilt;
    float tiltMix = 0.0f;
    Mechanism mech;
    double mechStart = 0.0;
    bool keyUpPending = false;
    Drone::Stream* drone = nullptr;
    float pressing = 0.0f, vibrato = 0.0f, pitchEnv = 0.0f;     // expression now; vibrato and pitch envelope in semitones
    double vibPhase = 0.0;
    float synthetic = 0.0f;                     // share of the sound that Reanima makes up, 0 to 1
    float follower = 0;
    float holdL = 0, holdR = 0, holdCount = 0;
};

//==============================================================================
SynthEngine::SynthEngine()
{
    for (int i = 0; i < physicalVoices; ++i)
        voices.push_back (std::make_unique<Voice> (*this, i));
    chanBendRange.fill (48.0f);     // MPE's default for the note channels
    chanTimbre.fill (0.5f);
    fx = std::make_unique<FxRack>();
    pedalNoise = std::make_unique<Mechanism>();
    pedalNoise->seed (0x9edd);
    heldNotes.reserve (128);
    beatCutsLive.reserve ((size_t) maxBeatCuts);
    for (auto& ps : playSlices)
        ps.store (-1, std::memory_order_relaxed);
    for (auto& ph : playheads)
        ph.store (-1.0f);
}

SynthEngine::~SynthEngine() = default;

void SynthEngine::prepare (double sr)
{
    // Tables built on first use must not be built on the audio thread.
    hann();
    sinc();
    Dsp::sine();
    NoiseSynth::window();
    NoiseSynth::fft();
    MechanismLibrary::get();

    sampleRate = sr;
    // The stretchers are rebuilt here (the audio is stopped) when the rate changes, if LOOP has asked for them.
    if (stretchersReady.load (std::memory_order_acquire) && std::abs (stretcherRate - sr) > 0.5)
    {
        stretchersReady.store (false, std::memory_order_release);
        for (auto& v : voices)
            v->stopImmediately();       // gives its stretcher back before the old ones go
        buildStretchers (sr);
    }
    pedalNoise->prepare (sr);
    for (auto& v : voices)
        v->prepare (sr);
    fx->prepare (sr);
    reset();
}

// Some 2 MB each, 40 of them: made only once LOOP is chosen (prepareFx, message thread, or prepare), and the
// audio thread takes them only after stretchersReady says they are all there.
void SynthEngine::buildStretchers (double sr)
{
    stretchers.clear();
    for (int i = 0; i < numStretchers; ++i)
    {
        auto st = std::make_unique<LoopStretcher>();
        st->stretch.presetDefault (2, (float) sr, true);        // the work spread over the blocks
        const size_t room = (size_t) st->stretch.blockSamples() + (size_t) st->stretch.intervalSamples() + 4096;
        for (auto& v : st->in)
            v.assign (room, 0.0f);
        for (auto& v : st->out)
            v.assign (4096, 0.0f);
        st->bungee = std::make_unique<Bungee::Stretcher<Bungee::Basic>> (Bungee::SampleRates { (int) std::lround (sr), (int) std::lround (sr) }, 2);
        const int maxIn = st->bungee->maxInputFrameCount();
        st->grain.assign ((size_t) (2 * maxIn), 0.0f);
        int ring = 1;
        while (ring < 2 * maxIn + 8192)
            ring <<= 1;
        for (auto& v : st->cache)
            v.assign ((size_t) ring, 0.0f);
        for (auto& v : st->fifo)
            v.assign (16384, 0.0f);
        stretchers.push_back (std::move (st));
    }
    stretcherRate = sr;
    stretchersReady.store (true, std::memory_order_release);
}

LoopStretcher* SynthEngine::acquireStretcher() noexcept
{
    if (! stretchersReady.load (std::memory_order_acquire))
        return nullptr;
    for (auto& st : stretchers)
        if (! st->inUse)
        {
            st->inUse = true;
            return st.get();
        }
    return nullptr;
}

void SynthEngine::setSlices (const std::vector<int>& cuts, const std::vector<int>& order) noexcept
{
    const int n = std::min (maxSlices, (int) cuts.size());
    slicesUsed.store (0, std::memory_order_release);
    for (int i = 0; i < n; ++i)
    {
        const int k = i < (int) order.size() && order[(size_t) i] >= 0 && order[(size_t) i] < n ? order[(size_t) i] : i;
        cutPoints[(size_t) i].store (cuts[(size_t) i], std::memory_order_relaxed);
        sliceStarts[(size_t) i].store (cuts[(size_t) k], std::memory_order_relaxed);
        sliceEnds[(size_t) i].store (k + 1 < n ? cuts[(size_t) k + 1] : -1, std::memory_order_relaxed);
        sliceOriginal[(size_t) i].store (k, std::memory_order_relaxed);
    }
    slicesUsed.store (n, std::memory_order_release);
}

void SynthEngine::setSlices (const std::vector<int>& starts) noexcept
{
    setSlices (starts, {});
}

void SynthEngine::setBeatCuts (const SampleData* owner, const std::vector<int>& cuts) noexcept
{
    const unsigned seq = beatCutsSeq.load (std::memory_order_relaxed);
    beatCutsSeq.store (seq + 1, std::memory_order_release);           // odd: being written
    std::atomic_thread_fence (std::memory_order_release);
    const int n = (int) std::min (cuts.size(), (size_t) maxBeatCuts);
    for (int i = 0; i < n; ++i)
        beatCutsShared[(size_t) i].store (cuts[(size_t) i], std::memory_order_relaxed);
    // More than there is room for: the analysis's own cuts rather than a list cut short.
    beatCutsCount.store (cuts.size() > (size_t) maxBeatCuts ? 0 : n, std::memory_order_relaxed);
    beatCutsOwner.store (owner, std::memory_order_relaxed);
    beatCutsSeq.store (seq + 2, std::memory_order_release);
}

void SynthEngine::takeBeatCuts() noexcept
{
    const unsigned seq = beatCutsSeq.load (std::memory_order_acquire);
    if (seq == beatCutsLiveSeq || (seq & 1u) != 0)
        return;
    const int n = std::min (beatCutsCount.load (std::memory_order_relaxed), maxBeatCuts);
    beatCutsLive.resize ((size_t) n);           // within the room made in the constructor
    for (int i = 0; i < n; ++i)
        beatCutsLive[(size_t) i] = beatCutsShared[(size_t) i].load (std::memory_order_relaxed);
    const SampleData* owner = beatCutsOwner.load (std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_acquire);
    if (beatCutsSeq.load (std::memory_order_relaxed) != seq)
        return;                                 // written meanwhile: taken next block
    beatCutsLiveOwner = owner;
    beatCutsLiveSeq = seq;
}

const std::vector<int>& SynthEngine::beatSlices (const SampleData& s) const noexcept
{
    return beatCutsLiveOwner == &s && ! beatCutsLive.empty() ? beatCutsLive : s.loopInfo.slices;
}

void SynthEngine::prepareFx (const ParamSnapshot& p)
{
    fx->ensureModules (p);

    if ((int) p[P::PlayMode] == P::PlayLoop && ! stretchersReady.load (std::memory_order_acquire))
        buildStretchers (sampleRate);

    if ((int) p[P::Engine] == P::EngineDrone && ! dronesReady.load())
    {
        droneShared = std::make_unique<Drone::Shared>();
        for (int i = 0; i < numDroneStreams; ++i)
            droneStreams.push_back (std::make_unique<Drone::Stream>());
        dronesReady.store (true, std::memory_order_release);
    }
}

void SynthEngine::setDroneCurve (const float* gainsDb) noexcept
{
    std::copy (gainsDb, gainsDb + Drone::curvePoints, droneCurve.begin());
}

Drone::Stream* SynthEngine::acquireDrone() noexcept
{
    if (! dronesReady.load (std::memory_order_acquire))
        return nullptr;
    for (auto& stream : droneStreams)
    {
        if (! stream->inUse)
        {
            stream->inUse = true;
            return stream.get();
        }
    }
    return nullptr;
}

void SynthEngine::reset()
{
    for (auto& v : voices)
        v->stopImmediately();
    fx->reset();
    heldNotes.clear();
    pedalDown = false;
    sideEnv = sideHighEnv = sideValue = 0.0f;
    sideResponse = -1;          // its filters set again, at the current rate
    pedalNoise->reset();
    limiterGain = 1.0f;
    masterGainsReady = false;
}

std::shared_ptr<const SampleData> SynthEngine::setSample (std::shared_ptr<const SampleData> newSample)
{
    if (newSample == sample)
        return {};
    for (auto& v : voices)
    {
        v->stopImmediately();
        v->onPrevious = false;
    }
    previous.reset();               // the caller still holds it (previousInUse): not freed here
    previousInUse.store (false, std::memory_order_release);
    std::swap (sample, newSample);
    return newSample;
}

std::shared_ptr<const SampleData> SynthEngine::upgradeSample (std::shared_ptr<const SampleData> newSample)
{
    if (newSample == sample)
        return {};
    if (previous != nullptr || sample == nullptr)
        return setSample (std::move (newSample));
    bool any = false;
    for (auto& v : voices)
        if (v->isActive())
        {
            v->onPrevious = true;
            any = true;
        }
    if (any)
    {
        previous = sample;
        previousInUse.store (true, std::memory_order_release);
    }
    std::swap (sample, newSample);
    return newSample;
}

const SampleData& SynthEngine::sampleFor (const Voice& v) const noexcept
{
    return v.onPrevious && previous != nullptr ? *previous : *sample;
}

int SynthEngine::destOf (int id) noexcept
{
    switch (id)
    {
        case P::FltFreq:        return P::DstCutoff;
        case P::FltRes:         return P::DstRes;
        case P::FltDrive:       return P::DstDrive;
        case P::FltMorph:       return P::DstMorph;
        case P::Volume:         return P::DstVolume;
        case P::Pan:            return P::DstPan;
        case P::StartPos:
        case P::GrainPos:       return P::DstStart;
        case P::Spray:          return P::DstSpray;
        case P::GrainMs:        return P::DstGrain;
        case P::UniDetune:      return P::DstDetune;
        case P::Transpose:      return P::DstPitch;
        case P::Fine:           return P::DstFine;
        case P::FltHarmGain:    return P::DstDrive;         // Armónico: the matrix's Drive moves Realce
        case P::Lfo1Rate:       return P::DstLfo1Rate;
        case P::Lfo2Rate:       return P::DstLfo2Rate;
        case P::TransientGain:  return P::DstTransient;
        case P::Bow:            return P::DstBow;
        case P::Breath:         return P::DstBreath;
        default:                return P::DstNone;
    }
}

// As the voices apply each destination (renderVoice, postProcess, updateModulation).
float SynthEngine::modulated (int id, float v, float m) noexcept
{
    const auto& d = P::defs()[(size_t) id];
    float out = v;
    switch (destOf (id))
    {
        case P::DstCutoff:      out = v * std::exp2 (5.0f * m); break;            // 60 semitones a unit
        case P::DstDrive:
        case P::DstVolume:
        case P::DstTransient:   out = v + 24.0f * m; break;                       // dB
        case P::DstPitch:       out = v + 24.0f * m; break;                       // semitones
        case P::DstFine:        out = v + 100.0f * m; break;                      // cents
        case P::DstDetune:      out = v + 100.0f * m; break;                      // %
        case P::DstGrain:       out = v * std::exp2 (2.0f * m); break;
        case P::DstLfo1Rate:
        case P::DstLfo2Rate:    out = v * std::exp2 (4.0f * m); break;
        case P::DstNone:        return v;
        default:                out = v + m; break;                               // Res, Morph, Pan, Start, Spray, Bow, Breath
    }
    return juce::jlimit (d.min, d.max, out);
}

double SynthEngine::loopBpmOf (const SampleData& s, const ParamSnapshot& p) noexcept
{
    const int length = std::max (1, s.loopInfo.length > 0 ? std::min (s.loopInfo.length, s.length) : s.length);
    if (p[P::LoopBpm] > 0.0f)
        return p[P::LoopBpm];
    const int chosen = P::loopBeatChoices[(size_t) juce::jlimit (0, 12, (int) p[P::LoopBeats])];
    const int beats = chosen > 0 ? chosen : s.loopInfo.beats;
    return beats > 0 ? beats * 60.0 / (length / s.sampleRate) : 0.0;
}

SynthEngine::LoopWindow SynthEngine::loopWindow (const SampleData& s, const ParamSnapshot& p) noexcept
{
    LoopWindow w;
    const int full = std::max (1, s.loopInfo.length > 0 ? std::min (s.loopInfo.length, s.length) : s.length);
    w.length = full;
    const int choice = juce::jlimit (0, (int) std::size (P::loopWindowBeats) - 1, (int) p[P::LoopWinLen]);
    const double bpm = loopBpmOf (s, p);
    if (choice == 0 || bpm <= 0.0 || (int) p[P::LoopKind] == P::LoopSlice)
        return w;
    const double beat = 60.0 / bpm * s.sampleRate;                      // samples of the file a beat
    const int length = (int) std::lround (P::loopWindowBeats[(size_t) choice] * beat);
    if (length < 32 || length >= full)
        return w;
    w.on = true;
    w.length = length;
    w.start = juce::jlimit (0, full - length, (int) std::lround (std::max (0.0f, p[P::LoopWinPos]) * beat));
    return w;
}

float SynthEngine::hitPeak (const SampleData& s, int at) noexcept
{
    const auto& buf = s.loopAudio.getNumSamples() >= s.length ? s.loopAudio : s.audio;
    const int from = std::max (0, at), to = std::min (buf.getNumSamples(), at + std::max (1, (int) (0.015 * s.sampleRate)));
    float peak = 0.0f;
    for (int c = 0; c < std::min (2, buf.getNumChannels()); ++c)
    {
        const float* x = buf.getReadPointer (c);
        for (int i = from; i < to; ++i)
            peak = std::max (peak, std::abs (x[i]));
    }
    return peak;
}

float SynthEngine::mechanismAmount (const ParamSnapshot& p) noexcept
{
    return p[P::MechOn] > 0.5f ? p[P::RelNoise] : 0.0f;
}

// Tonos lays the sample's own cycles closer together or further apart, which moves the note only
// when one cycle is like the next: a chord, detuned voices or cycles that do not repeat alike come
// out at the original pitch, an octave low, or melted into one tone. So on loading Tonos plays the
// sample 7 and 12 semitones up, and the first 0.6 s of each note (after 30 ms) are set against the
// same note in Re-Pitch, whose harmonics are the right ones. Two ways to miss: energy where the
// note has no harmonic (the original pitch, an octave low), and harmonics of the note with nothing
// on them (the other notes of a chord melted into one periodic tone). The strongest partials of
// each are looked for among all those of the other, within 2.7 Hz or 1.2 %, as in the audit of
// the library; where too much is missing either way, the sample plays Tonos as Textura.
// Some milliseconds, once, on the analysis thread.
void SynthEngine::checkTones (SampleData& s)
{
    s.tonesHold = true;
    s.tonesGrid = 1.0f;
    if (! s.pitched || s.marks.size() < 3 || s.cycle <= 1.0)
        return;

    const double sr = s.sampleRate;
    WarpReader::Source source;
    source.channels = 2;
    for (int c = 0; c < 2; ++c)
    {
        source.ch[c] = s.audio.getReadPointer (std::min (c, s.audio.getNumChannels() - 1));
        source.length[c] = s.length;
    }
    source.attackEnd = warpAttackEndOf (s);
    source.fadeStep = 1.0f / (0.01f * (float) sr);

    const int from = (int) (0.03 * sr), to = (int) (0.63 * sr);
    const float power = 1.0f - std::exp (-1.0f / (0.02f * (float) sr));
    const int textureLen = std::max (16, (int) (0.06 * sr));
    std::vector<float> played ((size_t) (to - from)), repitched ((size_t) (to - from));
    float worst = 1.0f;
    for (const double semitones : { 7.0, 12.0 })
    {
        const double ratio = std::exp2 (semitones / 12.0);
        auto reader = std::make_unique<WarpReader>();
        float out[WarpReader::Source::maxChannels] {};
        for (int t = 0; t < to; ++t)
        {
            reader->tick (source, (double) t, 1.0, ratio, true, &s.marks, s.cycle, textureLen, power, out);
            if (t >= from)
            {
                const double at = t * ratio;
                played[(size_t) (t - from)] = 0.5f * (out[0] + out[1]);
                repitched[(size_t) (t - from)] = 0.5f * (readCubic (source.ch[0], at, s.length) + readCubic (source.ch[1], at, s.length));
            }
        }
        double binHz = 1.0;
        const auto wantedSpectrum = powerSpectrum (repitched, sr, binHz);
        const auto playedSpectrum = powerSpectrum (played, sr, binHz);
        const auto wanted = strongestPartials (wantedSpectrum, binHz, 25.0, 4000.0, 30, 50.0f);    // as many as the audit took
        const auto wantedTop = strongestPartials (wantedSpectrum, binHz, 25.0, 4000.0, 15, 30.0f);
        const auto got = strongestPartials (playedSpectrum, binHz, 25.0, 4000.0, 80, 50.0f);
        const auto gotTop = strongestPartials (playedSpectrum, binHz, 25.0, 4000.0, 15, 30.0f);
        const float onNote = shareOnGrid (gotTop, wanted);
        const float kept = shareOnGrid (wantedTop, got);
        worst = std::min ({ worst, onNote, kept });
    }
    s.tonesGrid = worst;
    s.tonesHold = worst >= tonesPass;
}

bool SynthEngine::tonesFallBack (const SampleData& s, int engine) noexcept
{
    return s.pitched && ! s.tonesHold
        && (engine == P::EngineRebuild || engine == P::EngineReenvelope || engine == P::EngineHybrid);
}

float SynthEngine::snapShift (const SampleData& s, bool snap) noexcept
{
    return s.pitched && snap ? (float) s.nearestC() - s.rootMidi : 0.0f;
}

// Play con el host: while the host plays, one voice plays the loop where the host is; it stops when the host
// stops and starts again where the host starts. Keys then only tune it (noteOn).
void SynthEngine::manageTransport (const ParamSnapshot& p)
{
    const bool want = sample != nullptr && p[P::LoopPlay] > 0.5f && (int) p[P::PlayMode] == P::PlayLoop
                      && (int) p[P::LoopKind] == P::LoopNormal && hostPlaying && tempoKnown;
    if (transportVoice != nullptr && (! transportVoice->isActive() || transportVoice->isFading() || ! transportVoice->transport))
        transportVoice = nullptr;
    if (! want && transportVoice != nullptr)
    {
        transportVoice->transport = false;
        transportVoice->fadeOut();
        transportVoice = nullptr;
    }
    if (want && transportVoice == nullptr)
        if (auto* v = allocateVoice())
        {
            v->channel = 1;
            transportPpqNow = hostPpq;
            v->start (*sample, p, transportNote, 1.0f, (float) transportNote, ++noteCounter);
            v->transport = true;
            transportVoice = v;
        }
}

void SynthEngine::process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, const ParamSnapshot& p, double newBpm)
{
    tempoKnown = newBpm > 0.0;
    bpm = newBpm > 0.0 ? newBpm : 120.0;
    buffer.clear();

    if (buffer.getNumChannels() < 2)
        return;
    takeBeatCuts();
    manageTransport (p);
    grainBudget = 6;

    const int n = buffer.getNumSamples();
    int pos = 0;

    // Engine F prepares its frames a stage at a time before the block is rendered, the frame due
    // first first: one stage per 64 samples, more only when a frame would be late.
    lastBlock = n;
    if (sample != nullptr && dronesReady.load (std::memory_order_acquire))
    {
        int budget = std::max (1, n / 64);     // a stage is a fraction of a millisecond
        // With many notes the work is spread over the blocks before it is due, as much in each: left to
        // the last blocks, several notes came due in the same one and it ran over its time (64 samples).
        double demand = 0.0;
        for (auto& v : voices)
            if (v->isActive())
                demand += v->droneDemand (p, n);
        budget = std::max (budget, (int) std::ceil (demand));
        // The frame due first goes first, and it is finished before the next note's: notes started together
        // would otherwise run the same (heavy) stage in the same block.
        for (int guard = 0; guard < 8 * numDroneStreams; ++guard)
        {
            Voice* next = nullptr;
            Voice* urgent = nullptr;
            int least = std::numeric_limits<int>::max();
            std::pair<int, int> first { std::numeric_limits<int>::max(), 0 };
            for (auto& v : voices)
            {
                if (! v->isActive())
                    continue;
                const int slack = v->droneSlack (p, n);
                if (slack < least)
                {
                    least = slack;
                    urgent = v.get();
                }
                const auto order = v->droneOrder (p);
                if (order.first != std::numeric_limits<int>::max() && order < first)
                {
                    first = order;
                    next = v.get();
                }
            }
            if (least <= 0)
                next = urgent;          // a frame would be late: it goes now, whatever the budget
            if (next == nullptr || (budget <= 0 && least > 0))
                break;
            next->droneStep (sampleFor (*next), p);
            --budget;
        }
    }

    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, n, meta.samplePosition);
        if (at > pos)
        {
            renderVoices (buffer, pos, at - pos, p);
            pos = at;
        }
        handleMidi (meta.getMessage(), p);
    }

    if (pos < n)
        renderVoices (buffer, pos, n - pos, p);
    fx->process (buffer, p, bpm);
    setSidechain (nullptr, nullptr, 0);     // given again for each block: never read after the host's buffer is gone

    const float monoTarget = p[P::MonoOut] > 0.5f ? 1.0f : 0.0f;
    if (! masterGainsReady)
        monoMix = monoTarget;
    if (monoMix > 0.0f || monoTarget > 0.0f)
    {
        float* L = buffer.getWritePointer (0);
        float* R = buffer.getWritePointer (1);
        const float step = (monoTarget - monoMix) / (float) n;
        for (int i = 0; i < n; ++i)
        {
            monoMix += step;
            const float mid = 0.5f * (L[i] + R[i]);
            L[i] += (mid - L[i]) * monoMix;
            R[i] += (mid - R[i]) * monoMix;
        }
        monoMix = monoTarget;
    }

    // The lows in mono: a Linkwitz-Riley crossover keeps only the side (L-R) above the frequency, 24 dB per
    // octave under it. The centre goes through the all-pass that crossover amounts to, so above the cut both
    // keep their relation (the image does not move) and no level changes anywhere; only the lows are a
    // little late (some 2 ms at 100 Hz). At 0 the output is bit for bit the engine's.
    // (A low-pass of the side taken away left most of it: -0.5 dB at 60 Hz for a 150 Hz cut. A high-pass
    // on the side alone turned it against the centre up to ~1 kHz: a sound on the left came to the middle.)
    if (p[P::MonoBass] >= 20.0f)
    {
        if (std::abs (p[P::MonoBass] - bassMonoHz) > 0.5f)
        {
            bassMonoHz = p[P::MonoBass];
            bassHigh1.set (Dsp::Biquad::HighPass, sampleRate, bassMonoHz, 0.7071f);
            bassHigh2.set (Dsp::Biquad::HighPass, sampleRate, bassMonoHz, 0.7071f);
            bassAll.set (Dsp::Biquad::AllPass, sampleRate, bassMonoHz, 0.7071f);
        }
        float* L = buffer.getWritePointer (0);
        float* R = buffer.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float mid = bassAll.process (0, 0.5f * (L[i] + R[i]));
            const float side = bassHigh2.process (0, bassHigh1.process (0, 0.5f * (L[i] - R[i])));
            L[i] = mid + side;
            R[i] = mid - side;
        }
    }
    else if (bassMonoHz > 0.0f)
    {
        bassMonoHz = -1.0f;
        bassHigh1.reset();
        bassHigh2.reset();
        bassAll.reset();
    }

    const float gain = dbToGain (p[P::Volume]) * expression * expression;     // CC11 acts as a fader
    float gainL, gainR;
    Dsp::panGains (p[P::Pan], gainL, gainR);
    gainL *= gain;
    gainR *= gain;
    if (! masterGainsReady)
    {
        masterGainL = gainL;
        masterGainR = gainR;
        masterGainsReady = true;
    }
    buffer.applyGainRamp (0, 0, n, masterGainL, gainL);
    buffer.applyGainRamp (1, 0, n, masterGainR, gainR);
    masterGainL = gainL;
    masterGainR = gainR;

    // Protection against runaways (resonance, feedback), far above any normal level.
    {
        float* L = buffer.getWritePointer (0);
        float* R = buffer.getWritePointer (1);
        const float recover = 1.0f - std::exp (-1.0f / (0.08f * (float) sampleRate));
        for (int i = 0; i < n; ++i)
        {
            const float peak = std::max (std::abs (L[i]), std::abs (R[i]));
            if (peak * limiterGain > limiterCeiling)
                limiterGain = limiterCeiling / peak;
            if (limiterGain < 1.0f)
            {
                L[i] *= limiterGain;
                R[i] *= limiterGain;
                limiterGain += (1.0f - limiterGain) * recover;
                if (limiterGain > 0.99999f)
                    limiterGain = 1.0f;
            }
        }
    }

    for (size_t i = 0; i < voices.size(); ++i)
    {
        playheads[i].store (voices[i]->getPlayhead(), std::memory_order_relaxed);
        playSlices[i].store (voices[i]->getSlice(), std::memory_order_relaxed);
    }

    // The knobs' outer ring: the newest note's modulation, or the macros alone when none sounds.
    {
        const Voice* newest = nullptr;
        for (const auto& v : voices)
            if (v->isActive() && ! v->isFading() && (newest == nullptr || (juce::int32) (v->startOrder() - newest->startOrder()) > 0))
                newest = v.get();
        std::array<float, P::NumDests> shown {};
        if (newest != nullptr)
            for (int d = 0; d < P::NumDests; ++d)
                shown[(size_t) d] = newest->modulationOf (d);
        else
            for (int slot = 0; slot < P::numSlots; ++slot)
            {
                const int base = P::M1Src + slot * P::slotStride;
                const int src = (int) p[(size_t) base], dst = (int) p[(size_t) base + 1];
                if (src >= P::SrcMacro1 && src <= P::SrcMacro4 && dst > 0 && dst < P::NumDests)
                    shown[(size_t) dst] += p[(size_t) (P::Macro1 + src - P::SrcMacro1)] * p[(size_t) base + 2];
            }
        for (int d = 0; d < P::NumDests; ++d)
            modShown[(size_t) d].store (shown[(size_t) d], std::memory_order_relaxed);
    }
}

void SynthEngine::setSidechain (const float* left, const float* right, int length) noexcept
{
    sideL = length > 0 ? left : nullptr;
    sideR = length > 0 ? right : nullptr;
    sideLen = length;
    fx->setSidechain (sideL, sideR, sideLen);
}

// The Sidechain source: the level of the sound coming in (or of its lows, mids or highs), followed with Ataque and
// Release and read in decibels (-48 dB to 0 dB over the source's 0 to 1, Sensibilidad moving that window), so that
// a soft and a loud passage both move what it modulates. Brillo: how much of the sound is above 2.5 kHz, whatever
// its level (a hi-hat bright, a kick dark), falling back to 0 when nothing comes in.
void SynthEngine::followSidechain (int start, int num, const ParamSnapshot& p) noexcept
{
    const float sr = (float) sampleRate;
    const float atk = 1.0f - std::exp (-1.0f / (std::max (0.1f, p[P::ScAttack]) * 0.001f * sr));
    const float rel = 1.0f - std::exp (-1.0f / (std::max (1.0f, p[P::ScRelease]) * 0.001f * sr));
    const int response = juce::jlimit (0, 4, (int) p[P::ScResponse]);
    if (response != sideResponse)
    {
        switch (response)
        {
            case P::ScLows:  sideBand.set (Dsp::Biquad::LowPass, sampleRate, 150.0f, 0.707f); break;
            case P::ScMids:  sideBand.set (Dsp::Biquad::BandPass, sampleRate, 1000.0f, 0.7f); break;
            case P::ScHighs: sideBand.set (Dsp::Biquad::HighPass, sampleRate, 3000.0f, 0.707f); break;
            default:         break;
        }
        sideHigh.set (Dsp::Biquad::HighPass, sampleRate, 2500.0f, 0.707f);
        sideBand.reset();
        sideHigh.reset();
        sideResponse = response;
    }
    const bool band = response == P::ScLows || response == P::ScMids || response == P::ScHighs;
    const bool present = sideL != nullptr && start + num <= sideLen;

    for (int i = 0; i < num; ++i)
    {
        const float x = present ? 0.5f * (sideL[start + i] + (sideR != nullptr ? sideR[start + i] : sideL[start + i])) : 0.0f;
        const float a = std::abs (band ? sideBand.process (0, x) : x);
        sideEnv += (a - sideEnv) * (a > sideEnv ? atk : rel);
        if (response == P::ScBright)
        {
            const float h = std::abs (sideHigh.process (0, x));
            sideHighEnv += (h - sideHighEnv) * (h > sideHighEnv ? atk : rel);
        }
    }
    if (sideEnv < 1.0e-9f) sideEnv = 0.0f;          // no denormals once the sound has gone
    if (sideHighEnv < 1.0e-9f) sideHighEnv = 0.0f;

    const float sens = p[P::ScSens];
    float v;
    if (response == P::ScBright)
    {
        // Below -60 dB there is nothing to judge: the brightness falls with the level.
        const float gate = juce::jlimit (0.0f, 1.0f, (Dsp::gainToDb (sideEnv + 1.0e-9f) + 66.0f) / 12.0f);
        v = juce::jlimit (0.0f, 1.0f, sideHighEnv / (sideEnv + 1.0e-9f) * 2.0f * Dsp::dbToGain (sens)) * gate;
    }
    else
        v = juce::jlimit (0.0f, 1.0f, (Dsp::gainToDb (sideEnv * 1.4142f + 1.0e-9f) + sens + 48.0f) / 48.0f);
    sideValue = v;
    sideShown.store (v, std::memory_order_relaxed);
}

void SynthEngine::renderVoices (juce::AudioBuffer<float>& buffer, int start, int num, const ParamSnapshot& p)
{
    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);

    for (int done = 0; done < num;)
    {
        const int chunk = std::min (subBlock, num - done);

        followSidechain (start + done, chunk, p);
        if (sample != nullptr)
        {
            transportPpqNow = hostPpq + (start + done) / sampleRate * bpm / 60.0;
            for (auto& v : voices)
                if (v->isActive())
                    v->render (L + start + done, R + start + done, chunk, sampleFor (*v), p);
            pedalNoise->process (L + start + done, R + start + done, chunk, *sample);
        }

        // The older analysis is let go once its last note has ended. The processor still holds it, so
        // it is freed later on the message thread, never here.
        if (previous != nullptr)
        {
            bool used = false;
            for (auto& v : voices)
                used = used || (v->isActive() && v->onPrevious);
            if (! used)
            {
                previous.reset();
                previousInUse.store (false, std::memory_order_release);
            }
        }

        for (int l = 0; l < P::numLfos; ++l)
        {
            auto& ph = globalLfoPhase[(size_t) l];
            ph += lfoRateHz (p, l, bpm) * chunk / sampleRate;
            if (ph >= 1.0)
            {
                ph -= Dsp::floorFast (ph);
                globalSampleHold[(size_t) l] = rng.nextFloat() * 2.0f - 1.0f;
            }
        }

        done += chunk;
    }
}

void SynthEngine::handleMidi (const juce::MidiMessage& m, const ParamSnapshot& p)
{
    const int ch = juce::jlimit (1, 16, m.getChannel());
    if (m.isNoteOn())
    {
        // Two notes held on two different channels other than the first: MPE.
        if (ch != 1 && ! mpe)
            for (auto& v : voices)
                if (v->isActive() && ! v->isReleased() && v->channel != 1 && v->channel != ch)
                {
                    // From now on the bend and pressure of the first channel are the global ones; the others
                    // belong to their notes (they were kept even before MPE was known).
                    mpe = true;
                    pitchBend = chanBend[1];
                    aftertouch = chanPressure[1];
                }
        noteOn (m.getNoteNumber(), m.getFloatVelocity(), p, ch);
    }
    else if (m.isNoteOff())
        noteOff (m.getNoteNumber(), p, ch);
    else if (m.isController() && (m.getControllerNumber() == 101 || m.getControllerNumber() == 100))
        (m.getControllerNumber() == 101 ? rpnMsb : rpnLsb)[(size_t) ch] = m.getControllerValue();
    else if (m.isController() && m.getControllerNumber() == 6)
    {
        // RPN 0: the bend range of this channel; RPN 6: an MPE configuration message.
        if (rpnMsb[(size_t) ch] == 0 && rpnLsb[(size_t) ch] == 0)
            chanBendRange[(size_t) ch] = (float) m.getControllerValue();
        else if (rpnMsb[(size_t) ch] == 0 && rpnLsb[(size_t) ch] == 6 && (ch == 1 || ch == 16))
            mpe = m.getControllerValue() > 0;
    }
    else if (m.isController() && m.getControllerNumber() == 74)
    {
        chanTimbre[(size_t) ch] = (float) m.getControllerValue() / 127.0f;
        timbreSeen[(size_t) ch] = true;
    }
    else if (m.isPitchWheel())
    {
        const float bend = juce::jlimit (-1.0f, 1.0f, ((float) m.getPitchWheelValue() - 8192.0f) / 8191.0f);
        chanBend[(size_t) ch] = bend;
        if (! mpe || ch == 1)
            pitchBend = bend;
    }
    else if (m.isChannelPressure())
    {
        const float pressure = (float) m.getChannelPressureValue() / 127.0f;
        chanPressure[(size_t) ch] = pressure;
        if (! mpe || ch == 1)
            aftertouch = pressure;
    }
    else if (m.isSustainPedalOn())
    {
        if (! pedalDown && sample != nullptr && ((int) p[P::MechType] == Mechanism::Piano || (int) p[P::MechType] == Mechanism::RealPiano))
            pedalNoise->pedal (true, mechanismAmount (p), *sample, dbToGain (p[P::MechDown]));
        pedalDown = true;
    }
    else if (m.isSustainPedalOff())
    {
        if (pedalDown && sample != nullptr && ((int) p[P::MechType] == Mechanism::Piano || (int) p[P::MechType] == Mechanism::RealPiano))
            pedalNoise->pedal (false, mechanismAmount (p), *sample, dbToGain (p[P::MechUp]));
        pedalDown = false;
        for (auto& v : voices)
            if (v->isActive() && v->pedalHeld)
                v->release();
    }
    else if (m.isController() && m.getControllerNumber() == 1)
        modWheel = (float) m.getControllerValue() / 127.0f;
    else if (m.isController() && m.getControllerNumber() == 2)
        breath = (float) m.getControllerValue() / 127.0f;
    else if (m.isController() && m.getControllerNumber() == 11)
        expression = (float) m.getControllerValue() / 127.0f;
    else if (m.isAftertouch())
    {
        // Polyphonic aftertouch: only the voices of that key.
        for (auto& v : voices)
            if (v->isActive() && v->getNote() == m.getNoteNumber())
                v->pressure = (float) m.getAfterTouchValue() / 127.0f;
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (auto& v : voices)
        {
            if (! v->isActive())
                continue;
            if (m.isAllSoundOff())
                v->fadeOut();
            else
                v->release();
        }
        heldNotes.clear();
    }
}

Voice* SynthEngine::allocateVoice()
{
    for (auto& v : voices)
        if (! v->isActive())
            return v.get();

    Voice* oldest = voices.front().get();
    for (auto& v : voices)
        if (v->getOrder() < oldest->getOrder())
            oldest = v.get();
    oldest->stopImmediately();
    return oldest;
}

void SynthEngine::noteOn (int note, float velocity, const ParamSnapshot& p, int channel)
{
    heldNotes.erase (std::remove (heldNotes.begin(), heldNotes.end(), note), heldNotes.end());
    heldNotes.push_back (note);

    if (sample == nullptr)
        return;

    // SLICE: every key starts its own slice. A slice is never released (it plays to its end), so Legato would
    // only have bent the pitch of the slice already playing: the first slice heard on every key. Legato
    // plays as Mono there (a new slice cuts the one before), Poly lets them ring together.
    if (transportVoice != nullptr && transportVoice->isActive() && transportVoice->transport)
    {
        // Play con el host: the loop goes on, in the key's pitch (no restart, no new voice).
        transportNote = note;
        transportVoice->glideTo (note, p);
        return;
    }
    const bool slicing = (int) p[P::PlayMode] == P::PlayLoop && (int) p[P::LoopKind] == P::LoopSlice;
    const int mode = slicing && (int) p[P::PolyMode] == P::Legato ? (int) P::Mono : (int) p[P::PolyMode];
    const float fromPitch = lastNotePitch;
    lastNotePitch = (float) note;

    if (mode == P::Legato)
    {
        for (auto& v : voices)
        {
            if (v->isActive() && ! v->isReleased() && ! v->isFading())
            {
                v->glideTo (note, p);
                v->channel = channel;
                return;
            }
        }
    }

    float startPitch = fromPitch;

    if (mode != P::Poly)
    {
        for (auto& v : voices)
        {
            if (v->isActive() && ! v->isFading())
            {
                startPitch = v->getPitch();
                v->fadeOut();
            }
        }
    }
    else
    {
        int sounding = 0;
        Voice* victim = nullptr;
        for (auto& v : voices)
        {
            if (! v->isActive() || v->isFading())
                continue;
            ++sounding;
            if (victim == nullptr
                || (v->isReleased() && ! victim->isReleased())
                || (v->isReleased() == victim->isReleased() && v->getOrder() < victim->getOrder()))
                victim = v.get();
        }
        if (sounding >= maxVoices && victim != nullptr)
            victim->fadeOut();
    }

    if (slicing)
        for (auto& v : voices)
            if (v->isActive() && ! v->isFading() && v->getNote() == note)
                v->fadeOut();

    if (auto* v = allocateVoice())
    {
        v->channel = channel;
        v->start (*sample, p, note, velocity, startPitch, ++noteCounter);
    }
}

void SynthEngine::noteOff (int note, const ParamSnapshot& p, int channel)
{
    heldNotes.erase (std::remove (heldNotes.begin(), heldNotes.end(), note), heldNotes.end());

    const int mode = (int) p[P::PolyMode];

    for (auto& v : voices)
    {
        if (! v->isActive() || v->isReleased() || v->isFading() || v->getNote() != note || (mpe && v->channel != channel) || v->transport)
            continue;

        if ((int) p[P::PlayMode] == P::PlayLoop && (int) p[P::LoopKind] == P::LoopSlice)
            v->release();       // a slice ignores it and plays to its end; never a glide to another key
        else if (mode != P::Poly && ! heldNotes.empty())
        {
            v->glideTo (heldNotes.back(), p);
            lastNotePitch = (float) heldNotes.back();
        }
        else if (pedalDown)
            v->pedalHeld = true;
        else
            v->release();
    }
}
