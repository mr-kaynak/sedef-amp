// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sedef
{

enum class ParamKind : std::uint8_t
{
    Float,
    Int,
    Bool,
    Choice
};

struct ParamSpec
{
    std::string_view id;
    std::string_view name;
    std::string_view group;
    ParamKind kind;
    float min;
    float max;
    float defaultValue;
    float skew;              ///< JUCE NormalisableRange skew: norm = proportion^skew
    std::string_view unit;   ///< "dB", "Hz", "ms", "s", "st", "%", "bpm", "knob" or empty
    std::string_view choices; ///< '|'-separated labels for Choice parameters
    bool automatable;

    /// Real value -> 0..1 (host/UI normalised space).
    float toNormalised (float value) const noexcept;
    /// 0..1 -> real value, snapped for discrete kinds.
    float fromNormalised (float norm) const noexcept;
    /// Clamps and snaps a real value to what this parameter can hold.
    float sanitise (float value) const noexcept;

    int numChoices() const noexcept;
    std::string_view choiceLabel (int index) const noexcept;
};

enum class ParamId : std::uint16_t
{
#define SEDEF_PARAM(id, name, group, kind, lo, hi, def, skew, unit, choices, automatable) id,
#include "sedef/params/ParameterList.inc"
#undef SEDEF_PARAM
    Count
};

inline constexpr std::size_t kNumParams = static_cast<std::size_t> (ParamId::Count);

inline constexpr std::array<ParamSpec, kNumParams> kParamSpecs {
#define SEDEF_PARAM(id, name, group, kind, lo, hi, def, skew, unit, choices, automatable)          \
    ParamSpec { #id, name, group, ParamKind::kind, lo, hi, def, skew, unit, choices, automatable },
#include "sedef/params/ParameterList.inc"
#undef SEDEF_PARAM
};

constexpr const ParamSpec& spec (ParamId id) noexcept
{
    return kParamSpecs[static_cast<std::size_t> (id)];
}

constexpr std::size_t index (ParamId id) noexcept
{
    return static_cast<std::size_t> (id);
}

/// Looks up a parameter by its string id (linear scan; not for the audio thread).
std::optional<ParamId> findParam (std::string_view id) noexcept;

/// Plain-old-data copy of every parameter's real value, handed to the engine once per block.
struct ParamSnapshot
{
    std::array<float, kNumParams> values {};

    static ParamSnapshot defaults() noexcept
    {
        ParamSnapshot s;
        for (std::size_t i = 0; i < kNumParams; ++i)
            s.values[i] = kParamSpecs[i].defaultValue;
        return s;
    }

    float operator[] (ParamId id) const noexcept { return values[index (id)]; }
    float& operator[] (ParamId id) noexcept { return values[index (id)]; }

    bool flag (ParamId id) const noexcept { return values[index (id)] >= 0.5f; }
    int choice (ParamId id) const noexcept
    {
        return static_cast<int> (values[index (id)] + 0.5f);
    }
};

} // namespace sedef
