#include "SampleAnalysis.h"
#include <complex>
#include "Reconstruct.h"
#include "SynthEngine.h"
#include "Mechanism.h"
#include "Params.h"
#include "DspUtil.h"
#include "NoiseSynth.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <numeric>

float AdsrShape::heldLevel (double t) const noexcept
{
    if (t <= 0.0)
        return 0.0f;

    if (t < attack)
        return (float) std::pow (t / attack, (double) curve);

    const double td = t - attack;
    return (float) (sustain * std::pow (10.0, -fadeDbPerSec * td / 20.0)
                     + (1.0 - sustain) * std::exp (-5.0 * td / std::max (0.001f, decay)));
}

float SampleData::envAt (double pos) const noexcept
{
    if (env.empty())
        return 0.0f;

    const double f = juce::jmax (0.0, pos / envHop);
    const auto i = (size_t) f;
    if (i + 1 >= env.size())
        return env.back();

    const float frac = (float) (f - (double) i);
    return env[i] + frac * (env[i + 1] - env[i]);
}

int SampleData::nearestC() const noexcept
{
    return (int) std::floor (rootMidi / 12.0f + 0.5f - 1.0e-4f) * 12;
}

namespace SampleAnalysis
{
namespace
{
constexpr float minPitchHz = 30.0f, maxPitchHz = 2000.0f;

// RMS envelope using a prefix sum of squares, window centred on each hop.
std::vector<float> rmsEnvelope (const std::vector<float>& x, int hop, int window)
{
    const int n = (int) x.size();
    std::vector<double> prefix ((size_t) n + 1, 0.0);
    for (int i = 0; i < n; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) x[(size_t) i] * x[(size_t) i];

    const int frames = n / hop + 1;
    std::vector<float> env ((size_t) frames);
    const int half = std::max (1, window / 2);

    for (int f = 0; f < frames; ++f)
    {
        const int c = f * hop;
        const int a = juce::jlimit (0, n, c - half);
        const int b = juce::jlimit (0, n, c + half);
        env[(size_t) f] = b > a ? (float) std::sqrt ((prefix[(size_t) b] - prefix[(size_t) a]) / (double) (b - a)) : 0.0f;
    }

    return env;
}

// YIN fundamental estimate for one window. Returns {frequency, aperiodicity}.
// The difference function comes from an autocorrelation through the FFT, so that many
// windows can be measured along the whole note.
std::pair<float, float> yin (const float* x, int window, double sr)
{
    const int maxTau = std::min (window / 2, (int) (sr / minPitchHz));
    const int minTau = std::max (2, (int) (sr / maxPitchHz));
    const int w = window / 2;

    int order = 1;
    while ((1 << order) < window + w)
        ++order;
    const int size = 1 << order;
    const juce::dsp::FFT fft (order);

    std::vector<float> head ((size_t) size * 2, 0.0f), all ((size_t) size * 2, 0.0f);
    std::copy (x, x + w, head.begin());
    std::copy (x, x + window, all.begin());
    fft.performRealOnlyForwardTransform (head.data());
    fft.performRealOnlyForwardTransform (all.data());
    for (size_t k = 0; k <= (size_t) size / 2; ++k)
    {
        const std::complex<float> h (head[2 * k], head[2 * k + 1]), a (all[2 * k], all[2 * k + 1]);
        const auto product = a * std::conj (h);
        all[2 * k] = product.real();
        all[2 * k + 1] = product.imag();
    }
    fft.performRealOnlyInverseTransform (all.data());     // all[tau] = sum of x[j] * x[j + tau], j < w

    // d(tau) = energy of the first half + energy of the shifted half - 2 * correlation.
    double first = 0.0;
    for (int j = 0; j < w; ++j)
        first += (double) x[j] * x[j];
    double shifted = first;

    std::vector<float> d ((size_t) maxTau + 1, 0.0f);
    for (int tau = 1; tau <= maxTau; ++tau)
    {
        shifted += (double) x[w + tau - 1] * x[w + tau - 1] - (double) x[tau - 1] * x[tau - 1];
        d[(size_t) tau] = (float) std::max (0.0, first + shifted - 2.0 * all[(size_t) tau]);
    }

    // Cumulative mean normalised difference.
    std::vector<float> cmnd ((size_t) maxTau + 1, 1.0f);
    double running = 0.0;
    for (int tau = 1; tau <= maxTau; ++tau)
    {
        running += d[(size_t) tau];
        cmnd[(size_t) tau] = running > 0.0 ? (float) (d[(size_t) tau] * tau / running) : 1.0f;
    }

    int best = -1;
    for (int tau = minTau; tau < maxTau; ++tau)
    {
        if (cmnd[(size_t) tau] < 0.15f)
        {
            while (tau + 1 < maxTau && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                ++tau;
            best = tau;
            break;
        }
    }

    if (best < 0)
    {
        best = minTau;
        for (int tau = minTau; tau < maxTau; ++tau)
            if (cmnd[(size_t) tau] < cmnd[(size_t) best])
                best = tau;
    }

    // Parabolic interpolation around the minimum.
    double tauF = best;
    if (best > 1 && best < maxTau)
    {
        const double a = cmnd[(size_t) best - 1], b = cmnd[(size_t) best], c = cmnd[(size_t) best + 1];
        const double denom = a - 2.0 * b + c;
        if (std::abs (denom) > 1.0e-12)
            tauF += 0.5 * (a - c) / denom;
    }

    return { (float) (sr / tauF), cmnd[(size_t) best] };
}

float median (std::vector<float> v)
{
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

struct DecayFit { float sustain, decay, fade; };

// Grid search of  s*10^(-fade t/20) + (1-s) e^(-5t/d)  against the envelope (in dB) after the peak.
// Both terms are exponentials, and the search could hand the quick fall to the sustain and the slow one
// to the decay: then a higher Sustain made the note shorter (the knob worked backwards). `ordered` is
// the best fit whose sustain never falls faster than its decay (the decay loses 43.4 dB in d); `any`
// the best of all, as up to 0.9.5, kept for the projects saved then.
struct DecayFits { DecayFit ordered, any; };

DecayFits fitDecay (const std::vector<float>& env, int peakFrame, int endFrame, double frameSeconds)
{
    const int span = endFrame - peakFrame;
    if (span < 4)
        return { { 1.0f, 0.05f, 0.0f }, { 1.0f, 0.05f, 0.0f } };

    const int points = std::min (span, 400);
    std::vector<double> t ((size_t) points), y ((size_t) points);
    for (int i = 0; i < points; ++i)
    {
        const int fr = peakFrame + (int) ((double) i * span / points);
        t[(size_t) i] = (fr - peakFrame) * frameSeconds;
        y[(size_t) i] = 20.0 * std::log10 (std::max (1.0e-3f, env[(size_t) fr]));
    }

    const double total = std::max (0.01, span * frameSeconds);
    std::vector<double> sValues { 0.0 };
    for (int i = 0; i < 30; ++i)
        sValues.push_back (0.003 * std::pow (1.0 / 0.003, i / 29.0));

    static const double fades[] = { 0, 0.5, 1, 2, 3, 5, 7.5, 10, 15, 20, 30, 40, 60, 80, 120 };
    constexpr size_t numFades = std::size (fades);

    std::vector<double> fadeCurves (numFades * (size_t) points), decayCurve ((size_t) points);
    for (size_t f = 0; f < numFades; ++f)
        for (int i = 0; i < points; ++i)
            fadeCurves[f * (size_t) points + (size_t) i] = std::pow (10.0, -fades[f] * t[(size_t) i] / 20.0);

    DecayFit best { 1.0f, 0.05f, 0.0f }, bestOrdered = best;
    double bestErr = 1.0e300, bestOrderedErr = 1.0e300;

    for (int di = 0; di < 40; ++di)
    {
        const double d = 0.005 * std::pow (std::max (2.0, total / 0.005), di / 39.0);
        for (int i = 0; i < points; ++i)
            decayCurve[(size_t) i] = std::exp (-5.0 * t[(size_t) i] / d);

        for (const double s : sValues)
        {
            for (size_t f = 0; f < numFades; ++f)
            {
                const double* fadeCurve = fadeCurves.data() + f * (size_t) points;
                const bool ordered = s <= 0.0 || s >= 1.0 || fades[f] * d <= 43.4;
                const double bound = ordered ? std::max (bestErr, bestOrderedErr) : bestErr;
                double err = 0.0;
                for (int i = 0; i < points && err < bound; ++i)
                {
                    const double lvl = s * fadeCurve[i] + (1.0 - s) * decayCurve[(size_t) i];
                    const double e = 20.0 * std::log10 (std::max (1.0e-3, lvl)) - y[(size_t) i];
                    err += e * e;
                }

                if (err < bestErr)
                {
                    bestErr = err;
                    best = { (float) s, (float) d, (float) fades[f] };
                }
                if (ordered && err < bestOrderedErr)
                {
                    bestOrderedErr = err;
                    bestOrdered = { (float) s, (float) d, (float) fades[f] };
                }
            }
        }
    }

    // A sustain at the top has no decay to measure (every d fits alike, and the first tried, 5 ms, stayed):
    // turned down, the note then fell in a click. It gets a decay a hand would set, within the order.
    if (bestOrdered.sustain >= 1.0f)
        bestOrdered.decay = (float) std::min (0.3, bestOrdered.fade > 0.0f ? 43.4 / bestOrdered.fade : 0.3);
    return { bestOrdered, best };
}

// Random-phase spectral freeze, used when no partials were tracked.
void spectralFreeze (const SampleData& d, double from, double to, juce::AudioBuffer<float>& out)
{
    constexpr int analysisOrder = 12, analysisSize = 1 << analysisOrder;
    constexpr int outOrder = 17, outSize = 1 << outOrder;
    constexpr int bins = analysisSize / 2 + 1;

    juce::dsp::FFT analysisFft (analysisOrder), outFft (outOrder);
    juce::dsp::WindowingFunction<float> window (analysisSize, juce::dsp::WindowingFunction<float>::hann, false);
    juce::Random rng (0x5eed);

    out.setSize (2, outSize);
    out.clear();

    const int start = juce::jlimit (0, std::max (0, d.length - analysisSize), (int) from);
    const int stop = juce::jlimit (start, std::max (start, d.length - analysisSize), (int) to);

    for (int ch = 0; ch < 2; ++ch)
    {
        std::vector<float> mag ((size_t) bins, 0.0f);
        std::vector<float> frame ((size_t) analysisSize * 2);
        int frames = 0;
        double sumSq = 0.0;
        int count = 0;

        for (int pos = start; pos <= stop; pos += analysisSize / 4)
        {
            std::fill (frame.begin(), frame.end(), 0.0f);
            const float* src = d.audio.getReadPointer (ch);
            for (int i = 0; i < analysisSize && pos + i < d.length; ++i)
            {
                frame[(size_t) i] = src[pos + i];
                sumSq += (double) src[pos + i] * src[pos + i];
                ++count;
            }

            window.multiplyWithWindowingTable (frame.data(), (size_t) analysisSize);
            analysisFft.performFrequencyOnlyForwardTransform (frame.data());

            for (int k = 0; k < bins; ++k)
                mag[(size_t) k] += frame[(size_t) k];
            ++frames;
        }

        if (frames == 0 || count == 0)
            continue;

        std::vector<float> spec ((size_t) outSize * 2, 0.0f);
        const double ratio = (double) analysisSize / outSize;
        for (int k = 1; k < outSize / 2; ++k)
        {
            const double a = k * ratio;
            const int i0 = (int) a;
            const float frac = (float) (a - i0);
            const float m = (i0 + 1 < bins) ? mag[(size_t) i0] + frac * (mag[(size_t) i0 + 1] - mag[(size_t) i0]) : 0.0f;
            const float phase = rng.nextFloat() * juce::MathConstants<float>::twoPi;
            spec[(size_t) k * 2] = m * std::cos (phase);
            spec[(size_t) k * 2 + 1] = m * std::sin (phase);
        }

        outFft.performRealOnlyInverseTransform (spec.data());

        const double targetRms = std::sqrt (sumSq / count);
        double outSq = 0.0;
        for (int i = 0; i < outSize; ++i)
            outSq += (double) spec[(size_t) i] * spec[(size_t) i];
        const double outRms = std::sqrt (outSq / outSize);
        const float gain = outRms > 0.0 ? (float) (targetRms / outRms) : 0.0f;

        float* dst = out.getWritePointer (ch);
        for (int i = 0; i < outSize; ++i)
            dst[i] = spec[(size_t) i] * gain;
    }
}

int orderFor (int size)
{
    int order = 0;
    while ((1 << order) < size)
        ++order;
    return order;
}

// YIN can lock onto a strong upper harmonic (a fifth above), onto a sub-octave, or miss a
// fundamental that is not in the spectrum at all, as in many basses. The note is the
// fundamental whose harmonics explain the spectral peaks best: candidates are the YIN
// estimate, the strongest peaks, and their sub-multiples. Among those that explain about
// as much, the highest wins, so that an octave below never wins by being lower.
// The pitch a tuner reads: the sustained part, past the attack (where many sounds start sharp or flat).
// YIN's difference function, searched only within 6 % of the period already found (no octave can be
// mistaken for another), on windows across the note; the median of the clear ones. 0 if too few.
float steadyPitch (const std::vector<float>& mono, double sr, int from, int to, float hzGuess)
{
    const double period = sr / hzGuess;
    const int tauLo = std::max (2, (int) (period * 0.94)), tauHi = (int) (period * 1.06) + 2;
    const int win = std::max (2048, (int) (period * 4.0)), hop = win / 2;
    to = std::min (to, (int) mono.size() - win - tauHi - 1);
    std::vector<float> estimates;
    std::vector<double> d ((size_t) tauHi + 2);
    for (int s = from; s < to; s += hop)
    {
        double energy = 0.0;
        for (int i = 0; i < win; ++i)
            energy += (double) mono[(size_t) (s + i)] * mono[(size_t) (s + i)];
        if (energy < 1.0e-8 * win)
            continue;
        for (int tau = tauLo - 1; tau <= tauHi + 1; ++tau)
        {
            double sum = 0.0;
            for (int i = 0; i < win; ++i)
            {
                const double diff = mono[(size_t) (s + i)] - mono[(size_t) (s + i + tau)];
                sum += diff * diff;
            }
            d[(size_t) tau] = sum;
        }
        int best = tauLo;
        for (int tau = tauLo; tau <= tauHi; ++tau)
            if (d[(size_t) tau] < d[(size_t) best])
                best = tau;
        if (d[(size_t) best] > 0.3 * 2.0 * energy)      // not periodic enough here
            continue;
        const double a = d[(size_t) best - 1], b = d[(size_t) best], c = d[(size_t) best + 1];
        const double den = a - 2.0 * b + c;
        const double off = std::abs (den) > 1.0e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / den) : 0.0;
        estimates.push_back ((float) (sr / (best + off)));
    }
    if (estimates.size() < 3)
        return 0.0f;
    std::nth_element (estimates.begin(), estimates.begin() + (long) estimates.size() / 2, estimates.end());
    return estimates[estimates.size() / 2];
}

float refineFundamental (const std::vector<float>& mono, double sr, double from, float f0)
{
    const int n = (int) mono.size();
    const int N = juce::nextPowerOfTwo ((int) (sr * 0.34));
    juce::dsp::FFT fft (orderFor (N));
    std::vector<float> buf ((size_t) N * 2, 0.0f);
    const int start = juce::jlimit (0, std::max (0, n - N), (int) from);
    for (int i = 0; i < N && start + i < n; ++i)
        buf[(size_t) i] = mono[(size_t) (start + i)] * (0.5f - 0.5f * std::cos (Dsp::twoPi * (float) i / (float) N));
    fft.performFrequencyOnlyForwardTransform (buf.data(), true);

    const float binHz = (float) (sr / N);
    const int kmax = std::min (N / 2 - 2, (int) (5000.0f / binHz));
    float maxMag = 0.0f;
    for (int k = 2; k < kmax; ++k)
        maxMag = std::max (maxMag, buf[(size_t) k]);
    if (maxMag <= 0.0f)
        return f0;

    struct Peak { float hz, energy; };
    std::vector<Peak> peaks;
    for (int k = 2; k < kmax; ++k)
    {
        const float m = buf[(size_t) k];
        if (m > maxMag * 0.0056f && m > buf[(size_t) k - 1] && m >= buf[(size_t) k + 1] && (float) k * binHz > 20.0f)
        {
            const float a = buf[(size_t) k - 1], c = buf[(size_t) k + 1];
            const float denom = a - 2.0f * m + c;
            const float p = std::abs (denom) > 1.0e-12f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denom) : 0.0f;
            peaks.push_back ({ ((float) k + p) * binHz, m });     // amplitude: weak partials still shape the pitch we hear
        }
    }
    if (peaks.empty())
        return f0;

    double total = 0.0;
    for (const auto& pk : peaks)
        total += pk.energy;

    // Share of the spectrum explained by the first harmonics of c, and the fundamental
    // that those harmonics agree on.
    constexpr int maxHarmonic = 12;
    const auto explain = [&] (float c, float& fitted)
    {
        double explained = 0.0, weighted = 0.0, weights = 0.0;
        for (const auto& pk : peaks)
        {
            const float h = std::round (pk.hz / c);
            if (h < 1.0f || h > (float) maxHarmonic)
                continue;
            if (std::abs (pk.hz - h * c) < std::max (0.02f * c, 1.5f * binHz))
            {
                explained += pk.energy;
                if (h <= 8.0f)
                {
                    weighted += pk.energy * pk.hz / h;
                    weights += pk.energy;
                }
            }
        }
        fitted = weights > 0.0 ? (float) (weighted / weights) : c;
        return explained / total;
    };

    std::vector<float> candidates;
    for (int m = 1; m <= 5; ++m)
        candidates.push_back (f0 / (float) m);

    auto strongest = peaks;
    std::sort (strongest.begin(), strongest.end(), [] (const Peak& a, const Peak& b) { return a.energy > b.energy; });
    for (size_t i = 0; i < std::min<size_t> (3, strongest.size()); ++i)
        for (int m = 1; m <= 5; ++m)
            candidates.push_back (strongest[i].hz / (float) m);

    double bestScore = 0.0;
    for (const float c : candidates)
    {
        float fitted;
        if (c >= minPitchHz)
            bestScore = std::max (bestScore, explain (c, fitted));
    }

    float chosen = 0.0f;
    for (const float c : candidates)
    {
        float fitted;
        if (c >= minPitchHz && c > chosen && explain (c, fitted) >= 0.92 * bestScore)
            chosen = c;
    }
    if (chosen <= 0.0f)
        return f0;

    // YIN measures a steady pitch more finely than the spectrum: keep it when it agrees.
    if (std::abs (12.0f * std::log2 (chosen / f0)) < 0.3f)
        return f0;
    float fitted;
    explain (chosen, fitted);
    return fitted;
}

// Sines + noise analysis: partials picked in a reference frame after the peak and
// tracked forwards and backwards with a Blackman-Harris STFT; the residual after
// subtracting the resynthesised partials is stored as band amplitudes per frame.
// Engine C plays the partials it measured. On a thick sound (detuned layers, chorus) their peaks can sit off
// the note the ear and a tuner hear (26 cents on a lead of the library): all of them are moved together, at
// most half a semitone, so that their harmonics agree with the root found in the sustained part.
void tunePartials (SampleData& d)
{
    auto& pm = d.partials;
    if (! d.pitched || ! pm.valid())
        return;
    const double root = 440.0 * std::exp2 ((d.rootMidi - 69.0) / 12.0);
    std::vector<std::pair<float, float>> votes;     // fundamental implied, weight
    for (int f = 0; f <= pm.lastFrame && f < pm.numFrames; ++f)
        for (int k = 0; k < pm.numPartials; ++k)
        {
            const size_t i = (size_t) f * pm.numPartials + k;
            const float hz = pm.freq[i], a = pm.amp[i];
            const float h = std::round (hz / (float) root);
            if (h >= 1.0f && h <= 12.0f && a > 0.0f && std::abs (hz / h - (float) root) < 0.03f * (float) root)
                votes.push_back ({ hz / h, a / h });
        }
    if (votes.size() < 8)
        return;
    std::sort (votes.begin(), votes.end());
    double total = 0.0, run = 0.0;
    for (const auto& v : votes)
        total += v.second;
    float median = votes.front().first;
    for (const auto& v : votes)
        if ((run += v.second) >= 0.5 * total)
        {
            median = v.first;
            break;
        }
    const float ratio = (float) root / median;
    if (std::abs (1200.0f * std::log2 (ratio)) > 50.0f || std::abs (1200.0f * std::log2 (ratio)) < 0.5f)
        return;
    for (auto& hz : pm.freq)
        hz *= ratio;
    pm.refFreq *= ratio;
}

void analysePartials (SampleData& d, const std::vector<float>& mono)
{
    auto& pm = d.partials;
    const double sr = d.sampleRate;
    const int n = std::min (d.length, (int) (sr * 20.0));
    if (n < 2048)
        return;

    pm.hop = sr > 60000.0 ? 512 : 256;
    const int hop = pm.hop;
    const double rateScale = sr / 48000.0;
    int N = juce::nextPowerOfTwo ((int) std::max (2048.0 * rateScale, d.pitched ? 6.0 * d.period : 0.0));
    N = std::min (N, juce::nextPowerOfTwo ((int) (8192.0 * rateScale)));
    const int fftSize = N * 2;
    juce::dsp::FFT fft (orderFor (fftSize));
    const int bins = fftSize / 2;
    const float binHz = (float) (sr / fftSize);

    std::vector<float> w ((size_t) N);
    double wsum = 0.0;
    for (int i = 0; i < N; ++i)
    {
        const double x = juce::MathConstants<double>::twoPi * i / (N - 1);
        w[(size_t) i] = (float) (0.35875 - 0.48829 * std::cos (x) + 0.14128 * std::cos (2 * x) - 0.01168 * std::cos (3 * x));
        wsum += w[(size_t) i];
    }
    const float ampScale = (float) (2.0 / wsum);

    pm.numFrames = n / hop + 1;
    const int kmin = (int) std::ceil (25.0f / binHz) + 1;
    const int kmax = std::min (bins - 2, (int) (std::min (18000.0, 0.45 * sr) / binHz));

    std::vector<float> bufL ((size_t) fftSize * 2), bufR ((size_t) fftSize * 2);
    std::vector<float> magDb ((size_t) bins + 1), re ((size_t) bins + 1), im ((size_t) bins + 1), magL ((size_t) bins + 1), magR ((size_t) bins + 1);
    const float* L = d.audio.getReadPointer (0);
    const float* R = d.audio.getReadPointer (1);

    auto analyseFrame = [&] (int f)
    {
        std::fill (bufL.begin(), bufL.end(), 0.0f);
        std::fill (bufR.begin(), bufR.end(), 0.0f);
        const int c = f * hop;
        for (int i = 0; i < N; ++i)
        {
            const int t = c - N / 2 + i;
            if (t < 0 || t >= d.length)
                continue;
            const int pos = (i - N / 2 + fftSize) % fftSize;   // zero-phase: frame centre at index 0
            bufL[(size_t) pos] = L[t] * w[(size_t) i];
            bufR[(size_t) pos] = R[t] * w[(size_t) i];
        }
        fft.performRealOnlyForwardTransform (bufL.data(), true);
        fft.performRealOnlyForwardTransform (bufR.data(), true);

        for (int k = 0; k <= bins; ++k)
        {
            const float rl = bufL[(size_t) k * 2], il = bufL[(size_t) k * 2 + 1];
            const float rr = bufR[(size_t) k * 2], ir = bufR[(size_t) k * 2 + 1];
            re[(size_t) k] = 0.5f * (rl + rr);
            im[(size_t) k] = 0.5f * (il + ir);
            const float m = std::sqrt (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]);
            magDb[(size_t) k] = 20.0f * std::log10 (std::max (m * ampScale, 1.0e-9f));
            magL[(size_t) k] = std::sqrt (rl * rl + il * il);
            magR[(size_t) k] = std::sqrt (rr * rr + ir * ir);
        }
    };

