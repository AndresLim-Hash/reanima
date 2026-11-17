#pragma once

#include "DspUtil.h"
#include <juce_dsp/juce_dsp.h>

// Real-time stereo noise shaped by band amplitudes: overlap-add of random-phase
// spectra (Hann windows, 75 % overlap). Band amplitudes are the per-bin RMS
// magnitudes measured by the analysis with the same FFT size and window, so the
// output has the analysed level.
class NoiseSynth
{
public:
    static constexpr int order = 10, size = 1 << order, hop = size / 4;
    static constexpr int numBands = 24;

    explicit NoiseSynth (juce::int64 seed = 1) : rng (seed) {}

    void reset() noexcept
    {
        accL.fill (0.0f);
        accR.fill (0.0f);
        readPos = 0;
        untilNext = 0;
    }

    // Adds n samples of noise to out. bandAmp: numBands values; edges: numBands + 1
    // band edges in Hz; freqScale: pitch ratio applied to the spectral envelope.
    void process (float* outL, float* outR, int n, const float* bandAmp, const float* edges,
                  float freqScale, double sampleRate) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            if (untilNext <= 0)
            {
                synthFrame (accL, bandAmp, edges, freqScale, sampleRate);
                synthFrame (accR, bandAmp, edges, freqScale, sampleRate);
                untilNext = hop;
            }

            outL[i] += accL[(size_t) readPos];
            outR[i] += accR[(size_t) readPos];
            accL[(size_t) readPos] = 0.0f;
            accR[(size_t) readPos] = 0.0f;
            readPos = (readPos + 1) & (size - 1);
            --untilNext;
        }
    }

    static const std::array<float, size>& window()
    {
        static const auto w = []
        {
            std::array<float, size> a {};
            for (int i = 0; i < size; ++i)
                a[(size_t) i] = 0.5f - 0.5f * std::cos (Dsp::twoPi * (float) i / (float) size);
            return a;
        }();
        return w;
    }

    static const juce::dsp::FFT& fft()
    {
        static const juce::dsp::FFT f (order);
        return f;
    }

private:
    void synthFrame (std::array<float, size>& acc, const float* bandAmp, const float* edges, float freqScale, double sr) noexcept
    {
        // Hann analysis and synthesis at 75 % overlap with an inverse FFT scaled by 1/N.
        const float gain = std::sqrt (64.0f * hop / (9.0f * size));
        const auto& sn = Dsp::sine();
        std::fill (work.begin(), work.end(), 0.0f);

        int band = 0;
        const float binHz = (float) sr / size / std::max (0.01f, freqScale);
        for (int k = 1; k < size / 2; ++k)
        {
            const float f = (float) k * binHz;
            while (band < numBands - 1 && f >= edges[band + 1])
                ++band;
            const float amp = (f < edges[0] || f > edges[numBands]) ? 0.0f : bandAmp[band] * gain;
            if (amp <= 0.0f)
                continue;
            const double ph = rng.nextDouble();
            work[(size_t) k * 2] = amp * sn (ph + 0.25);
            work[(size_t) k * 2 + 1] = amp * sn (ph);
        }

        fft().performRealOnlyInverseTransform (work.data());

        const auto& w = window();
        for (int i = 0; i < size; ++i)
            acc[(size_t) ((readPos + i) & (size - 1))] += work[(size_t) i] * w[(size_t) i];
    }

    juce::Random rng;
    std::array<float, size> accL {}, accR {};
    std::array<float, size * 2> work {};
    int readPos = 0, untilNext = 0;
};
