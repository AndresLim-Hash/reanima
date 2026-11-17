#include "Reconstruct.h"
#include <juce_dsp/juce_dsp.h>
#include <complex>

namespace Reconstruct
{
namespace
{
constexpr float twoPi = juce::MathConstants<float>::twoPi;

inline float wrapPhase (float x) noexcept
{
    return x - twoPi * std::round (x / twoPi);
}

float gaussian (juce::Random& rng) noexcept
{
    return (rng.nextFloat() + rng.nextFloat() + rng.nextFloat() + rng.nextFloat() - 2.0f) * 1.732f;
}

// One analysed frame of the region.
struct Frame
{
    std::vector<float> magL, magR, angL, angR, angM, freq;  // freq: cycles per sample
    std::vector<int> owner;                                 // tonal peak that owns each bin, or -1
    std::vector<int> peaks;
};

// Short enough for the frames to follow how the sound moves, long enough to resolve its
// harmonics (about eight periods when the pitch is known).
int fftOrderFor (int regionLength, double sr, double period)
{
    const double wanted = period > 0.0 ? 8.0 * period : 0.05 * sr;
    int order = 11;
    while ((1 << order) < wanted && order < 15)
        ++order;
    while (order > 10 && (1 << order) > regionLength / 2)
        --order;
    return order;
}

void findPeaks (Frame& f, int bins, float tonalDb, int N)
{
    std::vector<float> logP ((size_t) bins);
    for (int k = 0; k < bins; ++k)
    {
        const float p = f.magL[(size_t) k] * f.magL[(size_t) k] + f.magR[(size_t) k] * f.magR[(size_t) k];
        logP[(size_t) k] = 10.0f * std::log10 (p + 1.0e-14f);
    }

    // Local floor: mean of the log spectrum around each bin.
    const int W = std::max (12, N / 256);
    std::vector<double> prefix ((size_t) bins + 1, 0.0);
    for (int k = 0; k < bins; ++k)
        prefix[(size_t) k + 1] = prefix[(size_t) k] + logP[(size_t) k];

    const float top = *std::max_element (logP.begin(), logP.end());
    f.owner.assign ((size_t) bins, -1);
    f.peaks.clear();

    for (int k = 2; k < bins - 2; ++k)
    {
        const float v = logP[(size_t) k];
        if (v < logP[(size_t) k - 1] || v < logP[(size_t) k + 1] || v < logP[(size_t) k - 2] || v < logP[(size_t) k + 2] || v < top - 80.0f)
            continue;
        const int a = std::max (0, k - W), b = std::min (bins, k + W + 1);
        const float floorDb = (float) ((prefix[(size_t) b] - prefix[(size_t) a]) / (b - a));
        if (v - floorDb < tonalDb)
            continue;
        f.peaks.push_back (k);
    }

    // A peak owns the bins of its main lobe, up to the valley shared with its neighbours.
    for (const int k : f.peaks)
    {
        int lo = k, hi = k;
        while (lo > 1 && k - lo < 5 && logP[(size_t) lo - 1] < logP[(size_t) lo])
            --lo;
        while (hi < bins - 2 && hi - k < 5 && logP[(size_t) hi + 1] < logP[(size_t) hi])
            ++hi;
        for (int b = lo; b <= hi; ++b)
            if (f.owner[(size_t) b] < 0 || std::abs (b - k) < std::abs (b - f.owner[(size_t) b]))
                f.owner[(size_t) b] = k;
    }
}


// Look-ahead limiter over a loop: done here, with time to look ahead, it costs no latency when
// playing. The gain is the minimum of the needs around each sample, then smoothed.
void limitLoop (juce::AudioBuffer<float>& loop, double sr, float ceiling)
{
    const int n = loop.getNumSamples();
    const int reach = std::max (8, (int) (0.002 * sr));
    if (n <= 4 * reach)
        return;

    std::vector<float> need ((size_t) n);
    const float* l = loop.getReadPointer (0);
    const float* r = loop.getReadPointer (1);
    bool anyOver = false;
    for (int i = 0; i < n; ++i)
    {
        const float peak = std::max (std::abs (l[i]), std::abs (r[i]));
        need[(size_t) i] = peak > ceiling ? ceiling / peak : 1.0f;
        anyOver = anyOver || peak > ceiling;
    }
    if (! anyOver)
        return;

    // Sliding minimum over 2 * reach + 1 samples, wrapping around the loop.
    const int window = 2 * reach + 1;
    std::vector<float> lowest ((size_t) n);
    std::vector<int> queue ((size_t) (n + 2 * window));
    int head = 0, tail = 0;
    for (int i = -window + 1; i < n; ++i)
    {
        const int enter = i + reach;
        const float v = need[(size_t) ((enter + 2 * n) % n)];
        while (tail > head && need[(size_t) ((queue[(size_t) tail - 1] + 2 * n) % n)] >= v)
            --tail;
        queue[(size_t) tail++] = enter;
        while (queue[(size_t) head] < i - reach)
            ++head;
        if (i >= 0)
            lowest[(size_t) i] = need[(size_t) ((queue[(size_t) head] + 2 * n) % n)];
    }

    // Mean over the same span: never above what any sample in reach needs.
    double sum = 0.0;
    for (int i = -reach; i <= reach; ++i)
        sum += lowest[(size_t) ((i + n) % n)];

    float* wl = loop.getWritePointer (0);
    float* wr = loop.getWritePointer (1);
    for (int i = 0; i < n; ++i)
    {
        const float g = (float) (sum / window);
        wl[i] *= g;
        wr[i] *= g;
        sum += lowest[(size_t) ((i + reach + 1) % n)] - lowest[(size_t) ((i - reach + n) % n)];
    }
}
}

juce::AudioBuffer<float> flattenedRegion (const juce::AudioBuffer<float>& audio, double sr, Region r)
{
    const int len = std::max (0, r.end - r.start);
    juce::AudioBuffer<float> out (2, len);
    const int ch1 = audio.getNumChannels() > 1 ? 1 : 0;
    const float* L = audio.getReadPointer (0) + r.start;
    const float* R = audio.getReadPointer (ch1) + r.start;

    std::vector<double> prefix ((size_t) len + 1, 0.0);
    for (int i = 0; i < len; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + 0.5 * ((double) L[i] * L[i] + (double) R[i] * R[i]);

    const int half = std::max (32, (int) (0.025 * sr));
    double loudest = 0.0;
    std::vector<float> level ((size_t) len);
    for (int i = 0; i < len; ++i)
    {
        const int a = std::max (0, i - half), b = std::min (len, i + half);
        level[(size_t) i] = (float) std::sqrt ((prefix[(size_t) b] - prefix[(size_t) a]) / std::max (1, b - a));
        loudest = std::max (loudest, (double) level[(size_t) i]);
    }

    const float floorLevel = (float) (loudest * 0.01) + 1.0e-9f;
    for (int i = 0; i < len; ++i)
    {
        const float g = 1.0f / std::max (floorLevel, level[(size_t) i]);
        out.setSample (0, i, L[i] * g);
        out.setSample (1, i, R[i] * g);
    }
    return out;
}

juce::AudioBuffer<float> makeBed (const juce::AudioBuffer<float>& audio, double sr, Region region, const BedOptions& o)
{
    const auto flat = flattenedRegion (audio, sr, region);
    const int len = flat.getNumSamples();
    const int order = fftOrderFor (len, sr, o.period);
    const int N = 1 << order, bins = N / 2 + 1, H = N / 8;
    juce::dsp::FFT fft (order);
    juce::Random rng (o.seed);

    std::vector<float> window ((size_t) N);
    for (int i = 0; i < N; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (twoPi * (float) i / (float) N);

    // Analysis: spectra along the region, with the instantaneous frequency of every bin.
    const int span = std::max (0, len - N);
    const int numFrames = span == 0 ? 1 : juce::jlimit (2, 40, span / (N / 16) + 2);
    const int lag = N / 32;
    std::vector<Frame> frames ((size_t) numFrames);
    std::vector<float> bufL ((size_t) N * 2), bufR ((size_t) N * 2), bufM ((size_t) N * 2), bufLag ((size_t) N * 2);

    auto load = [&] (std::vector<float>& dst, int start, int mode)
    {
        std::fill (dst.begin(), dst.end(), 0.0f);
        for (int i = 0; i < N; ++i)
        {
            const int p = start + i;
            if (p < 0 || p >= len)
                continue;
            const float l = flat.getSample (0, p), r = flat.getSample (1, p);
            dst[(size_t) i] = window[(size_t) i] * (mode == 0 ? l : (mode == 1 ? r : l + r));
        }
        fft.performRealOnlyForwardTransform (dst.data(), true);
    };

    for (int q = 0; q < numFrames; ++q)
    {
        auto& f = frames[(size_t) q];
        const int start = numFrames > 1 ? (int) ((double) q / (numFrames - 1) * span) : 0;
        const int other = start + lag <= span ? start + lag : start - lag;
        const float sign = other > start ? 1.0f : -1.0f;

        load (bufL, start, 0);
        load (bufR, start, 1);
        load (bufM, start, 2);
        load (bufLag, other, 2);

        f.magL.resize ((size_t) bins);
        f.magR.resize ((size_t) bins);
        f.angL.resize ((size_t) bins);
        f.angR.resize ((size_t) bins);
        f.angM.resize ((size_t) bins);
        f.freq.resize ((size_t) bins);

        for (int k = 0; k < bins; ++k)
        {
            const std::complex<float> l (bufL[(size_t) k * 2], bufL[(size_t) k * 2 + 1]);
            const std::complex<float> r (bufR[(size_t) k * 2], bufR[(size_t) k * 2 + 1]);
            const std::complex<float> m (bufM[(size_t) k * 2], bufM[(size_t) k * 2 + 1]);
            const std::complex<float> d (bufLag[(size_t) k * 2], bufLag[(size_t) k * 2 + 1]);
            f.magL[(size_t) k] = std::abs (l);
            f.magR[(size_t) k] = std::abs (r);
            f.angL[(size_t) k] = std::arg (l);
            f.angR[(size_t) k] = std::arg (r);
            f.angM[(size_t) k] = std::abs (m) > 1.0e-3f * (std::abs (l) + std::abs (r)) ? std::arg (m) : std::arg (l);

            const float expected = twoPi * (float) k / (float) N * (float) lag;
            const float dev = wrapPhase ((std::arg (d) - std::arg (m)) * sign - expected);
            f.freq[(size_t) k] = ((float) k / (float) N) + dev / (twoPi * (float) lag);
        }

        findPeaks (f, bins, o.tonalDb, N);
    }

    // Channels detuned from each other: the phase between them keeps turning, and freezing it
    // would leave the sound stuck at one moment of that cycle, even out of phase. Measured over
    // the whole region, and trusted only where it turns steadily (cycles per sample, 0 if not).
    std::vector<float> freqGap ((size_t) bins, 0.0f);
    if (numFrames > 2)
    {
        const float frameStep = (float) span / (float) (numFrames - 1);
        for (int k = 1; k < bins - 1; ++k)
        {
            std::complex<float> turn (0.0f, 0.0f);
            float weight = 0.0f;
            for (int q = 0; q + 1 < numFrames; ++q)
            {
                const auto& a = frames[(size_t) q];
                const auto& b = frames[(size_t) q + 1];
                const float w = std::min ({ a.magL[(size_t) k], a.magR[(size_t) k], b.magL[(size_t) k], b.magR[(size_t) k] });
                const float step = (b.angR[(size_t) k] - b.angL[(size_t) k]) - (a.angR[(size_t) k] - a.angL[(size_t) k]);
                turn += std::polar (w, step);
                weight += w;
            }
            if (weight > 0.0f && std::abs (turn) > 0.8f * weight)
            {
                const float perSample = std::arg (turn) / (twoPi * frameStep);
                if (std::abs (perSample) * (float) span >= 0.5f)     // at least half a turn inside the region
                    freqGap[(size_t) k] = perSample;
            }
        }
    }

    // The noisy part of the spectrum stays steady: a burst inside the region must not come
    // back as a pulse every time the bed passes over it. Tonal peaks keep their own motion.
    if (numFrames > 1)
    {
        for (int k = 0; k < bins; ++k)
        {
            double sumL = 0.0, sumR = 0.0;
            int count = 0;
            for (const auto& f : frames)
                if (f.owner[(size_t) k] < 0)
                {
                    sumL += (double) f.magL[(size_t) k] * f.magL[(size_t) k];
                    sumR += (double) f.magR[(size_t) k] * f.magR[(size_t) k];
                    ++count;
                }
            if (count == 0)
                continue;
            const float meanL = (float) (sumL / count), meanR = (float) (sumR / count);
            for (auto& f : frames)
                if (f.owner[(size_t) k] < 0)
                {
                    f.magL[(size_t) k] = std::sqrt (0.8f * meanL + 0.2f * f.magL[(size_t) k] * f.magL[(size_t) k]);
                    f.magR[(size_t) k] = std::sqrt (0.8f * meanR + 0.2f * f.magR[(size_t) k] * f.magR[(size_t) k]);
                }
        }
    }

    // Resynthesis into a circular buffer, so that the result loops without any seam.
    const int J = std::max (16, (int) std::ceil (o.seconds * sr / H));
    const int T = J * H;
    juce::AudioBuffer<float> bed (2, T);
    bed.clear();

    constexpr float tonalScale = 1.0f / 3.0f;                       // sum of Hann^2 at 8x overlap
    const float noiseScale = tonalScale * std::sqrt (8.0f);         // uncorrelated frames add in power
    const float phaseA = rng.nextFloat(), phaseB = rng.nextFloat();
    const int closing = std::min (J / 3, std::max (8, (int) (1.5 * sr / H)));

    std::vector<float> phi ((size_t) bins, 0.0f), phiPrev ((size_t) bins, 0.0f), phiFirst ((size_t) bins, 0.0f);
    std::vector<float> gap ((size_t) bins, 0.0f), gapPrev ((size_t) bins, 0.0f), gapFirst ((size_t) bins, 0.0f);
    std::vector<int> ownerPrev ((size_t) bins, -1), ownerFirst ((size_t) bins, -1);
    std::vector<float> outL ((size_t) N * 2), outR ((size_t) N * 2);

    auto put = [] (std::vector<float>& dst, int k, float mag, float ang)
    {
        dst[(size_t) k * 2] = mag * std::cos (ang);
        dst[(size_t) k * 2 + 1] = mag * std::sin (ang);
    };

    for (int j = 0; j < J; ++j)
    {
        // Slow periodic wander through the region, so that the bed evolves like the source does.
        const float u = (float) j / (float) J;
        const float wander = 0.5f + 0.5f * (0.6f * std::sin (twoPi * (2.0f * u + phaseA)) + 0.4f * std::sin (twoPi * (5.0f * u + phaseB)));
        const float qf = wander * (float) (numFrames - 1);
        const int qa = juce::jlimit (0, numFrames - 1, (int) qf);
        const int qb = std::min (numFrames - 1, qa + 1);
        const float frac = qf - (float) qa;
        const auto& fa = frames[(size_t) qa];
        const auto& fb = frames[(size_t) qb];
        const auto& f = frac < 0.5f ? fa : fb;

        std::fill (outL.begin(), outL.end(), 0.0f);
        std::fill (outR.begin(), outR.end(), 0.0f);

        // Phase of each tonal peak: continues the peak of the previous frame.
        const int remaining = J - j;
        for (const int k : f.peaks)
        {
            const float advance = twoPi * f.freq[(size_t) k] * (float) H;
            const int before = ownerPrev[(size_t) k];
            const float gapAdvance = twoPi * freqGap[(size_t) k] * (float) H;
            const bool turning = gapAdvance != 0.0f;
            float g = turning && before >= 0 ? gapPrev[(size_t) before] + gapAdvance : f.angR[(size_t) k] - f.angL[(size_t) k];
            float p;
            if (before >= 0)
            {
                p = phiPrev[(size_t) before] + advance;
                const float fade = std::min (1.0f, (float) remaining / (float) closing);
                p += gaussian (rng) * advance * (o.jitterCents / 1731.0f) * fade;
            }
            else
                p = rng.nextFloat() * twoPi;

            // Bring the peak to the phase it had at the start of the loop.
            const int first = ownerFirst[(size_t) k];
            if (j > 0 && remaining <= closing && first >= 0)
            {
                const float atEnd = p + advance * (float) (remaining - 1);
                p += wrapPhase (phiFirst[(size_t) first] - atEnd - advance) / (float) remaining;
                if (turning)
                    g += wrapPhase (gapFirst[(size_t) first] - g - gapAdvance * (float) remaining) / (float) remaining;
            }
            phi[(size_t) k] = wrapPhase (p);
            gap[(size_t) k] = wrapPhase (g);
        }

        for (int k = 1; k < bins - 1; ++k)
        {
            const float ml = fa.magL[(size_t) k] + frac * (fb.magL[(size_t) k] - fa.magL[(size_t) k]);
            const float mr = fa.magR[(size_t) k] + frac * (fb.magR[(size_t) k] - fa.magR[(size_t) k]);
            const int own = f.owner[(size_t) k];
            float rotation, scale, turn = 0.0f;

            if (own >= 0)
            {
                rotation = phi[(size_t) own] - f.angL[(size_t) own];
                turn = gap[(size_t) own] - (f.angR[(size_t) own] - f.angL[(size_t) own]);
                scale = tonalScale;
            }
            else
            {
                rotation = rng.nextFloat() * twoPi;
                scale = noiseScale;
            }

            put (outL, k, ml * scale, f.angL[(size_t) k] + rotation);
            put (outR, k, mr * scale, f.angR[(size_t) k] + rotation + turn);
        }

        fft.performRealOnlyInverseTransform (outL.data());
        fft.performRealOnlyInverseTransform (outR.data());

        float* dl = bed.getWritePointer (0);
        float* dr = bed.getWritePointer (1);
        const int base = j * H;
        for (int i = 0; i < N; ++i)
        {
            const int at = (base + i) % T;
            dl[at] += outL[(size_t) i] * window[(size_t) i];
            dr[at] += outR[(size_t) i] * window[(size_t) i];
        }

        for (const int k : f.peaks)
        {
            phiPrev[(size_t) k] = phi[(size_t) k];
            gapPrev[(size_t) k] = gap[(size_t) k];
        }
        ownerPrev = f.owner;
        if (j == 0)
        {
            phiFirst = phi;
            gapFirst = gap;
            ownerFirst = f.owner;
        }
    }

    // Same balance between centre and sides as the region has when it is heard. Measured before
    // flattening: flattening lifts the quiet end, and a room tail would make the whole bed too wide.
    {
        double srcMid = 0.0, srcSide = 0.0, bedMid = 0.0, bedSide = 0.0;
        const float* srcL = audio.getReadPointer (0);
        const float* srcR = audio.getReadPointer (audio.getNumChannels() > 1 ? 1 : 0);
        for (int i = region.start; i < region.end; ++i)
        {
            srcMid += ((double) srcL[i] + srcR[i]) * ((double) srcL[i] + srcR[i]);
            srcSide += ((double) srcL[i] - srcR[i]) * ((double) srcL[i] - srcR[i]);
        }
        for (int i = 0; i < T; ++i)
        {
            const double x = bed.getSample (0, i), y = bed.getSample (1, i);
            bedMid += (x + y) * (x + y);
            bedSide += (x - y) * (x - y);
        }

        if (srcMid > 1.0e-9 && srcSide > 1.0e-6 * srcMid && bedMid > 1.0e-9 && bedSide > 1.0e-9)
        {
            const double excessDb = juce::jlimit (-24.0, 24.0, 10.0 * std::log10 ((bedSide / bedMid) / (srcSide / srcMid)));
            const float gMid = (float) std::pow (10.0, excessDb / 40.0), gSide = 1.0f / gMid;   // their ratio moves twice, in power
            float* wl = bed.getWritePointer (0);
            float* wr = bed.getWritePointer (1);
            for (int i = 0; i < T; ++i)
            {
                const float m = 0.5f * (wl[i] + wr[i]) * gMid, sd = 0.5f * (wl[i] - wr[i]) * gSide;
                wl[i] = m + sd;
                wr[i] = m - sd;
            }
        }
    }

    double sum = 0.0;
    const float* l = bed.getReadPointer (0);
    const float* r = bed.getReadPointer (1);
    for (int i = 0; i < T; ++i)
    {
        const double m = 0.5 * ((double) l[i] + r[i]);
        sum += m * m;
    }
    bed.applyGain (o.targetRms / (float) std::sqrt (sum / T + 1.0e-18));
    return bed;
}

namespace
{
// The bright bed with the colour of the dark one: its average spectrum turned into the dark
// one's by a zero-phase filter, applied around the loop so that it still has no seam. Both beds
// then have the same waves, and fading from one to the other changes the colour, never the level:
// two beds made apart carry the same tones with phases that drift, and while both sound (a
// sustain held in between, a long decay) they cancel each other for a moment every second or so.
static juce::AudioBuffer<float> recolour (const juce::AudioBuffer<float>& bright, const juce::AudioBuffer<float>& dark, float targetRms)
{
    constexpr int order = 12, size = 1 << order, half = size / 2, bins = half + 1;
    const juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (twoPi * (float) i / (float) size);

    // Average power spectrum of a loop, both channels, frames taken around it.
    auto spectrum = [&] (const juce::AudioBuffer<float>& loop)
    {
        std::vector<double> power ((size_t) bins, 0.0);
        std::vector<float> buf ((size_t) size * 2);
        const int T = loop.getNumSamples();
        const int frames = std::max (4, T / half);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* x = loop.getReadPointer (ch);
            for (int f = 0; f < frames; ++f)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < size; ++i)
                    buf[(size_t) i] = x[(f * half + i) % T] * window[(size_t) i];
                fft.performRealOnlyForwardTransform (buf.data());
                for (int k = 0; k < bins; ++k)
                    power[(size_t) k] += (double) buf[2 * (size_t) k] * buf[2 * (size_t) k] + (double) buf[2 * (size_t) k + 1] * buf[2 * (size_t) k + 1];
            }
        }
        // Smoothed over a sixth of an octave (at least two bins each way): the colour, not the notes.
        std::vector<double> sum ((size_t) bins + 1, 0.0);
        for (int k = 0; k < bins; ++k)
            sum[(size_t) k + 1] = sum[(size_t) k] + power[(size_t) k];
        std::vector<double> smooth ((size_t) bins);
        for (int k = 0; k < bins; ++k)
        {
            const int a = std::max (0, std::min (k - 2, (int) (k * 0.891))), b = std::min (bins - 1, std::max (k + 2, (int) (k * 1.122) + 1));
            smooth[(size_t) k] = (sum[(size_t) b + 1] - sum[(size_t) a]) / (b - a + 1);
        }
        return smooth;
    };

    const auto pb = spectrum (bright), pd = spectrum (dark);
    double totalB = 1.0e-30, totalD = 1.0e-30;
    for (int k = 0; k < bins; ++k)
    {
        totalB += pb[(size_t) k];
        totalD += pd[(size_t) k];
    }

    // The filter: amplitude ratio per bin (as shares of each spectrum, -48 to +12 dB), zero phase,
    // windowed to the FFT length.
    std::vector<float> h ((size_t) size * 2, 0.0f);
    for (int k = 0; k < bins; ++k)
    {
        const double ratio = (pd[(size_t) k] / totalD) / std::max (1.0e-30, pb[(size_t) k] / totalB);
        h[2 * (size_t) k] = (float) juce::jlimit (0.004, 4.0, std::sqrt (ratio));
    }
    fft.performRealOnlyInverseTransform (h.data());
    std::vector<float> kernel ((size_t) size * 2, 0.0f);       // taps -half..half-1, around zero
    for (int i = -half; i < half; ++i)
    {
        const float w = 0.5f + 0.5f * std::cos (twoPi * (float) i / (float) size);
        kernel[(size_t) ((i + size) % size)] = h[(size_t) ((i + size) % size)] * w;
    }
    // Twice the length, so that a block of half the FFT convolved with it never wraps.
    const juce::dsp::FFT big (order + 1);
    std::vector<float> spectrumH ((size_t) size * 4, 0.0f);
    for (int i = 0; i < half; ++i)
        spectrumH[(size_t) i] = kernel[(size_t) i];
    for (int i = 1; i <= half; ++i)
        spectrumH[(size_t) (2 * size - i)] = kernel[(size_t) (size - i)];
    big.performRealOnlyForwardTransform (spectrumH.data());

    // Overlap-add around the loop.
    const int T = bright.getNumSamples();
    juce::AudioBuffer<float> out (2, T);
    out.clear();
    std::vector<float> buf ((size_t) size * 4);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* x = bright.getReadPointer (ch);
        float* y = out.getWritePointer (ch);
        for (int start = 0; start < T; start += size)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            const int count = std::min (size, T - start);
            for (int i = 0; i < count; ++i)
                buf[(size_t) i] = x[start + i];
            big.performRealOnlyForwardTransform (buf.data());
            for (int k = 0; k <= size; ++k)
            {
                const std::complex<float> a (buf[2 * (size_t) k], buf[2 * (size_t) k + 1]), b (spectrumH[2 * (size_t) k], spectrumH[2 * (size_t) k + 1]);
                const auto c = a * b;
                buf[2 * (size_t) k] = c.real();
                buf[2 * (size_t) k + 1] = c.imag();
            }
            big.performRealOnlyInverseTransform (buf.data());
            for (int i = 0; i < 2 * size; ++i)
            {
                const int at = ((start + (i < size + half ? i : i - 2 * size)) % T + T) % T;
                y[at] += buf[(size_t) i];
            }
        }
    }

    double sum = 0.0;
    for (int i = 0; i < T; ++i)
    {
        const double m = 0.5 * ((double) out.getSample (0, i) + out.getSample (1, i));
        sum += m * m;
    }
    out.applyGain (targetRms / (float) std::sqrt (sum / T + 1.0e-18));
    return out;
}

