// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/drums/Grooves.h"

#include <algorithm>
#include <array>

namespace sedef::dsp::drums
{
namespace
{
constexpr auto K = Instrument::kick;
constexpr auto S = Instrument::snare;
constexpr auto H = Instrument::hatClosed;
constexpr auto O = Instrument::hatOpen;
constexpr auto R = Instrument::ride;
constexpr auto C = Instrument::crash;
constexpr auto T1 = Instrument::tomHigh;
constexpr auto T2 = Instrument::tomMid;
constexpr auto T3 = Instrument::tomFloor;

// Original patterns from common drumming idioms (docs/drums.md, section 4.4). One character per
// slot; lanes are listed in the order events on the same slot fire.

// 0 Straight rock, 4/4, sixteenths.
constexpr Lane kStraightRock[] = {
    { H, "X...x...X...x...", 1, 1 }, { H, "X.x.X.x.X.x.X.x.", 2, 4 },
    { R, "X.x.X.x.X.x.X.x.", 5, 5 }, { S, "....X.......X...", 1, 5, 3.0f },
    { S, ".......g.g......", 4, 5 }, { K, "X.......X.......", 1, 2 },
    { K, "X.......X.x.....", 3, 3 }, { K, "X.....x.X.x.....", 4, 5 },
};

// 1 Half-time, 4/4, sixteenths.
constexpr Lane kHalfTime[] = {
    { H, "X...x...X...x...", 1, 1 }, { H, "X.x.X.x.X.x.X.x.", 2, 5 },
    { O, "..............o.", 4, 5 }, { S, "........X.......", 1, 5, 5.0f },
    { S, "...........g..g.", 4, 5 }, { K, "X...............", 1, 1 },
    { K, "X.....x.........", 2, 3 }, { K, "X.....x...x...x.", 4, 5 },
};

// 2 Ballad, 4/4, sixteenths.
constexpr Lane kBallad[] = {
    { H, "o...o...o...o...", 1, 2 },       { H, "o.g.o.g.o.g.o.g.", 3, 4 },
    { R, "o.g.o.g.o.g.o.g.", 5, 5 },       { S, "....o.......o...", 1, 2, 5.0f },
    { S, "....x.......x...", 3, 5, 5.0f }, { K, "x.......x.......", 1, 3 },
    { K, "x.....g.x.......", 4, 5 },
};

// 3 Fast eighths, 4/4, sixteenths.
constexpr Lane kFastEighths[] = {
    { H, "x.x.x.x.x.x.x.x.", 1, 3 }, { R, "X.x.X.x.X.x.X.x.", 4, 5 },
    { C, "X...............", 5, 5 }, { K, "x.......x.......", 1, 1 },
    { S, "....x.......x...", 1, 1 }, { K, "x...x...x...x...", 2, 5 },
    { S, "..x...x...x...x.", 2, 5 },
};

// 4 Shuffle, 4/4, triplets.
constexpr Lane kShuffle[] = {
    { H, "x..x..x..x..", 1, 1 },       { H, "X.xx.xX.xx.x", 2, 4 }, { R, "X.xx.xX.xx.x", 5, 5 },
    { S, "...X.....X..", 1, 5, 3.0f }, { S, "........g...", 4, 5 }, { K, "x.....x.....", 1, 3 },
    { K, "x.....x....x", 4, 5 },
};

// 5 Swing, 4/4, triplets.
constexpr Lane kSwing[] = {
    { R, "x..x..x..x..", 1, 1 }, { R, "X..x.xX..x.x", 2, 5 }, { H, "...o.....o..", 1, 5 },
    { K, "g..g..g..g..", 3, 5 }, { S, "........g..g", 4, 5 },
};

// 6 Waltz, 3/4, sixteenths.
constexpr Lane kWaltz[] = {
    { H, "x...x...x...", 1, 2 },       { H, "x.x.x.x.x.x.", 3, 5 }, { K, "X...........", 1, 5 },
    { S, "....x...x...", 1, 5, 3.0f }, { K, "..........g.", 4, 5 },
};

// 7 Two-step, 2/4, sixteenths.
constexpr Lane kTwoStep[] = {
    { H, "x...x...", 1, 1 }, { H, "x.x.x.x.", 2, 5 },       { K, "X.......", 1, 3 },
    { K, "X.....x.", 4, 5 }, { S, "....x...", 1, 5, 3.0f },
};

// 8 Five, 5/4, sixteenths: three plus two.
constexpr Lane kFive[] = {
    { H, "x...x...x...x...x...", 1, 1 }, { H, "x.x.x.x.x.x.x.x.x.x.", 2, 5 },
    { K, "X...........x.......", 1, 5 }, { S, "........x.......x...", 1, 5 },
    { K, "......g.............", 4, 5 },
};

// 9 Six-eight, 6/8, sixteenths over eighth steps.
constexpr Lane kSixEight[] = {
    { H, "x.....x.....", 1, 1 }, { H, "x.x.x.x.x.x.", 2, 5 }, { K, "X...........", 1, 5 },
    { S, "......X.....", 1, 5 }, { K, "..........x.", 3, 5 },
};

// 10 Seven, 7/8: two, two, three.
constexpr Lane kSeven[] = {
    { H, "x...x...x.....", 1, 1 },
    { H, "x.x.x.x.x.x.x.", 2, 5 },
    { K, "X.......x.....", 1, 5 },
    { S, "....x.......x.", 1, 5 },
};

// 11 Nine, 9/8: two, two, two, three.
constexpr Lane kNine[] = {
    { H, "x...x...x...x.....", 1, 1 }, { H, "x.x.x.x.x.x.x.x.x.", 2, 5 },
    { K, "X.......x.........", 1, 5 }, { S, "....x.......x.....", 1, 5 },
    { S, "................g.", 4, 5 },
};

// 12 Slow twelve-eight, 12/8, one slot per eighth step.
constexpr Lane kTwelveEight[] = {
    { H, "X..X..X..X..", 1, 1 },       { H, "XxxXxxXxxXxx", 2, 5 }, { K, "X.....x.....", 1, 5 },
    { S, "...X.....X..", 1, 5, 5.0f }, { K, "..........x.", 4, 5 },
};

// Signature indices (Metronome::stepsFor): 2/4, 3/4, 4/4, 5/4, 6/8, 7/8, 9/8, 12/8.
constexpr int k24 = 0, k34 = 1, k44 = 2, k54 = 3, k68 = 4, k78 = 5, k98 = 6, k128 = 7;

constexpr std::array<Groove, 13> kGrooves { {
    { "Straight rock", "Eighths on the hat, backbeat on 2 and 4", k44, 4, FillFamily::sixteenths,
      kStraightRock },
    { "Half-time", "Snare on 3: slow and heavy over a steady pulse", k44, 4, FillFamily::sixteenths,
      kHalfTime },
    { "Ballad", "Soft and open, room for a slow song", k44, 4, FillFamily::sixteenths, kBallad },
    { "Fast eighths", "Kick and snare trade eighths, fast and loud", k44, 4, FillFamily::sixteenths,
      kFastEighths },
    { "Shuffle", "Triplet bounce on the hat, backbeat on 2 and 4", k44, 3, FillFamily::triplets,
      kShuffle },
    { "Swing", "Swing time on the ride, hat on 2 and 4", k44, 3, FillFamily::triplets, kSwing },
    { "Waltz", "One-two-three, kick on one", k34, 4, FillFamily::sixteenths, kWaltz },
    { "Two-step", "Bass on one, snare on two: a quick march", k24, 4, FillFamily::sixteenths,
      kTwoStep },
    { "Five", "Three plus two, kick on the first of each group", k54, 4, FillFamily::sixteenths,
      kFive },
    { "Six-eight", "Two rolling groups of three, snare on four", k68, 2, FillFamily::eighthMeters,
      kSixEight },
    { "Seven", "Grouped two, two, three", k78, 2, FillFamily::eighthMeters, kSeven },
    { "Nine", "Grouped two, two, two, three", k98, 2, FillFamily::eighthMeters, kNine },
    { "Slow twelve-eight", "Slow triplets with a heavy backbeat", k128, 1, FillFamily::triplets,
      kTwelveEight },
} };

// Fills (docs/drums.md, section 4.5), written for the long length.
constexpr Lane kSixteenthsA[] = {
    { S, "XxXx....", 1, 5 },
    { T1, "....xx..", 1, 5 },
    { T3, "......xX", 1, 5 },
    { K, "x...x...", 1, 5 },
};
constexpr Lane kSixteenthsB[] = {
    { S, "x.xx....", 1, 5 },
    { T2, "....x.x.", 1, 5 },
    { T3, ".....x.X", 1, 5 },
    { K, "x...x...", 1, 5 },
};
constexpr Lane kTripletsA[] = {
    { S, "xxx...", 1, 5 },  { T1, "...x..", 1, 5 }, { T2, "....x.", 1, 5 },
    { T3, ".....X", 1, 5 }, { K, "x..x..", 1, 5 },
};
constexpr Lane kTripletsB[] = {
    { S, "x.x...", 1, 5 },
    { T1, "...xx.", 1, 5 },
    { T3, ".....X", 1, 5 },
    { K, "x..x..", 1, 5 },
};
constexpr Lane kEighthMetersA[] = {
    { S, "xx....", 1, 5 },
    { T1, "..xx..", 1, 5 },
    { T3, "....xX", 1, 5 },
    { K, "x...x.", 1, 5 },
};
constexpr Lane kEighthMetersB[] = {
    { S, "x.x...", 1, 5 },
    { T2, "...xx.", 1, 5 },
    { T3, ".....X", 1, 5 },
    { K, "x...x.", 1, 5 },
};

constexpr std::array<std::array<Fill, 2>, 3> kFills { {
    { { { 8, 4, kSixteenthsA }, { 8, 4, kSixteenthsB } } },
    { { { 6, 3, kTripletsA }, { 6, 3, kTripletsB } } },
    { { { 6, 4, kEighthMetersA }, { 6, 4, kEighthMetersB } } },
} };
} // namespace

std::span<const Groove> grooves() noexcept
{
    return kGrooves;
}

const Fill& fill (FillFamily family, int variant) noexcept
{
    const auto f = std::clamp (static_cast<int> (family), 0, static_cast<int> (kFills.size()) - 1);
    return kFills[static_cast<size_t> (f)][static_cast<size_t> (variant & 1)];
}

int grooveFor (int groove, int signature) noexcept
{
    const int count = static_cast<int> (kGrooves.size());
    const int g = std::clamp (groove, 0, count - 1);
    if (kGrooves[static_cast<size_t> (g)].signature == signature)
        return g;
    for (int i = 0; i < count; ++i)
        if (kGrooves[static_cast<size_t> (i)].signature == signature)
            return i;
    return g; // every signature has a groove (tested); unreachable
}

int velocityOf (char c) noexcept
{
    switch (c)
    {
    case 'g':
        return 40;
    case 'o':
        return 72;
    case 'x':
        return 100;
    case 'X':
        return 124;
    default:
        return 0;
    }
}

} // namespace sedef::dsp::drums
