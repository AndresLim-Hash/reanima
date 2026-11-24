#include "DroneStretch.h"
#include "DspUtil.h"
#include <algorithm>
#include <cmath>
#include <limits>
#if defined (__SSE2__) || defined (_M_X64)
 #include <emmintrin.h>
#endif

namespace Drone
{
namespace
{
// Random-phase frames with Hann analysis and synthesis at 75 % overlap: each frame keeps 3/8 of
// the power of the input, and four of them under the synthesis window add up to 1.5 times one.
const float outputGain = 1.0f / std::sqrt (0.375f * 1.5f);

// Mean of v around each bin over a width that grows with the frequency (a constant fraction of
// an octave), from prefix sums.
void smooth (const float* v, float* out, int bins, float fraction, float minWidth, std::vector<double>& sums) noexcept
{
    sums[0] = 0.0;
    for (int k = 0; k < bins; ++k)
        sums[(size_t) k + 1] = sums[(size_t) k] + v[k];
    for (int k = 0; k < bins; ++k)
    {
        const int w = (int) (minWidth + fraction * (float) k);
        const int a = std::max (0, k - w), b = std::min (bins - 1, k + w);
        out[k] = (float) ((sums[(size_t) b + 1] - sums[(size_t) a]) / (double) (b - a + 1));
    }
}

// v read at a fractional bin, zero outside.
inline float binAt (const float* v, float x, int bins) noexcept
{
    if (x < 0.0f || x >= (float) (bins - 1))
        return 0.0f;
    const int i = (int) x;
    const float f = x - (float) i;
    return v[i] + f * (v[i + 1] - v[i]);
}

inline float hermite (const float* d, int i, float f) noexcept
{
    const float xm1 = d[i - 1], x0 = d[i], x1 = d[i + 1], x2 = d[i + 2];
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}
} // namespace

static bool trueFrequency = true;
void setTrueFrequency (bool on) noexcept { trueFrequency = on; }

int orderFor (int sizeChoice, double hostRate) noexcept
{
    const int base = 11 + juce::jlimit (0, numSizes - 1, sizeChoice);     // 2048 to 65536 samples at 48 kHz
    return juce::jlimit (minOrder, maxOrder, base + juce::roundToInt (std::log2 (hostRate / 48000.0)));
}

float curveFrequency (int point) noexcept
{
    return 20.0f * std::pow (1000.0f, (float) point / (float) (curvePoints - 1));
}

float curveGainDb (const float* curve, float hz) noexcept
{
    const float x = juce::jlimit (0.0f, (float) (curvePoints - 1), std::log2 (std::max (hz, 1.0f) / 20.0f) / std::log2 (1000.0f) * (float) (curvePoints - 1));
    const int i = std::min ((int) x, curvePoints - 2);
    const float f = x - (float) i;
    return curve[i] + f * (curve[i + 1] - curve[i]);
}

//==============================================================================
Shared::Shared()
{
    for (int order = minOrder; order <= maxOrder; ++order)
    {
        const size_t i = (size_t) (order - minOrder);
        ffts[i] = std::make_unique<juce::dsp::FFT> (order);
        const int n = 1 << order;
        windows[i].resize ((size_t) n);
        for (int k = 0; k < n; ++k)
            windows[i][(size_t) k] = 0.5f - 0.5f * std::cos (Dsp::twoPi * (float) k / (float) n);
    }
    transform.resize ((size_t) 1 << maxOrder);
    twiddle.resize ((size_t) 1 << (maxOrder - 1));
    for (size_t k = 0; k < twiddle.size(); ++k)
    {
        const double a = -2.0 * juce::MathConstants<double>::pi * (double) k / (double) (1 << maxOrder);
        twiddle[k] = Complex ((float) std::cos (a), (float) std::sin (a));
    }
    spectrum.resize (((size_t) 1 << (maxOrder - 1)) + 1);
    gains.resize (spectrum.size());
    sums.resize (spectrum.size() + 1);
    harmonicKeep.resize (spectrum.size());
    curveGain.resize (spectrum.size());
    colourGain.resize (spectrum.size());
}

//==============================================================================
Stream::Stream()
{
    for (auto& a : acc)
        a.assign ((size_t) ringSize, 0.0f);
    for (auto& m : mag)
        m.assign (((size_t) 1 << (maxOrder - 1)) + 1, 0.0f);
    frame.resize ((size_t) 1 << maxOrder);
    tracks.reserve (4096);
    lastTracks.reserve (4096);
    peakOf.assign (((size_t) 1 << (maxOrder - 1)) + 1, -1);
}

// The tonal peaks of the left channel's spectrum to play: a bin over both its neighbours, 6 dB over the mean of the
// eight around it and within 80 dB of the loudest. Its true place between bins from the parabola through the three
// (in log level). A peak within a bin of one of the frame before carries its phase on by the average of their
// frequencies over a hop (a hop is a quarter of a frame: pos / 4 cycles); a new one starts at random.
// PaulStretch places each partial in the middle of its bin, a new phase every frame: a pure tone of 440 Hz sounded
// 12 cents flat to 4.5 sharp, more on low notes. The noise between the peaks keeps its random phases.
void Stream::findPeaks (const Settings& d, juce::Random& apartRng) noexcept
{
    const float* m = mag[0].data();
    const int bins = size / 2;
    if (size != lastSize)
        lastTracks.clear();
    lastSize = size;
    tracks.clear();
    std::fill (peakOf.begin(), peakOf.begin() + bins + 1, -1);
    float top = 0.0f;
    for (int k = 1; k < bins; ++k)
        top = std::max (top, m[k]);
    if (top <= 0.0f)
        return;
    size_t last = 0;
    for (int k = 4; k < bins - 4 && tracks.size() < tracks.capacity(); ++k)
    {
        const float a = m[k - 1], b = m[k], c = m[k + 1];
        if (! (b > a && b >= c && b > 1.0e-4f * top))
            continue;
        float around = 0.0f;
        for (int j = 1; j <= 4; ++j)
            around += m[k - j] + m[k + j];
        if (b < 2.0f * around / 8.0f)
            continue;
        const float la = std::log (a + 1.0e-20f), lb = std::log (b + 1.0e-20f), lc = std::log (c + 1.0e-20f);
        const float den = la - 2.0f * lb + lc;
        const float pos = (float) k + (den < 0.0f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (la - lc) / den) : 0.0f);
        while (last + 1 < lastTracks.size() && lastTracks[last + 1].pos <= pos)
            ++last;
        const Track* match = nullptr;
        for (size_t q = last; q < std::min (lastTracks.size(), last + 2); ++q)
            if (std::abs (lastTracks[q].pos - pos) < 1.0f && (match == nullptr || std::abs (lastTracks[q].pos - pos) < std::abs (match->pos - pos)))
                match = &lastTracks[q];
        const double hz = pos * d.hostRate / size;
        const double lows = hz >= 300.0 ? 1.0 : hz <= 120.0 ? 0.0 : [&] { const double x = std::log2 (hz / 120.0) / std::log2 (2.5); return x * x * (3.0 - 2.0 * x); }();
        Track t;
        t.pos = pos;
        t.theta = match != nullptr ? match->theta + 0.125 * (double) (match->pos + pos) : apartRng.nextDouble();
        t.theta -= std::floor (t.theta);
        // The right channel's offset sweeps slowly to and fro, at most a quarter of a cycle either way (as the random
        // phases: never opposed), each partial at its own pace: a held partial is wide and moves a little, as a chorus.
        // A fixed offset left the two sides as alike as chance made them (side 14 to 23 dB down).
        const float reach = (float) (0.25 * d.width * lows);
        t.sweep = match != nullptr ? match->sweep + match->rate : (float) apartRng.nextDouble();
        t.sweep -= std::floor (t.sweep);
        t.rate = match != nullptr ? match->rate : (float) (0.01 + 0.04 * apartRng.nextDouble());
        t.apart = reach * std::sin (Dsp::twoPi * t.sweep);
        const int index = (int) tracks.size();
        tracks.push_back (t);
        for (int j = k - 2; j <= k + 2; ++j)
            if (peakOf[(size_t) j] < 0)
                peakOf[(size_t) j] = index;
    }
}