// A bed is meant to hold steady, but where the tones it is made of glide (the pitch drop of a
// kick, a vibrato) their phases can meet opposed for a moment and the loop has a hole, heard
// every time round as the note dropping and coming back. What lies more than 3 dB under the
// typical level of the loop is lifted back to it, by 20 dB at most; nothing is lowered.
static void fillDips (juce::AudioBuffer<float>& loop, double sr)
{
    const int T = loop.getNumSamples();
    const int hop = std::max (64, (int) (0.008 * sr));
    const int points = T / hop;
    if (points < 8)
        return;

    std::vector<float> level ((size_t) points);
    const float* l = loop.getReadPointer (0);
    const float* r = loop.getReadPointer (1);
    for (int p = 0; p < points; ++p)
    {
        double sum = 0.0;
        for (int i = -hop; i < hop; ++i)            // 16 ms around each point, round the loop
        {
            const int at = ((p * hop + i) % T + T) % T;
            const double m = 0.5 * ((double) l[at] + r[at]);
            sum += m * m;
        }
        level[(size_t) p] = (float) std::sqrt (sum / (2 * hop)) + 1.0e-9f;
    }
    auto sorted = level;
    std::nth_element (sorted.begin(), sorted.begin() + points / 2, sorted.end());
    const float floor = 0.708f * sorted[(size_t) points / 2];

    std::vector<float> gain ((size_t) points);
    for (int p = 0; p < points; ++p)
        gain[(size_t) p] = juce::jlimit (1.0f, 10.0f, floor / level[(size_t) p]);
    // Wide enough not to follow single waves, round the loop.
    std::vector<float> smooth ((size_t) points);
    for (int p = 0; p < points; ++p)
    {
        float s = 0.0f;
        for (int q = -2; q <= 2; ++q)
            s += gain[(size_t) ((p + q + points) % points)];
        smooth[(size_t) p] = s / 5.0f;
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        float* x = loop.getWritePointer (ch);
        for (int i = 0; i < T; ++i)
        {
            const float f = (float) i / (float) hop;
            const int a = (int) f % points, b = (a + 1) % points;
            const float w = f - std::floor (f);
            x[i] *= smooth[(size_t) a] + w * (smooth[(size_t) b] - smooth[(size_t) a]);
        }
    }
}

