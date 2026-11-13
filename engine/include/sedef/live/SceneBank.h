// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// A preset's scenes (docs/live.md §6): which switches are on and a few picked values, per scene.
// Plain data, so the message thread hands the whole bank to the audio thread by value.

#include "sedef/board/Board.h"
#include "sedef/live/LiveMap.h"
#include "sedef/params/Parameters.h"

#include <array>
#include <cstdint>

namespace sedef::live
{

/// Values the player may pick for scenes, per preset (§7.3).
inline constexpr int kMaxSceneValues = 16;
/// Parameters a scene holds: the switches plus the picked values, with room for new pedals.
inline constexpr int kMaxSceneParams = 64;

/// The switches every scene holds: each pedal's on/off in registry order, then the EQ banks,
/// the Doubler, Drift and Fathom, then each pedal's second copy's on/off (docs/board.md §8).
/// Each fades in and out on its own, so switching never clicks.
inline constexpr std::array<ParamId, kNumSlots + 6> kSceneSwitches = [] {
    std::array<ParamId, kNumSlots + 6> list {};
    std::size_t i = 0;
    for (const auto& pedal : kPedals)
        list[i++] = pedal.enable;
    for (const auto id :
         { ParamId::eqLagoonEnabled, ParamId::eqReefEnabled, ParamId::eqAbyssEnabled,
           ParamId::doublerEnabled, ParamId::driftEnabled, ParamId::fathomEnabled })
        list[i++] = id;
    for (std::size_t p = 0; p < kNumPedals; ++p)
        list[i++] = enableOf ({ static_cast<PedalId> (p), 1 });
    return list;
}();

static_assert (kSceneSwitches.size() + kMaxSceneValues <= kMaxSceneParams,
               "More pedals than a scene can hold: raise kMaxSceneParams (docs/live.md §7.3)");

/// Whether a parameter can be picked as a scene value: continuous ones only, which the modules
/// smooth (§6.2). The plug-in also leaves out its global parameters.
constexpr bool isSceneValueKind (const ParamSpec& spec) noexcept
{
    return spec.kind == ParamKind::Float;
}

struct SceneBank
{
    /// ParamId indices: kSceneSwitches first, then the picked values in the order picked.
    std::array<std::uint16_t, kMaxSceneParams> params {};
    std::uint8_t numParams = 0;
    /// Per scene, per entry of `params`: the value in real units. The active scene's row is not
    /// read while it is active (the live parameters are its values).
    std::array<std::array<float, kMaxSceneParams>, kNumScenes> values {};
    std::uint8_t active = 0;
    /// Bit s: scene s has been made (docs/live.md §6.3). A scene not made yet holds nothing; the
    /// first switch to it makes it a copy of the scene being left. The active scene is made.
    std::uint8_t made = 1;

    bool isMade (int scene) const noexcept { return (made >> scene & 1) != 0; }

    // Bookkeeping between the threads (docs/live.md §7.2), not part of a scene's content.
    /// Counts the switches made on screen (message thread).
    std::uint32_t switchCount = 0;
    /// The latest on-screen switch is for the audio thread to apply (false: the parameters were
    /// written already and nothing is to be applied).
    bool applySwitch = false;
    /// The audio thread's switch reports the message thread has taken in.
    std::uint32_t reportsSeen = 0;

    /// A bank holding only the switches, every scene equal to `values`.
    static SceneBank withSwitches (const ParamSnapshot& values) noexcept
    {
        SceneBank b;
        for (const auto id : kSceneSwitches)
            b.params[b.numParams++] = static_cast<std::uint16_t> (index (id));
        for (auto& scene : b.values)
            for (int i = 0; i < b.numParams; ++i)
                scene[static_cast<std::size_t> (i)] =
                    values.values[b.params[static_cast<std::size_t> (i)]];
        return b;
    }

    int numValues() const noexcept { return numParams - static_cast<int> (kSceneSwitches.size()); }

    /// Where a parameter is in `params`, or -1.
    int find (ParamId id) const noexcept
    {
        for (int i = 0; i < numParams; ++i)
            if (params[static_cast<std::size_t> (i)] == index (id))
                return i;
        return -1;
    }

    /// Picks a value for scenes, every scene starting at `current`. False when it can't be added
    /// (already there, not continuous, or full).
    bool addValue (ParamId id, float current) noexcept
    {
        if (find (id) >= 0 || !isSceneValueKind (spec (id)) || numValues() >= kMaxSceneValues)
            return false;
        const auto at = static_cast<std::size_t> (numParams++);
        params[at] = static_cast<std::uint16_t> (index (id));
        for (auto& scene : values)
            scene[at] = current;
        return true;
    }

    /// Drops a picked value (the switches stay). False when it isn't a picked value.
    bool removeValue (ParamId id) noexcept
    {
        const int at = find (id);
        if (at < static_cast<int> (kSceneSwitches.size()))
            return false;
        for (int i = at; i + 1 < numParams; ++i)
        {
            const auto to = static_cast<std::size_t> (i);
            params[to] = params[to + 1];
            for (auto& scene : values)
                scene[to] = scene[to + 1];
        }
        --numParams;
        return true;
    }
};

} // namespace sedef::live