void Stream::start (double sourcePos, int newOrder, int delay, juce::int64 seed) noexcept
{
    for (auto& a : acc)
        std::fill (a.begin(), a.end(), 0.0f);
    order = newOrder;
    size = 1 << order;
    hop = size / 4;
    pos = sourcePos;
    rng.setSeed (seed);
    readIdx = 0;
    untilFrame = juce::jlimit (1, size, delay);
    nextAt = untilFrame;
    stage = 1;
}

int Stream::slack (int blockSize) const noexcept
{
    if (stage == 0)
        return std::numeric_limits<int>::max();
    return untilFrame / std::max (1, blockSize) - (numStages() - stage + 1);
}

void Stream::render (const SampleData& s, const Settings& d, Shared& shared, const float* gain, float* outL, float* outR, int num) noexcept
{
    float* a = acc[0].data();
    float* b = acc[1].data();
    int done = 0;

    while (done < num)
    {
        if (untilFrame == 0)
        {
            while (stage != 0)              // late: finish the frame now
                step (s, d, shared);
            scheduleNext (d);
        }

        const int chunk = std::min (num - done, untilFrame);
        for (int i = 0; i < chunk; ++i)
        {
            const float g = gain[done + i] * outputGain;
            outL[done + i] += a[readIdx] * g;
            outR[done + i] += b[readIdx] * g;
            a[readIdx] = b[readIdx] = 0.0f;
            readIdx = (readIdx + 1) & (ringSize - 1);
        }
        untilFrame -= chunk;
        done += chunk;
    }
}

