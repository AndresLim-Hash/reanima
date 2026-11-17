#pragma once

#include "FactoryPresets.h"
#include <juce_core/juce_core.h>

// The dice beside the presets: a random sound made the way a preset is (the same values and effect settings),
// so it is applied, saved and undone like one. Every value stays inside a range that makes sense for its
// control, and the choices hang together: the engine's own controls for the engine chosen, modulation only
// to what is in use (the filter when it is on, unison detune when there is unison), effects of every kind in
// the Main chain and sometimes in the buses, all within limits that keep the CPU low. The level is set by
// the processor afterwards, by ear-like measurement (ReanimaProcessor::randomize).
namespace Randomizer
{
struct Result
{
    std::vector<Factory::Setting> values;
    std::vector<Factory::FxSetting> fx;
};

// keepEnvelope: the ADSR is locked (Fijar ADSR) and must not change. engines: the engines it may choose
// (bit per engine).
Result make (juce::Random& rng, bool keepEnvelope, int engines = 0x3f);
} // namespace Randomizer
