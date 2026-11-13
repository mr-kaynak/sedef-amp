// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/board/Board.h"

#include <juce_core/juce_core.h>
#include <optional>

namespace sedef::plugin
{

/// The pedalboard as presets and the bridge spell it: pedal ids in signal order, "kelp#2" for a
/// second copy and "amp" before the pedals after the amp (sedef/board/BoardText.h,
/// docs/board.md §3.3).
juce::var boardToVar (const Board& board);
/// The board of a list of ids, in order (sedef::BoardReader). Ids this build doesn't know (a newer
/// version's pedal), repeats and pedals their side can't take are skipped. Nothing when `v` is not
/// a list.
std::optional<Board> boardFromVar (const juce::var& v);

/// As sessions spell it: "gate,tide,pearl", "gate,kelp,kelp#2,amp,gyre". An empty board is an
/// empty string.
juce::String boardToText (const Board& board);
Board boardFromText (const juce::String& text);

/// The board for a preset's parameters (an object of id: value) when the preset has none:
/// Board::legacyFor with the values it stores (missing ones default).
Board legacyBoardFor (const juce::var& parameters);
/// Board::switchedOn for the same values: only the pedals they switch on. With no parameters
/// (void), the defaults': Init's board, and a fresh instance's.
Board switchedOnBoardFor (const juce::var& parameters);

} // namespace sedef::plugin