void Stream::scheduleNext (const Settings& d) noexcept
{
    // A new size takes effect from the next frame.
    if (d.order != order)
    {
        order = d.order;
        size = 1 << order;
        hop = size / 4;
    }

    nextAt = (nextAt + hop) & (ringSize - 1);
    untilFrame = hop;
    stage = 1;

    const double length = std::max (1.0, d.regionEnd - d.regionStart);
    pos = d.regionStart + std::fmod (pos + d.advance * hop - d.regionStart, length);
    if (pos < d.regionStart)
        pos += length;
}

// Stages: read; the forward FFT (reorder, then one pass per order); magnitudes; shape each channel;
// build the spectrum to play; the inverse FFT the same way; overlap-add.
void Stream::step (const SampleData& s, const Settings& d, Shared& shared) noexcept
{
    if (stage <= 0)
        return;
    const int fwd = 2, mags = fwd + order + 1, shapeL = mags + 1, shapeR = shapeL + 1, peaks = shapeR + 1, build1 = peaks + 1,
              build2 = build1 + 1, inv = build2 + 1, add = inv + order + 1;
    if (stage == 1)
        read (s, d, shared);
    else if (stage < mags)
        fftPass (stage - fwd, false, shared);
    else if (stage == mags)
        magnitudes();
    else if (stage == shapeL)
        shape (0, d, shared);
    else if (stage == shapeR)
        shape (1, d, shared);
    else if (stage == peaks)
    {
        // a stage of its own: over the 32,768 bins of a long frame it is too much to add to the building of it
        if (trueFrequency)
        {
            const juce::int64 seed = rng.nextInt64();
            buildPhases.setSeed (seed);
            buildApart.setSeed (seed ^ 0x5DEECE66DLL);
            findPeaks (d, buildApart);
            seededForPeaks = true;
        }
    }
    else if (stage == build1)
        build (d, 0);
    else if (stage == build2)
        build (d, 1);
    else if (stage < add)
        fftPass (stage - inv, true, shared);
    else
        synthesise (shared);
    stage = stage >= numStages() ? 0 : stage + 1;
}

// In-place radix-2 FFT, one step at a time: the same transform as juce::dsp::FFT (forward with
// e^-i, inverse with e^+i and scaled by 1/n when the frame is played).
void Stream::fftPass (int pass, bool inverse, const Shared& shared) noexcept
{
    Complex* x = frame.data();
    const int n = size;
    if (pass == 0)
    {
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (x[i], x[j]);
        }
        return;
    }
    const int len = 1 << pass, half = len / 2, stride = (1 << maxOrder) / len;
    const Complex* tw = shared.twiddle.data();
