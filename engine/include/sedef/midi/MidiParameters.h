// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/midi/GuitarToMidi.h"
#include "sedef/params/Parameters.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

/// The Sedef MIDI product's parameter schema (MidiParameterList.inc), separate from Sedef Amp's.
/// It reuses ParamSpec, so hosts, the bridge and the UI read it the same way.
namespace sedef::midi
{

enum class ParamId : std::uint16_t
{
#define SEDEF_MIDI_PARAM(id, name, kind, lo, hi, def, unit, choices, automatable) id,
#include "sedef/midi/MidiParameterList.inc"
#undef SEDEF_MIDI_PARAM
    Count
};

inline constexpr std::size_t kNumParams = static_cast<std::size_t> (ParamId::Count);

inline constexpr std::array<ParamSpec, kNumParams> kParamSpecs {
#define SEDEF_MIDI_PARAM(id, name, kind, lo, hi, def, unit, choices, automatable)                  \
    ParamSpec { #id, name, "MIDI", ParamKind::kind, lo, hi, def, 1.0f, unit, choices, automatable },
#include "sedef/midi/MidiParameterList.inc"
#undef SEDEF_MIDI_PARAM
};

constexpr std::size_t index (ParamId id) noexcept
{
    return static_cast<std::size_t> (id);
}

constexpr const ParamSpec& spec (ParamId id) noexcept
{
    return kParamSpecs[index (id)];
}

/// Looks up a parameter by its string id (not for the audio thread).
std::optional<ParamId> findParam (std::string_view id) noexcept;

/// Every parameter's real value, as the processor reads them once per block.
using Values = std::array<float, kNumParams>;

Values defaultValues() noexcept;

/// The converter's settings for a set of parameter values (sanitised: any input is safe).
Settings settingsFrom (const Values& values) noexcept;

/// Whether the audio passes through (true) or is muted.
bool audioThrough (const Values& values) noexcept;

} // namespace sedef::midi
