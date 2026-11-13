// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/board/BoardText.h"

namespace sedef
{

std::string tokenOf (PedalRef r)
{
    std::string token (pedalInfo (r.id).id);
    if (r.instance > 0)
        token += "#" + std::to_string (r.instance + 1);
    return token;
}

std::optional<PedalRef> refOf (std::string_view token) noexcept
{
    int copy = 0;
    if (const auto hash = token.find ('#'); hash != std::string_view::npos)
    {
        // Only "#2": the copies a board holds (docs/board.md §2).
        if (token.substr (hash + 1) != "2")
            return std::nullopt;
        copy = 1;
        token = token.substr (0, hash);
    }
    if (const auto id = findPedal (token))
        return PedalRef { *id, copy };
    return std::nullopt;
}

std::vector<std::string> boardTokens (const Board& board)
{
    std::vector<std::string> tokens;
    tokens.reserve (board.size() + 1);
    for (const auto r : board.pre)
        tokens.push_back (tokenOf (r));
    if (!board.post.empty())
    {
        tokens.emplace_back (kAmpToken);
        for (const auto r : board.post)
            tokens.push_back (tokenOf (r));
    }
    return tokens;
}

void BoardReader::read (std::string_view token) noexcept
{
    if (token == kAmpToken)
    {
        side_ = Side::post; // a second "amp" changes nothing
        return;
    }
    const auto r = refOf (token);
    if (!r || board_.contains (*r))
        return;
    board_.place (*r, side_, board_.side (side_).size()); // false when the side can't take it
}

std::string boardToText (const Board& board)
{
    std::string text;
    for (const auto& token : boardTokens (board))
        text += (text.empty() ? "" : ",") + token;
    return text;
}

Board boardFromText (std::string_view text) noexcept
{
    BoardReader reader;
    const auto trim = [] (std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.remove_prefix (1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.remove_suffix (1);
        return s;
    };
    while (!text.empty())
    {
        const auto comma = text.find (',');
        reader.read (trim (text.substr (0, comma)));
        if (comma == std::string_view::npos)
            break;
        text.remove_prefix (comma + 1);
    }
    return reader.board();
}

} // namespace sedef