    auto interpolate = [&] (int k, float& bin, float& db)
    {
        const float a = magDb[(size_t) k - 1], b = magDb[(size_t) k], c = magDb[(size_t) k + 1];
        const float denom = a - 2.0f * b + c;
        const float p = std::abs (denom) > 1.0e-9f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denom) : 0.0f;
        bin = (float) k + p;
        db = b - 0.25f * (a - c) * p;
    };

    // Reference frame: 30 ms after the peak.
    const int refFrame = juce::jlimit (0, pm.numFrames - 1, (int) std::round ((d.peak + 0.03 * sr) / hop));
    analyseFrame (refFrame);

    float maxDb = -200.0f;
    for (int k = kmin; k <= kmax; ++k)
        maxDb = std::max (maxDb, magDb[(size_t) k]);
    const float floorDb = std::max (maxDb - 70.0f, -110.0f);

    struct Peak { float bin, db; };
    std::vector<Peak> peaks;
    for (int k = kmin + 1; k < kmax; ++k)
    {
        if (magDb[(size_t) k] > floorDb && magDb[(size_t) k] > magDb[(size_t) k - 1] && magDb[(size_t) k] >= magDb[(size_t) k + 1])
        {
            Peak pk;
            interpolate (k, pk.bin, pk.db);
            peaks.push_back (pk);
        }
    }
    std::sort (peaks.begin(), peaks.end(), [] (const Peak& a, const Peak& b) { return a.db > b.db; });

    constexpr int maxPartials = 128;
    std::vector<Peak> chosen;
    for (const auto& pk : peaks)
    {
        if ((int) chosen.size() >= maxPartials)
            break;
        bool clash = false;
        for (const auto& c : chosen)
            clash = clash || std::abs (c.bin - pk.bin) < 6.0f;
        if (! clash)
            chosen.push_back (pk);
    }
    if (chosen.empty())
        return;

    std::sort (chosen.begin(), chosen.end(), [] (const Peak& a, const Peak& b) { return a.bin < b.bin; });
    const int K = (int) chosen.size();
    pm.numPartials = K;
    pm.amp.assign ((size_t) pm.numFrames * K, 0.0f);
    pm.freq.assign ((size_t) pm.numFrames * K, 0.0f);
    pm.phase.assign ((size_t) pm.numFrames * K, 0.0f);
    std::vector<double> panSum ((size_t) K, 0.0), panWeight ((size_t) K, 0.0);

    for (const auto& c : chosen)
    {
        if (c.db > maxDb - 30.0f)
        {
            pm.refFreq = std::max (20.0f, c.bin * binHz);
            break;
        }
    }

    std::vector<float> current ((size_t) K);
    auto track = [&] (int f)
    {
        analyseFrame (f);
        for (int k = 0; k < K; ++k)
        {
            const float prev = current[(size_t) k];
            float radius = std::max (3.0f, prev * 0.02f);
            if (k > 0) radius = std::min (radius, 0.45f * (prev - current[(size_t) k - 1]));
            if (k + 1 < K) radius = std::min (radius, 0.45f * (current[(size_t) k + 1] - prev));
            radius = std::max (1.0f, radius);

            const int lo = std::max (kmin, (int) std::floor (prev - radius));
            const int hi = std::min (kmax - 1, (int) std::ceil (prev + radius));
            int best = -1;
            for (int j = std::max (lo, 1); j <= hi; ++j)
                if (magDb[(size_t) j] > magDb[(size_t) j - 1] && magDb[(size_t) j] >= magDb[(size_t) j + 1]
                    && (best < 0 || magDb[(size_t) j] > magDb[(size_t) best]))
                    best = j;

            float bin = prev, db;
            if (best >= 0 && magDb[(size_t) best] > floorDb - 20.0f)
            {
                interpolate (best, bin, db);
                current[(size_t) k] = bin;
            }
            else
                db = magDb[(size_t) juce::jlimit (0, bins, (int) std::round (prev))];

            const int j0 = juce::jlimit (0, bins, (int) std::round (bin));
            const size_t idx = (size_t) f * K + k;
            pm.amp[idx] = std::pow (10.0f, db / 20.0f);
            pm.freq[idx] = bin * binHz;
            pm.phase[idx] = std::atan2 (im[(size_t) j0], re[(size_t) j0]) / juce::MathConstants<float>::twoPi;

            const float ml = magL[(size_t) j0], mr = magR[(size_t) j0];
            panSum[(size_t) k] += pm.amp[idx] * (mr - ml) / (mr + ml + 1.0e-12f);
            panWeight[(size_t) k] += pm.amp[idx];
        }
    };

    for (int k = 0; k < K; ++k) current[(size_t) k] = chosen[(size_t) k].bin;
    for (int f = refFrame; f < pm.numFrames; ++f) track (f);
    for (int k = 0; k < K; ++k) current[(size_t) k] = chosen[(size_t) k].bin;
    for (int f = refFrame - 1; f >= 0; --f) track (f);

    // The partials of a frame cannot hold more power than the sound has there (measured over the same window).
    // Peaks read off a noisy spectrum each take the level of its loudest bin near them, and many of them added up
    // to more than the whole: they are scaled down together to it.
    {
        double wPower = 0.0;
        for (const float x : w)
            wPower += (double) x * x;
        for (int f = 0; f < pm.numFrames; ++f)
        {
            const int c = f * hop;
            double power = 0.0;
            for (int i = 0; i < N; ++i)
            {
                const int t = c - N / 2 + i;
                if (t >= 0 && t < (int) mono.size())
                    power += (double) (mono[(size_t) t] * w[(size_t) i]) * (mono[(size_t) t] * w[(size_t) i]);
            }
            power /= wPower;
            double partials = 0.0;
            for (int k = 0; k < K; ++k)
                partials += 0.5 * (double) pm.amp[(size_t) f * K + k] * pm.amp[(size_t) f * K + k];
            if (partials > power && partials > 0.0)
            {
                const float scale = (float) std::sqrt (power / partials);
                for (int k = 0; k < K; ++k)
                    pm.amp[(size_t) f * K + k] *= scale;
            }
        }
    }

    pm.pan.resize ((size_t) K);
    for (int k = 0; k < K; ++k)
        pm.pan[(size_t) k] = panWeight[(size_t) k] > 0.0 ? (float) (panSum[(size_t) k] / panWeight[(size_t) k]) : 0.0f;

    // Last audible frame and each partial's natural decay rate over the tail.
    {
        std::vector<double> energy ((size_t) pm.numFrames, 0.0);
        double maxEnergy = 0.0;
        for (int f = 0; f < pm.numFrames; ++f)
        {
            for (int k = 0; k < K; ++k)
                energy[(size_t) f] += (double) pm.amp[(size_t) f * K + k] * pm.amp[(size_t) f * K + k];
            maxEnergy = std::max (maxEnergy, energy[(size_t) f]);
        }
        pm.lastFrame = pm.numFrames - 1;
        while (pm.lastFrame > refFrame && energy[(size_t) pm.lastFrame] < maxEnergy * 1.0e-6)
            --pm.lastFrame;
        // (only for a file cut while it still sounds, its last frame within 30 dB of the loudest: one that fades
        // out to its end reads its last frames as they are, and its tail falls as it was measured)
        pm.holdFrame = energy[(size_t) pm.lastFrame] >= maxEnergy * 1.0e-3 ? juce::jlimit (0, pm.lastFrame, (n - N / 2) / hop) : pm.lastFrame;

        pm.decayDbPerSec.assign ((size_t) K, std::max (3.0f, d.detected.fadeDbPerSec));
        const int from = std::max (refFrame, pm.lastFrame / 2);
        for (int k = 0; k < K; ++k)
        {
            float peakAmp = 0.0f;
            for (int f = 0; f < pm.numFrames; ++f)
                peakAmp = std::max (peakAmp, pm.amp[(size_t) f * K + k]);

            double st = 0, sy = 0, stt = 0, sty = 0;
            int count = 0;
            for (int f = from; f <= pm.lastFrame; ++f)
            {
                const float a = pm.amp[(size_t) f * K + k];
                if (a < peakAmp * 1.0e-3f)
                    continue;
                const double t = (double) f * hop / sr, y = 20.0 * std::log10 (a);
                st += t; sy += y; stt += t * t; sty += t * y;
                ++count;
            }
            if (count >= 4)
            {
                const double slope = (count * sty - st * sy) / std::max (1.0e-12, count * stt - st * st);
                pm.decayDbPerSec[(size_t) k] = (float) juce::jlimit (0.3, 400.0, -slope);
            }
        }
    }

