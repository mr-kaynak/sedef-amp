// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BoardText.h"

#include "ParameterLayout.h"

namespace sedef::plugin
{

juce::var boardToVar (const Board& board)
{
    juce::Array<juce::var> ids;
    for (const auto id : board)
        ids.add (toJuce (pedalInfo (id).id));
    return ids;
}

std::optional<Board> boardFromVar (const juce::var& v)
{
    const auto* list = v.getArray();
    if (list == nullptr)
        return std::nullopt;
    Board board;
    for (const auto& item : *list)
        if (const auto id = findPedal (item.toString().toStdString()))
            board.insert (*id, board.size()); // false for a repeat
    return board;
}

juce::String boardToText (const Board& board)
{
    juce::StringArray ids;
    for (const auto id : board)
        ids.add (toJuce (pedalInfo (id).id));
    return ids.joinIntoString (",");
}

Board boardFromText (const juce::String& text)
{
    Board board;
    for (const auto& token : juce::StringArray::fromTokens (text, ",", ""))
        if (const auto id = findPedal (token.trim().toStdString()))
            board.insert (*id, board.size());
    return board;
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
