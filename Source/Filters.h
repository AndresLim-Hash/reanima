#pragma once

#include "DspUtil.h"
#include "HalfBand.h"
#include <algorithm>
#include <array>
#include <vector>

//==============================================================================
// Auto Filter-style filter: SVF (Clean/OSR/MS2), transistor ladder (SMP/PRD lowpass),
// DJ, Comb and Vowel modes, 12/24 dB, drive and dry/wet.
// The circuits that saturate run at four times the sample rate (twice at 88.2 kHz and above: the same
// 176-192 kHz inside, half the work): their saturation makes harmonics far above the audible band, and
// at the host's rate those fold back as notes foreign to the sound (measured: -3 dB of folded energy for
// a 6 kHz tone with 24 dB of drive). Clean, Comb and Vowel do not saturate their filter and run as they are.
class VoiceFilter final
{
    static constexpr float pi = Dsp::pi;
    static float dbToGain (float db) noexcept { return Dsp::dbToGain (db); }
    static float fastTanh (float x) noexcept { return Dsp::fastTanh (x); }

public:
    void prepare (double sr)
    {
        sampleRate = sr;
        runRate = sr * factor;
        const int size = juce::nextPowerOfTwo ((int) (sr / 15.0) + 8);
        for (auto& c : comb)
            c.assign ((size_t) size, 0.0f);
        combMask = size - 1;
        dcPole = 1.0f - (float) (2.0 * pi * 5.0 / sr);

        // Outer stage (1x to 2x): flat to 20 kHz at 48 kHz. Inner stage (2x to 4x): wider band.
        static const auto outer = HalfBand::design (outerCoefs, 0.0417);
        static const auto inner = HalfBand::design (innerCoefs, 0.1);
        upOuter.setCoefs (outer);
        downOuter.setCoefs (outer);
        upInner.setCoefs (inner);
        downInner.setCoefs (inner);
        reset();
    }

    void reset()
    {
        for (auto& ch : svf) for (auto& s : ch) s = {};
#if HALFBAND_SSE
        for (int i = 0; i < 2; ++i)
            for (int lane = 0; lane < 4; ++lane)
                pairIc1[i][lane] = pairIc2[i][lane] = 0.0f;
#endif
        for (auto& ch : vowelState) for (auto& s : ch) s = {};
        for (auto& ch : ladder) ch.fill (0.0f);
        ladderFb.fill (0.0f);
        for (auto& c : comb) std::fill (c.begin(), c.end(), 0.0f);
        combIndex = 0;
        dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0.0f;
        resetOversampling();
    }

    void resetOversampling() noexcept
    {
        upOuter.reset();
        downOuter.reset();
        upInner.reset();
        downInner.reset();
    }