    // Residual = signal - resynthesised partials (overlap-add with triangular windows).
    std::vector<float> residual (mono.begin(), mono.begin() + n);
    {
        const auto& sn = Dsp::sine();
        for (int f = 0; f < pm.numFrames; ++f)
        {
            const int c = f * hop;
            for (int k = 0; k < K; ++k)
            {
                const size_t idx = (size_t) f * K + k;
                const float a = pm.amp[idx];
                if (a < 1.0e-6f)
                    continue;
                const double inc = pm.freq[idx] / sr;
                for (int t = std::max (0, c - hop); t < std::min (n, c + hop); ++t)
                {
                    const float tri = 1.0f - (float) std::abs (t - c) / (float) hop;
                    residual[(size_t) t] -= tri * a * sn (pm.phase[idx] + 0.25 + inc * (t - c));
                }
            }
        }
    }

    // Noise: band amplitudes of the residual with the synthesiser's FFT size and window.
    {
        const float lo = 40.0f, hi = (float) (0.48 * sr);
        for (int b = 0; b <= SampleData::PartialModel::numBands; ++b)
            pm.bandEdges[(size_t) b] = lo * std::pow (hi / lo, (float) b / SampleData::PartialModel::numBands);

        constexpr int size = NoiseSynth::size;
        const auto& win = NoiseSynth::window();
        std::vector<float> buf ((size_t) size * 2), orig ((size_t) size * 2);
        std::vector<char> covered ((size_t) size / 2 + 1);
        pm.noise.assign ((size_t) pm.numFrames * SampleData::PartialModel::numBands, 0.0f);
        const float nBinHz = (float) (sr / size);

        for (int f = 0; f < pm.numFrames; ++f)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            const int c = f * hop;
            for (int i = 0; i < size; ++i)
            {
                const int t = c - size / 2 + i;
                if (t >= 0 && t < n)
                    buf[(size_t) i] = residual[(size_t) t] * win[(size_t) i];
            }
            NoiseSynth::fft().performRealOnlyForwardTransform (buf.data(), true);

            // The same frame of the sound itself, and the bins its partials do not cover (more than 2 bins from
            // each, the main lobe of the window): the noise between them, measured without the partials. Where the
            // partials taken away from the sound do not cancel it exactly (a phase a little off) what is left was
            // counted as noise, and engine C sounded noisier than the sample (39 of 321 one-shots, the most by
            // 19 dB of flatness): each band takes the lower of the two measures.
            std::fill (orig.begin(), orig.end(), 0.0f);
            for (int i = 0; i < size; ++i)
            {
                const int t = c - size / 2 + i;
                if (t >= 0 && t < n)
                    orig[(size_t) i] = mono[(size_t) t] * win[(size_t) i];
            }
            NoiseSynth::fft().performRealOnlyForwardTransform (orig.data(), true);
            std::fill (covered.begin(), covered.end(), (char) 0);
            for (int k = 0; k < K; ++k)
                if (pm.amp[(size_t) f * K + k] > 1.0e-6f)
                {
                    const int at = (int) std::round (pm.freq[(size_t) f * K + k] / nBinHz);
                    for (int j = std::max (0, at - 2); j <= std::min (size / 2, at + 2); ++j)
                        covered[(size_t) j] = 1;
                }

            for (int b = 0; b < SampleData::PartialModel::numBands; ++b)
            {
                int k0 = (int) std::ceil (pm.bandEdges[(size_t) b] / nBinHz);
                int k1 = (int) std::floor (pm.bandEdges[(size_t) b + 1] / nBinHz);
                k1 = std::min (k1, size / 2 - 1);
                if (k1 < k0) k1 = k0 = juce::jlimit (1, size / 2 - 1, (int) std::round (0.5f * (pm.bandEdges[(size_t) b] + pm.bandEdges[(size_t) b + 1]) / nBinHz));
                double power = 0.0, between = 0.0;
                int free = 0;
                for (int k = k0; k <= k1; ++k)
                {
                    power += (double) buf[(size_t) k * 2] * buf[(size_t) k * 2] + (double) buf[(size_t) k * 2 + 1] * buf[(size_t) k * 2 + 1];
                    if (! covered[(size_t) k])
                    {
                        between += (double) orig[(size_t) k * 2] * orig[(size_t) k * 2] + (double) orig[(size_t) k * 2 + 1] * orig[(size_t) k * 2 + 1];
                        ++free;
                    }
                }
                double perBin = power / (k1 - k0 + 1);
                if (free > 0)
                    perBin = std::max (0.25 * perBin, std::min (perBin, between / free));       // down by 6 dB at most
                pm.noise[(size_t) f * SampleData::PartialModel::numBands + b] = (float) std::sqrt (perBin);
            }
        }
    }
}

// Engine C on a sound whose sides differ (their correlation under 0.3): each side analysed on its own, the
// partials of both in one model ordered by frequency (so that Parciales takes the lowest of both), each marked
// with its side; the noise, the two sides' mean (in power). Twice the partials to play: about twice the work.
void splitPartials (SampleData& d)
{
    if (! d.partials.valid() || d.audio.getNumChannels() < 2)
        return;
    const int n = std::min (d.length, (int) (d.sampleRate * 20.0));
    const float* L = d.audio.getReadPointer (0);
    const float* R = d.audio.getReadPointer (1);
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (int i = 0; i < n; ++i)
    {
        lr += (double) L[i] * R[i];
        ll += (double) L[i] * L[i];
        rr += (double) R[i] * R[i];
    }
    // (only a wide sound, its sides correlated under 0.3: measured on the reference library, side by side the two
    // apart come closer to the sample, by 0.7 dB of spectrum under 0.3, 0.3 dB to 0.6, 0.1 dB to 0.9; summed to mono
    // they come 0.4 to 0.7 dB further; where the sides are much alike that is not worth it)
    if (ll <= 1.0e-12 || rr <= 1.0e-12 || lr / std::sqrt (ll * rr) >= 0.3)
        return;
    const auto side = [&d] (int ch)
    {
        SampleData t;
        t.sampleRate = d.sampleRate;
        t.length = d.length;
        t.pitched = d.pitched;
        t.period = d.period;
        t.peak = d.peak;
        t.rootMidi = d.rootMidi;
        t.detected = d.detected;
        t.audio.setSize (2, d.length);
        t.audio.copyFrom (0, 0, d.audio, ch, 0, d.length);
        t.audio.copyFrom (1, 0, d.audio, ch, 0, d.length);
        std::vector<float> m (d.audio.getReadPointer (ch), d.audio.getReadPointer (ch) + d.length);
        analysePartials (t, m);
        tunePartials (t);
        return t.partials;
    };
    auto pl = side (0), pr = side (1);
    if (! pl.valid() || ! pr.valid() || pl.numFrames != pr.numFrames || pl.hop != pr.hop)
        return;
    // The same partial on both sides (the same note, as most stereo recordings have it): one frequency for both,
    // their mean, so that the two keep the phase between them they had. Each side measured its own, a little
    // apart, and the two drifted in and out of phase: summed to mono a pluck lost 19 dB at moments.
    {
        const auto loudest = [] (const SampleData::PartialModel& pm, int k)
        {
            float top = 0.0f, hz = 0.0f;
            for (int f = 0; f < pm.numFrames; ++f)
                if (const float a = pm.amp[(size_t) f * pm.numPartials + k]; a > top)
                {
                    top = a;
                    hz = pm.freq[(size_t) f * pm.numPartials + k];
                }
            return hz;
        };
        std::vector<float> hzR ((size_t) pr.numPartials);
        for (int k = 0; k < pr.numPartials; ++k)
            hzR[(size_t) k] = loudest (pr, k);
        std::vector<bool> taken ((size_t) pr.numPartials, false);
        const float tolerance = (float) (pl.hop * 4) > 0.0f ? (float) (d.sampleRate / 8192.0) : 3.0f;     // about a bin
        for (int k = 0; k < pl.numPartials; ++k)
        {
            const float hz = loudest (pl, k);
            int match = -1;
            for (int j = 0; j < pr.numPartials; ++j)
                if (! taken[(size_t) j] && std::abs (hzR[(size_t) j] - hz) <= std::max (tolerance, 0.004f * hz)
                    && (match < 0 || std::abs (hzR[(size_t) j] - hz) < std::abs (hzR[(size_t) match] - hz)))
                    match = j;
            if (match < 0)
                continue;
            taken[(size_t) match] = true;
            for (int f = 0; f < pl.numFrames; ++f)
            {
                auto& a = pl.freq[(size_t) f * pl.numPartials + k];
                auto& b = pr.freq[(size_t) f * pr.numPartials + match];
                const float mean = a > 0.0f && b > 0.0f ? 0.5f * (a + b) : std::max (a, b);
                a = b = mean;
                // and never more than a quarter of a cycle apart (phases in cycles): in a sound whose sides are
                // unrelated, the phase between them at the moment the partials are set is anyone's, and frozen
                // opposed it cancelled summed to mono (a pluck at -0.66 of correlation, where the sample is at 0)
                const float pa = pl.phase[(size_t) f * pl.numPartials + k];
                auto& pb = pr.phase[(size_t) f * pr.numPartials + match];
                float apart = pb - pa;
                apart -= std::round (apart);
                pb = pa + juce::jlimit (-0.25f, 0.25f, apart);
            }
        }
    }
    // the order: by each partial's frequency where it is loudest
    struct Entry { float hz; int side, k; };
    std::vector<Entry> order;
    for (int s = 0; s < 2; ++s)
    {
        const auto& pm = s == 0 ? pl : pr;
        for (int k = 0; k < pm.numPartials; ++k)
        {
            float top = 0.0f, hz = 0.0f;
            for (int f = 0; f < pm.numFrames; ++f)
                if (const float a = pm.amp[(size_t) f * pm.numPartials + k]; a > top)
                {
                    top = a;
                    hz = pm.freq[(size_t) f * pm.numPartials + k];
                }
            order.push_back ({ hz, s, k });
        }
    }
    std::stable_sort (order.begin(), order.end(), [] (const Entry& a, const Entry& b) { return a.hz < b.hz; });
    auto& pm = d.partials;
    const int K = (int) order.size(), frames = pl.numFrames;
    pm.numPartials = K;
    pm.numFrames = frames;
    pm.hop = pl.hop;
    pm.lastFrame = std::max (pl.lastFrame, pr.lastFrame);
    pm.holdFrame = std::min (pl.holdFrame, pr.holdFrame);
    pm.refFreq = std::min (pl.refFreq, pr.refFreq);
    pm.amp.assign ((size_t) frames * K, 0.0f);
    pm.freq.assign ((size_t) frames * K, 0.0f);
    pm.phase.assign ((size_t) frames * K, 0.0f);
    pm.pan.assign ((size_t) K, 0.0f);
    pm.decayDbPerSec.assign ((size_t) K, 0.0f);
    for (int j = 0; j < K; ++j)
    {
        const auto& e = order[(size_t) j];
        const auto& src = e.side == 0 ? pl : pr;
        for (int f = 0; f < frames; ++f)
        {
            const size_t from = (size_t) f * src.numPartials + e.k, to = (size_t) f * K + j;
            pm.amp[to] = src.amp[from];
            pm.freq[to] = src.freq[from];
            pm.phase[to] = src.phase[from];
        }
        pm.pan[(size_t) j] = e.side == 0 ? -1.0f : 1.0f;
        pm.decayDbPerSec[(size_t) j] = src.decayDbPerSec[(size_t) e.k];
    }
    pm.bandEdges = pl.bandEdges;
    pm.noise.assign (pl.noise.size(), 0.0f);
    for (size_t i = 0; i < pm.noise.size() && i < pr.noise.size(); ++i)
        pm.noise[i] = std::sqrt (0.5f * (pl.noise[i] * pl.noise[i] + pr.noise[i] * pr.noise[i]));
    pm.split = true;
}

// Crossfades the tail [loopLen, loopLen + xf) into the head [0, xf) of `region` so the
// loop is seamless. Gains are linear and normalised by the measured correlation of the
// two segments, so correlated (in-phase) material neither bumps nor dips.
void crossfadeLoop (const float* region, float* out, int loopLen, int xf)
{
    double xy = 0.0, xx = 0.0, yy = 0.0;
    for (int i = 0; i < xf; ++i)
    {
        xy += (double) region[i] * region[i + loopLen];
        xx += (double) region[i] * region[i];
        yy += (double) region[i + loopLen] * region[i + loopLen];
    }
    const double rho = juce::jlimit (-1.0, 1.0, xy / std::sqrt (xx * yy + 1.0e-20));

    for (int i = 0; i < loopLen; ++i)
    {
        if (i < xf)
        {
            const double a = (double) i / xf, b = 1.0 - a;
            const double norm = std::sqrt (std::max (0.05, a * a + b * b + 2.0 * rho * a * b));
            out[i] = (float) ((region[i] * a + region[i + loopLen] * b) / norm);
        }
        else
            out[i] = region[i];
    }
}

