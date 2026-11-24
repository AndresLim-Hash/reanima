#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>
#if defined (__SSE2__) || defined (_M_X64)
 #include <emmintrin.h>
 #define MASTER_SSE 1
#endif

// The MASTER output stage (24-11-2026, docs/master_valvula_limitador_2026-11-24.md):
//
//   FX of the master -> Volumen -> [meter] -> -4 dB of room -> VALVE -> OUTPUT -> TRUE PEAK LIMITER -> out
//
// The meter shows the level that enters the stage; its 0 dB leaves at -4 dBFS when nothing acts (as before).
//
// The valve: one soft stage after a triode, four times oversampled (twice at 88.2 kHz and above: the same
// 176-192 kHz inside) through linear-phase FIR half-bands, so what the curve makes above the audio band is
// filtered out instead of folding back. Its curve, g(s) = tanh(d s) / (d (1 + b tanh(d s))), has a gain of
// exactly 1 for small signals at every setting, rounds the peaks more as they grow (the 3rd harmonic) and the
// positive half more than the negative (b: the 2nd, the even harmonics of a triode). Only what the curve adds
// (the residual g(s) - s) is coloured: its DC taken out, and with WARM and BODY its highest part softened; the
// signal itself goes through untouched. AMOUNT is the drive d; CHARACTER moves b, the colour of the residual
// and, towards BODY, a low shelf before the curve (undone after it) so that the lows drive it more. The level
// is compensated: the fundamental of a tone at the meter's -6 dB comes out as loud at every setting.
// ON/BYPASS crossfades inside the oversampled domain over 30 ms; off, the signal goes through the same filters
// (so its delay is always the same and the clean path is exactly the old one).
//
// OUTPUT: a gain after the valve and before the limiter. It never changes what the valve does.
//
// The limiter (TruePeakLimiter below): true peaks read at eight points between the samples, a 1.5 ms lookahead
// (the gain reaches what a peak needs before the peak arrives, as a smooth ramp: never a hard clip) and a
// release that is quicker after a single hit and slower while the reduction lasts (no pumping). Under the
// ceiling its gain is exactly 1: the signal comes out bit for bit, only delayed. Its delay is always there
// (on or off) and is reported to the host with the valve's.
//
// Before (until 0.9.5) the stage was a fixed knee from -4 to -1 dBFS right after Volumen: each dB the meter
// went over 0 became distortion (THD -59 dB at +1 dB, -18 dB at +6 dB, a near hard clip above), and a guard
// at the host's rate let drum hits through at +0.7 dBTP. Kept here: the -4 dB of room, the oversampling and
// its clean path.
class TruePeakLimiter
{
public:
    // 8x detection, 64 taps per phase (32 samples of delay). Measured against a 64x reference on noise: it reads
    // the true peak within 0.08 dB up to 0.9 of Nyquist (20 kHz at 44.1), 0.16 dB up to 0.95 of it.
    static constexpr int phases = 8, taps = 64;
    static constexpr float margin = 0.9942601f;       // 0.05 dB under the ceiling: what the reading may miss

