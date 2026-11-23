#pragma once

#include "Guide.h"
#include <map>

// The guide of each tab, in three levels: 1 Fundamentos, 2 Aplicación práctica, 3 Avanzado. Each step
// points at a real control of that tab; it is found by what the window already knows of it (the
// parameter of a knob, menu or switch, the title of a panel, the text of a button), so the guides
// follow the layout wherever it goes. To add a step, add it to the list of its tab in TabGuides.cpp.
namespace TabGuides
{
using Area = std::function<juce::Rectangle<int>()>;

// How the guides find things in the window, all in the guide's coordinates; an empty rectangle when the
// thing is not showing.
using Focus = std::function<std::vector<juce::Rectangle<int>>()>;

// A thing in the window is named by a key: its component ID ("p:<parameter>" for a knob, menu or switch,
// "s:<title>" for a panel, "fxslot:<n>", "mrow:<n>"...) or "text:<text>" for a button by what it says.
struct Targets
{
    std::function<juce::Rectangle<int> (const juce::String& key)> rect;    // the thing itself
    std::function<juce::Rectangle<int> (const juce::String& key)> group;   // the whole panel (or row, or slot) it belongs to
    std::map<juce::String, Area> areas;                                     // the rest, by name (see the editor)

    Area area (const juce::String& name) const;
};

constexpr int numTabs = 6;
juce::String tabName (int tab);
std::vector<GuideOverlay::Step> forTab (int tab, const Targets& targets);
} // namespace TabGuides