// Seamless loop of a region with its spectral envelope equalised over time, so
// level and brightness do not jump at the loop point.
juce::AudioBuffer<float> makeFlatLoop (const SampleData& d, int start, int len)
{
    const int N = d.sampleRate > 60000.0 ? 4096 : 2048, H = N / 4, bins = N / 2;
    juce::dsp::FFT fft (orderFor (N));
    std::vector<float> win ((size_t) N);
    for (int i = 0; i < N; ++i)
        win[(size_t) i] = 0.5f - 0.5f * std::cos (Dsp::twoPi * (float) i / (float) N);

    const int frames = len / H + N / H + 1;
    std::vector<std::vector<float>> logEnv ((size_t) frames, std::vector<float> ((size_t) bins + 1));
    std::vector<float> buf ((size_t) N * 2), prefix ((size_t) bins + 2);

    auto frameStart = [&] (int j) { return start - N / 2 + j * H; };
    auto fill = [&] (int j, int ch)   // ch -1 = mono
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i)
        {
            const int t = frameStart (j) + i;
            if (t < 0 || t >= d.length) continue;
            const float v = ch < 0 ? 0.5f * (d.audio.getSample (0, t) + d.audio.getSample (1, t)) : d.audio.getSample (ch, t);
            buf[(size_t) i] = v * win[(size_t) i];
        }
        fft.performRealOnlyForwardTransform (buf.data(), true);
    };

    std::vector<float> avg ((size_t) bins + 1, 0.0f);
    for (int j = 0; j < frames; ++j)
    {
        fill (j, -1);
        prefix[0] = 0.0f;
        for (int k = 0; k <= bins; ++k)
            prefix[(size_t) k + 1] = prefix[(size_t) k] + std::sqrt (buf[(size_t) k * 2] * buf[(size_t) k * 2] + buf[(size_t) k * 2 + 1] * buf[(size_t) k * 2 + 1]);
        for (int k = 0; k <= bins; ++k)
        {
            const int r = std::max (2, (int) (k * 0.06f));
            const int a = std::max (0, k - r), b = std::min (bins, k + r);
            const float mean = (prefix[(size_t) b + 1] - prefix[(size_t) a]) / (float) (b - a + 1);
            logEnv[(size_t) j][(size_t) k] = std::log (std::max (mean, 1.0e-9f));
            avg[(size_t) k] += logEnv[(size_t) j][(size_t) k] / (float) frames;
        }
    }

    juce::AudioBuffer<float> region (2, len + N * 2);
    region.clear();
    const float limit = std::log (4.0f);
    for (int ch = 0; ch < 2; ++ch)
    {
        float* out = region.getWritePointer (ch);
        for (int j = 0; j < frames; ++j)
        {
            fill (j, ch);
            for (int k = 0; k <= bins; ++k)
            {
                const float g = std::exp (juce::jlimit (-limit, limit, avg[(size_t) k] - logEnv[(size_t) j][(size_t) k]));
                buf[(size_t) k * 2] *= g;
                buf[(size_t) k * 2 + 1] *= g;
            }
            fft.performRealOnlyInverseTransform (buf.data());
            for (int i = 0; i < N; ++i)
            {
                const int o = j * H + i;   // origin at start - N/2
                if (o < region.getNumSamples())
                    out[o] += buf[(size_t) i] * win[(size_t) i] / 1.5f;
            }
        }
    }

    // Crossfade the tail into the head.
    const int xf = std::max (8, std::min (len / 4, (int) (0.15 * d.sampleRate)));
    const int loopLen = len - xf;
    juce::AudioBuffer<float> loop (2, loopLen);
    for (int ch = 0; ch < 2; ++ch)
        crossfadeLoop (region.getReadPointer (ch, N / 2), loop.getWritePointer (ch), loopLen, xf);

    // What the spectral correction leaves of the level's fall (a short region that decays fast: a bass
    // one-shot, a kick) would come back every turn of the loop as a pulse, and the granular source,
    // which reads this loop at random, would wobble by as much. The level is followed around the loop
    // (RMS over at least four periods and 100 ms, wrapping: the fall, not a tremolo or a repeated hit)
    // and evened out, within 12 dB either way: a loop that is already level hardly changes.
    {
        const int w = std::min (loopLen / 3, std::max ((int) (0.1 * d.sampleRate), (int) (4.0 * d.period)));
        if (w > 16 && loopLen > 2 * w)
        {
            std::vector<double> power ((size_t) loopLen);
            for (int i = 0; i < loopLen; ++i)
                power[(size_t) i] = 0.5 * ((double) loop.getSample (0, i) * loop.getSample (0, i) + (double) loop.getSample (1, i) * loop.getSample (1, i));
            // Circular moving average of the power, twice (a triangular window: smooth gains).
            auto smooth = [&] (const std::vector<double>& in)
            {
                std::vector<double> out ((size_t) loopLen);
                double sum = 0.0;
                for (int k = -w / 2; k < w - w / 2; ++k)
                    sum += in[(size_t) ((k + loopLen) % loopLen)];
                for (int i = 0; i < loopLen; ++i)
                {
                    out[(size_t) i] = sum / w;
                    sum += in[(size_t) ((i + w - w / 2) % loopLen)] - in[(size_t) ((i - w / 2 + loopLen) % loopLen)];
                }
                return out;
            };
            const auto env = smooth (smooth (power));
            double mean = 0.0;
            for (double v : env)
                mean += v / loopLen;
            // Only a level that falls all along the region (past the crossfade at its head) is a fall to
            // even out: a region with hits or a tremolo of its own keeps them.
            bool falls = false;
            {
                double st = 0.0, sv = 0.0, stt = 0.0, svv = 0.0, stv = 0.0;
                int count = 0;
                for (int i = xf; i < loopLen; i += 16, ++count)
                {
                    const double t = i, v = 10.0 * std::log10 (env[(size_t) i] + 1.0e-20);
                    st += t; sv += v; stt += t * t; svv += v * v; stv += t * v;
                }
                if (count > 8)
                {
                    const double ct = stt - st * st / count, cv = svv - sv * sv / count, ctv = stv - st * sv / count;
                    const double r = ctv / std::sqrt (ct * cv + 1.0e-20);
                    const double dropDb = -ctv / std::max (1.0e-20, ct) * (loopLen - xf);
                    falls = r < -0.7 && dropDb > 3.0;
                }
            }
            if (falls && mean > 1.0e-14)
                for (int i = 0; i < loopLen; ++i)
                {
                    const float g = (float) juce::jlimit (0.25, 4.0, std::sqrt (mean / std::max (env[(size_t) i], 1.0e-20)));
                    loop.getWritePointer (0)[i] *= g;
                    loop.getWritePointer (1)[i] *= g;
                }
        }
    }

    double rawSq = 0.0, loopSq = 0.0;
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* o = loop.getReadPointer (ch);
        for (int i = 0; i < loopLen; ++i)
        {
            loopSq += (double) o[i] * o[i];
            const int t = start + i;
            if (t < d.length)
                rawSq += (double) d.audio.getSample (ch, t) * d.audio.getSample (ch, t);
        }
    }

    if (loopSq > 0.0)
        loop.applyGain ((float) std::sqrt (rawSq / loopSq));
    return loop;
}

// Additive freeze from the partial model: the region's partials with slow amplitude
// and pitch movement (breathing) plus its noise. Every partial completes a whole
// number of cycles in the loop and the movements are periodic too, so the loop is
// seamless without a crossfade; only the noise is crossfaded.
bool makeAdditiveFreeze (const SampleData& d, int start, int end, juce::AudioBuffer<float>& out)
{
    const auto& pm = d.partials;
    if (! pm.valid())
        return false;

    const double sr = d.sampleRate;
    const int K = pm.numPartials;
    const int f0 = juce::jlimit (0, pm.lastFrame, start / pm.hop);
    const int f1 = juce::jlimit (f0, pm.lastFrame, end / pm.hop);
    const int frames = f1 - f0 + 1;

    std::vector<float> amp ((size_t) K, 0.0f), freq ((size_t) K, 0.0f);
    std::array<float, SampleData::PartialModel::numBands> noise {};
    for (int f = f0; f <= f1; ++f)
    {
        for (int k = 0; k < K; ++k)
        {
            amp[(size_t) k] += pm.amp[(size_t) f * K + k] / (float) frames;
            freq[(size_t) k] += pm.freq[(size_t) f * K + k] / (float) frames;
        }
        for (int b = 0; b < SampleData::PartialModel::numBands; ++b)
            noise[(size_t) b] += pm.noise[(size_t) f * SampleData::PartialModel::numBands + b] / (float) frames;
    }

    const int L = (int) (6.0 * sr);
    const double seconds = (double) L / sr;
    out.setSize (2, L);
    out.clear();
    float* outL = out.getWritePointer (0);
    float* outR = out.getWritePointer (1);

    juce::Random rng (1234);
    const auto& sn = Dsp::sine();
    for (int k = 0; k < K; ++k)
    {
        const float a = amp[(size_t) k];
        if (a < 1.0e-6f || freq[(size_t) k] > 0.45 * sr)
            continue;

        const double cycles = std::max (1.0, std::round (freq[(size_t) k] * seconds));
        const double startPhase = rng.nextDouble();
        // Breathing: amplitude ±12 % and pitch about ±1.5 cents, at 1-4 cycles per loop.
        const double ampRate = 1 + rng.nextInt (4), ampPhase = rng.nextDouble();
        const double pitchRate = 1 + rng.nextInt (3), pitchPhase = rng.nextDouble();
        const double pitchDepth = freq[(size_t) k] * 0.0009 / (juce::MathConstants<double>::twoPi * pitchRate / seconds);
        float gl, gr;
        pm.gains (k, gl, gr);
        gl *= a;
        gr *= a;

        // The movements are slow, so they are evaluated every 32 samples.
        for (int blockStart = 0; blockStart < L; blockStart += 32)
        {
            const double xb = (double) blockStart / L;
            const float m = 1.0f + 0.12f * sn (ampRate * xb + ampPhase);
            const double offset = startPhase + pitchDepth * sn (pitchRate * xb + pitchPhase);
            const float bl = gl * m, br = gr * m;
            for (int i = blockStart; i < std::min (L, blockStart + 32); ++i)
            {
                const float v = sn (offset + cycles * ((double) i / L));
                outL[i] += v * bl;
                outR[i] += v * br;
            }
        }
    }

    // Noise: render longer and crossfade (it is uncorrelated, so equal power).
    const int xf = (int) (0.5 * sr);
    juce::AudioBuffer<float> noiseBuf (2, L + xf);
    noiseBuf.clear();
    NoiseSynth noiseSynth (99);
    noiseSynth.process (noiseBuf.getWritePointer (0), noiseBuf.getWritePointer (1), L + xf, noise.data(), pm.bandEdges.data(), 1.0f, sr);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* nz = noiseBuf.getReadPointer (ch);
        float* o = out.getWritePointer (ch);
        for (int i = 0; i < L; ++i)
        {
            if (i < xf)
            {
                const float a = (float) i / (float) xf;
                o[i] += nz[i] * std::sin (a * Dsp::pi * 0.5f) + nz[i + L] * std::cos (a * Dsp::pi * 0.5f);
            }
            else
                o[i] += nz[i];
        }
    }

    double rawSq = 0.0, outSq = 0.0;
    for (int ch = 0; ch < 2; ++ch)
    {
        for (int i = 0; i < L; ++i)
            outSq += (double) out.getSample (ch, i) * out.getSample (ch, i);
        for (int t = start; t < std::min (end, d.length); ++t)
            rawSq += (double) d.audio.getSample (ch, t) * d.audio.getSample (ch, t);
    }

    const double rawMean = rawSq / std::max (1, std::min (end, d.length) - start);
    const double outMean = outSq / L;
    if (outMean > 0.0)
        out.applyGain ((float) std::sqrt (rawMean / outMean));
    return true;
}

static bool sustainNoAntiphase = true;

// The sample's colour in time for engine F's Seguir: each frame's partials and noise gathered into the 16 bands of
// the drawn filter (Drone::curveFrequency), in dB against the band's mean over the frames that sound. A band with
// nothing in it stays at 0.
void colourBands (SampleData& d)
{
    const auto& pm = d.partials;
    d.colourBands.clear();
    if (! pm.valid())
        return;
    constexpr int bands = 16;
    const auto bandOf = [] (float hz)
    {
        const float x = std::log2 (std::max (hz, 1.0f) / 20.0f) / std::log2 (1000.0f) * (float) (bands - 1);
        return juce::jlimit (0, bands - 1, juce::roundToInt (x));
    };
    const int frames = pm.numFrames, nb = SampleData::PartialModel::numBands;
    std::vector<double> power ((size_t) frames * bands, 0.0);
    for (int f = 0; f < frames; ++f)
    {
        for (int k = 0; k < pm.numPartials; ++k)
        {
            const float a = pm.amp[(size_t) f * pm.numPartials + k];
            if (a > 0.0f)
                power[(size_t) f * bands + bandOf (pm.freq[(size_t) f * pm.numPartials + k])] += (double) a * a;
        }
        for (int b = 0; b < nb; ++b)
        {
            const float n = pm.noise[(size_t) f * nb + b];
            const float centre = std::sqrt (pm.bandEdges[(size_t) b] * pm.bandEdges[(size_t) b + 1]);
            power[(size_t) f * bands + bandOf (centre)] += (double) n * n * (pm.bandEdges[(size_t) b + 1] - pm.bandEdges[(size_t) b]) / std::max (1.0f, centre * 0.05f);
        }
    }
    // Colour, not level: each frame's bands against its own total (a voice that fades must not take the drone
    // down with it; with the level left in, the drone fell 24 dB as the sample did).
    for (int f = 0; f < frames; ++f)
    {
        double total = 0.0;
        for (int b = 0; b < bands; ++b)
            total += power[(size_t) f * bands + b];
        for (int b = 0; b < bands; ++b)
            power[(size_t) f * bands + b] = total > 1.0e-14 ? power[(size_t) f * bands + b] / total : 0.0;
    }
    const int last = std::max (1, std::min (frames, pm.lastFrame + 1));
    d.colourBands.assign ((size_t) frames * bands, 0.0f);
    {
        double sum = 0.0;
        std::array<double, bands> share {};
        for (int f = 0; f < last; ++f)
            for (int b = 0; b < bands; ++b)
                share[(size_t) b] += power[(size_t) f * bands + b];
        for (const double v : share)
            sum += v;
        for (int b = 0; b < bands; ++b)
            d.colourShare[(size_t) b] = sum > 0.0 ? (float) (share[(size_t) b] / sum) : 1.0f / bands;
    }
    for (int b = 0; b < bands; ++b)
    {
        double meanDb = 0.0;
        int counted = 0;
        for (int f = 0; f < last; ++f)
            if (const double p = power[(size_t) f * bands + b]; p > 1.0e-9)
            {
                meanDb += 10.0 * std::log10 (p);
                ++counted;
            }
        if (counted == 0)
            continue;
        meanDb /= counted;
        for (int f = 0; f < frames; ++f)
        {
            const double p = power[(size_t) f * bands + b];
            d.colourBands[(size_t) f * bands + b] = p > 1.0e-9 ? (float) juce::jlimit (-36.0, 18.0, 10.0 * std::log10 (p) - meanDb) : 0.0f;
        }
    }
}

void buildSource (SampleData& d, SampleData::SustainSource& src, double start)
{
    const double sr = d.sampleRate;
    double len = 0.6 * sr;
    const double minLen = 0.08 * sr;
    const double available = std::max (d.usableEnd, start + minLen) - start;
    len = std::max (minLen, std::min (len, available));
    if (start + len > d.length - 1)
        start = std::max (d.onset, d.length - 1 - len);
    if (d.period > 0.0)
        len = std::max (d.period, std::round (len / d.period) * d.period);

    src.start = start;
    src.end = std::min ((double) d.length - 1, start + len);
    src.loop = makeFlatLoop (d, (int) src.start, (int) (src.end - src.start));
    if (! makeAdditiveFreeze (d, (int) src.start, (int) src.end, src.freeze))
        spectralFreeze (d, src.start, src.end, src.freeze);

    // A held note raises the source by many dB: any offset in it would come up as DC.
    for (auto* b : { &src.loop, &src.freeze })
        for (int ch = 0; ch < b->getNumChannels(); ++ch)
        {
            float* x = b->getWritePointer (ch);
            const int n = b->getNumSamples();
            double mean = 0.0;
            for (int i = 0; i < n; ++i)
                mean += x[i];
            mean /= std::max (1, n);
            juce::FloatVectorOperations::add (x, (float) -mean, n);
        }

    // A held part that is more side than middle (the two channels opposed: summed to mono they cancel) is narrowed
    // until its sides are no louder than its middle, which stays as it was. The tail of a music box swings into
    // opposition, and its sustain held that moment for the whole note: in mono 11 dB went (as engine F, 22-11).
    // Proposed 23-11, for A/B.
    if (sustainNoAntiphase)
        for (auto* b : { &src.loop, &src.freeze })
        {
            if (b->getNumChannels() < 2)
                continue;
            float* l = b->getWritePointer (0);
            float* r = b->getWritePointer (1);
            const int n = b->getNumSamples();
            double mid = 0.0, side = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double m = 0.5 * ((double) l[i] + r[i]), sd = 0.5 * ((double) l[i] - r[i]);
                mid += m * m;
                side += sd * sd;
            }
            if (side <= mid || side < 1.0e-20)
                continue;
            // the sides brought down to the middle's power, and the whole brought back to the power it had
            const float k = (float) std::sqrt (mid / side), keep = (float) std::sqrt ((mid + side) / (2.0 * mid + 1.0e-30));
            for (int i = 0; i < n; ++i)
            {
                const float m = 0.5f * (l[i] + r[i]), sd = 0.5f * (l[i] - r[i]) * k;
                l[i] = (m + sd) * keep;
                r[i] = (m - sd) * keep;
            }
        }

    double loopSq = 0.0;
    for (int ch = 0; ch < src.loop.getNumChannels(); ++ch)
        for (int i = 0; i < src.loop.getNumSamples(); ++i)
            loopSq += (double) src.loop.getSample (ch, i) * src.loop.getSample (ch, i);
    const double loopRms = std::sqrt (loopSq / std::max (1, src.loop.getNumChannels() * src.loop.getNumSamples()));
    src.level = (float) juce::jmax (1.0e-3, loopRms / std::max (1.0e-6f, d.peakRms));

    double modelSum = 0.0, envSum = 0.0;
    int count = 0;
    for (double pos = src.start; pos <= src.end; pos += SampleData::modelHop, ++count)
    {
        modelSum += d.modelFast (pos);
        envSum += std::max (1.0e-3f, d.envAt (pos));
    }
    if (count > 0)
    {
        src.model = (float) (modelSum / count);
        src.env = (float) (envSum / count);
    }
}
} // namespace

juce::String audioWildcard()
{
    static const juce::String wildcard = []
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        return fm.getWildcardForAllFormats();
    }();
    return wildcard;
}