#if defined (__SSE2__) || defined (_M_X64)
    // Two butterflies at a time, each lane with the operations of the scalar loop below in their order
    // (re = a c - b d, im = a d + b c, then u + v and u - v): the same result bit for bit, in about half
    // the time. A subtraction is the addition of the negated value, which IEEE makes exact.
    if (half >= 2)
    {
        float* f = reinterpret_cast<float*> (x);
        const __m128 negRe = _mm_setr_ps (-0.0f, 0.0f, -0.0f, 0.0f);
        const float sign = inverse ? -1.0f : 1.0f;      // conj: the imaginary part negated (exact)
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; k += 2)
            {
                const Complex w0 = tw[k * stride], w1 = tw[(k + 1) * stride];
                const __m128 c = _mm_setr_ps (w0.real(), w0.real(), w1.real(), w1.real());
                const __m128 d = _mm_setr_ps (sign * w0.imag(), sign * w0.imag(), sign * w1.imag(), sign * w1.imag());
                float* pu = f + 2 * (i + k);
                float* pv = f + 2 * (i + k + half);
                const __m128 u = _mm_loadu_ps (pu), v = _mm_loadu_ps (pv);       // (a0, b0, a1, b1)
                const __m128 vc = _mm_mul_ps (v, c);                                // (a c, b c)
                const __m128 vd = _mm_mul_ps (_mm_shuffle_ps (v, v, _MM_SHUFFLE (2, 3, 0, 1)), d);     // (b d, a d)
                const __m128 vw = _mm_add_ps (vc, _mm_xor_ps (vd, negRe));        // (a c - b d, b c + a d)
                _mm_storeu_ps (pu, _mm_add_ps (u, vw));
                _mm_storeu_ps (pv, _mm_sub_ps (u, vw));
            }
        return;
    }
#endif
    for (int i = 0; i < n; i += len)
        for (int k = 0; k < half; ++k)
        {
            const Complex w = inverse ? std::conj (tw[k * stride]) : tw[k * stride];
            const Complex u = x[i + k], v = x[i + k + half] * w;
            x[i + k] = u + v;
            x[i + k + half] = u - v;
        }
}

