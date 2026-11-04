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

Board Board::legacy() noexcept
{
    // The fixed chain before the board, in its order. 0.3.0 had Gate, Tide, Pearl, Coral and
    // Ripple; the pedals added after it joined the fixed chain before the board replaced it.
    Board b;
    for (auto id : { PedalId::gate, PedalId::trench, PedalId::remora, PedalId::tide,
                     PedalId::urchin, PedalId::sargasso, PedalId::pearl, PedalId::kelp,
                     PedalId::coral, PedalId::ripple, PedalId::breaker })
        b.insert (id, b.size());
    return b;
}

Board Board::everyPedal() noexcept
{
    Board b;
    for (std::size_t i = 0; i < kNumPedals; ++i)
        b.add (static_cast<PedalId> (i));
    return b;
}

std::optional<std::size_t> Board::indexOf (PedalId id) const noexcept
{
    for (std::size_t i = 0; i < size_; ++i)
        if (pedals_[i] == id)
            return i;
    return std::nullopt;
}

bool Board::add (PedalId id) noexcept
{
    if (id >= PedalId::Count || contains (id))
        return false;
    const int rank = pedalInfo (id).rank;
    std::size_t position = 0;
    for (std::size_t i = 0; i < size_; ++i)
        if (pedalInfo (pedals_[i]).rank <= rank)
            position = i + 1;
    return insert (id, position);
}

bool Board::insert (PedalId id, std::size_t position) noexcept
{
    if (id >= PedalId::Count || contains (id) || size_ >= kCapacity)
        return false;
    position = std::min (position, size_);
    for (std::size_t i = size_; i > position; --i)
        pedals_[i] = pedals_[i - 1];
    pedals_[position] = id;
    ++size_;
    return true;
}

bool Board::remove (PedalId id) noexcept
{
    const auto at = indexOf (id);
    if (!at)
        return false;
    for (std::size_t i = *at; i + 1 < size_; ++i)
        pedals_[i] = pedals_[i + 1];
    pedals_[--size_] = PedalId::Count;
    return true;
}

bool Board::move (PedalId id, std::size_t position) noexcept
{
    if (!remove (id))
        return false;
    return insert (id, position);
}

} // namespace sedef