// Slides a region inside the usable part of the sample, keeping a minimum length.
Region fit (const SampleData& s, double start, double end, double minLen)
{
    const double limit = std::max (s.usableEnd, s.onset + 1.0);
    const double shortest = 0.12 * s.sampleRate;    // short samples settle for what they have
    end = std::min (end, limit);
    if (end - start < minLen)
    {
        end = std::min (limit, start + minLen);
        if (end - start < shortest)
            start = std::max (s.onset, end - shortest);
    }
    return { (int) start, std::max ((int) start + 1, (int) end) };
}

double firstBelow (const SampleData& s, double from, float level)
{
    const double step = 0.002 * s.sampleRate;
    for (double p = from; p < s.usableEnd; p += step)
        if (s.envAt (p) < level)
            return p;
    return s.usableEnd;
}
}

Region earlyRegion (const SampleData& s)
{
    const double sr = s.sampleRate;
    const double start = s.peak + std::max (0.012 * sr, (double) s.transientMs * 0.001 * sr);
    const double end = std::min (firstBelow (s, start, 0.125f), start + 1.2 * sr);
    return fit (s, start, end, 0.3 * sr);
}

Region lateRegion (const SampleData& s)
{
    const double sr = s.sampleRate;
    const auto early = earlyRegion (s);
    const double start = std::max ((double) early.start + 0.5 * (early.end - early.start), firstBelow (s, s.peak, 0.2f));
    const double end = std::min (firstBelow (s, start, 0.03f), start + 1.2 * sr);
    return fit (s, start, end, 0.3 * sr);
}

