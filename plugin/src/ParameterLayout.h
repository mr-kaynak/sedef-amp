// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/params/Parameters.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace sedef::plugin
{

/// Builds the host-facing parameter layout from the engine schema (ADR-0004).
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/// Human-readable value text for a parameter, used by hosts and the preset browser.
juce::String formatValue (const ParamSpec& spec, float value, int maxLength = 0);
/// Inverse of formatValue for host text entry ("1.2 kHz", "1.5 s", "Hall", "7.5" on a knob).
float parseValue (const ParamSpec& spec, const juce::String& text);

inline juce::String toJuce (std::string_view s)
{
    return juce::String::fromUTF8 (s.data(), static_cast<int> (s.size()));
}

} // namespace sedef::plugin
