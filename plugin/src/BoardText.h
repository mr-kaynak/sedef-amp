// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/board/Board.h"

#include <juce_core/juce_core.h>
#include <optional>

namespace sedef::plugin
{

/// The pedalboard as presets and the bridge spell it: pedal ids in signal order.
juce::var boardToVar (const Board& board);
/// The pedals of a list of ids, in order. Ids this build doesn't know (a newer version's pedal)
/// and repeats are skipped. Nothing when `v` is not a list.
std::optional<Board> boardFromVar (const juce::var& v);

/// As sessions spell it: "gate,tide,pearl". An empty board is an empty string.
juce::String boardToText (const Board& board);
Board boardFromText (const juce::String& text);

/// The board for a preset's parameters (an object of id: value) when the preset has none:
/// Board::legacyFor with the values it stores (missing ones default).
Board legacyBoardFor (const juce::var& parameters);

} // namespace sedef::plugin
