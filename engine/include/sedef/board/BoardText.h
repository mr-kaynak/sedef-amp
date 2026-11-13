// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// How sessions, presets, the UI bridge and the tools spell a board (docs/board.md §3.3): pedal ids
// in signal order, `kelp#2` for a second copy, and `amp` where the amp is, written only when a
// pedal stands after it. Message thread and tools only: these allocate.

#include "sedef/board/Board.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sedef
{

inline constexpr std::string_view kAmpToken = "amp";

/// "kelp" for a first copy, "kelp#2" for a second.
std::string tokenOf (PedalRef r);
/// The pedal a token names; nothing for "amp" or an id this build doesn't know.
std::optional<PedalRef> refOf (std::string_view token) noexcept;

/// The board as tokens in signal order.
std::vector<std::string> boardTokens (const Board& board);

/// Reads tokens in order, the side switching at the first "amp". Unknown ids, repeats, a second
/// "amp" and pedals their side doesn't take (or that would exceed the drive budget) are skipped,
/// so whatever a newer or hand-edited file holds, the board is valid.
class BoardReader
{
public:
    void read (std::string_view token) noexcept;
    const Board& board() const noexcept { return board_; }

private:
    Board board_;
    Side side_ = Side::pre;
};

/// "gate,kelp,amp,gyre": the tokens joined by commas (an empty board is "").
std::string boardToText (const Board& board);
/// Splits at commas, trims spaces and reads each token (BoardReader).
Board boardFromText (std::string_view text) noexcept;

} // namespace sedef