// Tones of the bed and the sample that meet out of phase cancel each other during the
// crossfade, and by a different amount in each note. The bed is read at the point where its
// waves coincide with those of the sample: both advance at the same speed, so they stay together.
static void lineUp (SampleData::Bed& bed, const SampleData& s, Region region)
{
    constexpr int order = 17, size = 1 << order, maxWindow = 8192;
    const int window = std::min ({ maxWindow, region.end - region.start, bed.length });
    if (window < 64)
        return;

    const int start = region.start + (region.end - region.start - window) / 2;
    const float* sampleL = s.audio.getReadPointer (0) + start;
    const float* sampleR = s.audio.getReadPointer (1) + start;
    const float* bedL = bed.audio.getReadPointer (0) + SampleData::Bed::pad;
    const float* bedR = bed.audio.getReadPointer (1) + SampleData::Bed::pad;

    std::vector<float> a ((size_t) size * 2), b ((size_t) size * 2);
    for (int i = 0; i < window; ++i)
        a[(size_t) i] = (sampleL[i] + sampleR[i]) * 0.5f;
    for (int i = 0; i < size; ++i)
    {
        const int at = i % bed.length;
        b[(size_t) i] = (bedL[at] + bedR[at]) * 0.5f;
    }

    // Correlation through the frequency domain: the bed times the conjugate of the sample.
    const juce::dsp::FFT fft (order);
    fft.performRealOnlyForwardTransform (a.data());
    fft.performRealOnlyForwardTransform (b.data());
    for (size_t k = 0; k <= (size_t) size / 2; ++k)
    {
        const std::complex<float> x (a[2 * k], a[2 * k + 1]), y (b[2 * k], b[2 * k + 1]);
        const auto product = y * std::conj (x);
        b[2 * k] = product.real();
        b[2 * k + 1] = product.imag();
    }
    fft.performRealOnlyInverseTransform (b.data());

    const int candidates = std::min (bed.length, size - window);
    const int best = (int) (std::max_element (b.begin(), b.begin() + candidates) - b.begin());

    double sampleEnergy = 1.0e-12, bedEnergy = 1.0e-12, product = 0.0;
    for (int i = 0; i < window; ++i)
    {
        const double x = (sampleL[i] + sampleR[i]) * 0.5, y = (bedL[(best + i) % bed.length] + bedR[(best + i) % bed.length]) * 0.5;
        sampleEnergy += x * x;
        bedEnergy += y * y;
        product += x * y;
    }

    bed.coherence = juce::jlimit (0.0f, 1.0f, (float) (product / std::sqrt (sampleEnergy * bedEnergy)));
    bed.origin = ((best - start) % bed.length + bed.length) % bed.length;
}