    void set (int newType, int slope, int newCircuit, float cutoff, float res, float driveDb, float newMorph, float newMix)
    {
        type = newType;
        circuit = newCircuit;
        twoStage = slope == 1;
        morph = juce::jlimit (0.0f, 1.0f, newMorph);
        mix = juce::jlimit (0.0f, 1.0f, newMix);
        res = juce::jlimit (0.0f, 1.0f, res);

        drive = circuit == P::Clean ? 1.0f : dbToGain (juce::jmax (0.0f, driveDb)) * (circuit == P::PRD ? 1.5f : 1.0f);
        driveComp = 1.0f / std::sqrt (drive);

        float fc = cutoff;
        djHighpass = false;
        if (type == P::DJ)
        {
            if (morph < 0.5f)
                fc = 20.0f * std::pow (1000.0f, morph * 2.0f);
            else
            {
                djHighpass = true;
                fc = 20.0f * std::pow (1000.0f, (morph - 0.5f) * 2.0f);
            }
        }

        fc = juce::jlimit (20.0f, (float) sampleRate * 0.45f, fc);
        const int oversampling = circuit != P::Clean && type != P::Comb && type != P::Vowel ? (sampleRate < 60000.0 ? 4 : 2) : 1;
        if (oversampling != factor)
        {
            factor = oversampling;
            resetOversampling();
        }
        runRate = sampleRate * factor;
        computeSvf (fc, 0.5f + res * res * 24.0f, g, k, a1, a2, a3);

        ladderG = g / (1.0f + g);
        ladderK = res * (twoStage ? 3.9f : 1.9f);
        useLadder = (circuit == P::SMP || circuit == P::PRD) && type == P::LP;
        {
            const float pos = morph * 4.0f;
            morphIndex = std::min (3, (int) pos);
            morphFrac = pos - (float) morphIndex;
        }
#if HALFBAND_SSE
        pairPath = ! useLadder && type != P::Comb && type != P::Vowel;
#endif

        combDelay = juce::jlimit (2.0f, (float) combMask - 2.0f, (float) sampleRate / fc);
        combFb = res * 0.97f * (morph < 0.5f ? 1.0f : -1.0f);

        if (type == P::Vowel)
        {
            static const float formants[5][3] = { { 800, 1150, 2900 }, { 400, 1600, 2700 }, { 270, 2290, 3010 },
                                                  { 450, 800, 2830 }, { 325, 700, 2530 } };
            static const float gains[3] = { 1.0f, 0.5f, 0.25f };
            const float pos = morph * 4.0f;
            const int i0 = std::min (3, (int) pos);
            const float frac = pos - (float) i0;
            const float scale = juce::jlimit (0.5f, 2.0f, std::sqrt (cutoff / 1000.0f));

            for (int j = 0; j < 3; ++j)
            {
                const float f = (formants[i0][j] + frac * (formants[i0 + 1][j] - formants[i0][j])) * scale;
                float unused;
                computeSvf (juce::jlimit (20.0f, (float) sampleRate * 0.45f, f), 4.0f + res * 16.0f,
                            unused, vk[j], va1[j], va2[j], va3[j]);
                vgain[j] = gains[j];
            }
        }
    }

    // At four times the rate the two channels go through the half-band stages together.
    void process (float& l, float& r) noexcept
    {
#if HALFBAND_SSE
        if (pairPath && factor == 1)
            corePair (l, r);
        else
#endif
        if (factor == 1)
        {
            l = processCore (0, l);
            r = processCore (1, r);
        }
        else if (factor == 2)
        {
            alignas (16) float u[4];
            upOuter.process (l, r, u);     // u: left's two samples, then right's
#if HALFBAND_SSE
            if (pairPath)
            {
                corePair (u[0], u[2]);
                corePair (u[1], u[3]);
            }
            else
#endif
            {
                u[0] = processCore (0, u[0]);
                u[1] = processCore (0, u[1]);
                u[2] = processCore (1, u[2]);
                u[3] = processCore (1, u[3]);
            }
            downOuter.process (u[0], u[1], u[2], u[3], l, r);
        }
        else
        {
            alignas (16) float u[4], first[4], second[4];
            upOuter.process (l, r, u);
            upInner.process (u[0], u[2], first);
            upInner.process (u[1], u[3], second);
            float l0 = first[0], l1 = first[1], l2 = second[0], l3 = second[1];
            float r0 = first[2], r1 = first[3], r2 = second[2], r3 = second[3];
#if HALFBAND_SSE
            if (pairPath)
            {
                corePair (l0, r0);
                corePair (l1, r1);
                corePair (l2, r2);
                corePair (l3, r3);
            }
            else
#endif
            {
                l0 = processCore (0, l0);
                l1 = processCore (0, l1);
                l2 = processCore (0, l2);
                l3 = processCore (0, l3);
                r0 = processCore (1, r0);
                r1 = processCore (1, r1);
                r2 = processCore (1, r2);
                r3 = processCore (1, r3);
            }
            float dl0, dr0, dl1, dr1;
            // In time order. Up to 0.9.5 the second pair went in first (the two calls were arguments of one
            // call, which the compiler evaluated right to left): the top octave came out darker, -1.2 dB at
            // 10 kHz and -3.4 dB at 19 kHz. In order the chain is flat (research/rendimiento_2026-11-15/hbtest.py).
            downInner.process (l0, l1, r0, r1, dl0, dr0);
            downInner.process (l2, l3, r2, r3, dl1, dr1);
            downOuter.process (dl0, dl1, dr0, dr1, l, r);
        }
        combIndex = (combIndex + 1) & combMask;

        // MS2 saturates one side more than the other, which leaves a DC offset (measured: +0.02 at the
        // output with 24 dB of drive): taken out here. The other circuits are symmetric.
        if (circuit == P::MS2)
        {
            const float yl = l - dcX[0] + dcPole * dcY[0], yr = r - dcX[1] + dcPole * dcY[1];
            dcX[0] = l;
            dcX[1] = r;
            dcY[0] = yl;
            dcY[1] = yr;
            l = yl;
            r = yr;
        }
    }

private:
    struct SvfState { float ic1 = 0, ic2 = 0; };