    void prepare (double sampleRate)
    {
        rate = sampleRate;
        look = std::max (8, (int) std::lround (0.0015 * sampleRate));
        delay = taps / 2 + look;
        // The interpolator: a Kaiser-windowed sinc at the host's Nyquist, phases * taps + 1 points (its phase 0 is
        // the sample itself).
        const int length = phases * taps + 1, centre = phases * taps / 2;
        std::vector<double> h ((size_t) length);
        const double beta = 10.0, i0b = besselI0 (beta);
        for (int i = 0; i < length; ++i)
        {
            const double t = (double) (i - centre) / phases;
            const double sinc = t == 0.0 ? 1.0 : std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
            const double w = (double) (i - centre) / centre;
            h[(size_t) i] = sinc * besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - w * w))) / i0b;
        }
        // each phase's taps in the order of the history (oldest first), padded to a multiple of four
        for (int k = 0; k < phases; ++k)
        {
            coef[(size_t) k].fill (0.0f);
            for (int j = 0; j <= taps; ++j)
            {
                const int i = k + phases * j;
                coef[(size_t) k][(size_t) (taps - j)] = i < length ? (float) h[(size_t) i] : 0.0f;
            }
        }
        const int ring = 1;
        while (ringSize < delay + 4)
            ringSize <<= ring;
        for (auto& r : signal)
            r.assign ((size_t) ringSize, 0.0f);
        peaks.assign ((size_t) ringSize, 0.0f);
        holdValues.assign ((size_t) look + 1, 1.0f);
        boxValues.assign ((size_t) look + 1, 1.0f);
        reset();
    }

    void reset()
    {
        for (auto& h : history)
            h.fill (0.0f);
        for (auto& r : signal)
            std::fill (r.begin(), r.end(), 0.0f);
        std::fill (peaks.begin(), peaks.end(), 0.0f);
        std::fill (holdValues.begin(), holdValues.end(), 1.0f);
        std::fill (boxValues.begin(), boxValues.end(), 1.0f);
        holdHead = 0;
        boxHead = 0;
        boxSum = (double) boxValues.size();
        boxUnder = 0;
        deque.clear();
        dequeStart = 0;
        envelope = 1.0f;
        sustained = 0.0f;
        lastPeak = 0.0f;
        write = 0;
        historyPos = 0;
        counter = 0;
    }

    int latency() const noexcept { return delay; }

    // threshold: the most the true peak may reach (linear); <= 0: off (the gain releases to 1, the delay stays).
    // Returns the smallest gain of the block; outPeak: the largest true peak that leaves (estimated).
    float process (float* l, float* r, int n, float threshold, float& outPeak) noexcept
    {
        float smallest = 1.0f;
        const float relFast = 1.0f - std::exp (-1.0f / (0.06f * (float) rate));
        const float relSlow = 1.0f - std::exp (-1.0f / (0.5f * (float) rate));
        const float sustainCoef = 1.0f - std::exp (-1.0f / (0.4f * (float) rate));
        for (int i = 0; i < n; ++i)
        {
            // the true peak around the sample half the interpolator back (taps / 2)
            history[0][(size_t) historyPos] = l[i];
            history[1][(size_t) historyPos] = r[i];
            history[0][(size_t) (historyPos + taps + 1)] = l[i];
            history[1][(size_t) (historyPos + taps + 1)] = r[i];
            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* x = history[(size_t) ch].data() + historyPos + 1;     // x[taps] is the newest
                peak = std::max (peak, std::abs (x[taps / 2]));
                for (int k = 1; k < phases; ++k)
                    peak = std::max (peak, std::abs (dot (coef[(size_t) k].data(), x)));
            }
            historyPos = historyPos == taps ? 0 : historyPos + 1;
            // a sample's gain covers the stretch on either side of it
            const float around = std::max (peak, lastPeak);
            lastPeak = peak;

            // the gain this peak needs, a release that slows while the reduction lasts
            const float limit = threshold * margin;
            const float need = threshold > 0.0f && around > limit ? limit / around : 1.0f;
            if (need < envelope)
                envelope = need;
            else if (envelope < 1.0f)
            {
                sustained += ((envelope < 0.985f ? 1.0f : 0.0f) - sustained) * sustainCoef;
                envelope += (need - envelope) * (relFast + (relSlow - relFast) * sustained);
                if (envelope > 0.99999f)
                    envelope = 1.0f;
            }
            if (envelope >= 1.0f)
                sustained += (0.0f - sustained) * sustainCoef;

            // the smallest over the lookahead, then its mean over the same length: a ramp that is down when the
            // peak arrives
            const float held = holdMin (envelope);
            const float gain = boxMean (held);
            smallest = std::min (smallest, gain);

            // the signal, the lookahead (and the interpolator's half) later
            signal[0][(size_t) write] = l[i];
            signal[1][(size_t) write] = r[i];
            peaks[(size_t) write] = peak;
            const int from = (write - delay) & (ringSize - 1);
            const int peakFrom = (write - look) & (ringSize - 1);
            write = (write + 1) & (ringSize - 1);
            if (gain >= 1.0f)
            {
                l[i] = signal[0][(size_t) from];
                r[i] = signal[1][(size_t) from];
            }
            else
            {
                l[i] = signal[0][(size_t) from] * gain;
                r[i] = signal[1][(size_t) from] * gain;
            }
            outPeak = std::max (outPeak, peaks[(size_t) peakFrom] * gain);
        }
        return smallest;
    }

