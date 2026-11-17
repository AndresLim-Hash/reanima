#include "Randomizer.h"
#include "FxRack.h"

namespace Randomizer
{
namespace
{
struct Gen
{
    juce::Random& rng;
    Result out;

    float uni (float lo, float hi) { return lo + (hi - lo) * rng.nextFloat(); }
    float logu (float lo, float hi) { return lo * std::pow (hi / lo, rng.nextFloat()); }
    bool chance (float p) { return rng.nextFloat() < p; }
    int pick (std::initializer_list<int> list) { return *(list.begin() + rng.nextInt ((int) list.size())); }
    int weighted (std::initializer_list<std::pair<int, float>> list)
    {
        float total = 0.0f;
        for (const auto& [v, w] : list)
            total += w;
        float x = rng.nextFloat() * total;
        for (const auto& [v, w] : list)
            if ((x -= w) <= 0.0f)
                return v;
        return list.begin()->first;
    }
    void set (int param, float value) { out.values.push_back ({ param, value }); }
    float sign() { return chance (0.5f) ? 1.0f : -1.0f; }
};

using FxP = std::vector<std::pair<int, float>>;

FxP reverb (Gen& g)
{
    return { { 0, (float) g.rng.nextInt (5) }, { 1, g.uni (0.3f, 1.0f) }, { 2, g.logu (0.8f, 8.0f) }, { 3, g.uni (0, 80) },
             { 5, g.logu (80, 300) } };
}

FxP delay (Gen& g)
{
    if (g.chance (0.85f))
    {
        const int l = g.pick ({ 2, 3, 4, 5, 6 });
        const bool link = g.chance (0.6f);
        return { { 0, 1 }, { 3, (float) l }, { 4, (float) (link ? l : g.pick ({ 2, 3, 4, 5, 6, 7 })) }, { 5, link ? 1.0f : 0.0f },
                 { 6, g.uni (0.2f, 0.6f) }, { 7, (float) g.weighted ({ { 0, 1 }, { 1, 2 }, { 2, 1 } }) }, { 8, g.logu (800, 6000) },
                 { 9, g.uni (1, 6) } };
    }
    return { { 0, 0 }, { 1, g.logu (80, 700) }, { 2, g.logu (80, 700) }, { 5, 0 }, { 6, g.uni (0.1f, 0.5f) }, { 7, 0 },
             { 8, g.logu (800, 5000) } };
}

FxP shaper (Gen& g, int type)
{
    switch (type)
    {
        case Fx::Chorus:     return { { 0, g.logu (0.2f, 2) }, { 1, g.uni (5, 15) }, { 2, g.uni (8, 25) }, { 3, g.uni (0.2f, 0.7f) }, { 4, g.uni (0, 0.3f) } };
        case Fx::Phaser:     return { { 0, g.logu (0.05f, 2) }, { 1, g.uni (0.4f, 0.9f) }, { 2, g.logu (300, 3000) }, { 3, g.uni (-0.7f, 0.7f) },
                                      { 5, (float) g.rng.nextInt (5) } };
        case Fx::Flanger:    return { { 0, g.logu (0.05f, 2) }, { 1, g.uni (0.4f, 0.9f) }, { 2, g.uni (-0.8f, 0.8f) } };
        case Fx::Distortion:
        {
            FxP p { { 0, (float) g.rng.nextInt (16) }, { 1, g.uni (0.15f, 0.7f) } };
            if (g.chance (0.5f))
                p.insert (p.end(), { { 3, 2 }, { 4, 0 }, { 5, g.logu (2000, 8000) } });
            return p;
        }
        case Fx::Compressor: return { { 0, g.uni (-30, -12) }, { 1, g.uni (2, 6) }, { 2, g.logu (1, 30) }, { 3, g.logu (50, 300) }, { 4, g.uni (0, 6) } };
        case Fx::Bode:       return { { 0, g.sign() * g.logu (5, 300) }, { 1, g.uni (0, 0.5f) }, { 2, g.uni (0, 0.5f) } };
        case Fx::HyperDim:   return { { 0, g.logu (0.2f, 2) }, { 1, g.uni (0.2f, 0.5f) }, { 2, (float) g.pick ({ 2, 3, 4, 5, 6 }) } };
        default:             return {};
    }
}

float shaperMix (Gen& g, int type)
{
    switch (type)
    {
        case Fx::Distortion: return g.uni (0.3f, 0.8f);
        case Fx::Compressor: return 1.0f;
        case Fx::HyperDim:   return g.uni (0.5f, 1.0f);
        case Fx::Bode:       return g.uni (0.2f, 0.5f);
        default:             return g.uni (0.25f, 0.6f);
    }
}
} // namespace

Result make (juce::Random& rng, bool keepEnvelope, int engines)
{
    using namespace P;
    Gen g { rng, {} };

    // The engine, and its own controls.
    int engine = EngineRebuild;
    for (int tries = 0; tries < 50; ++tries)
    {
        engine = g.weighted ({ { EngineRebuild, 3 }, { EngineReenvelope, 2 }, { EngineResynth, 3 }, { EngineHybrid, 2 },
                               { EngineGranular, 2 }, { EngineDrone, 1.5f } });
        if ((engines >> engine) & 1)
            break;
    }
    g.set (Engine, (float) engine);
    const bool sampled = engine <= EngineHybrid;
    int warp = WarpRepitch;
    if (engine != EngineDrone && g.chance (0.2f))
        warp = g.pick ({ WarpTones, WarpTexture });
    g.set (WarpMode, (float) warp);

    switch (engine)
    {
        case EngineRebuild:
            if (g.chance (0.25f)) g.set (GrainMix, g.uni (0.1f, 0.4f));
            if (g.chance (0.3f)) g.set (Punch, g.uni (0.3f, 0.9f));
            break;
        case EngineReenvelope:
            g.set (SustainMode, (float) g.rng.nextInt (3));
            g.set (Breath, g.uni (0, 0.8f));
            g.set (Flatten, g.uni (0, 0.5f));
            g.set (Spray, g.uni (0, 0.7f));
            break;
        case EngineResynth:
            g.set (Partials, (float) g.pick ({ 6, 12, 24, 48, 96 }));
            g.set (NoiseLevel, g.uni (0, 1.5f));
            break;
        case EngineHybrid:
            g.set (TransientMs, g.logu (10, 80));
            g.set (TransientGain, g.uni (-6, 6));
            g.set (SustainMode, (float) g.rng.nextInt (3));
            break;
        case EngineGranular:
            g.set (GrainPos, g.uni (0.05f, 0.8f));
            g.set (GrainMs, g.logu (10, 300));
            g.set (GrainDensity, g.logu (5, 120));
            g.set (Spray, g.uni (0, 1));
            g.set (GranSpeed, g.chance (0.5f) ? 0.0f : g.uni (-0.3f, 0.3f));
            g.set (GrainPitch, g.chance (0.5f) ? 0.0f : g.uni (0, 12));
            g.set (GrainReverse, g.chance (0.6f) ? 0.0f : g.uni (0, 0.5f));
            g.set (GrainWidth, g.uni (0.5f, 1));
            break;
        case EngineDrone:
            g.set (GrainPos, g.uni (0.1f, 0.6f));
            g.set (DroneStretch, g.logu (5, 500));
            g.set (DroneSize, (float) g.pick ({ 2, 3, 4, 5 }));
            g.set (DroneHarm, g.chance (0.5f) ? 0.0f : g.uni (0, 0.7f));
            g.set (DroneTonal, g.uni (-0.6f, 0.6f));
            g.set (DroneSpread, g.uni (0, 0.6f));
            g.set (DroneSub1, g.chance (0.5f) ? g.uni (0, 0.6f) : 0.0f);
            g.set (DroneSub2, g.chance (0.3f) ? g.uni (0, 0.4f) : 0.0f);
            g.set (DroneFifth, g.chance (0.3f) ? g.uni (0, 0.4f) : 0.0f);
            g.set (DroneUp1, g.chance (0.5f) ? g.uni (0, 0.5f) : 0.0f);
            g.set (DroneUp2, g.chance (0.3f) ? g.uni (0, 0.4f) : 0.0f);
            if (g.chance (0.15f)) g.set (DroneShift, g.uni (-50, 50));
            break;
        default: break;
    }

    // Envelope.
    if (! keepEnvelope)
    {
        const bool synth = engine <= EngineHybrid && g.chance (0.5f);
        if (synth) g.set (EnvMode, EnvSynth);
        const bool slow = g.chance (0.3f) || engine == EngineDrone;
        g.set (AmpA, slow ? g.logu (200, 2500) : g.logu (1, 60));
        g.set (AmpD, g.logu (100, 3000));
        g.set (AmpS, g.chance (0.3f) ? 0.0f : g.uni (0.3f, 1));
        g.set (AmpR, g.logu (60, 3000));
        g.set (AmpCurve, g.uni (0.5f, 2));
        if (sampled && g.chance (0.4f)) g.set (SustainType, SustainHold);
    }

    // Voice. Unison costs a voice per copy: fewer of them with Warp.
    const int poly = g.weighted ({ { Poly, 8 }, { Mono, 1 }, { Legato, 1 } });
    g.set (PolyMode, (float) poly);
    if (poly != Poly) g.set (Glide, g.logu (20, 200));
    const int unison = g.chance (0.5f) ? 1 : g.pick ({ 2, 3, 4 });
    g.set (Unison, (float) std::min (unison, warp == WarpRepitch ? 4 : 2));
    if (unison > 1)
    {
        g.set (UniDetune, g.uni (5, 35));
        g.set (UniWidth, g.uni (0.5f, 1));
    }
    if (g.chance (0.25f)) g.set (Transpose, (float) g.pick ({ -12, -12, 12, -7, 7, -24 }));

    // Timbre and Mecanismo.
    g.set (VelBright, g.uni (0.2f, 0.9f));
    g.set (RelDark, g.uni (0.2f, 0.9f));
    if (g.chance (0.4f)) g.set (Variation, g.uni (0, 0.3f));
    if (g.chance (0.15f))
    {
        g.set (Bits, (float) g.pick ({ 6, 8, 10, 12 }));
        g.set (Downsample, g.uni (1, 4));
    }
    if (g.chance (0.35f))
    {
        g.set (MechType, (float) g.rng.nextInt (11));
        g.set (RelNoise, g.uni (0.2f, 0.5f));
    }
    if (g.chance (0.15f))
    {
        g.set (PitchEnvAmt, g.sign() * g.uni (3, 12));
        g.set (PitchEnvTime, g.logu (50, 800));
    }

    // Filter.
    const bool filter = g.chance (0.65f);
    int ftype = LP;
    if (filter)
    {
        ftype = g.weighted ({ { LP, 5 }, { HP, 1.5f }, { BP, 1.5f }, { Morph, 1 }, { DJ, 0.5f }, { Comb, 0.7f }, { Vowel, 1 } });
        g.set (FltOn, 1);
        g.set (FltType, (float) ftype);
        g.set (FltSlope, (float) g.rng.nextInt (2));
        g.set (FltCircuit, (float) g.rng.nextInt (5));
        g.set (FltFreq, ftype == HP ? g.logu (80, 1500) : g.logu (400, 8000));
        g.set (FltRes, g.uni (0, 0.6f));
        g.set (FltDrive, g.chance (0.4f) ? g.uni (0, 10) : 0.0f);
        g.set (FltKey, g.uni (0, 0.6f));
        if (ftype == Morph || ftype == DJ || ftype == Vowel || ftype == Comb) g.set (FltMorph, g.uni (0, 1));
        if (ftype == Vowel || ftype == Comb) g.set (FltMix, g.uni (0.5f, 0.9f));
        if (g.chance (0.5f))
        {
            g.set (FltEnvAmt, g.uni (-0.4f, 0.7f));
            g.set (E2A, g.logu (1, 800));
            g.set (E2D, g.logu (80, 2000));
            g.set (E2S, g.uni (0, 0.8f));
            g.set (E2R, g.logu (80, 2000));
        }
    }

    // Modulation: up to three routes, each to something in use, with amounts that move it audibly but sanely.
    struct Dest { int id; float lo, hi; bool bipolar; };
    std::vector<Dest> dests { { DstPitch, 0.003f, 0.03f, true }, { DstVolume, 0.1f, 0.3f, false }, { DstPan, 0.2f, 0.7f, true } };
    if (filter)
    {
        dests.push_back ({ DstCutoff, 0.2f, 0.6f, true });
        dests.push_back ({ DstRes, 0.1f, 0.3f, false });
        if (ftype == Morph || ftype == DJ || ftype == Vowel || ftype == Comb) dests.push_back ({ DstMorph, 0.2f, 0.5f, true });
    }
    if (sampled) dests.push_back ({ DstStart, 0.1f, 0.4f, false });
    if (engine == EngineGranular) { dests.push_back ({ DstSpray, 0.2f, 0.6f, false }); dests.push_back ({ DstGrain, 0.2f, 0.5f, true }); }
    if (unison > 1) dests.push_back ({ DstDetune, 0.2f, 0.5f, false });
    const int routes = g.weighted ({ { 0, 1 }, { 1, 3 }, { 2, 3 }, { 3, 2 } });
    std::vector<int> used;
    for (int r = 0; r < routes; ++r)
    {
        const int src = g.pick ({ SrcLfo1, SrcLfo1, SrcLfo2, SrcLfo3, SrcEnv2, SrcVelocity, SrcModWheel, SrcAftertouch, SrcRandom, SrcNote });
        const auto& d = dests[(size_t) g.rng.nextInt ((int) dests.size())];
        if (std::find (used.begin(), used.end(), d.id) != used.end())
            continue;
        used.push_back (d.id);
        const int base = M1Src + r * slotStride;
        g.set (base, (float) src);
        g.set (base + 1, (float) d.id);
        g.set (base + 2, g.uni (d.lo, d.hi) * (d.bipolar ? g.sign() : 1.0f));
        if (src >= SrcLfo1 && src <= SrcLfo3)
        {
            const int lfo = Lfo1Rate + (src - SrcLfo1) * lfoStride;
            if (g.chance (0.5f))
            {
                g.set (lfo + 1, 1);
                g.set (lfo + 2, (float) g.pick ({ 2, 3, 4, 5, 6, 7, 8 }));
            }
            else
                g.set (lfo, g.logu (0.05f, 8));
            g.set (lfo + 3, (float) g.rng.nextInt (6));
        }
        if (src == SrcEnv2 && ! filter)
        {
            g.set (E2A, g.logu (1, 800));
            g.set (E2D, g.logu (80, 2000));
            g.set (E2S, g.uni (0, 0.8f));
        }
    }

    // Effects: one to three in Main (shapers first, then time, or now and then the other way round), and the
    // buses for an echo or a room in parallel, sometimes with a shaper of their own.
    std::vector<int> shapers { Fx::Chorus, Fx::Phaser, Fx::Flanger, Fx::Distortion, Fx::Compressor, Fx::Bode, Fx::HyperDim };
    std::vector<int> chain;
    const int count = g.weighted ({ { 1, 3 }, { 2, 4 }, { 3, 3 } });
    for (int i = 0; i < count; ++i)
    {
        int t;
        do
            t = g.chance (0.45f) ? g.pick ({ Fx::Delay, Fx::Reverb }) : shapers[(size_t) g.rng.nextInt ((int) shapers.size())];
        while (std::find (chain.begin(), chain.end(), t) != chain.end());
        chain.push_back (t);
    }
    std::stable_sort (chain.begin(), chain.end(), [] (int a, int b) { return (a == Fx::Delay || a == Fx::Reverb) < (b == Fx::Delay || b == Fx::Reverb); });
    if (g.chance (0.2f))
        std::reverse (chain.begin(), chain.end());
    for (size_t i = 0; i < chain.size(); ++i)
    {
        const int t = chain[i];
        const bool time = t == Fx::Delay || t == Fx::Reverb;
        g.out.fx.push_back ({ Factory::main1 + (int) i, t, time ? g.uni (0.12f, 0.35f) : shaperMix (g, t),
                              t == Fx::Delay ? delay (g) : t == Fx::Reverb ? reverb (g) : shaper (g, t) });
    }
    if (g.chance (0.35f))
    {
        const int t = g.pick ({ Fx::Delay, Fx::Reverb });
        g.out.fx.push_back ({ Factory::bus1, t, 1.0f, t == Fx::Delay ? delay (g) : reverb (g) });
        if (g.chance (0.4f))
        {
            const int s = g.pick ({ Fx::Chorus, Fx::Distortion, Fx::Phaser, Fx::Flanger });
            g.out.fx.push_back ({ Factory::bus1 + 1, s, shaperMix (g, s), shaper (g, s) });
        }
        g.set (FxBus1Send, g.uni (0.2f, 0.5f));
    }
    if (g.chance (0.25f))
    {
        g.out.fx.push_back ({ Factory::bus2, Fx::Reverb, 1.0f, reverb (g) });
        g.set (FxBus2Send, g.uni (0.15f, 0.4f));
    }
    return g.out;
}
} // namespace Randomizer