    void computeSvf (float fc, float q, float& gOut, float& kOut, float& a1Out, float& a2Out, float& a3Out) const noexcept
    {
        gOut = std::tan (pi * fc / (float) runRate);
        kOut = 1.0f / q;
        a1Out = 1.0f / (1.0f + gOut * (gOut + kOut));
        a2Out = gOut * a1Out;
        a3Out = gOut * a2Out;
    }

    static inline void tickSvf (SvfState& st, float x, float a1, float a2, float a3, float& lp, float& bp) noexcept
    {
        const float v3 = x - st.ic2;
        const float v1 = a1 * st.ic1 + a2 * v3;
        const float v2 = st.ic2 + a2 * st.ic1 + a3 * v3;
        st.ic1 = 2.0f * v1 - st.ic1;
        st.ic2 = 2.0f * v2 - st.ic2;
        lp = v2;
        bp = v1;
    }

    float pickOutput (float x, float lp, float bp) const noexcept
    {
        const float hp = x - k * bp - lp;
        const float bpn = bp * k;
        const float notch = lp + hp;

        switch (type)
        {
            case P::LP:    return lp;
            case P::HP:    return hp;
            case P::BP:    return bpn;
            case P::Notch: return notch;
            case P::DJ:    return djHighpass ? hp : lp;
            case P::Morph:
            {
                const float outs[5] = { lp, bpn, hp, notch, lp };
                return outs[morphIndex] + morphFrac * (outs[morphIndex + 1] - outs[morphIndex]);
            }
            default:       return lp;
        }
    }

    float stage (int ch, int idx, float x) noexcept
    {
        auto& st = svf[(size_t) ch][(size_t) idx];
        float lp, bp;
        tickSvf (st, x, a1, a2, a3, lp, bp);

        switch (circuit)
        {
            case P::OSR: st.ic1 = 2.0f * fastTanh (st.ic1 * 0.5f); break;
            case P::MS2: st.ic1 = st.ic1 > 0.0f ? 2.0f * fastTanh (st.ic1 * 0.5f) : 1.6f * fastTanh (st.ic1 * 0.625f); break;
            case P::SMP:
            case P::PRD: st.ic1 = 1.25f * fastTanh (st.ic1 * 0.8f); break;
            default: break;
        }

        return pickOutput (x, lp, bp);
    }

    float processLadder (int ch, float x) noexcept
    {
        auto& s = ladder[(size_t) ch];
        float u = fastTanh (x * drive - ladderK * ladderFb[(size_t) ch]);
        float y2 = 0.0f;

        for (int j = 0; j < 4; ++j)
        {
            const float v = (u - s[(size_t) j]) * ladderG;
            const float y = v + s[(size_t) j];
            s[(size_t) j] = y + v;
            u = y;
            if (j == 1)
                y2 = y;
        }

        const float out = twoStage ? u : y2;
        ladderFb[(size_t) ch] = out;
        const float comp = circuit == P::PRD ? 1.0f + ladderK * 0.5f : 1.0f + ladderK * 0.25f;
        return out * comp * driveComp;
    }

#if HALFBAND_SSE
    // The state-variable path (every circuit but the ladder; not Comb or Vowel) for both channels side by
    // side: lanes hold (L, R, L, R), and each lane does the operations of processCore, stage and pickOutput
    // in their order, so the result is the same bit for bit in about half the time.
    static __m128 tanhQuad (__m128 x) noexcept
    {
        x = _mm_min_ps (_mm_set1_ps (3.0f), _mm_max_ps (_mm_set1_ps (-3.0f), x));
        const __m128 x2 = _mm_mul_ps (x, x);
        return _mm_div_ps (_mm_mul_ps (x, _mm_add_ps (_mm_set1_ps (27.0f), x2)),
                           _mm_add_ps (_mm_set1_ps (27.0f), _mm_mul_ps (_mm_set1_ps (9.0f), x2)));
    }

