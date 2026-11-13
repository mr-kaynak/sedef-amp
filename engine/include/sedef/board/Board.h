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

// -------------------------------------------------------------------------------------------------
// What may go where (docs/board.md, section 2)

/// Copies of one pedal a board may hold.
inline constexpr std::size_t kCopies = 2;
/// Every copy of every pedal: the slots a rack holds.
inline constexpr std::size_t kNumSlots = kNumPedals * kCopies;

/// The pedals that oversample, and so have latency: the drives and fuzzes (a test checks the
/// modules agree, at every rate and in either mode).
constexpr bool hasLatency (PedalId id) noexcept
{
    const auto c = pedalInfo (id).category;
    return c == PedalCategory::Drive || c == PedalCategory::Fuzz;
}

/// The pedals that may also stand after the amp: the modulation, which has no latency.
constexpr bool fitsAfterAmp (PedalId id) noexcept
{
    return pedalInfo (id).category == PedalCategory::Modulation;
}

/// Drives and fuzzes a board may hold in all: one latency each, the latency the rig reports.
inline constexpr std::size_t kLatencySlots = [] {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kNumPedals; ++i)
        n += hasLatency (static_cast<PedalId> (i)) ? 1 : 0;
    return n;
}();

// -------------------------------------------------------------------------------------------------
// Second copies' parameters (docs/board.md, section 7)

/// For each parameter of a pedal's first copy, its second copy's (`kelpDrive` -> `kelp2Drive`);
/// every other parameter maps to itself. The second copies are one block in the schema, starting
/// at `gate2Enabled`: registry order, then each pedal's parameters in schema order.
inline constexpr std::array<ParamId, kNumParams> kSecondCopy = [] {
    std::array<ParamId, kNumParams> map {};
    for (std::size_t i = 0; i < kNumParams; ++i)
        map[i] = static_cast<ParamId> (i);
    const auto first = index (ParamId::gate2Enabled);
    auto next = first;
    for (const auto& pedal : kPedals)
        for (std::size_t i = 0; i < first; ++i)
            if (kParamSpecs[i].group == pedal.name)
                map[i] = static_cast<ParamId> (next++);
    return map;
}();

constexpr ParamId secondCopyOf (ParamId id) noexcept
{
    return kSecondCopy[index (id)];
}

namespace detail
{
/// The second-copy block matches the first copies id for id: `<pedal>2<rest>`, named
/// "<Name> 2<rest>", in group "<Name> 2", the same in everything else but the switch's default,
/// which is off (a second copy is on only when the player switches it on).
constexpr bool secondCopiesMirrorFirst() noexcept
{
    for (const auto& pedal : kPedals)
        for (std::size_t i = 0; i < index (ParamId::gate2Enabled); ++i)
        {
            const auto& a = kParamSpecs[i];
            if (a.group != pedal.name)
                continue;
            const auto& b = spec (kSecondCopy[i]);
            const auto n = pedal.id.size();
            const auto m = pedal.name.size();
            if (a.id.substr (0, n) != pedal.id || b.id.size() != a.id.size() + 1 ||
                b.id.substr (0, n) != pedal.id || b.id[n] != '2' ||
                b.id.substr (n + 1) != a.id.substr (n))
                return false;
            if (b.name.size() != a.name.size() + 2 || b.name.substr (0, m) != pedal.name ||
                b.name.substr (m, 2) != " 2" || b.name.substr (m + 2) != a.name.substr (m))
                return false;
            if (b.group.size() != m + 2 || b.group.substr (0, m) != pedal.name ||
                b.group.substr (m) != " 2")
                return false;
            const bool isSwitch = static_cast<ParamId> (i) == pedal.enable;
            const auto same = [] (float x, float y) {
                return !(x < y) && !(y < x);
            };
            if (a.kind != b.kind || !same (a.min, b.min) || !same (a.max, b.max) ||
                !same (a.skew, b.skew) || a.unit != b.unit || a.choices != b.choices ||
                a.automatable != b.automatable ||
                !same (b.defaultValue, isSwitch ? 0.0f : a.defaultValue))
                return false;
        }
    return true;
}
} // namespace detail