private:
    static float dot (const float* c, const float* x) noexcept
    {
       #if MASTER_SSE
        __m128 a = _mm_setzero_ps(), b = _mm_setzero_ps();
        for (int j = 0; j < padded; j += 8)
        {
            a = _mm_add_ps (a, _mm_mul_ps (_mm_loadu_ps (c + j), _mm_loadu_ps (x + j)));
            if (j + 4 < padded)
                b = _mm_add_ps (b, _mm_mul_ps (_mm_loadu_ps (c + j + 4), _mm_loadu_ps (x + j + 4)));
        }
        a = _mm_add_ps (a, b);
        a = _mm_add_ps (a, _mm_movehl_ps (a, a));
        a = _mm_add_ss (a, _mm_shuffle_ps (a, a, 1));
        return _mm_cvtss_f32 (a);
       #else
        float v = 0.0f;
        for (int j = 0; j < padded; ++j)
            v += c[j] * x[j];
        return v;
       #endif
    }

    static double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    // The smallest of the last look + 1 values (a deque of increasing values, positions counted in `counter`).
    float holdMin (float v) noexcept
    {
        const long long now = counter++;
        while (! deque.empty() && deque.back().second >= v)
            deque.pop_back();
        deque.push_back ({ now, v });
        while (deque.front().first <= now - (long long) holdValues.size())
            deque.pop_front();
        return deque.front().second;
    }

    // The mean of the last look + 1 values; exactly 1 when they all are.
    float boxMean (float v) noexcept
    {
        const float old = boxValues[(size_t) boxHead];
        boxUnder += (v < 1.0f ? 1 : 0) - (old < 1.0f ? 1 : 0);
        boxValues[(size_t) boxHead] = v;
        boxHead = boxHead + 1 == (int) boxValues.size() ? 0 : boxHead + 1;
        if (boxUnder == 0)
        {
            boxSum = (double) boxValues.size();
            return 1.0f;
        }
        boxSum += (double) v - (double) old;
        return (float) std::min (1.0, boxSum / (double) boxValues.size());
    }

    // A fixed-size deque without allocation.
    struct Ring
    {
        std::array<std::pair<long long, float>, 1024> items {};
        int head = 0, count = 0;
        bool empty() const noexcept { return count == 0; }
        void clear() noexcept { head = count = 0; }
        std::pair<long long, float>& front() noexcept { return items[(size_t) head]; }
        std::pair<long long, float>& back() noexcept { return items[(size_t) ((head + count - 1) & 1023)]; }
        void push_back (std::pair<long long, float> v) noexcept { items[(size_t) ((head + count) & 1023)] = v; ++count; }
        void pop_back() noexcept { --count; }
        void pop_front() noexcept { head = (head + 1) & 1023; --count; }
    };

    double rate = 48000.0;
    int look = 72, delay = 88, ringSize = 1;
    static constexpr int padded = (taps + 1 + 3) / 4 * 4;
    std::array<std::array<float, padded>, phases> coef {};
    std::array<std::array<float, 2 * (taps + 1) + padded>, 2> history {};     // room to read a padded stretch
    int historyPos = 0;
    std::array<std::vector<float>, 2> signal;
    std::vector<float> peaks, holdValues, boxValues;
    int write = 0, holdHead = 0, boxHead = 0, boxUnder = 0;
    double boxSum = 0.0;
    Ring deque;
    int dequeStart = 0;
    long long counter = 0;
    float envelope = 1.0f, sustained = 0.0f, lastPeak = 0.0f;
};

