// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/board/Board.h"

#include <algorithm>

namespace sedef
{

std::string_view categoryName (PedalCategory category) noexcept
{
    switch (category)
    {
    case PedalCategory::Dynamics:
        return "Dynamics";
    case PedalCategory::Drive:
        return "Drive";
    case PedalCategory::Fuzz:
        return "Fuzz";
    case PedalCategory::Filter:
        return "Filter";
    case PedalCategory::Modulation:
        return "Modulation";
    case PedalCategory::Pitch:
        return "Pitch";
    }
    return "Dynamics";
}

std::optional<PedalId> findPedal (std::string_view id) noexcept
{
    for (std::size_t i = 0; i < kNumPedals; ++i)
        if (kPedals[i].id == id)
            return static_cast<PedalId> (i);
    return std::nullopt;
}

// -------------------------------------------------------------------------------------------------

PedalOrder PedalOrder::legacy() noexcept
{
    // The fixed chain before the board, in its order. 0.3.0 had Gate, Tide, Pearl, Coral and
    // Ripple; the pedals added after it joined the fixed chain before the board replaced it.
    PedalOrder b;
    for (auto id : { PedalId::gate, PedalId::trench, PedalId::anemone, PedalId::conch,
                     PedalId::remora, PedalId::tide, PedalId::urchin, PedalId::sargasso,
                     PedalId::pearl, PedalId::kelp, PedalId::coral, PedalId::beacon, PedalId::gyre,
                     PedalId::jetty, PedalId::ripple, PedalId::breaker })
        b.insert (id, b.size());
    return b;
}

PedalOrder PedalOrder::everyPedal() noexcept
{
    PedalOrder b;
    for (std::size_t i = 0; i < kNumPedals; ++i)
        b.add (static_cast<PedalId> (i));
    return b;
}

std::optional<std::size_t> PedalOrder::indexOf (PedalRef r) const noexcept
{
    for (std::size_t i = 0; i < size_; ++i)
        if (refs_[i] == r)
            return i;
    return std::nullopt;
}

std::size_t PedalOrder::latencyCount() const noexcept
{
    return static_cast<std::size_t> (
        std::count_if (begin(), end(), [] (PedalRef r) { return hasLatency (r.id); }));
}

bool PedalOrder::add (PedalRef r) noexcept
{
    if (!r.valid() || contains (r))
        return false;
    const int rank = pedalInfo (r.id).rank;
    std::size_t position = 0;
    for (std::size_t i = 0; i < size_; ++i)
        if (pedalInfo (refs_[i].id).rank <= rank)
            position = i + 1;
    return insert (r, position);
}

bool PedalOrder::insert (PedalRef r, std::size_t position) noexcept
{
    if (!r.valid() || contains (r) || size_ >= kCapacity)
        return false;
    position = std::min (position, size_);
    for (std::size_t i = size_; i > position; --i)
        refs_[i] = refs_[i - 1];
    refs_[position] = r;
    ++size_;
    return true;
}

bool PedalOrder::remove (PedalRef r) noexcept
{
    const auto at = indexOf (r);
    if (!at)
        return false;
    for (std::size_t i = *at; i + 1 < size_; ++i)
        refs_[i] = refs_[i + 1];
    refs_[--size_] = {};
    return true;
}

bool PedalOrder::move (PedalRef r, std::size_t position) noexcept
{
    if (!remove (r))
        return false;
    return insert (r, position);
}

// -------------------------------------------------------------------------------------------------

std::optional<Board::Place> Board::find (PedalRef r) const noexcept
{
    if (const auto at = pre.indexOf (r))
        return Place { Side::pre, *at };
    if (const auto at = post.indexOf (r))
        return Place { Side::post, *at };
    return std::nullopt;
}

bool Board::canPlace (PedalRef r, Side s) const noexcept
{
    if (!r.valid())
        return false;
    if (s == Side::post && !fitsAfterAmp (r.id))
        return false;
    // A move keeps the count of drives and fuzzes; a new one needs room for its latency.
    return contains (r) || !hasLatency (r.id) || latencyCount() < kLatencySlots;
}

std::optional<PedalRef> Board::nextCopy (PedalId id, Side s) const noexcept
{
    if (id >= PedalId::Count)
        return std::nullopt;
    for (int copy = 0; copy < static_cast<int> (kCopies); ++copy)
        if (const PedalRef r { id, copy }; !contains (r))
            return canPlace (r, s) ? std::optional<PedalRef> (r) : std::nullopt;
    return std::nullopt;
}

std::optional<PedalRef> Board::add (PedalId id) noexcept
{
    const auto r = nextCopy (id);
    if (!r)
        return std::nullopt;
    // Next to a copy already on the board, on its side; else where the pedal usually sits.
    for (int copy = 0; copy < static_cast<int> (kCopies); ++copy)
        if (const auto at = find ({ id, copy }))
            return place (*r, at->side, at->position + 1) ? r : std::nullopt;
    return pre.add (*r) ? r : std::nullopt;
}

std::optional<PedalRef> Board::addAfterAmp (PedalId id) noexcept
{
    const auto r = nextCopy (id, Side::post);
    if (!r)
        return std::nullopt;
    return place (*r, Side::post, post.size()) ? r : std::nullopt;
}

bool Board::place (PedalRef r, Side s, std::size_t position) noexcept
{
    if (!canPlace (r, s))
        return false;
    pre.remove (r);
    post.remove (r);
    return side (s).insert (r, position);
}

bool Board::remove (PedalRef r) noexcept
{
    return pre.remove (r) || post.remove (r);
}

} // namespace sedef