static_assert (detail::secondCopiesMirrorFirst(),
               "The second copies' parameters must mirror the first copies' (docs/board.md §7)");

// -------------------------------------------------------------------------------------------------
// The board

/// One pedal on the board: which pedal, and which copy of it (0 or 1). The copy is the pedal's
/// identity, not its place: the second Kelp keeps its parameters wherever it stands.
struct PedalRef
{
    PedalId id = PedalId::Count;
    std::uint8_t instance = 0;

    constexpr PedalRef() noexcept = default;
    /// A first copy converts implicitly: `PedalRef r = PedalId::kelp`.
    constexpr PedalRef (PedalId pedal, int copy = 0) noexcept
        : id (pedal),
          instance (static_cast<std::uint8_t> (copy))
    {
    }
    constexpr bool valid() const noexcept { return id < PedalId::Count && instance < kCopies; }
    friend constexpr bool operator== (PedalRef, PedalRef) noexcept = default;
};

constexpr const PedalInfo& pedalInfo (PedalRef r) noexcept
{
    return pedalInfo (r.id);
}

/// Where a copy's state lives in a rack: first copies 0..15, second copies 16..31.
constexpr std::size_t slotIndex (PedalRef r) noexcept
{
    return static_cast<std::size_t> (r.instance) * kNumPedals + index (r.id);
}

constexpr PedalRef slotRef (std::size_t slot) noexcept
{
    return { static_cast<PedalId> (slot % kNumPedals), static_cast<int> (slot / kNumPedals) };
}

/// A copy's on/off parameter.
constexpr ParamId enableOf (PedalRef r) noexcept
{
    const auto first = pedalInfo (r.id).enable;
    return r.instance == 0 ? first : secondCopyOf (first);
}

/// The pedals on one side of the amp, in signal order (input first): what one rack plays. Each
/// copy at most once. Plain data: copying it never allocates, and it crosses to the audio thread
/// by value.
class PedalOrder
{
public:
    static constexpr std::size_t kCapacity = kNumSlots;

    constexpr PedalOrder() noexcept { refs_.fill ({}); }

    /// The fixed chain the board replaced, in its order: 0.3.0's Gate, Tide, Pearl, Coral and
    /// Ripple, with the pedals that joined that chain before the board in their places.
    static PedalOrder legacy() noexcept;
    /// Every pedal's first copy, in default order.
    static PedalOrder everyPedal() noexcept;

    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr PedalRef operator[] (std::size_t i) const noexcept { return refs_[i]; }
    constexpr const PedalRef* begin() const noexcept { return refs_.data(); }
    constexpr const PedalRef* end() const noexcept { return refs_.data() + size_; }

    bool contains (PedalRef r) const noexcept { return indexOf (r).has_value(); }
    std::optional<std::size_t> indexOf (PedalRef r) const noexcept;
    /// Copies with latency (drives and fuzzes) in it.
    std::size_t latencyCount() const noexcept;

    /// Adds a copy at its pedal's default position: after the last pedal here whose rank is at or
    /// below its own, or first when there is none. False when it is here already.
    bool add (PedalRef r) noexcept;
    /// Inserts at `position` (clamped to the end). False when it is here already.
    bool insert (PedalRef r, std::size_t position) noexcept;
    bool remove (PedalRef r) noexcept;
    /// Moves a copy here so it ends up at `position` (clamped). False when it isn't here.
    bool move (PedalRef r, std::size_t position) noexcept;

    friend constexpr bool operator== (const PedalOrder&, const PedalOrder&) noexcept = default;

private:
    std::array<PedalRef, kCapacity> refs_ {}; // unused entries hold PedalId::Count
    std::size_t size_ = 0;
};