    // Lanes 0-1 from a where the mask is set, from b where not.
    static __m128 pick (__m128 mask, __m128 a, __m128 b) noexcept { return _mm_or_ps (_mm_and_ps (mask, a), _mm_andnot_ps (mask, b)); }
    static __m128 low (__m128 v) noexcept { return _mm_movelh_ps (v, v); }      // (a, b, a, b)
    static __m128 high (__m128 v) noexcept { return _mm_movehl_ps (v, v); }     // (c, d, c, d)

    __m128 stagePair (int idx, __m128 x) noexcept
    {
        __m128 ic1 = _mm_load_ps (pairIc1[idx]), ic2 = _mm_load_ps (pairIc2[idx]);
        const __m128 A1 = _mm_set1_ps (a1), A2 = _mm_set1_ps (a2), A3 = _mm_set1_ps (a3), two = _mm_set1_ps (2.0f);
        const __m128 v3 = _mm_sub_ps (x, ic2);
        const __m128 v1 = _mm_add_ps (_mm_mul_ps (A1, ic1), _mm_mul_ps (A2, v3));
        const __m128 v2 = _mm_add_ps (_mm_add_ps (ic2, _mm_mul_ps (A2, ic1)), _mm_mul_ps (A3, v3));
        ic1 = _mm_sub_ps (_mm_mul_ps (two, v1), ic1);
        ic2 = _mm_sub_ps (_mm_mul_ps (two, v2), ic2);

        switch (circuit)
        {
            case P::OSR: ic1 = _mm_mul_ps (two, tanhQuad (_mm_mul_ps (ic1, _mm_set1_ps (0.5f)))); break;
            case P::MS2:
            {
                // (L, R) * 0.5 for the positive side, (L, R) * 0.625 for the negative, in one call.
                const __m128 t = tanhQuad (_mm_mul_ps (ic1, _mm_setr_ps (0.5f, 0.5f, 0.625f, 0.625f)));
                const __m128 scaled = _mm_mul_ps (_mm_setr_ps (2.0f, 2.0f, 1.6f, 1.6f), t);
                ic1 = low (pick (_mm_cmpgt_ps (ic1, _mm_setzero_ps()), scaled, high (scaled)));
                break;
            }
            case P::SMP:
            case P::PRD: ic1 = _mm_mul_ps (_mm_set1_ps (1.25f), tanhQuad (_mm_mul_ps (ic1, _mm_set1_ps (0.8f)))); break;
            default: break;
        }
        _mm_store_ps (pairIc1[idx], ic1);
        _mm_store_ps (pairIc2[idx], ic2);

        const __m128 lp = v2, bp = v1, K = _mm_set1_ps (k);
        const __m128 hp = _mm_sub_ps (_mm_sub_ps (x, _mm_mul_ps (K, bp)), lp);
        const __m128 bpn = _mm_mul_ps (bp, K);
        const __m128 notch = _mm_add_ps (lp, hp);
        switch (type)
        {
            case P::LP:    return lp;
            case P::HP:    return hp;
            case P::BP:    return bpn;
            case P::Notch: return notch;
            case P::DJ:    return djHighpass ? hp : lp;
            case P::Morph:
            {
                const __m128 outs[5] = { lp, bpn, hp, notch, lp };
                const __m128 from = outs[morphIndex], to = outs[morphIndex + 1];
                return _mm_add_ps (from, _mm_mul_ps (_mm_set1_ps (morphFrac), _mm_sub_ps (to, from)));
            }
            default:       return lp;
        }
    }