// Stage 1: both channels under the window, the left one as the real part and the right one as
// the imaginary part of a single complex signal.
void Stream::read (const SampleData& s, const Settings& d, Shared& shared) noexcept
{
    const float* win = shared.window (order);
    const float* srcL = s.audio.getReadPointer (0);
    const float* srcR = s.audio.getReadPointer (1);
    const double length = std::max (1.0, d.regionEnd - d.regionStart);
    const int last = s.length - 3;

    // The drone sounds at the level of the sample's loudest part wherever it reads, up to +12 dB.
    level = juce::jlimit (1.0f, 4.0f, 1.0f / std::max (0.25f, s.envAt (pos)));

    double q = std::fmod (pos - size / 2 * d.rateRatio - d.regionStart, length);
    if (q < 0.0)
        q += length;

    int n = 0;
#if defined (__SSE2__) || defined (_M_X64)
    // Two samples at a time, left and right side by side (lanes L, R of one sample, then of the next): the
    // positions worked out one after another as below, and each lane with hermite's operations in their order,
    // so the frame is the same bit for bit. Outside the file a sample is 0, as below.
    {
        float* out = reinterpret_cast<float*> (frame.data());
        const __m128 half = _mm_set1_ps (0.5f), twoAndHalf = _mm_set1_ps (2.5f), two = _mm_set1_ps (2.0f), oneAndHalf = _mm_set1_ps (1.5f);
        for (; s.length >= 4 && n + 2 <= size; n += 2)
        {
            int idx[2];
            float frac[2];
            bool inside[2];
            for (int j = 0; j < 2; ++j)
            {
                const double at = d.regionStart + q;
                idx[j] = (int) at;
                frac[j] = (float) (at - idx[j]);
                inside[j] = idx[j] >= 1 && idx[j] < last;
                if (! inside[j])
                    idx[j] = 1;         // read somewhere safe; the lanes are zeroed below
                q += d.rateRatio;
                if (q >= length)
                    q -= length;
            }
            const int i0 = idx[0], i1 = idx[1];
            const __m128 xm1 = _mm_setr_ps (srcL[i0 - 1], srcR[i0 - 1], srcL[i1 - 1], srcR[i1 - 1]);
            const __m128 x0 = _mm_setr_ps (srcL[i0], srcR[i0], srcL[i1], srcR[i1]);
            const __m128 x1 = _mm_setr_ps (srcL[i0 + 1], srcR[i0 + 1], srcL[i1 + 1], srcR[i1 + 1]);
            const __m128 x2 = _mm_setr_ps (srcL[i0 + 2], srcR[i0 + 2], srcL[i1 + 2], srcR[i1 + 2]);
            const __m128 f = _mm_setr_ps (frac[0], frac[0], frac[1], frac[1]);
            const __m128 c1 = _mm_mul_ps (half, _mm_sub_ps (x1, xm1));
            const __m128 c2 = _mm_sub_ps (_mm_add_ps (_mm_sub_ps (xm1, _mm_mul_ps (twoAndHalf, x0)), _mm_mul_ps (two, x1)), _mm_mul_ps (half, x2));
            const __m128 c3 = _mm_add_ps (_mm_mul_ps (half, _mm_sub_ps (x2, xm1)), _mm_mul_ps (oneAndHalf, _mm_sub_ps (x0, x1)));
            const __m128 h = _mm_add_ps (_mm_mul_ps (_mm_add_ps (_mm_mul_ps (_mm_add_ps (_mm_mul_ps (c3, f), c2), f), c1), f), x0);
            const __m128 w = _mm_setr_ps (win[n], win[n], win[n + 1], win[n + 1]);
            const __m128 keep = _mm_castsi128_ps (_mm_setr_epi32 (inside[0] ? -1 : 0, inside[0] ? -1 : 0, inside[1] ? -1 : 0, inside[1] ? -1 : 0));
            _mm_storeu_ps (out + 2 * n, _mm_and_ps (keep, _mm_mul_ps (w, h)));
        }
    }
#endif
    for (; n < size; ++n)
    {
        const double at = d.regionStart + q;
        const int i = (int) at;
        const float f = (float) (at - i);
        frame[(size_t) n] = (i >= 1 && i < last) ? Complex (win[n] * hermite (srcL, i, f), win[n] * hermite (srcR, i, f)) : Complex();
        q += d.rateRatio;
        if (q >= length)
            q -= length;
    }
}

// Stage 2: one transform, then the two spectra apart (each is the even or the odd part of it).
void Stream::magnitudes() noexcept
{
    const Complex* z = frame.data();

    float* ml = mag[0].data();
    float* mr = mag[1].data();
    for (int k = 0; k <= size / 2; ++k)
    {
        const Complex a = z[k], b = std::conj (z[(size - k) & (size - 1)]);
        ml[k] = 0.5f * std::abs (a + b);
        mr[k] = 0.5f * std::abs (a - b);
    }
    ml[0] = mr[0] = 0.0f;      // no DC
}

