#pragma once

#include "Params.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace Dsp
{
constexpr float pi = juce::MathConstants<float>::pi;
constexpr float twoPi = juce::MathConstants<float>::twoPi;

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }

// Constant power: both sides at unity in the centre, the far side at zero when panned fully.
inline void panGains (float pan, float& left, float& right) noexcept
{
    const float angle = (pan + 1.0f) * pi * 0.25f;
    left = std::cos (angle) * juce::MathConstants<float>::sqrt2;
    right = std::sin (angle) * juce::MathConstants<float>::sqrt2;
}

// Length in beats of each entry of the tempo division menus.
inline double beatsForDivision (int index) noexcept
{
    static constexpr double beats[] = { 0.125, 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0, 2.0, 4.0, 8.0, 16.0 };
    return beats[juce::jlimit (0, 10, index)];
}

// std::floor without the library call it is on x64 without SSE4.1 (one per sample per read in the hot
// loops): the same value bit for bit, -0.0 and NaN included. Past 2^52 (2^23 for float) a value is whole.
inline double floorFast (double x) noexcept
{
    if (! (std::abs (x) < 4503599627370496.0))
        return x;
    const double t = (double) (long long) x;
    return t > x ? t - 1.0 : (t == x ? x : t);
}

inline float floorFast (float x) noexcept
{
    if (! (std::abs (x) < 8388608.0f))
        return x;
    const float t = (float) (int) x;
    return t > x ? t - 1.0f : (t == x ? x : t);
}

inline float fastTanh (float x) noexcept
{
    x = juce::jlimit (-3.0f, 3.0f, x);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Sine lookup (phase in cycles, any value).
struct SineTable
{
    static constexpr int size = 4096;
    std::array<float, size + 2> v {};       // the phase can round up to `size`, and it reads one more

    SineTable()
    {
        for (int i = 0; i <= size + 1; ++i)
            v[(size_t) i] = std::sin (twoPi * (float) i / (float) size);
    }

    float operator() (double cycles) const noexcept
    {
        const double frac = cycles - floorFast (cycles);
        const float f = (float) frac * size;
        const int i = (int) f;
        return v[(size_t) i] + (f - (float) i) * (v[(size_t) i + 1] - v[(size_t) i]);
    }
};

inline const SineTable& sine()
{
    static const SineTable table;
    return table;
}

// Smoothed random walk in [-1, 1] for slow "breathing" variations.
struct Drift
{
    float value = 0.0f, target = 0.0f;
    int countdown = 0;

    void tick (juce::Random& rng, int samples, float rateHz, double sr) noexcept
    {
        countdown -= samples;
        if (countdown <= 0)
        {
            target = rng.nextFloat() * 2.0f - 1.0f;
            countdown = std::max (1, (int) (sr / std::max (0.05f, rateHz) * (0.6 + 0.8 * rng.nextFloat())));
        }
        value += (target - value) * (1.0f - std::exp (-(float) samples * rateHz * 2.0f / (float) sr));
    }
};

// RBJ biquad, transposed direct form II.
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] {}, z2[2] {};

    void reset() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0f; }

    float process (int ch, float x) noexcept
    {
        const float y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }

    enum Kind { LowPass, HighPass, BandPass, Peak, LowShelf, HighShelf, AllPass };

    void set (Kind kind, double sr, float freq, float q, float gainDb = 0.0f) noexcept
    {
        const double w0 = juce::MathConstants<double>::twoPi * juce::jlimit (10.0, sr * 0.49, (double) freq) / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * std::max (0.05, (double) q));
        const double A = std::pow (10.0, gainDb / 40.0);
        double nb0 = 1, nb1 = 0, nb2 = 0, na0 = 1, na1 = 0, na2 = 0;

        switch (kind)
        {
            case LowPass:  nb0 = (1 - cw) / 2; nb1 = 1 - cw; nb2 = (1 - cw) / 2; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
            case HighPass: nb0 = (1 + cw) / 2; nb1 = -(1 + cw); nb2 = (1 + cw) / 2; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
            case AllPass:  nb0 = 1 - alpha; nb1 = -2 * cw; nb2 = 1 + alpha; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
            case BandPass: nb0 = alpha; nb1 = 0; nb2 = -alpha; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
            case Peak:     nb0 = 1 + alpha * A; nb1 = -2 * cw; nb2 = 1 - alpha * A; na0 = 1 + alpha / A; na1 = -2 * cw; na2 = 1 - alpha / A; break;
            case LowShelf:
            {
                const double s = 2 * std::sqrt (A) * alpha;
                nb0 = A * ((A + 1) - (A - 1) * cw + s); nb1 = 2 * A * ((A - 1) - (A + 1) * cw); nb2 = A * ((A + 1) - (A - 1) * cw - s);
                na0 = (A + 1) + (A - 1) * cw + s; na1 = -2 * ((A - 1) + (A + 1) * cw); na2 = (A + 1) + (A - 1) * cw - s;
                break;
            }
            case HighShelf:
            {
                const double s = 2 * std::sqrt (A) * alpha;
                nb0 = A * ((A + 1) + (A - 1) * cw + s); nb1 = -2 * A * ((A - 1) + (A + 1) * cw); nb2 = A * ((A + 1) + (A - 1) * cw - s);
                na0 = (A + 1) - (A - 1) * cw + s; na1 = 2 * ((A - 1) - (A + 1) * cw); na2 = (A + 1) - (A - 1) * cw - s;
                break;
            }
        }

        b0 = (float) (nb0 / na0); b1 = (float) (nb1 / na0); b2 = (float) (nb2 / na0);
        a1 = (float) (na1 / na0); a2 = (float) (na2 / na0);
    }
};

// Circular delay line with linear interpolation.
struct DelayLine
{
    std::vector<float> buf;
    int mask = 0, index = 0;

    void prepare (int maxSamples)
    {
        const int size = juce::nextPowerOfTwo (std::max (4, maxSamples + 4));
        buf.assign ((size_t) size, 0.0f);
        mask = size - 1;
        index = 0;
    }

    void reset() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); index = 0; }

    void push (float x) noexcept
    {
        buf[(size_t) index] = x;
        index = (index + 1) & mask;
    }

    // Delay in samples, >= 1 after push. Whole and fractional parts stay apart, so that the
    // precision does not depend on how long the line has been running.
    float read (float delay) const noexcept
    {
        const int whole = (int) delay;
        const float f = delay - (float) whole;
        const float a = buf[(size_t) ((index - whole) & mask)], b = buf[(size_t) ((index - whole - 1) & mask)];
        return a + f * (b - a);
    }
};
} // namespace Dsp
