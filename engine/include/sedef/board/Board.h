// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/params/Parameters.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sedef
{

/// What a pedal does, for the Add pedal picker.
enum class PedalCategory : std::uint8_t
{
    Dynamics,
    Drive,
    Fuzz,
    Filter,
    Modulation,
    Pitch
};

enum class PedalId : std::uint8_t
{
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings) id,
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
    Count
};

inline constexpr std::size_t kNumPedals = static_cast<std::size_t> (PedalId::Count);

struct PedalInfo
{
    std::string_view id; ///< stable, saved in sessions and presets
    std::string_view name;
    PedalCategory category;
    int rank; ///< default position (PedalList.inc)
    ParamId enable;
};

inline constexpr std::array<PedalInfo, kNumPedals> kPedals {
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings)                            \
    PedalInfo { #id, name, PedalCategory::category, rank, ParamId::enable },
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
};

constexpr std::size_t index (PedalId id) noexcept
{
    return static_cast<std::size_t> (id);
}

constexpr const PedalInfo& pedalInfo (PedalId id) noexcept
{
    return kPedals[index (id)];
}

std::string_view categoryName (PedalCategory category) noexcept;

/// Looks a pedal up by its string id (linear scan; not for the audio thread).
std::optional<PedalId> findPedal (std::string_view id) noexcept;

/// The pedals on the board, in signal order (input first). Each pedal appears at most once, so
/// the capacity is the number of pedals there are. Plain data: copying it never allocates, and it
/// crosses to the audio thread by value.
class Board
{
public:
    static constexpr std::size_t kCapacity = kNumPedals;

    constexpr Board() noexcept { pedals_.fill (PedalId::Count); }

    /// The fixed chain the board replaced, in its order: 0.3.0's Gate, Tide, Pearl, Coral and
    /// Ripple, with the pedals that joined that chain before the board in their places.
    static Board legacy() noexcept;
    /// Every pedal, in default order.
    static Board everyPedal() noexcept;

    /// The board for a session or preset saved without one: the legacy board, plus every newer
    /// pedal it switches on (`isOn (enableParam)`), each at its default position. A session saved
    /// before the board gets exactly the legacy board, so it plays the fixed chain it had.
    template <typename IsOn>
    static Board legacyFor (IsOn&& isOn)
    {
        auto board = legacy();
        for (std::size_t i = 0; i < kNumPedals; ++i)
            if (isOn (kPedals[i].enable))
                board.add (static_cast<PedalId> (i));
        return board;
    }

    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr PedalId operator[] (std::size_t i) const noexcept { return pedals_[i]; }
    constexpr const PedalId* begin() const noexcept { return pedals_.data(); }
    constexpr const PedalId* end() const noexcept { return pedals_.data() + size_; }

    bool contains (PedalId id) const noexcept { return indexOf (id).has_value(); }
    std::optional<std::size_t> indexOf (PedalId id) const noexcept;

    /// Adds a pedal at its default position: after the last pedal on the board whose rank is at
    /// or below its own, or first when there is none. False when it is already on the board.
    bool add (PedalId id) noexcept;
    /// Inserts at `position` (clamped to the end). False when it is already on the board.
    bool insert (PedalId id, std::size_t position) noexcept;
    bool remove (PedalId id) noexcept;
    /// Moves a pedal on the board so it ends up at `position` (clamped). False when it isn't on.
    bool move (PedalId id, std::size_t position) noexcept;

    friend constexpr bool operator== (const Board&, const Board&) noexcept = default;

private:
    std::array<PedalId, kCapacity> pedals_ {}; // unused entries hold PedalId::Count
    std::size_t size_ = 0;
};

} // namespace sedef