// Stage 3: the spectral treatment, on the magnitudes.
void Stream::shape (int ch, const Settings& d, Shared& shared) noexcept
{
    const int bins = size / 2 + 1;
    const float binHz = (float) (d.hostRate / size);
    float* m = mag[(size_t) ch].data();
    float* t = shared.spectrum.data();
    float* g = shared.gains.data();

    double energyIn = 0.0;
    for (int k = 0; k < bins; ++k)
        energyIn += (double) m[k] * m[k];
    if (energyIn <= 1.0e-20)
        return;

    // Only the harmonics of the note: a Gaussian bump on each, in cents.
    if (d.harmonics > 0.0f && d.fundamentalHz > 0.0f)
    {
        const float f0 = d.fundamentalHz, width = std::max (1.0f, d.harmonicWidthCents);
        float* keep = shared.harmonicKeep.data();
        if (f0 != shared.keepF0 || width != shared.keepWidth || binHz != shared.keepBinHz || bins != shared.keepBins)
        {
            for (int k = 1; k < bins; ++k)
            {
                const float f = (float) k * binHz;
                const float harmonic = std::max (1.0f, std::round (f / f0));
                const float cents = 1200.0f * std::log2 (f / (harmonic * f0)) / width;
                keep[k] = f < 0.5f * f0 ? 0.0f : std::exp (-cents * cents);
            }
            shared.keepF0 = f0;
            shared.keepWidth = width;
            shared.keepBinHz = binHz;
            shared.keepBins = bins;
        }
        for (int k = 1; k < bins; ++k)
            m[k] *= 1.0f - d.harmonics + d.harmonics * keep[k];
    }

    // Tone against noise: what stands over the local mean of the spectrum, or what stays under it.
    if (d.tonal != 0.0f)
    {
        smooth (m, t, bins, 0.08f, 2.0f, shared.sums);
        const float amount = std::abs (d.tonal);
        for (int k = 0; k < bins; ++k)
        {
            const float part = d.tonal > 0.0f ? std::max (0.0f, m[k] - t[k]) : std::min (m[k], t[k]);
            m[k] += (part - m[k]) * amount;
        }
    }

    // Spread: every peak widens, by up to a quarter of its frequency each side.
    if (d.spread > 0.0f)
    {
        smooth (m, t, bins, 0.25f * d.spread, 1.0f + 6.0f * d.spread, shared.sums);
        std::copy (t, t + bins, m);
    }

    // Pitch, intervals and unison: copies of the spectrum at other ratios. A copy stretched over
    // r times more bins keeps its energy with 1/sqrt(r).
    // Under Warp Tonos what moves is the fine structure (the partials over the envelope, the
    // envelope taken over one fundamental, which runs from one partial to the next as a line
    // would, or over 200 Hz without a note); the envelope
    // stays put, the formants with it. The level is set again below, whatever this does.
    const bool plain = d.numCopies == 1 && std::abs (d.copyRatio[0] - 1.0f) < 1.0e-5f;
    if (! plain)
    {
        float* env = g;                      // the gains are only filled further down
        float lowest = 0.0f;                 // bin of the lowest part within 20 dB of the loudest
        if (d.keepFormants)
        {
            float loudest = 0.0f;
            for (int k = 1; k < bins; ++k)
                loudest = std::max (loudest, m[k]);
            for (int k = 1; k < bins; ++k)
                if (m[k] >= 0.1f * loudest)
                {
                    lowest = (float) k;
                    break;
                }
            const float across = 0.5f * (d.fundamentalHz > 0.0f ? d.fundamentalHz : 200.0f) / binHz;
            smooth (m, env, bins, 0.0f, std::max (1.0f, across), shared.sums);
            // Under the fundamental there is no envelope, only the floor: it is taken as flat at the
            // fundamental's level, so that the floor moved up under the new note stays a floor.
            if (d.fundamentalHz > 0.0f)
            {
                const int k0 = std::min (bins - 1, (int) std::lround (d.fundamentalHz / binHz));
                std::fill (env, env + k0, env[k0]);
            }
        }
        std::fill (t, t + bins, 0.0f);
        for (int c = 0; c < d.numCopies; ++c)
        {
            const float r = std::max (0.01f, d.copyRatio[(size_t) c]);
            const float copyLevel = d.copyLevel[(size_t) c] / std::sqrt (r);
            if (copyLevel <= 0.0f)
                continue;
            const int top = std::min (bins, (int) ((float) (bins - 1) * r));
            if (d.keepFormants)
            {
                for (int k = 0; k < top; ++k)
                {
                    const float from = (float) k / r;
                    // Raised by 18 dB at most, and not at all what came from under the lowest part
                    // of the sound: the floor must not become a note of its own.
                    const float most = from < lowest ? 1.0f : 8.0f;
                    const float lift = std::min (most, env[k] / std::max (1.0e-12f, binAt (env, from, bins)));
                    t[k] += copyLevel * binAt (m, from, bins) * lift;
                }
            }
            else
            {
                for (int k = 0; k < top; ++k)
                    t[k] += copyLevel * binAt (m, (float) k / r, bins);
            }
        }
        std::copy (t, t + bins, m);
    }

    // Frequency shift: every partial moves by the same number of hertz.
    if (d.shiftHz != 0.0f)
    {
        const float shift = d.shiftHz / binHz;
        for (int k = 0; k < bins; ++k)
            t[k] = binAt (m, (float) k - shift, bins);
        std::copy (t, t + bins, m);
    }

    // Seguir: the sample's colour at this moment of the note, band by band, with the frame's energy kept (a colour,
    // not a level).
    if (d.colour != nullptr)
    {
        float* cg = shared.colourGain.data();
        double before = 0.0, after = 0.0;
        for (int k = 0; k < bins; ++k)
        {
            before += (double) m[k] * m[k];
            cg[k] = Dsp::dbToGain (curveGainDb (d.colour, (float) k * binHz));
            m[k] *= cg[k];
            after += (double) m[k] * m[k];
        }
        if (after > 1.0e-20)
        {
            const float back = (float) std::sqrt (before / after);
            for (int k = 0; k < bins; ++k)
                m[k] *= back;
        }
    }

    // The level stays that of the sample at this point, whatever was taken out or added.
    double energyOut = 0.0;
    for (int k = 0; k < bins; ++k)
        energyOut += (double) m[k] * m[k];
    const float scale = energyOut > 1.0e-20 ? (float) std::min (8.0, std::sqrt (energyIn / energyOut)) * level : 0.0f;

    // The drawn curve acts like a filter after that: what it cuts, it cuts.
    if (d.curve != nullptr)
    {
        float* gain = shared.curveGain.data();
        if (binHz != shared.curveBinHz || bins != shared.curveBins || ! std::equal (d.curve, d.curve + curvePoints, shared.curveKey.begin()))
        {
            for (int k = 0; k < bins; ++k)
                gain[k] = Dsp::dbToGain (curveGainDb (d.curve, (float) k * binHz));
            shared.curveBinHz = binHz;
            shared.curveBins = bins;
            std::copy (d.curve, d.curve + curvePoints, shared.curveKey.begin());
        }
        for (int k = 0; k < bins; ++k)
            g[k] = scale * gain[k];
    }
    else
        std::fill (g, g + bins, scale);

    for (int k = 0; k < bins; ++k)
        m[k] *= g[k];
    m[bins - 1] = 0.0f;
}

