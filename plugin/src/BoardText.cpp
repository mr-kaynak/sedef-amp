// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BoardText.h"

#include "ParameterLayout.h"
#include "sedef/board/BoardText.h"

namespace sedef::plugin
{

juce::var boardToVar (const Board& board)
{
    juce::Array<juce::var> ids;
    for (const auto& token : boardTokens (board))
        ids.add (juce::String (token));
    return ids;
}

std::optional<Board> boardFromVar (const juce::var& v)
{
    const auto* list = v.getArray();
    if (list == nullptr)
        return std::nullopt;
    BoardReader reader;
    for (const auto& item : *list)
        reader.read (item.toString().toStdString());
    return reader.board();
}

juce::String boardToText (const Board& board)
{
    return juce::String (sedef::boardToText (board));
}

Board boardFromText (const juce::String& text)
{
    return sedef::boardFromText (text.toStdString());
}

namespace
{
/// Whether `parameters` switches `id` on; a value it doesn't hold is the default.
bool storedOn (const juce::var& parameters, ParamId id)
{
    const auto& s = spec (id);
    const juce::Identifier key (toJuce (s.id));
    const float value =
        parameters.hasProperty (key) ? static_cast<float> (parameters[key]) : s.defaultValue;
    return s.sanitise (value) >= 0.5f;
}
} // namespace

Board legacyBoardFor (const juce::var& parameters)
{
    return Board::legacyFor ([&parameters] (ParamId id) { return storedOn (parameters, id); });
}

Board switchedOnBoardFor (const juce::var& parameters)
{
    return Board::switchedOn ([&parameters] (ParamId id) { return storedOn (parameters, id); });
}

} // namespace sedef::plugin