class MasterStage
{
public:
    struct Settings
    {
        bool valveOn = false;
        float amount = 0.35f;           // 0..1
        float character = 0.5f;         // 0 TRANSPARENT, 0.5 WARM, 1 BODY
        float outputDb = 0.0f;
        bool limiterOn = true;
        float ceilingDb = -1.0f;        // dBTP
    };

    // Read by the editor (the audio thread only writes; the editor takes and resets).
    struct Telemetry
    {
        std::atomic<float> inPeak { 0.0f };         // what enters the stage, meter scale
        std::atomic<float> outPeak { 0.0f };        // the true peak that leaves (estimated by the limiter)
        std::atomic<float> reduction { 1.0f };      // the smallest gain of the limiter
        std::atomic<float> residual { 0.0f };       // mean square of what the valve adds, meter scale
        std::atomic<float> level { 0.0f };          // mean square of what enters, meter scale
        std::atomic<int> samples { 0 };
    };

    static float room() noexcept { return 0.6309573f; }      // -4 dB: the meter's 0 dB leaves at -4 dBFS
    static float knee() noexcept { return room(); }

    // Tests: 1 = 2x, 2 = 4x, 3 = 8x (0: four times, twice at 88.2 kHz and above).
    static inline int oversamplingOrder = 0;
    static inline bool maxQuality = false;

    void prepare (double sampleRate, int maxBlock)
    {
        using OS = juce::dsp::Oversampling<float>;
        rate = sampleRate;
        order = oversamplingOrder > 0 ? oversamplingOrder : (sampleRate < 60000.0 ? 2 : 1);
        oversampling = std::make_unique<OS> (2, (size_t) order, OS::filterHalfBandFIREquiripple, maxQuality, true);
        oversampling->initProcessing ((size_t) chunk);
        oversampling->reset();
        osRate = sampleRate * (double) (1 << order);
        limiter.prepare (sampleRate);
        (void) maxBlock;
        reset();
    }

    void reset() noexcept
    {
        if (oversampling != nullptr)
            oversampling->reset();
        limiter.reset();
        for (auto& c : valve)
            c = {};
        primed = false;
    }

    int latency() const noexcept
    {
        return (oversampling != nullptr ? (int) std::lround (oversampling->getLatencyInSamples()) : 0) + limiter.latency();
    }

    void setSettings (const Settings& s) noexcept { target = s; }
    Telemetry& telemetry() noexcept { return meters; }