// Stage 4: new phases, drawn at random. Both channels draw the same ones; the right one moves
// away from them by Estéreo. The two half spectra go back into one complex spectrum.
void Stream::build (const Settings& d, int half) noexcept
{
    if (half == 0 && ! seededForPeaks)
    {
        const juce::int64 seed = rng.nextInt64();
        buildPhases.setSeed (seed);
        buildApart.setSeed (seed ^ 0x5DEECE66DLL);
    }
    if (half == 1)
        seededForPeaks = false;
    auto& phases = buildPhases;
    auto& apart = buildApart;
    const auto& sn = Dsp::sine();
    const float* ml = mag[0].data();
    const float* mr = mag[1].data();

    // The lows stay in the middle: two channels with unrelated phases under 120 Hz half cancel when they
    // are summed to mono (a kick or a sub lost up to 9 dB) and wander in a club. The difference grows
    // smoothly from 120 to 300 Hz and is as before above that.
    const double binHz = d.hostRate / size;
    const int from = half == 0 ? 1 : size / 4, to = half == 0 ? size / 4 : size / 2;
    if (half == 0)
        frame[0] = Complex();
    for (int k = from; k < to; ++k)
    {
        // (0 under 120 Hz and 1 over 300 Hz exactly: the logarithm only where it moves.)
        const double hz = k * binHz;
        double lowsInMiddle = hz >= 300.0 ? 1.0 : 0.0;
        if (hz > 120.0 && hz < 300.0)
        {
            const double x = juce::jlimit (0.0, 1.0, std::log2 (std::max (1.0, hz) / 120.0) / std::log2 (300.0 / 120.0));
            lowsInMiddle = x * x * (3.0 - 2.0 * x);
        }
        double ph = phases.nextDouble();
        // The right channel moves away from the left by at most a quarter of a cycle (at Estéreo 100 %): every
        // frequency stays between in phase and 90 degrees apart, never opposed. Half a cycle each way put about a
        // third of them in opposition: summed to mono the drone lost 2.6 to 5.3 dB, and some notes their colour.
        // Now it loses under 1 dB, and still sounds wide (the two sides are 64 % alike, against 0 before).
        double phr = ph + (double) d.width * lowsInMiddle * 0.5 * (apart.nextDouble() - 0.5);
        float locked = 1.0f;
        if (trueFrequency)
            if (const int tr = peakOf[(size_t) k]; tr >= 0)
            {
                // a bin of a partial's peak: the partial's phase at the frame's middle, moved back by half a cycle per
                // bin (from the middle to the frame's start), the right channel by its own fixed offset
                const auto& t = tracks[(size_t) tr];
                ph = t.theta - 0.5 * k;
                ph -= std::floor (ph);
                phr = ph + t.apart;
                locked = 0.494f;        // frames and the bins of a peak in phase add up in amplitude, not in power: the same level (measured on pure tones)
            }
        const float gl = locked == 1.0f ? ml[k] : ml[k] * locked, gr = locked == 1.0f ? mr[k] : mr[k] * locked;
        const Complex a (gl * sn (ph + 0.25), gl * sn (ph));
        const Complex b (gr * sn (phr + 0.25), gr * sn (phr));
        frame[(size_t) k] = a + Complex (-b.imag(), b.real());                          // a + i b
        frame[(size_t) (size - k)] = std::conj (a) + Complex (b.imag(), b.real());      // conj (a) + i conj (b)
    }
    if (half == 1)
    {
        frame[(size_t) size / 2] = Complex();
        if (trueFrequency)
            lastTracks.swap (tracks);
    }
}

