#pragma once

#include <array>
#include <cmath>
#include <vector>
#if defined (__SSE2__) || defined (_M_X64)
 #include <emmintrin.h>
 #define HALFBAND_SSE 1
#endif

// Polyphase IIR half-band filters for 2x up- and downsampling, after the design of Laurent de
// Soras' HIIR (public domain): two chains of first-order allpass sections running at the lower
// rate. A few samples of phase lag and no latency to report, so a saturating stage can run at a
// higher rate inside a per-sample loop.
namespace HalfBand
{
// Allpass coefficients for `count` sections and a transition band `transition` (a fraction of the
// higher rate; the pass band reaches fs/4 - transition*fs).
inline std::vector<float> design (int count, double transition)
{
    constexpr double pi = 3.14159265358979323846;
    double k = std::tan ((1.0 - transition * 2.0) * pi / 4.0);
    k *= k;
    const double kksqrt = std::pow (1.0 - k * k, 0.25);
    const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
    const double e2 = e * e, e4 = e2 * e2;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    const int order = count * 2 + 1;

    std::vector<float> coefs ((size_t) count);
    for (int index = 0; index < count; ++index)
    {
        const int c = index + 1;
        double num = 0.0, term = 0.0;
        int i = 0, sign = 1;
        do
        {
            term = std::pow (q, i * (i + 1)) * std::sin ((i * 2 + 1) * c * pi / order) * sign;
            num += term;
            ++i;
            sign = -sign;
        } while (std::abs (term) > 1.0e-100 && i < 100);
        num *= std::pow (q, 0.25);

        double den = 0.0;
        i = 1;
        sign = -1;
        do
        {
            term = std::pow (q, i * i) * std::cos (i * 2 * c * pi / order) * sign;
            den += term;
            ++i;
            sign = -sign;
        } while (std::abs (term) > 1.0e-100 && i < 100);
        den += 0.5;

        const double ww = num / den, wwsq = ww * ww;
        const double x = std::sqrt ((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
        coefs[(size_t) index] = (float) ((1.0 - x) / (1.0 + x));
    }
    return coefs;
}

// The two chains: even coefficients on one sample of the pair, odd ones on the other.
template <int N>
struct Chains
{
    std::array<float, N> coef {}, x {}, y {};

    void reset() noexcept { x.fill (0.0f); y.fill (0.0f); }

    void run (float& a, float& b) noexcept
    {
        int i = 0;
        for (; i + 1 < N; i += 2)
        {
            const float ta = (a - y[(size_t) i]) * coef[(size_t) i] + x[(size_t) i];
            const float tb = (b - y[(size_t) i + 1]) * coef[(size_t) i + 1] + x[(size_t) i + 1];
            x[(size_t) i] = a;
            x[(size_t) i + 1] = b;
            y[(size_t) i] = ta;
            y[(size_t) i + 1] = tb;
            a = ta;
            b = tb;
        }
        if (i < N)
        {
            const float ta = (a - y[(size_t) i]) * coef[(size_t) i] + x[(size_t) i];
            x[(size_t) i] = a;
            y[(size_t) i] = ta;
            a = ta;
        }
    }
};

template <int N>
struct Up2x
{
    Chains<N> chains;
    void setCoefs (const std::vector<float>& c) noexcept { for (int i = 0; i < N; ++i) chains.coef[(size_t) i] = c[(size_t) i]; }
    void reset() noexcept { chains.reset(); }
    void process (float in, float& out0, float& out1) noexcept
    {
        float even = in, odd = in;
        chains.run (even, odd);
        out0 = even;
        out1 = odd;
    }
};

template <int N>
struct Down2x
{
    Chains<N> chains;
    void setCoefs (const std::vector<float>& c) noexcept { for (int i = 0; i < N; ++i) chains.coef[(size_t) i] = c[(size_t) i]; }
    void reset() noexcept { chains.reset(); }
    float process (float in0, float in1) noexcept
    {
        float a = in1, b = in0;
        chains.run (a, b);
        return 0.5f * (a + b);
    }
};
// Both channels at once: left in lanes 0-1, right in 2-3 (the two chains of each). Every lane does the
// arithmetic of Chains, in the same order, so the result is the same bit for bit in a quarter of the
// operations. N even.
template <int N>
struct StereoChains
{
    static_assert (N % 2 == 0, "pairs of sections");
    alignas (16) std::array<float, 2 * N> coef {}, x {}, y {};     // pair j: (L a, L b, R a, R b) at 4j

    void setCoefs (const std::vector<float>& c) noexcept
    {
        for (int j = 0; j < N / 2; ++j)
            for (int ch = 0; ch < 2; ++ch)
            {
                coef[(size_t) (4 * j + 2 * ch)] = c[(size_t) (2 * j)];
                coef[(size_t) (4 * j + 2 * ch + 1)] = c[(size_t) (2 * j + 1)];
            }
    }

    void reset() noexcept { x.fill (0.0f); y.fill (0.0f); }

    void run (float* v) noexcept     // four lanes in, four out
    {
#if HALFBAND_SSE
        __m128 io = _mm_loadu_ps (v);
        for (int j = 0; j < N / 2; ++j)
        {
            const __m128 t = _mm_add_ps (_mm_mul_ps (_mm_sub_ps (io, _mm_load_ps (y.data() + 4 * j)), _mm_load_ps (coef.data() + 4 * j)),
                                         _mm_load_ps (x.data() + 4 * j));
            _mm_store_ps (x.data() + 4 * j, io);
            _mm_store_ps (y.data() + 4 * j, t);
            io = t;
        }
        _mm_storeu_ps (v, io);
#else
        for (int j = 0; j < N / 2; ++j)
            for (int lane = 0; lane < 4; ++lane)
            {
                const size_t k = (size_t) (4 * j + lane);
                const float t = (v[lane] - y[k]) * coef[k] + x[k];
                x[k] = v[lane];
                y[k] = t;
                v[lane] = t;
            }
#endif
    }
};

template <int N>
struct StereoUp2x
{
    StereoChains<N> chains;
    void setCoefs (const std::vector<float>& c) noexcept { chains.setCoefs (c); }
    void reset() noexcept { chains.reset(); }
    // out: left's two samples, then right's.
    void process (float l, float r, float* out) noexcept
    {
        out[0] = l;
        out[1] = l;
        out[2] = r;
        out[3] = r;
        chains.run (out);
    }
};

template <int N>
struct StereoDown2x
{
    StereoChains<N> chains;
    void setCoefs (const std::vector<float>& c) noexcept { chains.setCoefs (c); }
    void reset() noexcept { chains.reset(); }
    void process (float l0, float l1, float r0, float r1, float& outL, float& outR) noexcept
    {
        alignas (16) float v[4] { l1, l0, r1, r0 };
        chains.run (v);
        outL = 0.5f * (v[0] + v[1]);
        outR = 0.5f * (v[2] + v[3]);
    }
};
}