bool isAudioFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;
    for (const auto& pattern : juce::StringArray::fromTokens (audioWildcard(), ";", ""))
        if (file.getFileName().matchesWildcard (pattern.trim(), true))
            return true;
    return false;
}

juce::File audioFileFromText (const juce::String& text)
{
    for (auto line : juce::StringArray::fromLines (text))
    {
        line = line.trim().unquoted();
        if (line.startsWithIgnoreCase ("file:"))
            line = juce::URL (line).getLocalFile().getFullPathName();
        if (line.isEmpty() || ! juce::File::isAbsolutePath (line))
            continue;
        const juce::File file (line);
        if (SampleAnalysis::isAudioFile (file))
            return file;
    }
    return {};
}

bool loadFile (const juce::File& file, juce::AudioBuffer<float>& out, double& sampleRate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0)
        return false;

    const int maxLen = (int) (reader->sampleRate * 120.0);
    const int len = (int) std::min<juce::int64> (reader->lengthInSamples, maxLen);
    const int ch = (int) std::min<unsigned> (2u, reader->numChannels);

    out.setSize (ch, len);
    reader->read (&out, 0, len, 0, true, ch > 1);
    sampleRate = reader->sampleRate;
    return true;
}

// How often the wave of a pitched sample repeats. Usually the period of its note, but the note can
// read an octave low (a bass whose fundamental is weak, a tone made of even harmonics), and laid on
// twice its cycle Tonos would play an octave under the key. Over the steady part, if the wave already
// repeats after a half, a third or a quarter of the period as well as it does after the whole, the
// cycle is that. The note itself is left as it is.
static double cyclePeriod (const SampleData& d, const std::vector<float>& mono)
{
    const double base = d.period;
    const int n = d.length;
    const int window = (int) std::ceil (base * 3.0);
    const int from = (int) std::min ((double) n, d.peak), to = (int) std::min ((double) n, d.peak + 0.5 * d.sampleRate);
    const int span = to - from - window - (int) std::ceil (base * 1.03) - 1;
    if (span <= 0)
        return base;
    constexpr int numWindows = 8;

    // The best normalised correlation within 3 % of the lag, averaged over the windows.
    const auto repeatsAfter = [&] (double lag)
    {
        const int lo = std::max (1, (int) std::floor (lag * 0.97)), hi = std::max (lo, (int) std::ceil (lag * 1.03));
        double total = 0.0;
        for (int w = 0; w < numWindows; ++w)
        {
            const int at = from + (int) ((double) span * w / (numWindows - 1));
            double e0 = 1.0e-12;
            for (int i = 0; i < window; ++i)
                e0 += (double) mono[(size_t) (at + i)] * mono[(size_t) (at + i)];
            double best = -1.0;
            for (int t = lo; t <= hi; ++t)
            {
                double c = 0.0, e1 = 1.0e-12;
                for (int i = 0; i < window; ++i)
                {
                    const double y = mono[(size_t) (at + i + t)];
                    c += (double) mono[(size_t) (at + i)] * y;
                    e1 += y * y;
                }
                best = std::max (best, c / std::sqrt (e0 * e1));
            }
            total += best;
        }
        return total / numWindows;
    };

    const double whole = repeatsAfter (base);
    for (const int k : { 4, 3, 2 })
    {
        if (base / k < 4.0)
            continue;
        const double part = repeatsAfter (base / k);
        if (part >= 0.9 && part >= whole - 0.03)
            return base / k;
    }
    return base;
}

// The cycles of a pitched sample (SampleData::marks). The period is followed by a normalised
// autocorrelation every few milliseconds, searched only near the period of the note (so it never
// jumps an octave), and the marks are laid one local period apart from a peak of the steady part,
// forwards to the end and backwards to the onset.
static void findMarks (SampleData& d, const std::vector<float>& mono)
{
    d.marks.clear();
    d.cycle = d.pitched ? d.period : 0.0;
    if (! d.pitched || d.period < 4.0)
        return;
    const int n = d.length;
    const double base = cyclePeriod (d, mono);
    d.cycle = base;
    const int hop = std::max (64, (int) (base * 2));
    const int window = (int) std::ceil (base * 3.0);
    const int minTau = std::max (2, (int) std::floor (base * 0.8)), maxTau = (int) std::ceil (base * 1.25);

    // The local period every `hop` samples.
    std::vector<double> track;
    for (int at = 0; at < n; at += hop)
    {
        const int from = juce::jlimit (0, std::max (0, n - window - maxTau - 1), at - window / 2);
        double best = -2.0, tau = base;
        double e0 = 1.0e-12;
        for (int i = 0; i < window; ++i)
            e0 += (double) mono[(size_t) (from + i)] * mono[(size_t) (from + i)];
        for (int t = minTau; t <= maxTau && from + window + t < n; ++t)
        {
            double c = 0.0, e1 = 1.0e-12;
            for (int i = 0; i < window; ++i)
            {
                const double y = mono[(size_t) (from + i + t)];
                c += (double) mono[(size_t) (from + i)] * y;
                e1 += y * y;
            }
            const double r = c / std::sqrt (e0 * e1);
            if (r > best)
            {
                best = r;
                tau = t;
            }
        }
        track.push_back (best > 0.5 ? tau : (track.empty() ? base : track.back()));
    }
    const auto periodAt = [&] (double pos)
    {
        const double f = juce::jlimit (0.0, (double) track.size() - 1.0, pos / hop);
        const int i = (int) f;
        const double w = f - i;
        return i + 1 < (int) track.size() ? track[(size_t) i] * (1.0 - w) + track[(size_t) i + 1] * w : track[(size_t) i];
    };

    // A peak in the first steady cycle after the attack.
    const int from = (int) std::min ((double) n - 1, d.peak);
    int anchor = from;
    for (int i = from; i < std::min (n, from + (int) base); ++i)
        if (mono[(size_t) i] > mono[(size_t) anchor])
            anchor = i;

    std::vector<double> back;
    for (double m = anchor - periodAt (anchor); m > d.onset - base; m -= periodAt (m))
        back.push_back (m);
    d.marks.assign (back.rbegin(), back.rend());
    for (double m = anchor; m < n; m += periodAt (m))
        d.marks.push_back (m);
}

// The colour of a sound in SampleData::Colour's octave bands: the power of each band in one frame (Hann window)
// of `x` at `centre`, in dB.
static void colourOfFrame (const juce::dsp::FFT& fft, std::vector<float>& work, const std::vector<float>& window,
                           const float* x, int length, int centre, double sr, float* outDb)
{
    using C = SampleData::Colour;
    const int size = fft.getSize();
    std::fill (work.begin(), work.end(), 0.0f);
    for (int i = 0; i < size; ++i)
    {
        const int at = centre - size / 2 + i;
        if (at >= 0 && at < length)
            work[(size_t) i] = x[at] * window[(size_t) i];
    }
    fft.performFrequencyOnlyForwardTransform (work.data());
    std::array<double, C::numBands> power {};
    const double binHz = sr / size;
    for (int k = 1; k <= size / 2; ++k)
    {
        const double hz = k * binHz;
        const int b = (int) std::floor (std::log2 (hz / C::lowestHz) + 0.5);    // the band whose centre is nearest
        if (b >= 0 && b < C::numBands)
            power[(size_t) b] += (double) work[(size_t) k] * work[(size_t) k];
    }
    for (int b = 0; b < C::numBands; ++b)
        outDb[b] = (float) (10.0 * std::log10 (power[(size_t) b] + 1.0e-20));
}

// SampleData::Colour: the sound's colour frame by frame, and that of its two sustain sources (their whole loop).
static void measureColour (SampleData& d, const std::vector<float>& mono)
{
    using C = SampleData::Colour;
    auto& c = d.colour;
    const double sr = d.sampleRate;
    int order = 12;
    while (order > 9 && (1 << order) > 0.1 * sr)        // about 85 ms at 44.1 and 48 kHz, as long at any rate
        --order;
    while ((1 << order) < 0.07 * sr && order < 15)
        ++order;
    const juce::dsp::FFT fft (order);
    const int size = 1 << order;
    c.hop = size / 4;
    std::vector<float> work ((size_t) size * 2), window ((size_t) size);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (Dsp::twoPi * (float) i / (float) size);

    c.numFrames = std::max (1, d.length / c.hop + 1);
    c.db.assign ((size_t) c.numFrames * C::numBands, -200.0f);
    std::vector<float> total ((size_t) c.numFrames, -200.0f);
    float loudest = -200.0f;
    for (int f = 0; f < c.numFrames; ++f)
    {
        float* out = c.db.data() + (size_t) f * C::numBands;
        colourOfFrame (fft, work, window, mono.data(), d.length, f * c.hop, sr, out);
        double sum = 0.0;
        for (int b = 0; b < C::numBands; ++b)
            sum += std::pow (10.0, out[b] / 10.0);
        total[(size_t) f] = (float) (10.0 * std::log10 (sum + 1.0e-20));
        loudest = std::max (loudest, total[(size_t) f]);
    }
    const int endFrame = std::min (c.numFrames - 1, (int) (std::max (d.onset, d.usableEnd) / c.hop));
    c.lastGood = 0;
    for (int f = 0; f <= endFrame; ++f)
        if (total[(size_t) f] > loudest - 50.0f)
            c.lastGood = f;

    // A sustain source: its colour over the whole loop (read round, as it plays).
    const auto ofSource = [&] (const SampleData::SustainSource& src, std::array<float, C::numBands>& out)
    {
        out.fill (-200.0f);
        const int len = src.loop.getNumSamples();
        if (len <= 0)
            return;
        std::vector<float> loop ((size_t) len * 3);
        for (int i = 0; i < len; ++i)
        {
            const float v = 0.5f * (src.loop.getSample (0, i) + src.loop.getSample (std::min (1, src.loop.getNumChannels() - 1), i));
            loop[(size_t) i] = loop[(size_t) (i + len)] = loop[(size_t) (i + 2 * len)] = v;
        }
        std::array<double, C::numBands> power {};
        int frames = 0;
        std::array<float, C::numBands> one {};
        for (int centre = len; centre < 2 * len || frames == 0; centre += c.hop)
        {
            colourOfFrame (fft, work, window, loop.data(), 3 * len, centre, sr, one.data());
            for (int b = 0; b < C::numBands; ++b)
                power[(size_t) b] += std::pow (10.0, one[(size_t) b] / 10.0);
            ++frames;
        }
        for (int b = 0; b < C::numBands; ++b)
            out[(size_t) b] = (float) (10.0 * std::log10 (power[(size_t) b] / frames + 1.0e-20));
    };
    ofSource (d.early, c.early);
    ofSource (d.late, c.late);
}

// How the sound is set going (SampleData::Excitation): its attack and how it holds, how much
// of it is tone and how much noise, its colour and its lowest register, and the moment and
// polarity of the first peak. Measured on the mix of both channels.
static void measureExcitation (SampleData& d, const std::vector<float>& mono)
{
    auto& e = d.excitation;
    const double sr = d.sampleRate;
    const int n = d.length;
    const int on = (int) d.onset;

    const auto rms = [&] (double from, double to)
    {
        const int a = juce::jlimit (0, n, on + (int) (from * sr)), b = juce::jlimit (0, n, on + (int) (to * sr));
        double sum = 0.0;
        for (int i = a; i < b; ++i)
            sum += (double) mono[(size_t) i] * mono[(size_t) i];
        return std::sqrt (sum / std::max (1, b - a)) + 1.0e-9;
    };

    e.attackMs = d.detected.attack * 1000.0f;
    e.transientDb = (float) (20.0 * std::log10 (rms (0.0, 0.015) / rms (0.06, 0.25)));
    e.sustain = juce::jlimit (0.0f, 1.0f, (20.0f * std::log10 (std::max (1.0e-6f, d.envAt (d.peak + 0.4 * sr))) + 30.0f) / 27.0f);
    {
        Dsp::Biquad low;
        low.set (Dsp::Biquad::LowPass, sr, 120.0f, 0.7071f);
        const int to = juce::jlimit (on, n, (int) std::max (d.usableEnd, (double) on + 1.0));
        double all = 0.0, lows = 0.0;
        for (int i = on; i < to; ++i)
        {
            const float x = mono[(size_t) i], y = low.process (0, x);
            all += (double) x * x;
            lows += (double) y * y;
        }
        e.lowShare = all > 1.0e-20 ? (float) std::min (1.0, lows / all) : 0.0f;
    }

    // The strike, within the attack and 10 ms more (40 ms at most): where the attack first
    // reaches half its height and climbs on to its first top (RMS over 1 ms, every quarter of a
    // ms), the moment the ear takes for the start of a sound; and the polarity of its largest
    // sample there.
    {
        const int span = juce::jlimit ((int) (0.002 * sr), (int) (0.04 * sr), (int) ((d.detected.attack + 0.01) * sr));
        const int w = std::max (8, (int) (0.001 * sr)), hop = std::max (2, w / 4);
        std::vector<float> level;
        for (int i = on; i + w <= std::min (n, on + span + w); i += hop)
        {
            double sum = 0.0;
            for (int j = i; j < i + w; ++j)
                sum += (double) mono[(size_t) j] * mono[(size_t) j];
            level.push_back ((float) sum);
        }
        if (! level.empty())
        {
            const float top = *std::max_element (level.begin(), level.end());
            size_t rise = 0;
            while (rise < level.size() && level[rise] < 0.01f * top)    // a tenth of the height
                ++rise;
            e.riseAt = (float) (((double) rise * hop + w / 2) / sr);
            size_t k = 0;
            while (k < level.size() && level[k] < 0.25f * top)         // half the height: a quarter of the power
                ++k;
            while (k + 1 < level.size() && level[k + 1] >= level[k])
                ++k;
            const int from = on + (int) k * hop;
            int at = from;
            for (int j = from; j < std::min (n, from + w); ++j)
                if (std::abs (mono[(size_t) j]) > std::abs (mono[(size_t) at]))
                    at = j;
            e.transientAt = (float) ((from + w / 2 - on) / sr);
            e.transientSign = mono[(size_t) at] < 0.0f ? -1.0f : 1.0f;
        }
    }

    // Spectra of the attack (the strike and what follows, 30 to 80 ms) and of the body after it.
    constexpr int order = 13, size = 1 << order;
    const juce::dsp::FFT fft (order);
    const auto spectrum = [&] (int from, int length)
    {
        std::vector<float> buf ((size_t) size * 2, 0.0f);
        length = std::min ({ length, size, n - from });
        for (int i = 0; i < length; ++i)
            buf[(size_t) i] = mono[(size_t) (from + i)] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) length));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        std::vector<double> power ((size_t) size / 2);
        for (int k = 0; k < size / 2; ++k)
            power[(size_t) k] = (double) buf[(size_t) k] * buf[(size_t) k];
        return power;
    };
    const double binHz = sr / size;
    const int attackLen = juce::jlimit ((int) (0.03 * sr), (int) (0.08 * sr), (int) ((d.detected.attack + 0.02) * sr));
    const auto attack = spectrum (on, attackLen);
    const int bodyFrom = on + std::min ((int) ((d.detected.attack + 0.02) * sr), (int) (0.3 * sr));
    const auto body = bodyFrom < n - 256 ? spectrum (bodyFrom, (int) (0.3 * sr)) : attack;

    using X = SampleData::Excitation;
    std::array<double, X::numBands> bandPower {};
    double total = 0.0, weighted = 0.0;
    for (int k = 1; k < size / 2; ++k)
    {
        const double f = k * binHz;
        for (int b = 0; b < X::numBands; ++b)
            if (f >= X::bandEdges[b] && f < X::bandEdges[b + 1])
                bandPower[(size_t) b] += attack[(size_t) k];
        if (f >= 50.0 && f < 16000.0)
        {
            total += attack[(size_t) k];
            weighted += attack[(size_t) k] * f;
        }
    }
    e.centroidHz = (float) (weighted / std::max (1.0e-30, total));

    double top = 0.0;
    for (const double p : bandPower)
        top = std::max (top, p);
    double mean = 0.0;
    int counted = 0;
    for (int b = 0; b < X::numBands; ++b)
    {
        e.attackColour[(size_t) b] = (float) (10.0 * std::log10 (std::max (bandPower[(size_t) b], top * 1.0e-6) + 1.0e-30));
        mean += e.attackColour[(size_t) b];
        ++counted;
    }
    for (auto& c : e.attackColour)
        c -= (float) (mean / counted);

    // The lowest register: where 3 % of the energy of attack and body lies below.
    double all = 0.0;
    for (int k = 1; k < size / 2; ++k)
        all += attack[(size_t) k] + body[(size_t) k];
    double below = 0.0;
    e.lowEdgeHz = 20.0f;
    for (int k = 1; k < size / 2; ++k)
    {
        below += attack[(size_t) k] + body[(size_t) k];
        if (below > 0.03 * all)
        {
            e.lowEdgeHz = (float) std::max (20.0, k * binHz);
            break;
        }
    }

    // Noise against tone in the body: spectral flatness from 100 Hz to 10 kHz, and the share of
    // the energy that lies on the harmonics of the note.
    double logSum = 0.0, linSum = 0.0, harmonic = 0.0, inBand = 0.0;
    int bins = 0;
    const double f0 = d.pitched ? 440.0 * std::exp2 ((d.rootMidi - 69.0) / 12.0) : 0.0;
    for (int k = 1; k < size / 2; ++k)
    {
        const double f = k * binHz;
        if (f < 100.0 || f > 10000.0)
            continue;
        const double p = body[(size_t) k] + 1.0e-20;
        logSum += std::log (p);
        linSum += p;
        ++bins;
        inBand += p;
        if (f0 > 0.0)
        {
            const double h = f / f0;
            if (std::abs (h - std::round (h)) * f0 < std::max (0.03 * f0, 2.0 * binHz) && std::round (h) >= 1.0)
                harmonic += p;
        }
    }
    e.noisiness = bins > 0 ? (float) juce::jlimit (0.0, 1.0, std::exp (logSum / bins) / (linSum / bins)) : 0.0f;
    e.tonal = f0 > 0.0 ? (float) juce::jlimit (0.0, 1.0, harmonic / std::max (1.0e-30, inBand)) : 0.2f * (1.0f - e.noisiness);
}