// Stage 5: back to time, under the window, into the ring: left from the real part, right from
// the imaginary part.
void Stream::synthesise (Shared& shared) noexcept
{
    const Complex* z = frame.data();
    const float* win = shared.window (order);
    const float scale = 1.0f / (float) size;        // the inverse transform's 1/n
    float* l = acc[0].data();
    float* r = acc[1].data();
    int n = 0;
#if defined (__SSE2__) || defined (_M_X64)
    // Four at a time where the ring does not wrap, each with (part * window) * scale, then added: the same
    // operations in the same order as below.
    {
        const float* zf = reinterpret_cast<const float*> (z);
        const __m128 S = _mm_set1_ps (scale);
        while (n + 4 <= size)
        {
            const int at = (nextAt + n) & (ringSize - 1);
            const int run = std::min (size - n, ringSize - at) & ~3;
            if (run == 0)       // the last samples before the ring wraps: one at a time
            {
                l[at] += z[n].real() * win[n] * scale;
                r[at] += z[n].imag() * win[n] * scale;
                ++n;
                continue;
            }
            for (int j = 0; j < run; j += 4)
            {
                const int m = n + j;
                const __m128 w01 = _mm_setr_ps (win[m], win[m], win[m + 1], win[m + 1]);
                const __m128 w23 = _mm_setr_ps (win[m + 2], win[m + 2], win[m + 3], win[m + 3]);
                const __m128 p01 = _mm_mul_ps (_mm_mul_ps (_mm_loadu_ps (zf + 2 * m), w01), S);           // (l0, r0, l1, r1)
                const __m128 p23 = _mm_mul_ps (_mm_mul_ps (_mm_loadu_ps (zf + 2 * m + 4), w23), S);
                const __m128 left = _mm_shuffle_ps (p01, p23, _MM_SHUFFLE (2, 0, 2, 0));
                const __m128 right = _mm_shuffle_ps (p01, p23, _MM_SHUFFLE (3, 1, 3, 1));
                _mm_storeu_ps (l + at + j, _mm_add_ps (_mm_loadu_ps (l + at + j), left));
                _mm_storeu_ps (r + at + j, _mm_add_ps (_mm_loadu_ps (r + at + j), right));
            }
            n += run;
        }
    }
#endif
    for (; n < size; ++n)
    {
        const int at = (nextAt + n) & (ringSize - 1);
        l[at] += z[n].real() * win[n] * scale;
        r[at] += z[n].imag() * win[n] * scale;
    }
}
} // namespace Drone