    void process (float* l, float* r, int n) noexcept
    {
        if (oversampling == nullptr)
            return;
        if (! primed)
        {
            current = target;
            mix = target.valveOn ? 1.0f : 0.0f;
            outGain = juce::Decibels::decibelsToGain (target.outputDb);
            updateValve (true);
            primed = true;
        }
        float inPeak = 0.0f, outPeak = 0.0f, smallest = 1.0f;
        double residualSum = 0.0, levelSum = 0.0;
        const float g = room();
        for (int start = 0; start < n; start += chunk)
        {
            const int count = std::min (chunk, n - start);
            // the settings move a little every chunk (about 20 ms to settle), the switch over 30 ms
            const float k = 1.0f - std::exp (-(float) count / (0.02f * (float) rate));
            current.amount += (target.amount - current.amount) * k;
            current.character += (target.character - current.character) * k;
            updateValve (false);
            for (int i = start; i < start + count; ++i)
            {
                inPeak = std::max (inPeak, std::max (std::abs (l[i]), std::abs (r[i])));
                l[i] *= g;
                r[i] *= g;
            }
            const float mixTarget = target.valveOn ? 1.0f : 0.0f;
            if (mix > 0.0f || mixTarget > 0.0f)
            {
                float* channels[] = { l + start, r + start };
                juce::dsp::AudioBlock<float> io (channels, 2, (size_t) count);
                auto up = oversampling->processSamplesUp (io);
                const int m = (int) up.getNumSamples();
                const float step = 1.0f / (0.03f * (float) osRate);
                float mixEnd = mix;
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    float* x = up.getChannelPointer (ch);
                    auto& v = valve[ch];
                    float mx = mix;
                    for (int i = 0; i < m; ++i)
                    {
                        mx = mixTarget > mx ? std::min (mixTarget, mx + step) : std::max (mixTarget, mx - step);
                        const float dry = x[i];
                        const float s = dry * (1.0f / g);
                        // low shelf in (BODY), the curve, its residual coloured, the shelf undone
                        const double e = v.pre.process (s, preShelf);
                        const float t = std::tanh (drive * (float) e);
                        const float shaped = t / (drive * (1.0f + bias * t));
                        double res = (double) shaped - e;
                        // DC out of the residual (the even harmonics' offset), then its highs softened
                        const double hp = res - v.dcIn + dcCoef * v.dcOut;
                        v.dcIn = res;
                        v.dcOut = hp;
                        v.lp += (hp - v.lp) * lpCoef;
                        res = v.lp;
                        const double y = v.post.process (e + res, postShelf) * makeup;
                        residualSum += res * res;
                        levelSum += (double) s * s;
                        x[i] = dry + mx * ((float) y * g - dry);
                    }
                    mixEnd = mx;
                }
                mix = mixEnd;
                oversampling->processSamplesDown (io);
            }
            else
            {
                // off: the same filters, nothing in between (the delay and the clean sound of before)
                float* channels[] = { l + start, r + start };
                juce::dsp::AudioBlock<float> io (channels, 2, (size_t) count);
                oversampling->processSamplesUp (io);
                oversampling->processSamplesDown (io);
                for (auto& c : valve)
                    c = {};
            }
            // OUTPUT: a ramp over the chunk
            const float gainTo = juce::Decibels::decibelsToGain (target.outputDb);
            if (gainTo != outGain || outGain != 1.0f)
            {
                const float step = (gainTo - outGain) / (float) count;
                for (int i = start; i < start + count; ++i)
                {
                    outGain += step;
                    l[i] *= outGain;
                    r[i] *= outGain;
                }
                outGain = gainTo;
            }
            const float threshold = target.limiterOn ? juce::Decibels::decibelsToGain (juce::jlimit (-12.0f, -0.1f, target.ceilingDb)) : 0.0f;
            smallest = std::min (smallest, limiter.process (l + start, r + start, count, threshold, outPeak));
        }
        if (inPeak > meters.inPeak.load (std::memory_order_relaxed))
            meters.inPeak.store (inPeak, std::memory_order_relaxed);
        if (outPeak > meters.outPeak.load (std::memory_order_relaxed))
            meters.outPeak.store (outPeak, std::memory_order_relaxed);
        if (smallest < meters.reduction.load (std::memory_order_relaxed))
            meters.reduction.store (smallest, std::memory_order_relaxed);
        if (meters.samples.load (std::memory_order_relaxed) < (1 << 22))
        {
            const float scale = 1.0f / (float) (1 << order);
            meters.residual.store (meters.residual.load (std::memory_order_relaxed) + (float) residualSum * scale * 0.5f, std::memory_order_relaxed);
            meters.level.store (meters.level.load (std::memory_order_relaxed) + (float) levelSum * scale * 0.5f, std::memory_order_relaxed);
            meters.samples.fetch_add (n, std::memory_order_relaxed);
        }
    }

    // The curve and what CHARACTER does with it, for a setting (also used by the measurements).
    struct Curve
    {
        float drive, bias, lowShelfDb, residualHz, makeup;
    };

    static Curve curveFor (float amount, float character) noexcept
    {
        const float a = juce::jlimit (0.0f, 1.0f, amount), c = juce::jlimit (0.0f, 1.0f, character);
        // TRANSPARENT -> WARM -> BODY
        const float w = std::min (1.0f, c * 2.0f), b = std::max (0.0f, c * 2.0f - 1.0f);
        Curve k;
        const float driveScale = 0.85f + 0.15f * w;
        k.drive = std::max (1.0e-5f, maxDrive * std::pow (a, 1.5f) * driveScale);
        k.bias = 0.10f + 0.20f * w - 0.04f * b;
        k.lowShelfDb = 2.5f * b;
        k.residualHz = 18000.0f * std::pow (7000.0f / 18000.0f, w) * std::pow (5000.0f / 7000.0f, b);
        // the fundamental of a tone at the meter's -6 dB through the curve: its gain undone
        constexpr int points = 64;
        constexpr float amp = 0.5011872f;
        double fundamental = 0.0;
        for (int i = 0; i < points; ++i)
        {
            const double s = std::sin ((i + 0.5) * juce::MathConstants<double>::twoPi / points);
            const float t = std::tanh (k.drive * amp * (float) s);
            fundamental += (double) (t / (k.drive * (1.0f + k.bias * t))) * s;
        }
        fundamental *= 2.0 / points / amp;
        k.makeup = (float) (1.0 / std::max (0.1, fundamental));
        return k;
    }

    static inline float maxDrive = 1.0f;       // AMOUNT 100 %: at the edge of distortion (0.6 until 24-11-2026, too polite)

    static Settings settingsFrom (const float* p) noexcept;     // defined where the parameters are known