//==============================================================================
// Loop or one shot. A loop exported from a DAW or a sample pack lasts a whole number of beats, starts on
// the beat, its hits fall on a grid of sixteenths that starts at its first sample, and it still sounds at
// its last one (it goes on into its own start). A one shot starts, decays and ends. Its name often says
// which ("122bpm", "Loop", "One Shot"). Every cue is weighed; only a clear loop switches the mode by itself.
namespace LoopDetect
{
// The tempo a name gives: "122bpm", "122 BPM", "87.5_bpm", "bpm120". 0 when there is none.
double bpmFromName (const juce::String& name)
{
    const auto s = name.toLowerCase();
    const auto isNum = [] (juce::juce_wchar c) { return juce::CharacterFunctions::isDigit (c) || c == '.' || c == ','; };
    const auto isSep = [] (juce::juce_wchar c) { return c == ' ' || c == '_' || c == '-'; };
    for (int at = s.indexOf ("bpm"); at >= 0; at = s.indexOf (at + 1, "bpm"))
    {
        int j = at - 1;
        while (j >= 0 && isSep (s[j]))
            --j;
        int end = j + 1;
        while (j >= 0 && isNum (s[j]))
            --j;
        double v = s.substring (j + 1, end).replaceCharacter (',', '.').getDoubleValue();
        if (v >= 40.0 && v <= 250.0)
            return v;
        int k = at + 3;
        while (k < s.length() && isSep (s[k]))
            ++k;
        int from = k;
        while (k < s.length() && isNum (s[k]))
            ++k;
        v = s.substring (from, k).replaceCharacter (',', '.').getDoubleValue();
        if (v >= 40.0 && v <= 250.0)
            return v;
    }
    return 0.0;
}

// Plain numbers of the name that could be a tempo (packs write "<pack>_120_<what>_<key>"): 60 to 200, a
// token of their own. Only taken when the length of the file agrees with them.
std::vector<double> plainTempos (const juce::String& name)
{
    std::vector<double> out;
    juce::StringArray tokens;
    tokens.addTokens (name.toLowerCase(), " _-()[]", "");
    for (const auto& t : tokens)
        if (t.containsOnly ("0123456789") && t.length() >= 2 && t.length() <= 3)
            if (const int v = t.getIntValue(); v >= 60 && v <= 200)
                out.push_back ((double) v);
    return out;
}

// +1: the name says loop; -1: it says one shot (or a single hit); 0: nothing.
int wordHint (const juce::String& name)
{
    const auto s = name.toLowerCase();
    if (s.contains ("one shot") || s.contains ("oneshot") || s.contains ("one_shot") || s.contains ("one-shot"))
        return -1;
    juce::StringArray tokens;
    tokens.addTokens (s, " _-.()[]", "");
    if (s.contains ("loop"))
        return 1;
    for (const auto& t : tokens)
        if (t == "os" || t == "hit" || t == "shot" || t == "stab" || t == "single")
            return -1;
    // A single drum or an effect, unless the name also says loop (above): a weak hint.
    for (const auto& t : tokens)
        if (t == "kick" || t == "snare" || t == "clap" || t == "rim" || t == "crash" || t == "cymbal" || t == "tom" || t == "impact"
            || t == "riser" || t == "downlifter" || t == "uplifter" || t == "sweep" || t == "boom" || t == "808")
            return -2;
    return 0;
}

// The magnitudes of the spectrum frame by frame (Hann window), bins 1 to size / 2.
std::vector<std::vector<float>> spectrogram (const std::vector<float>& mono, int hop, int order)
{
    const int size = 1 << order, n = (int) mono.size();
    std::vector<std::vector<float>> out;
    if (n < size)
        return out;
    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size), buf ((size_t) size * 2);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);
    for (int start = 0; start + size <= n; start += hop)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buf[(size_t) i] = mono[(size_t) (start + i)] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data(), true);
        out.emplace_back (buf.begin() + 1, buf.begin() + size / 2 + 1);
    }
    return out;
}

// The onset strength of each frame (spectral flux of the log magnitudes, half-wave rectified).
std::vector<float> flux (const std::vector<std::vector<float>>& spec)
{
    std::vector<float> out;
    for (size_t f = 0; f < spec.size(); ++f)
    {
        float sum = 0.0f;
        if (f > 0)
            for (size_t k = 0; k < spec[f].size(); ++k)
                sum += std::max (0.0f, std::log1p (100.0f * spec[f][k]) - std::log1p (100.0f * spec[f - 1][k]));
        out.push_back (sum);        // the first frame rises from nothing: 0
    }
    return out;
}

// The share of the energy in hits: harmonic/percussive separation by median filtering (a line that lasts
// across time is harmonic, a line across frequency is a hit), each bin given to the larger of the two. Only
// from `firstBin` up: under 150 Hz the long tone of a kick (an 808 above all) would count as a held note.
float percussiveShare (const std::vector<std::vector<float>>& spec, int firstBin)
{
    const int frames = (int) spec.size();
    if (frames < 5)
        return 0.0f;
    const int bins = (int) spec[0].size();
    constexpr int halfTime = 8, halfFreq = 8;
    double harmonic = 0.0, percussive = 0.0;
    std::vector<float> win;
    win.reserve (2 * std::max (halfTime, halfFreq) + 1);
    for (int f = 0; f < frames; ++f)
        for (int k = std::max (0, firstBin); k < bins; k += 2)
        {
            win.clear();
            for (int j = std::max (0, f - halfTime); j <= std::min (frames - 1, f + halfTime); ++j)
                win.push_back (spec[(size_t) j][(size_t) k]);
            std::nth_element (win.begin(), win.begin() + (long) win.size() / 2, win.end());
            const float h = win[win.size() / 2];
            win.clear();
            for (int j = std::max (0, k - halfFreq); j <= std::min (bins - 1, k + halfFreq); ++j)
                win.push_back (spec[(size_t) f][(size_t) j]);
            std::nth_element (win.begin(), win.begin() + (long) win.size() / 2, win.end());
            const float p = win[win.size() / 2];
            const double e = (double) spec[(size_t) f][(size_t) k] * spec[(size_t) f][(size_t) k];
            (p > h ? percussive : harmonic) += e;
        }
    return (float) (percussive / std::max (1.0e-20, percussive + harmonic));
}

struct Onset { int sample; float strength; };

// The peaks of the flux that stand out of their surroundings, placed on the sample where the sound rises.
std::vector<Onset> onsets (const std::vector<float>& fl, const std::vector<float>& mono, int hop, int frameSize)
{
    std::vector<Onset> out;
    const int frames = (int) fl.size();
    if (frames < 3)
        return out;
    std::vector<float> sorted (fl);
    std::sort (sorted.begin(), sorted.end());
    const float p95 = sorted[(size_t) (0.95 * (sorted.size() - 1))];
    for (int f = 1; f < frames; ++f)
    {
        bool peak = true;
        for (int k = std::max (0, f - 3); k <= std::min (frames - 1, f + 3) && peak; ++k)
            peak = k == f || fl[(size_t) k] < fl[(size_t) f] || (fl[(size_t) k] == fl[(size_t) f] && k > f);
        if (! peak)
            continue;
        std::vector<float> local;
        for (int k = std::max (0, f - 16); k <= std::min (frames - 1, f + 16); ++k)
            local.push_back (fl[(size_t) k]);
        std::nth_element (local.begin(), local.begin() + (long) local.size() / 2, local.end());
        const float median = local[local.size() / 2];
        if (fl[(size_t) f] < median + 0.15f * p95 || fl[(size_t) f] < 0.08f * p95)
            continue;
        // The frame sees the rise somewhere in its window. The hit's peak there, then back from it while the
        // level (the peak of each 0.5 ms) stays over a tenth of the rise: where that rise begins is the hit.
        // The rise is counted from what sounded in the 20 ms before the frame (the tail of the note before; 20 ms
        // hold a cycle down to 50 Hz), not from silence: over a loud tail the level never fell under a tenth of
        // the peak, and the walk went back through the tail itself, up to 25 ms early. A low tail can be well under
        // its peak just when the hit comes, and the hit then shows late in the level; its attack shows at once in
        // the slope (the difference from one sample to the next), where a low tail hardly is: the walk is made on
        // both, and the earlier start is taken when the slope's peak stands clear of what came before (a low hit
        // over a noisy tail has no slope of its own).
        const int from = std::max (0, f * hop - frameSize), to = std::min ((int) mono.size(), f * hop + frameSize);
        const int w = std::max (8, frameSize / 48);        // about 0.5 ms
        const int before = 40 * w;          // some 20 ms
        const auto walk = [&] (auto value)
        {
            int top = from;
            float topLevel = 0.0f;
            for (int i = std::max (1, from); i < to; ++i)
                if (value (i) > topLevel)
                {
                    topLevel = value (i);
                    top = i;
                }
            float earlier = 0.0f;
            for (int i = std::max (1, from - before); i < from; ++i)
                earlier = std::max (earlier, value (i));
            const float threshold = earlier + 0.1f * std::max (0.0f, topLevel - earlier);
            // A low tone crosses zero every half cycle, and a window of 0.5 ms there is nearly silent (6 ms into an
            // 80 Hz hit): the walk ends only after 10 ms under the threshold, at the last window over it.
            int begin = top, under = 0;
            for (int at = top - w; at >= std::max (1, from) && under < 20; at -= w)
            {
                float level = 0.0f;
                for (int i = at; i < at + w; ++i)
                    level = std::max (level, value (i));
                if (level < threshold)
                    ++under;
                else
                {
                    under = 0;
                    begin = at;
                }
            }
            return std::pair<int, float> { begin, topLevel / std::max (1.0e-9f, earlier) };
        };
        const auto byLevel = walk ([&mono] (int i) { return std::abs (mono[(size_t) i]); });
        const auto bySlope = walk ([&mono] (int i) { return std::abs (mono[(size_t) i] - mono[(size_t) i - 1]); });
        int begin = bySlope.second > 2.0f ? std::min (byLevel.first, bySlope.first) : byLevel.first;
        if (byLevel.second < 1.15f)
        {
            // The hit does not stand over what sounded before (a tone straight after another as loud, with no
            // gap): as it was, back from the peak while the level stays over a tenth of it, within the frame.
            const int start = std::max (0, f * hop - hop / 2);
            int top = start;
            float topLevel = 0.0f;
            for (int i = start; i < to; ++i)
                if (std::abs (mono[(size_t) i]) > topLevel)
                {
                    topLevel = std::abs (mono[(size_t) i]);
                    top = i;
                }
            begin = top;
            for (int at = top - w; at >= start; at -= w)
            {
                float level = 0.0f;
                for (int i = at; i < at + w; ++i)
                    level = std::max (level, std::abs (mono[(size_t) i]));
                if (level < 0.1f * topLevel)
                    break;
                begin = at;
            }
        }
        out.push_back ({ std::max (0, begin - w), fl[(size_t) f] / std::max (1.0e-9f, p95) });
    }
    return out;
}
} // namespace LoopDetect

// Over this share of energy in hits (over 150 Hz) a loop warps as Beats. Calibrated on 400 loops of the library:
// 88 % of the drum loops go to Beats, 87 % of pads, chords, keys and basses to Tonal.
static constexpr float percussiveThreshold = 0.5f;

static bool loopReleaseFromHits = true;
void setLoopReleaseFromHits (bool on) noexcept { loopReleaseFromHits = on; }

int noteClassFromName (const juce::String& name)
{
    auto words = juce::StringArray::fromTokens (name.upToLastOccurrenceOf (".", false, false).isNotEmpty()
                                                    && name.containsChar ('.') ? name.upToLastOccurrenceOf (".", false, false) : name,
                                                "_- .()[]", "");
    words.removeEmptyStrings();
    for (int w = words.size(); --w >= 0;)
    {
        const auto t = words[w];
        if (t.isEmpty() || t.length() > 7 || t[0] < 'A' || t[0] > 'G')
            continue;
        int pc = juce::String ("C D EF G A B").indexOfChar (t[0]);
        auto rest = t.substring (1);
        if (rest.startsWith ("#") || (rest.startsWith ("s") && ! rest.startsWithIgnoreCase ("sus")))
        {
            ++pc;
            rest = rest.substring (1);
        }
        else if (rest.startsWith ("b"))
        {
            --pc;
            rest = rest.substring (1);
        }
        for (const char* mode : { "minor", "major", "min", "maj", "m", "M" })
            if (rest.startsWith (mode))
            {
                rest = rest.substring ((int) std::strlen (mode));
                break;
            }
        if (rest.startsWith ("-"))
            rest = rest.substring (1);
        if (! rest.containsOnly ("0123456789") || rest.length() > 1)
            continue;
        // a lone capital in the middle of a name ("A_Day") says little: only near its end
        if (t.length() == 1 && w < words.size() - 3)
            continue;
        return (pc % 12 + 12) % 12;
    }
    return -1;
}
void setSustainNoAntiphase (bool on) noexcept { sustainNoAntiphase = on; }

// A loop's Release: how long its own hits take to die away (each one's fall to 20 dB under its top, three times over
// for 60 dB; the median of them), at most 2 s. The fit of a decay over a whole loop found almost none (a loop does not
// die away: 3 dB/s), and 60 dB at that rate is a Release of 20 s: a note let go went on sounding for seconds (680 of
// the library's 5,206 loops over 2 s). Only ever shorter than what the fit gave. Proposed 23-11, for A/B.
static void releaseFromHits (SampleData& d)
{
    if (! loopReleaseFromHits || d.loopInfo.hits.size() < 4 || d.detected.release <= 0.5f || d.env.empty())
        return;
    const auto& env = d.env;
    const int hop = std::max (1, d.envHop), frames = (int) env.size();
    const auto& hits = d.loopInfo.hits;
    std::vector<float> falls;
    for (size_t h = 0; h < hits.size(); ++h)
    {
        const int from = juce::jlimit (0, frames - 1, hits[h].first / hop);
        const int to = h + 1 < hits.size() ? juce::jlimit (from + 1, frames, hits[h + 1].first / hop) : frames;
        int top = from;
        for (int f = from; f < std::min (to, from + (int) (0.05 * d.sampleRate / hop) + 1); ++f)
            if (env[(size_t) f] > env[(size_t) top])
                top = f;
        if (env[(size_t) top] < 0.01f)
            continue;               // a hit 40 dB under the loudest says little
        for (int f = top; f < to; ++f)
            if (env[(size_t) f] < 0.1f * env[(size_t) top])
            {
                falls.push_back ((float) (3.0 * (f - top) * hop / d.sampleRate));
                break;
            }
    }
    if (falls.size() < 2)
        return;
    std::nth_element (falls.begin(), falls.begin() + (long) falls.size() / 2, falls.end());
    const auto& range = P::defs()[P::AmpR];
    const float release = juce::jlimit (range.min * 0.001f, std::min (2.0f, range.max * 0.001f), falls[falls.size() / 2]);
    if (release < d.detected.release)
        d.detected.release = d.earlierDetected.release = release;
}