void buildBeds (SampleData& s)
{
    auto make = [&s] (Region region, juce::int64 seed)
    {
        BedOptions o;
        o.targetRms = s.peakRms;
        o.period = s.pitched ? s.period : 0.0;
        o.jitterCents = 5.0f;
        o.seed = seed;
        return makeBed (s.audio, s.sampleRate, region, o);
    };

    auto finish = [&s] (SampleData::Bed& bed, juce::AudioBuffer<float> raw)
    {
        // A steady noise needs higher peaks than a short hit to sound as loud. Its level may
        // cost at most 6 dB of limiting, and its peaks never go above those of the sample.
        const float samplePeak = s.audio.getMagnitude (0, s.length);
        const float bedPeak = raw.getMagnitude (0, raw.getNumSamples());
        if (bedPeak > 2.0f * samplePeak)
            raw.applyGain (2.0f * samplePeak / bedPeak);
        limitLoop (raw, s.sampleRate, samplePeak);

        // Margins with the wrapped signal, so that the reader never has to check the ends.
        constexpr int pad = SampleData::Bed::pad;
        const int T = raw.getNumSamples();
        bed.length = T;
        bed.audio.setSize (2, T + 2 * pad);
        for (int ch = 0; ch < 2; ++ch)
        {
            bed.audio.copyFrom (ch, pad, raw, ch, 0, T);
            bed.audio.copyFrom (ch, 0, raw, ch, T - pad, pad);
            bed.audio.copyFrom (ch, pad + T, raw, ch, 0, pad);
        }
    };

    const auto early = earlyRegion (s), late = lateRegion (s);
    s.bedStart = early.start;
    s.bedEnd = early.end;

    // The two analyses do not depend on each other, and they are most of the time a load takes.
    // The dark bed is then the bright one with the late colour (see recolour).
    // The second one runs on a thread of its own, at low priority like the loader that calls this.
    juce::AudioBuffer<float> lateColour;
    juce::WaitableEvent lateDone;
    if (! juce::Thread::launch (juce::Thread::Priority::low, [&] { lateColour = make (late, 23); lateDone.signal(); }))
    {
        lateColour = make (late, 23);
        lateDone.signal();
    }
    auto brightRaw = make (early, 11);
    lateDone.wait();
    fillDips (brightRaw, s.sampleRate);
    auto darkRaw = recolour (brightRaw, lateColour, s.peakRms);

    finish (s.bedBright, std::move (brightRaw));
    lineUp (s.bedBright, s, early);
    finish (s.bedDark, std::move (darkRaw));
    s.bedDark.origin = s.bedBright.origin;
    s.bedDark.coherence = s.bedBright.coherence;

    s.bedQuad.clear();
    if (s.bedBright.length > 0 && s.bedDark.length == s.bedBright.length)
    {
        const int n = s.bedBright.length, pad = SampleData::Bed::pad;
        const float* in[] { s.bedBright.audio.getReadPointer (0) + pad, s.bedBright.audio.getReadPointer (1) + pad,
                            s.bedDark.audio.getReadPointer (0) + pad, s.bedDark.audio.getReadPointer (1) + pad };
        s.bedQuad.resize ((size_t) n * 4);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 4; ++c)
                s.bedQuad[(size_t) i * 4 + (size_t) c] = in[c][i];
    }
}
}
