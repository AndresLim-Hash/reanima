#pragma once

#include "Params.h"

// Text of the tooltips: what each control does, in the words of a musician.
namespace Help
{
juce::String forParam (int index);
juce::String forFxType (int type);
juce::String forFxParam (int type, int k);
}
