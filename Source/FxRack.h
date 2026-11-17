#pragma once

#include "Params.h"
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <memory>
#include <vector>

// Serum 2-style effects rack: a main chain and two parallel buses, each made of
// slots. A slot holds any effect (duplicates allowed) or a splitter; the modules
// after a splitter only process the bands marked "procesar", the rest bypasses
// them and rejoins at the end of the chain.
namespace Fx
{
enum Type
{
    Empty, HyperDim, Distortion, Flanger, Phaser, Chorus, Delay, Compressor, Reverb, EQ, Filter, Bode, Utility,
    SplitLH, SplitLMH, SplitMS,
    NumTypes
};

inline bool isSplitter (int t) noexcept { return t == SplitLH || t == SplitLMH || t == SplitMS; }

struct ParamSpec
{
    enum Kind { Knob, Choice, Toggle };
    enum Curve { Linear, Skew, Symmetric };

    const char* name;
    Kind kind = Knob;
    float min = 0, max = 1, def = 0;
    Curve curve = Linear;
    float centre = 0;           // for Skew
    const char* unit = "";
    const char* choices = "";   // "a|b|c" for Choice
    float step = 0;
    int numChoices = 0;         // kept apart: the audio thread must not parse the text
};

struct TypeSpec
{
    const char* name;
    float defaultMix = 1.0f;
    std::vector<ParamSpec> params;
};

const TypeSpec& spec (int type);
juce::StringArray typeNames();
juce::StringArray choicesOf (const ParamSpec& s);

float toValue (const ParamSpec& s, float norm);
float toNorm (const ParamSpec& s, float value);
int choiceIndex (const ParamSpec& s, float norm);
juce::String format (const ParamSpec& s, float norm);

// Value of parameter k of a slot, in real units (index for choices, 0/1 for toggles).
inline float value (int type, int k, const float* norm) { return toValue (spec (type).params[(size_t) k], norm[k]); }

struct Module
{
    virtual ~Module() = default;
    virtual void prepare (double sampleRate, int maxBlock) = 0;
    virtual void reset() = 0;
    virtual void process (float* L, float* R, int n, const float* norm, double bpm) = 0;

    // Gives the dry signal the same delay and phase the wet one picked up inside the module.
    virtual void alignDry (float*, float*, int) {}
};

std::unique_ptr<Module> create (int type);
}

class FxRack
{
public:
    FxRack();
    ~FxRack();

    void prepare (double sampleRate);
    void reset();

    // Message thread (or offline): creates the modules the current slot types need.
    void ensureModules (const ParamSnapshot& p);

    // Audio thread.
    void process (juce::AudioBuffer<float>& buffer, const ParamSnapshot& p, double bpm);

    static int chainStart (int chain) noexcept { return chain == 0 ? 0 : P::fxMainSlots + (chain - 1) * P::fxBusSlots; }
    static int chainSize (int chain) noexcept { return chain == 0 ? P::fxMainSlots : P::fxBusSlots; }

private:
    static constexpr int neverRan = -1;

    struct Slot
    {
        std::array<std::atomic<Fx::Module*>, Fx::NumTypes> live {};
        std::vector<std::unique_ptr<Fx::Module>> owned;
        std::array<juce::dsp::LinkwitzRileyFilter<float>, 3> xover;   // splitters
        float mix = 1.0f;       // value at the end of the last block, to ramp from
        int running = neverRan;     // the effect that was sounding in the last block
    };

    void processBlock (float* L, float* R, int n, const ParamSnapshot& p, double bpm, const bool* busOn, bool mainOn);
    void processChain (int chain, float* L, float* R, int n, const ParamSnapshot& p, double bpm);
    void split (Slot& slot, int type, const float* norm, float* L, float* R, int n);
    bool chainActive (int chain, const ParamSnapshot& p) const;

    std::array<Slot, P::numFxSlots> slots;
    juce::AudioBuffer<float> dry, bus1, bus2, scratch, bypass;
    double sampleRate = 48000.0;
    float mainLevel = 1.0f, sendLevel[2] { 0.0f, 0.0f }, busLevel[2] { 1.0f, 1.0f };
    int silentSamples = 0;
    bool prepared = false;
};