private:
    struct Shelf
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };

    struct Biquad
    {
        double z1 = 0, z2 = 0;
        double process (double x, const Shelf& c) noexcept
        {
            const double y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }
    };

    struct ValveState
    {
        Biquad pre, post;
        double dcIn = 0, dcOut = 0, lp = 0;
    };

    static Shelf lowShelf (double fs, double hz, double gainDb)
    {
        // RBJ low shelf, S = 1
        Shelf s;
        if (std::abs (gainDb) < 1.0e-4)
            return s;
        const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * hz / fs;
        const double cw = std::cos (w), sw = std::sin (w), alpha = sw / 2.0 * std::sqrt (2.0);
        const double sa = 2.0 * std::sqrt (A) * alpha;
        const double a0 = (A + 1) + (A - 1) * cw + sa;
        s.b0 = A * ((A + 1) - (A - 1) * cw + sa) / a0;
        s.b1 = 2 * A * ((A - 1) - (A + 1) * cw) / a0;
        s.b2 = A * ((A + 1) - (A - 1) * cw - sa) / a0;
        s.a1 = -2 * ((A - 1) + (A + 1) * cw) / a0;
        s.a2 = ((A + 1) + (A - 1) * cw - sa) / a0;
        return s;
    }

    static Shelf inverse (const Shelf& s)
    {
        // the zeros become the poles: exactly the shelf undone
        Shelf i;
        i.b0 = 1.0 / s.b0;
        i.b1 = s.a1 / s.b0;
        i.b2 = s.a2 / s.b0;
        i.a1 = s.b1 / s.b0;
        i.a2 = s.b2 / s.b0;
        return i;
    }

    void updateValve (bool force) noexcept
    {
        if (! force && std::abs (current.amount - shownAmount) < 1.0e-5f && std::abs (current.character - shownCharacter) < 1.0e-5f)
            return;
        shownAmount = current.amount;
        shownCharacter = current.character;
        const auto k = curveFor (current.amount, current.character);
        drive = k.drive;
        bias = k.bias;
        makeup = k.makeup;
        preShelf = lowShelf (osRate, 160.0, k.lowShelfDb);
        postShelf = inverse (preShelf);
        lpCoef = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * k.residualHz / osRate);
        dcCoef = std::exp (-juce::MathConstants<double>::twoPi * 5.0 / osRate);
    }

    static constexpr int chunk = 32;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    TruePeakLimiter limiter;
    std::array<ValveState, 2> valve {};
    Settings target, current;
    Telemetry meters;
    double rate = 48000.0, osRate = 192000.0;
    int order = 2;
    bool primed = false;
    float mix = 0.0f, outGain = 1.0f, shownAmount = -1.0f, shownCharacter = -1.0f;
    float drive = 0.1f, bias = 0.0f, makeup = 1.0f;
    Shelf preShelf, postShelf;
    double lpCoef = 1.0, dcCoef = 0.9999;
};