/// The two sides of the amp.
enum class Side : std::uint8_t
{
    pre,  ///< in front of the amp: mono, every pedal
    post, ///< after the amp and cab: stereo, modulation only
};

/// The pedalboard: the pedals in front of the amp and the pedals after it (docs/board.md §3).
/// At most two copies of a pedal, at most kLatencySlots drives and fuzzes, only modulation after
/// the amp; every edit keeps those, and does nothing (false) where it would break one.
struct Board
{
    PedalOrder pre;
    PedalOrder post;

    constexpr Board() noexcept = default;
    /// A board with nothing after the amp.
    constexpr Board (const PedalOrder& front) noexcept : pre (front) {}

    static Board legacy() noexcept { return PedalOrder::legacy(); }
    static Board everyPedal() noexcept { return PedalOrder::everyPedal(); }

    /// The board for a session or preset saved without one: the legacy board, plus every newer
    /// pedal it switches on (`isOn (enableParam)`), each at its default position. A session saved
    /// before the board gets exactly the legacy board, so it plays the fixed chain it had.
    template <typename IsOn>
    static Board legacyFor (IsOn&& isOn)
    {
        auto board = PedalOrder::legacy();
        for (std::size_t i = 0; i < kNumPedals; ++i)
            if (isOn (kPedals[i].enable))
                board.add (static_cast<PedalId> (i));
        return board;
    }

    /// Only the pedals switched on (`isOn (enableParam)`), each first copy at its default position
    /// and a second copy that is on right after its first: the board of a preset with nothing else
    /// on it, such as Init's (the Gate alone) or a factory preset's.
    template <typename IsOn>
    static Board switchedOn (IsOn&& isOn)
    {
        Board board;
        for (std::size_t i = 0; i < kNumPedals; ++i)
            if (isOn (kPedals[i].enable))
                board.pre.add (static_cast<PedalId> (i));
        for (std::size_t i = 0; i < kNumPedals; ++i)
        {
            const PedalRef second { static_cast<PedalId> (i), 1 };
            if (!isOn (enableOf (second)) || !board.canPlace (second, Side::pre))
                continue;
            if (const auto first = board.pre.indexOf (second.id))
                board.pre.insert (second, *first + 1);
            else
                board.pre.add (second);
        }
        return board;
    }

    constexpr PedalOrder& side (Side s) noexcept { return s == Side::pre ? pre : post; }
    constexpr const PedalOrder& side (Side s) const noexcept { return s == Side::pre ? pre : post; }

    std::size_t size() const noexcept { return pre.size() + post.size(); }
    bool empty() const noexcept { return pre.empty() && post.empty(); }
    bool contains (PedalRef r) const noexcept { return pre.contains (r) || post.contains (r); }
    struct Place
    {
        Side side;
        std::size_t position;
    };
    std::optional<Place> find (PedalRef r) const noexcept;
    /// Drives and fuzzes on the board.
    std::size_t latencyCount() const noexcept { return pre.latencyCount() + post.latencyCount(); }

    /// Whether `r` may stand on `side` of this board (moving it there, if it is on already).
    bool canPlace (PedalRef r, Side side) const noexcept;
    /// The copy Add would put on next: the lowest one not on the board, if the board takes it.
    std::optional<PedalRef> nextCopy (PedalId id, Side side = Side::pre) const noexcept;

    /// Add pedal: the next copy, right after the other copy when that is on the board (on its
    /// side), else in front of the amp at its default position.
    std::optional<PedalRef> add (PedalId id) noexcept;
    /// The next copy at the end of the side after the amp.
    std::optional<PedalRef> addAfterAmp (PedalId id) noexcept;
    /// Puts `r` on `side` at `position` (clamped), moving it there if it is on the board already.
    bool place (PedalRef r, Side side, std::size_t position) noexcept;
    bool remove (PedalRef r) noexcept;

    friend constexpr bool operator== (const Board&, const Board&) noexcept = default;
};

} // namespace sedef
