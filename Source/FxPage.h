#pragma once

#include "PluginProcessor.h"
#include "FxRack.h"
#include "Look.h"
#include "Displays.h"

// Serum 2-style FX page: three chains (Main, Bus 1, Bus 2) with their own routing, whose
// slots can be dragged to reorder or move between chains, and the editor of the selected
// slot underneath.
class FxPage : public juce::Component, private juce::Timer
{
public:
    explicit FxPage (ReanimaProcessor& p);
    ~FxPage() override;

    void resized() override;

    void select (int slot);
    int getSelected() const noexcept { return selected; }
    ReanimaProcessor& getProcessor() noexcept { return proc; }

private:
    class Strip;
    class ParamControl;

    void timerCallback() override;
    void rebuildEditor();

    ReanimaProcessor& proc;
    std::array<std::unique_ptr<Section>, 3> chains;
    Section editor;
    MasterPanel master;     // the valve and the limiter, after the chains
    std::vector<Strip*> strips;     // owned by their chain
    int selected = 0;
    int shownType = -1;
};