static void detectLoop (SampleData& d, const std::vector<float>& mono)
{
    auto& L = d.loopInfo;
    const double sr = d.sampleRate, seconds = d.length / sr;
    const double nameBpm = LoopDetect::bpmFromName (d.name);
    const int words = LoopDetect::wordHint (d.name);
    L.nameBpm = nameBpm;

    const int order = sr > 60000.0 ? 11 : 10, frameSize = 1 << order, hop = frameSize / 2;
    const auto spec = LoopDetect::spectrogram (mono, hop, order);
    const auto fl = LoopDetect::flux (spec);
    const auto hits = LoopDetect::onsets (fl, mono, hop, frameSize);

    // The tail: a loop still sounds at its end (within 26 dB of its loudest), a one shot has died away.
    float tail = 0.0f;
    if (! d.env.empty())
    {
        const int last = (int) d.env.size(), span = std::max (1, std::min (last, (int) (0.1 * sr / d.envHop)));
        for (int k = last - span; k < last; ++k)
            tail = std::max (tail, d.env[(size_t) k]);
    }
    L.tailAlive = tail > 0.05f;

    // A length a DAW writes: a whole number of beats at a whole (or half) tempo, to 2 samples.
    static constexpr int counts[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };
    // Every count that does (a length of 64 beats at 116 is also 48 at 87, and 16 at 120 is 12 at 90): which one,
    // the hits say below. The first one found, the smallest, used to win: 39 of 102 song stems at 3/4 of their tempo.
    constexpr int numCounts = (int) std::size (counts);
    std::array<bool, numCounts> exactAt {};
    bool exact = false;
    for (int c = 0; c < numCounts; ++c)
        for (double bpm = 60.0; bpm <= 200.0 && ! exactAt[(size_t) c]; bpm += 0.5)
            if (std::abs (seconds - counts[c] * 60.0 / bpm) * sr < 2.5)
                exactAt[(size_t) c] = exact = true;

    // The grid: for each count of beats, how well the hits fall on its sixteenths from the first sample on,
    // above what hits anywhere would do by chance; weighed by how likely such a tempo and length are.
    double bestScore = 0.0, bestFit = 0.0;
    int bestBeats = 0;
    double hitWeight = 0.0;
    std::array<double, numCounts> scoreAt {};
    for (const auto& h : hits)
        hitWeight += std::min (2.0f, h.strength);
    for (int c = 0; c < numCounts; ++c)
    {
        const int beats = counts[c];
        const double bpm = beats * 60.0 / seconds;
        if (bpm < 55.0 || bpm > 210.0)
            continue;
        const double grid = seconds / (4.0 * beats), sigma = std::min (0.015, grid * 0.15);
        double fit = 0.0;
        for (const auto& h : hits)
        {
            const double t = h.sample / sr, delta = t - grid * std::round (t / grid);
            fit += std::min (2.0f, h.strength) * std::exp (-(delta / sigma) * (delta / sigma));
        }
        fit = hitWeight > 0.0 ? fit / hitWeight : 0.0;
        const double chance = std::min (1.0, sigma * std::sqrt (juce::MathConstants<double>::pi) / grid);
        const double excess = chance < 1.0 ? (fit - chance) / (1.0 - chance) : 0.0;
        const double octaves = std::log2 (bpm / 120.0);
        const double prior = std::exp (-0.5 * (octaves / 0.55) * (octaves / 0.55))
                           * (beats == 4 || beats == 8 || beats == 16 || beats == 32 ? 1.0 : beats == 2 || beats == 64 || beats == 12 || beats == 24 ? 0.7 : 0.45);
        const double score = std::max (0.0, excess) * prior;
        scoreAt[(size_t) c] = score;
        if (score > bestScore)
        {
            bestScore = score;
            bestFit = excess;
            bestBeats = beats;
        }
    }

    // Spread: the hits reach every quarter of the file (a one shot's land at its start).
    int quarters = 0;
    for (int q = 0; q < 4; ++q)
    {
        bool any = false;
        for (const auto& h : hits)
            any = any || (h.sample >= q * d.length / 4 && h.sample < (q + 1) * d.length / 4 && h.strength > 0.2f);
        quarters += any ? 1 : 0;
    }

    // The beats: a tempo of the name if the length agrees with it ("bpm" first, then plain numbers), else an
    // exact DAW length, else the grid. A loop rendered with its tail (a reverb, a last note ringing) is longer
    // than its beats: with "bpm" in the name, the whole bars that fit, when the tail is under 40 %.
    int beats = 0, length = d.length;
    double why = 0.0, nameError = 1.0;
    std::vector<std::pair<double, double>> tempos;      // tempo, how sure the name is of it
    if (nameBpm > 0.0)
        tempos.push_back ({ nameBpm, 1.0 });
    for (const double t : LoopDetect::plainTempos (d.name))
        tempos.push_back ({ t, 0.8 });
    for (const auto& [tempo, sure] : tempos)
    {
        const double fromName = seconds * tempo / 60.0;
        const int whole = (int) std::lround (fromName);
        if (whole >= 1 && std::abs (fromName - whole) < 0.02 * whole + 0.05)
        {
            beats = whole;
            why = sure;
            nameError = std::abs (fromName - whole) / whole;
            L.nameBpm = tempo;
            break;
        }
    }
    // (a plain number counts too when the name also says loop)
    const auto plain = LoopDetect::plainTempos (d.name);
    const double tailTempo = nameBpm > 0.0 ? nameBpm : (words > 0 && ! plain.empty() ? plain.front() : 0.0);
    if (beats == 0 && tailTempo > 0.0)
    {
        const double fromName = seconds * tailTempo / 60.0;
        const int bars = (int) std::floor (fromName / 4.0);
        if (bars >= 1 && bars * 4 >= 0.6 * fromName)
        {
            beats = bars * 4;
            length = (int) std::lround (beats * 60.0 / tailTempo * sr);
            why = 0.7;
            nameError = 0.0;
            L.nameBpm = tailTempo;
        }
    }
    if (beats == 0 && exact)
    {
        // the exact count whose grid the hits fit best (the prior already prefers 4, 8, 16, 32 and a tempo near
        // 120); with no hits to tell, the one of those counts, else the largest
        int pick = -1;
        for (int c = 0; c < numCounts; ++c)
            if (exactAt[(size_t) c] && (pick < 0 || scoreAt[(size_t) c] > scoreAt[(size_t) pick] + 1.0e-9))
                pick = c;
        if (pick >= 0 && scoreAt[(size_t) pick] <= 1.0e-9)
            for (int c = 0; c < numCounts; ++c)
                if (exactAt[(size_t) c] && (counts[c] == 4 || counts[c] == 8 || counts[c] == 16 || counts[c] == 32 || counts[c] == 64))
                    pick = c;
        beats = counts[pick];
    }
    if (beats == 0 && bestBeats > 0 && bestFit > 0.3)
        beats = bestBeats;

    double confidence = 0.0;
    confidence += 0.55 * why + (why > 0.0 && nameError < 0.003 ? 0.2 : 0.0);
    confidence += exact ? 0.25 : 0.0;
    confidence += L.tailAlive ? 0.2 : (why > 0.0 ? -0.05 : -0.15);
    confidence += (hits.size() >= 4 && quarters >= 3) ? 0.35 * juce::jlimit (0.0, 1.0, bestFit / 0.6) : 0.0;
    confidence += words > 0 ? 0.2 : words == -1 ? -0.5 : words == -2 ? -0.3 : 0.0;
    if (seconds < 0.9)
        confidence -= 0.4;
    if (beats == 0)
        confidence = std::min (confidence, 0.35);
    L.confidence = (float) juce::jlimit (0.0, 1.0, confidence);
    L.beats = beats;
    L.length = juce::jlimit (1, d.length, length);
    L.bpm = beats > 0 ? beats * 60.0 / (L.length / sr) : 0.0;
    L.likely = L.confidence >= 0.65 && beats > 0;
    L.fromName = why > 0.0;

    // Hits or a texture: the share of its energy in hits.
    L.percussive = LoopDetect::percussiveShare (spec, (int) (150.0 / (sr / frameSize)));
    L.rhythmic = L.percussive >= percussiveThreshold;

    L.hits.clear();
    for (const auto& h : hits)
        if (h.sample < L.length)
            L.hits.push_back ({ h.sample, h.strength });

    // Where the Beats warping cuts: the clear hits, at least 40 ms apart, and the start.
    L.slices.clear();
    L.slices.push_back (0);
    const int minGap = (int) (0.04 * sr);
    for (const auto& h : hits)
        if (h.strength > 0.15f && h.sample - L.slices.back() >= minGap && L.length - h.sample >= minGap)
            L.slices.push_back (h.sample);
}

SampleData::LoopInfo analyseLoop (const juce::AudioBuffer<float>& source, double sr, const juce::String& name)
{
    SampleData d;
    d.sampleRate = sr;
    d.length = source.getNumSamples();
    d.name = name;
    std::vector<float> mono ((size_t) d.length);
    for (int i = 0; i < d.length; ++i)
        mono[(size_t) i] = 0.5f * (source.getSample (0, i) + source.getSample (std::min (1, source.getNumChannels() - 1), i));
    d.envHop = 64;
    auto env = rmsEnvelope (mono, d.envHop, (int) (0.01 * sr));
    const float top = env.empty() ? 1.0f : std::max (1.0e-9f, *std::max_element (env.begin(), env.end()));
    for (auto& v : env)
        v /= top;
    d.env = env;
    if (d.length >= 64)
        detectLoop (d, mono);
    return d.loopInfo;
}

