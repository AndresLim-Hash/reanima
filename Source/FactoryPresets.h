#pragma once

#include "Params.h"
#include <utility>
#include <vector>

// The presets that come with Reanima. They are templates: they set up the engine, envelope,
// timbre, filter and effects, and keep the sample that is loaded, so each works on any sound.
// Their levels are matched (research/presets/auditar_presets.py: about -24.5 LUFS on eight samples of
// different families, never above -1.5 dBTP), so changing preset does not change the loudness.
namespace Factory
{
struct Setting { int param; float value; };

// An effect in a given slot (Main 0-5, Bus 1 from slot 6, Bus 2 from slot 10), with its own settings: the
// index of each control in the effect and its value in that control's units (see Fx::spec).
struct FxSetting
{
    int slot, type;
    float mix;
    std::vector<std::pair<int, float>> params;
};
constexpr int main1 = 0, bus1 = P::fxMainSlots, bus2 = P::fxMainSlots + P::fxBusSlots;

struct Preset
{
    const char* name;
    const char* category;                           // how the browser groups them
    const char* why;                                // what it is, shown as its description
    const char* use;                                // what it is good for
    std::vector<Setting> values;                    // parameter and value, in the units of its knob
    std::vector<std::pair<int, float>> effects;     // effect type and mix, in chain order (Main)
    std::vector<FxSetting> fx {};                   // effects set in detail, in any chain
};

const std::vector<Preset>& presets();
const std::vector<const char*>& categories();      // in the order the browser shows them
} // namespace Factory
