#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <memory>

// The output stage of the master: 6 dB of drive above the meter's 0 dB, turned into saturation, never into
// level past the output's ceiling.
//
// The meter shows the signal before this stage. Up to its 0 dB the stage is clean: the signal goes out with
// 3 dB of room under the ceiling (a linear gain, nothing added). From 0 to +6 dB a soft knee (tanh, smooth to
// its second derivative) rounds the peaks more and more: more harmonics and density, a little more level,
// and at +6 dB the full saturation just under the ceiling. Above that it only gets denser.
//
// The shaping runs four times oversampled (twice at 88.2 kHz and above: the same 176-192 kHz inside) through
// linear-phase FIR half-bands: what the curve makes above the audio band is filtered out instead of folding
// back, and the wave keeps its shape (a clean signal is only delayed), so its peaks, between the samples too,
// stay where the curve put them. The delay is reported to the host, which compensates it.
//
// Measured before (a knee at -3 dBFS reaching -0.6 dBFS at 1x): already -28 dB of THD at the meter's 0 dB, a
// flat top (hard clipping) from +2 dB, aliasing at -47 dB and drum hits up to +4.4 dB over 0 dBFS between
// the samples: the clicks and the crackle. With IIR half-bands (no latency) the wave changed shape near
// 20 kHz and drum hits came out up to 2.6 dB over 0 dBFS: not used here.
class MasterStage
{
public:
    static float ceiling() noexcept { return 0.8912509f; }      // -1 dBFS: the most the shaped wave reaches
    static float knee() noexcept { return 0.6309573f; }         // -4 dBFS: the meter's 0 dB, 3 dB under it

    void prepare (double sampleRate, int maxBlock)
    {
        using OS = juce::dsp::Oversampling<float>;
        oversampling = std::make_unique<OS> (2, sampleRate < 60000.0 ? 2 : 1, OS::filterHalfBandFIREquiripple, maxQuality, true);
        block = std::max (1, maxBlock);
        oversampling->initProcessing ((size_t) block);
        oversampling->reset();
    }

    void reset() noexcept
    {
        if (oversampling != nullptr)
            oversampling->reset();
    }

    static inline bool maxQuality = false;     // measured both ways (Audit "master")

    int latency() const noexcept { return oversampling != nullptr ? (int) std::lround (oversampling->getLatencyInSamples()) : 0; }

    void process (float* l, float* r, int n) noexcept
    {
        if (oversampling == nullptr)
            return;
        const float g = knee();
        for (int start = 0; start < n; start += block)
        {
            const int count = std::min (block, n - start);
            for (int i = start; i < start + count; ++i)
            {
                l[i] *= g;
                r[i] *= g;
            }
            float* channels[] = { l + start, r + start };
            juce::dsp::AudioBlock<float> io (channels, 2, (size_t) count);
            auto up = oversampling->processSamplesUp (io);
            for (size_t ch = 0; ch < 2; ++ch)
            {
                float* x = up.getChannelPointer (ch);
                for (size_t i = 0; i < up.getNumSamples(); ++i)
                    x[i] = shape (x[i]);
            }
            oversampling->processSamplesDown (io);
            // Back at the host's rate, what the downsampling filter brought back over the shaped peaks (a wave
            // squared up loses its highest harmonics and rings a little): a soft guard over the last 0.2 dB,
            // so that no sample reaches -0.3 dBFS. Real material stays under it; white noise up to Nyquist
            // driven to +6 dB went 1.4 dB over 0 dBFS between the samples without it.
            for (int i = start; i < start + count; ++i)
            {
                l[i] = guard (l[i]);
                r[i] = guard (r[i]);
            }
        }
    }

    static float guard (float v) noexcept
    {
        constexpr float from = 0.9440609f, room = 0.0220730f;     // -0.5 dBFS, up to -0.3 dBFS
        const float a = std::abs (v);
        return a <= from ? v : std::copysign (from + room * std::tanh ((a - from) / room), v);
    }

    // The curve: exact up to the knee, then the knee rounds into the ceiling (C2 at the knee: no corner).
    static float shape (float v) noexcept
    {
        const float a = std::abs (v), k = knee();
        if (a <= k)
            return v;
        const float w = ceiling() - k;
        return std::copysign (k + w * std::tanh ((a - k) / w), v);
    }

private:
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    int block = 512;
};