    void corePair (float& l, float& r) noexcept
    {
        const __m128 dry = _mm_setr_ps (l, r, l, r);
        __m128 x = dry;
        if (circuit != P::Clean)
        {
            const __m128 driven = _mm_mul_ps (x, _mm_set1_ps (drive));
            if (circuit == P::MS2)
            {
                // fastTanh (driven * 1.25) in lanes 0-1 and fastTanh (driven) in 2-3, in one call.
                const __m128 t = tanhQuad (_mm_movelh_ps (_mm_mul_ps (driven, _mm_set1_ps (1.25f)), driven));
                const __m128 shaped = pick (_mm_cmplt_ps (driven, _mm_setzero_ps()), _mm_mul_ps (_mm_set1_ps (0.8f), t), high (t));
                x = _mm_mul_ps (low (shaped), _mm_set1_ps (driveComp));
            }
            else
                x = _mm_mul_ps (tanhQuad (driven), _mm_set1_ps (driveComp));
        }
        __m128 y = stagePair (0, x);
        if (twoStage)
            y = stagePair (1, y);
        alignas (16) float out[4];
        _mm_store_ps (out, _mm_add_ps (dry, _mm_mul_ps (_mm_sub_ps (y, dry), _mm_set1_ps (mix))));
        l = out[0];
        r = out[1];
    }

    alignas (16) float pairIc1[2][4] {}, pairIc2[2][4] {};
    bool pairPath = false;
#endif

    float processCore (int ch, float x) noexcept
    {
        const float dry = x;
        float y;

        if (circuit != P::Clean && ! useLadder)     // the ladder has its drive inside
        {
            const float driven = x * drive;
            x = (circuit == P::MS2 && driven < 0.0f ? 0.8f * fastTanh (driven * 1.25f) : fastTanh (driven)) * driveComp;
        }

        if (type == P::Comb)
        {
            auto& buf = comb[(size_t) ch];
            const int whole = (int) combDelay;
            const float frac = combDelay - (float) whole;
            const float newer = buf[(size_t) ((combIndex - whole) & combMask)], older = buf[(size_t) ((combIndex - whole - 1) & combMask)];
            y = x + combFb * (newer + frac * (older - newer));
            buf[(size_t) combIndex] = juce::jlimit (-4.0f, 4.0f, y);
            y *= 1.0f - std::abs (combFb) * 0.5f;
        }
        else if (type == P::Vowel)
        {
            y = 0.0f;
            for (int j = 0; j < 3; ++j)
            {
                float lp, bp;
                tickSvf (vowelState[(size_t) ch][(size_t) j], x, va1[j], va2[j], va3[j], lp, bp);
                y += bp * vk[j] * vgain[j];
            }
            y *= 1.5f;
        }
        else if (useLadder)
        {
            y = processLadder (ch, dry);
        }
        else
        {
            y = stage (ch, 0, x);
            if (twoStage)
                y = stage (ch, 1, y);
        }

        return dry + (y - dry) * mix;
    }

    double sampleRate = 48000.0, runRate = 48000.0;
    int factor = 1;
    float dcPole = 0.999f, dcX[2] {}, dcY[2] {};
    static constexpr int outerCoefs = 10, innerCoefs = 6;
    HalfBand::StereoUp2x<outerCoefs> upOuter;
    HalfBand::StereoDown2x<outerCoefs> downOuter;
    HalfBand::StereoUp2x<innerCoefs> upInner;
    HalfBand::StereoDown2x<innerCoefs> downInner;
    int type = 0, circuit = 0;
    bool twoStage = true, djHighpass = false, useLadder = false;
    float morph = 0, mix = 1, drive = 1, driveComp = 1;
    int morphIndex = 2;
    float morphFrac = 0.0f;
    float g = 0.1f, k = 1, a1 = 0, a2 = 0, a3 = 0;
    std::array<std::array<SvfState, 2>, 2> svf {};

    float ladderG = 0.1f, ladderK = 0;
    std::array<std::array<float, 4>, 2> ladder {};
    std::array<float, 2> ladderFb {};

    std::array<std::vector<float>, 2> comb;
    int combMask = 0, combIndex = 0;
    float combDelay = 100, combFb = 0;

    float vk[3] {}, va1[3] {}, va2[3] {}, va3[3] {}, vgain[3] {};
    std::array<std::array<SvfState, 3>, 2> vowelState {};
};