std::shared_ptr<SampleData> analyse (const juce::AudioBuffer<float>& source, double sr,
                                     const juce::String& name, bool reverse, bool forAllEngines)
{
    auto d = std::make_shared<SampleData>();
    const int n = source.getNumSamples();
    constexpr int pad = 8;

    d->name = name;
    d->sampleRate = sr;
    d->length = n;
    d->reversed = reverse;
    d->audio.setSize (2, n + pad);
    d->audio.clear();

    for (int ch = 0; ch < 2; ++ch)
    {
        const float* src = source.getReadPointer (std::min (ch, source.getNumChannels() - 1));
        float* dst = d->audio.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
            dst[i] = src[reverse ? n - 1 - i : i];
    }

    if (n > 0 && n <= (int) (70.0 * sr))
        d->loopAudio.makeCopyOf (d->audio);

    // Micro-fades where the file is cut, never on the file itself: a sample that ends in the middle of its
    // sound fades over its last 3 ms (otherwise every note, and every grain that reaches the end, would
    // click there); one that starts already loud gets 0.3 ms, short enough to keep a real attack.
    if (n > 0)
    {
        const float peakIn = std::max ({ 1.0e-9f, d->audio.getMagnitude (0, 0, n), d->audio.getMagnitude (1, 0, n) });
        const auto edge = [&] (int i) { return std::max (std::abs (d->audio.getSample (0, i)), std::abs (d->audio.getSample (1, i))); };
        const int tail = std::min (n / 4, (int) (0.003 * sr)), head = std::min (n / 4, (int) (0.0003 * sr));
        if (tail > 1 && edge (n - 1) > 1.0e-4f * peakIn)
            for (int ch = 0; ch < 2; ++ch)
                d->audio.applyGainRamp (ch, n - tail, tail, 1.0f, 0.0f);
        if (head > 1 && edge (0) > 0.05f * peakIn)
            for (int ch = 0; ch < 2; ++ch)
                d->audio.applyGainRamp (ch, 0, head, 0.0f, 1.0f);
    }

    std::vector<float> mono ((size_t) n);
    for (int i = 0; i < n; ++i)
        mono[(size_t) i] = 0.5f * (d->audio.getSample (0, i) + d->audio.getSample (1, i));
    d->peakLevel = std::max ({ 1.0e-6f, d->audio.getMagnitude (0, 0, n), d->audio.getMagnitude (1, 0, n) });

    // Onset: first sample above -45 dB of the absolute peak.
    float maxAbs = 0.0f;
    for (const float v : mono)
        maxAbs = std::max (maxAbs, std::abs (v));

    if (maxAbs <= 0.0f || n < 64)
    {
        d->env.assign (2, 0.0f);
        d->modelTable.assign (2, 1.0f);
        d->usableEnd = d->knee = d->sustainPos = n * 0.5;
        return d;
    }

    int onset = 0;
    while (onset < n && std::abs (mono[(size_t) onset]) < maxAbs * 0.0056f)
        ++onset;

    // Some recordings lead into the note with a steady floor well above -45 dB (the room, a piano's
    // damper lifting, a hum): the note would start, and its attack be measured, from there, a quarter
    // of a second early in the anechoic piano of the Iowa library. When what comes before the main rise
    // is only such a floor (35 dB or more under the top on average and never within 25 dB of it, steady
    // within 6 dB, at least 30 ms: a soft note before the loud one is louder than that) and the
    // rise out of it is sudden (20 dB within 30 ms), the onset is where that rise begins. A ghost note,
    // a breath that swells or a crescendo is not a steady floor and leaves the onset where it was.
    {
        const int frame = std::max (8, (int) (0.002 * sr));
        const int searchEnd = std::min (n, onset + (int) (2.0 * sr));
        std::vector<float> db;
        for (int a = onset; a + frame <= searchEnd; a += frame)
        {
            double s = 0.0;
            for (int i = a; i < a + frame; ++i)
                s += (double) mono[(size_t) i] * mono[(size_t) i];
            db.push_back ((float) (10.0 * std::log10 (s / frame + 1.0e-20)));
        }
        if (db.size() > 20)
        {
            const int top = (int) (std::max_element (db.begin(), db.end()) - db.begin());
            const float topDb = db[(size_t) top];
            int rise = top;
            while (rise > 0 && db[(size_t) rise - 1] > topDb - 20.0f)
                --rise;
            const int floorFrames = rise;                                   // frames before the rise
            int reach = rise;                                               // within 6 dB of the top
            while (reach < top && db[(size_t) reach] < topDb - 6.0f)
                ++reach;
            const int sharp = (int) (0.03 * sr / frame);
            if (floorFrames * frame >= (int) (0.03 * sr) && reach - rise <= sharp)
            {
                float hi = -400.0f;
                double power = 0.0;
                for (int f = 0; f < floorFrames; ++f)
                {
                    hi = std::max (hi, db[(size_t) f]);
                    power += std::pow (10.0, db[(size_t) f] / 10.0) / floorFrames;
                }
                const float floorDb = (float) (10.0 * std::log10 (power + 1.0e-20));
                // Steady: 10 ms averages stay within 6 dB of the floor's level.
                bool steady = hi < topDb - 25.0f && floorDb < topDb - 35.0f;
                for (int f = 0; steady && f + 5 <= floorFrames; f += 5)
                {
                    double p = 0.0;
                    for (int j = f; j < f + 5; ++j)
                        p += std::pow (10.0, db[(size_t) j] / 10.0) / 5.0;
                    steady = std::abs ((float) (10.0 * std::log10 (p + 1.0e-20)) - floorDb) < 6.0f;
                }
                if (steady)
                    onset += rise * frame;
            }
        }
    }
    d->onset = std::max (0, onset - 32);

    // Rough envelope to locate the stable region for pitch detection.
    const int hop = 64;
    d->envHop = hop;
    auto rough = rmsEnvelope (mono, hop, (int) (0.01 * sr));
    const int roughPeak = (int) (std::max_element (rough.begin(), rough.end()) - rough.begin());

    // Pitch: YIN along the whole note. The note is the pitch that sounds longest and loudest,
    // so that a glide into it, or the drop of a kick or an 808, does not decide it.
    {
        int window = 1024;
        while (window < (int) (2.5 * sr / minPitchHz) && window < 16384)
            window *= 2;

        struct Reading { float midi, weight; int pos; };
        std::vector<Reading> readings;
        const float roughMax = rough[(size_t) roughPeak];
        const int stride = window / 4;
        for (int pos = roughPeak * hop + (int) (0.01 * sr); pos + window < n && (int) readings.size() < 64; pos += stride)
        {
            if (rough[(size_t) (pos / hop)] < roughMax * 0.03f)
                break;

            const auto [hz, aperiodicity] = yin (mono.data() + pos, window, sr);
            if (aperiodicity < 0.25f && hz >= minPitchHz && hz <= maxPitchHz)
            {
                // Loud moments decide: the tail of an 808 drifts, but its note is the one you hear.
                const float level = rough[(size_t) (pos / hop)];
                readings.push_back ({ 69.0f + 12.0f * std::log2 (hz / 440.0f), level * level * (1.0f - aperiodicity), pos });
            }
        }

        float total = 0.0f;
        for (const auto& r : readings)
            total += r.weight;

        // Each reading votes for the pitches within a third of a semitone of it.
        float bestScore = 0.0f, bestMidi = 0.0f;
        for (const auto& candidate : readings)
        {
            float score = 0.0f;
            for (const auto& r : readings)
            {
                const float distance = (r.midi - candidate.midi) / 0.35f;
                score += r.weight * std::exp (-distance * distance);
            }
            if (score > bestScore)
            {
                bestScore = score;
                bestMidi = candidate.midi;
            }
        }

        // A sweep or a riser has no pitch that holds: better no root than a wrong one.
        if (readings.size() >= 2 && bestScore >= 0.35f * total)
        {
            float sum = 0.0f, weights = 0.0f, loudest = 0.0f;
            int where = readings.front().pos;
            for (const auto& r : readings)
            {
                if (std::abs (r.midi - bestMidi) > 0.5f)
                    continue;
                sum += r.midi * r.weight;
                weights += r.weight;
                if (r.weight > loudest)
                {
                    loudest = r.weight;
                    where = r.pos;
                }
            }

            d->pitched = true;
            float hz = refineFundamental (mono, sr, where, 440.0f * std::exp2 ((sum / weights - 69.0f) / 12.0f));
            // Then the sustained part's own pitch, which is what a tuner (and Snap a C) must match.
            const int steadyFrom = (int) (d->onset + 0.08 * sr), steadyTo = (int) std::min ((double) n, d->onset + 3.0 * sr);
            if (const float steady = steadyPitch (mono, sr, steadyFrom, steadyTo, hz);
                steady > 0.0f && std::abs (12.0f * std::log2 (steady / hz)) < 0.6f)
                hz = steady;
            d->rootMidi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
        }
    }

    // Envelope window: at least 10 ms and three periods of the fundamental.
    const double period = d->pitched ? sr / (440.0 * std::pow (2.0, (d->rootMidi - 69.0) / 12.0)) : 0.0;
    auto env = rmsEnvelope (mono, hop, (int) std::max (0.01 * sr, 3.0 * period));
    const auto peakIt = std::max_element (env.begin(), env.end());
    d->peakRms = std::max (1.0e-9f, *peakIt);
    for (auto& v : env)
        v /= d->peakRms;
    d->env = env;

    const int frames = (int) env.size();
    const int onsetFrame = (int) (d->onset / hop);

    // The peak of the envelope is where the sound first gets within 1 dB of its loudest. A sound
    // that holds at the top, like an 808, peaks where it arrives there, not at the moment a
    // ripple of its level happens to be highest.
    int peakFrame = (int) (peakIt - env.begin());
    float arrivalLevel = 0.891f;

    // A level that falls by half on the way to its loudest moment is more than one event: a
    // loop, a phrase, notes in an echo. Its attack is that of the first event that comes near
    // the top (within 6 dB), not the time to the loudest of them, which may be half a minute.
    {
        float highest = 0.0f;
        for (int f = std::max (0, onsetFrame); f < peakFrame; ++f)
        {
            highest = std::max (highest, env[(size_t) f]);
            if (highest > 0.1f && env[(size_t) f] < 0.5f * highest)
            {
                arrivalLevel = 0.5f;
                break;
            }
        }
    }
    for (int f = std::max (0, onsetFrame); f < peakFrame; ++f)
        if (env[(size_t) f] >= arrivalLevel)
        {
            peakFrame = f;
            break;
        }
    // A held note that keeps swelling once it has started (a bow, a breath, a crescendo, a slow drift)
    // reaches its loudest seconds later: the attack was read as 2.5 s on a cello played ff, and as 4.6 s on
    // a clarinet, whose notes are there in a fifth of a second. The attack ends where the rise stops: the
    // first moment within 6 dB of the top whose level (held over the last 200 ms, so that a tremolo counts
    // at its crests) is within 1 dB of all that comes in the next 300 ms. A swell that keeps climbing
    // faster than that (a pad, a riser) still ends at the top. Only for attacks over 150 ms: a short
    // one is read on the waveform just below.
    const int wholeRiseFrame = peakFrame;      // where the attack ended before (kept for old projects)
    bool settled = false;
    if (arrivalLevel >= 0.891f && (double) (peakFrame - onsetFrame) * hop > 0.15 * sr)
    {
        const int ahead = (int) (0.3 * sr / hop), back = (int) (0.2 * sr / hop);
        for (int f = std::max (0, onsetFrame); f < peakFrame && ! settled; ++f)
        {
            float held = 0.0f, next = 0.0f;
            for (int k = std::max (std::max (0, onsetFrame), f - back); k <= f; ++k)
                held = std::max (held, env[(size_t) k]);
            if (held < 0.5f)
                continue;
            for (int k = f; k < std::min (frames, f + ahead); ++k)
                next = std::max (next, env[(size_t) k]);
            if (next <= held * 1.122f)
            {
                peakFrame = f;
                settled = true;
            }
        }
    }
    // When it gets there, to within a period: the RMS window (three periods) blurs the start of
    // a low note, so the arrival is read on the peaks of the waveform, one period at a time.
    const auto arrivalFrom = [&] (int fromFrame, bool nearTop)
    {
        const int start = (int) d->onset;
        const int block = std::max (8, (int) std::max (0.001 * sr, period) / 8);
        // Up to the loudest moment; for several events (or a settled note), up to where it came near the top.
        const int loudestFrame = nearTop ? fromFrame + (int) (0.05 * sr / hop) : (int) (peakIt - env.begin());
        const int searchEnd = std::min (n, (int) ((loudestFrame + 1) * hop) + 8 * block);
        std::vector<float> maxima;
        for (int b = start; b < searchEnd; b += block)
        {
            float m = 0.0f;
            for (int i = b; i < std::min (searchEnd, b + block); ++i)
                m = std::max (m, std::abs (mono[(size_t) i]));
            maxima.push_back (m);
        }
        std::vector<float> trailing (maxima.size());
        float top = 0.0f;
        for (size_t k = 0; k < maxima.size(); ++k)
        {
            float m = 0.0f;
            for (size_t j = k >= 7 ? k - 7 : 0; j <= k; ++j)     // the last period
                m = std::max (m, maxima[j]);
            trailing[k] = m;
            top = std::max (top, m);
        }
        size_t arrival = trailing.size();
        for (size_t k = 0; k < trailing.size(); ++k)
            if (trailing[k] >= 0.891f * top)
            {
                arrival = k;
                break;
            }
        const double arrivedAt = arrival < trailing.size() ? start + (double) (arrival + 1) * block : (double) fromFrame * hop;
        return std::max (d->onset + 1.0, std::min ((double) loudestFrame * hop, arrivedAt));
    };
    d->peak = arrivalFrom (peakFrame, arrivalLevel < 0.891f || settled);
    peakFrame = std::max (peakFrame, (int) (d->peak / hop));

    int endFrame = frames - 1;
    while (endFrame > peakFrame && env[(size_t) endFrame] < 0.001f)
        --endFrame;

    int usableFrame = endFrame;
    while (usableFrame > peakFrame && env[(size_t) usableFrame] < 0.01f)
        --usableFrame;
    d->usableEnd = std::min ((double) n, (double) usableFrame * hop);
    d->endFallDbPerSec = 0.0f;
    {
        double sum = 0.0;
        int count = 0;
        for (int f = std::max (0, (int) (d->onset / hop)); f <= usableFrame && f < frames; ++f, ++count)
            sum += (double) env[(size_t) f] * env[(size_t) f];
        d->meanLevel = count > 0 ? (float) juce::jlimit (0.01, 1.0, std::sqrt (sum / count)) : 1.0f;
    }
    if (endFrame + 1 < frames)
    {
        const int from = std::max (peakFrame, endFrame - (int) std::ceil (0.05 * sr / hop));
        const double seconds = std::max (1, endFrame + 1 - from) * (double) hop / sr;
        const float a = std::max (1.0e-6f, env[(size_t) from]), b = std::max (1.0e-6f, env[(size_t) endFrame + 1]);
        d->endFallDbPerSec = (float) juce::jlimit (3.0, 2000.0, 20.0 * std::log10 (a / b) / seconds);
        d->soundEnd = std::min ((double) n, (double) (endFrame + 1) * hop);
    }

    // The envelope when the attack ends at `peakPos` (frame `fromFrame`); `underTop`: below the loudest.
    // `earlier`: also the one 0.9.5 found there (its decay fit could let the sustain fall first).
    const auto shapeFrom = [&] (double peakPos, int fromFrame, bool underTop, AdsrShape* earlier)
    {
        AdsrShape shape;
        shape.attack = (float) std::max (0.0005, (peakPos - d->onset) / sr);
        {
            const int mid = (onsetFrame + fromFrame) / 2;
            // Halfway, against where the attack ends.
            const float top = underTop ? std::max (1.0e-3f, env[(size_t) fromFrame]) : 1.0f;
            const float m = mid > onsetFrame && mid < fromFrame ? std::min (1.0f, env[(size_t) mid] / top) : 0.5f;
            shape.curve = juce::jlimit (0.3f, 4.0f, std::log (std::max (m, 1.0e-3f)) / std::log (0.5f));
        }

        // Decay / sustain / fade, and the release that follows from them.
        const auto fits = fitDecay (env, fromFrame, endFrame, hop / sr);
        const auto withFit = [&] (AdsrShape out, const DecayFit& fit)
        {
            out.sustain = fit.sustain;
            out.decay = fit.decay;
            out.fadeDbPerSec = fit.fade;
            if (out.fadeDbPerSec > 0.5f)
                out.release = juce::jlimit (0.02f, 20.0f, 60.0f / out.fadeDbPerSec);
            else
            {
                int tailStart = endFrame;
                while (tailStart > fromFrame && env[(size_t) tailStart] < 0.5f * std::max (out.sustain, 0.01f))
                    --tailStart;
                out.release = juce::jlimit (0.02f, 10.0f, (float) ((endFrame - tailStart) * hop / sr));
            }
            return out;
        };
        if (earlier != nullptr)
            *earlier = withFit (shape, fits.any);
        return withFit (shape, fits.ordered);
    };
    d->detected = shapeFrom (d->peak, peakFrame, settled, &d->earlierDetected);
    auto& adsr = d->detected;
    if (settled)
    {
        const double before = arrivalFrom (wholeRiseFrame, false);
        shapeFrom (before, std::max (wholeRiseFrame, (int) (before / hop)), false, &d->earlierDetected);
    }

    d->knee = std::min ((double) n, d->peak + adsr.decay * sr);
    // The hit lasts the attack and then at least 15 ms or three periods: a low note glides into pitch.
    d->transientMs = juce::jlimit (5.0f, 150.0f, (float) ((d->peak - d->onset + std::max (0.015 * sr, 3.0 * period)) / sr * 1000.0));

    // Sustain zone: the knee, or earlier if the signal has already dropped below -20 dB.
    {
        const double minPos = d->peak + std::max (0.02 * sr, d->transientMs * 0.001 * sr);
        double ts = d->knee;
        const float threshold = std::max (0.1f, adsr.sustain * 1.2f);
        for (int f = peakFrame; f < frames; ++f)
        {
            if (env[(size_t) f] < threshold)
            {
                ts = std::min (ts, (double) f * hop);
                break;
            }
        }

        ts = std::max (ts, minPos);
        ts = std::min (ts, std::max (d->peak, d->usableEnd - 0.1 * sr));
        d->sustainPos = juce::jlimit (0.0, (double) std::max (0, n - 1), ts);
    }

    d->period = period;

    // The model must be something the knobs can show, or "untouched" would not be the sample.
    {
        const auto& defs = P::defs();
        for (auto* shape : { &d->detected, &d->earlierDetected })
        {
            auto& m = *shape;
            m.attack = juce::jlimit (defs[P::AmpA].min * 0.001f, defs[P::AmpA].max * 0.001f, m.attack);
            m.curve = juce::jlimit (defs[P::AmpCurve].min, defs[P::AmpCurve].max, m.curve);
            m.decay = juce::jlimit (defs[P::AmpD].min * 0.001f, defs[P::AmpD].max * 0.001f, m.decay);
            m.fadeDbPerSec = juce::jlimit (defs[P::AmpFade].min, defs[P::AmpFade].max, m.fadeDbPerSec);
            m.release = juce::jlimit (defs[P::AmpR].min * 0.001f, defs[P::AmpR].max * 0.001f, m.release);
        }
        const auto& a = d->detected;
        const auto& e = d->earlierDetected;
        d->hasEarlierDetected = a.attack != e.attack || a.curve != e.curve || a.decay != e.decay || a.sustain != e.sustain
                                || a.fadeDbPerSec != e.fadeDbPerSec;
    }

    d->modelTable.resize ((size_t) (n / SampleData::modelHop + 2));
    for (size_t i = 0; i < d->modelTable.size(); ++i)
        d->modelTable[i] = std::max (1.0e-3f, d->modelAt ((double) i * SampleData::modelHop));

    if (forAllEngines)
    {
        analysePartials (*d, mono);
        tunePartials (*d);
        splitPartials (*d);
        colourBands (*d);

        // Sustain sources: at the sustain point (Sostenido) and the last part of the
        // audible sample (Natural, so the original plays almost to the end).
        buildSource (*d, d->early, d->sustainPos);
        const double lateLen = std::min (0.6 * sr, std::max (0.08 * sr, (d->usableEnd - d->sustainPos) * 0.5));
        buildSource (*d, d->late, std::max (d->onset, d->usableEnd - lateLen));
        measureColour (*d, mono);
        d->forAllEngines = true;
    }

    measureExcitation (*d, mono);
    findMarks (*d, mono);
    SynthEngine::checkTones (*d);
    d->mechanismHint = Mechanism::nameHint (name, d->mechanismHintWord);

    // Zero-phase high-pass of the first two seconds after the onset (see SampleData::attackHigh).
    {
        const int len = std::min (d->length, (int) (d->onset + 2.0 * sr));
        d->attackHigh.setSize (2, std::max (1, len));
        d->attackHigh.clear();
        Dsp::Biquad hp;
        hp.set (Dsp::Biquad::HighPass, sr, 1000.0f, 0.7071f);
        for (int ch = 0; ch < 2; ++ch)
        {
            float* y = d->attackHigh.getWritePointer (ch);
            const float* x = d->audio.getReadPointer (ch);
            hp.reset();
            for (int i = 0; i < len; ++i)
                y[i] = hp.process (0, x[i]);
            hp.reset();
            for (int i = len - 1; i >= 0; --i)
                y[i] = hp.process (0, y[i]);
        }
    }
    Reconstruct::buildBeds (*d);

    detectLoop (*d, mono);
    releaseFromHits (*d);
    d->nameRoot = d->pitched ? -1 : noteClassFromName (d->name);
    return d;
}

juce::String midiNoteName (float midi)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int note = juce::roundToInt (midi);
    return juce::String (names[((note % 12) + 12) % 12]) + juce::String ((int) std::floor (note / 12.0) - 2);
}
} // namespace SampleAnalysis
