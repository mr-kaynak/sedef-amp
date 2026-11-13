// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace sedef::dsp::drums
{

/// The kit's instruments (docs/drums.md, section 5.4). The open and closed hats are one
/// instrument in the kit, struck two ways.
enum class Instrument : std::uint8_t
{
    kick,
    snare,
    hatClosed,
    hatOpen,
    ride,
    crash,
    tomHigh,
    tomMid,
    tomFloor,
};
inline constexpr int kNumInstruments = 9;

/// One instrument's line in a groove: a character per slot of the bar (`.` rest, `g` ghost, `o`
/// soft, `x` normal, `X` accent), the intensities it plays at and an offset in milliseconds.
struct Lane
{
    Instrument instrument = Instrument::kick;
    std::string_view pattern;
    std::uint8_t lo = 1; ///< plays at intensity lo..hi (1..5)
    std::uint8_t hi = 5;
    float offsetMs = 0.0f; ///< late by this much (never early: docs/drums.md, section 3.3)
};

enum class FillFamily : std::uint8_t
{
    sixteenths,   ///< sixteenths over quarter steps
    triplets,     ///< triplet slots: 4/4 shuffle and swing, 12/8
    eighthMeters, ///< sixteenths over eighth steps: 6/8, 7/8, 9/8
};

struct Groove
{
    std::string_view name;
    std::string_view feel; ///< one line, for the groove picker
    int signature = 2;     ///< the metronome's signature index (2/4 … 12/8)
    int slotsPerStep = 4;
    FillFamily fill = FillFamily::sixteenths;
    std::span<const Lane> lanes;
};

/// A fill fills the last `longSlots` (intensity 3..5) or `shortSlots` (1..2) slots of a bar. Its
/// lanes are written for the long length; the short one plays their last `shortSlots` slots.
struct Fill
{
    int longSlots = 8;
    int shortSlots = 4;
    std::span<const Lane> lanes;
};

/// The grooves, in the order of the drumsGroove parameter's choices (append only).
std::span<const Groove> grooves() noexcept;
/// The two variants of a family's fill (0 = A, 1 = B).
const Fill& fill (FillFamily family, int variant) noexcept;

/// The groove that plays for a `groove` index in `signature`: that groove when it is written in
/// the signature, otherwise the signature's first groove (docs/drums.md, section 3.5).
int grooveFor (int groove, int signature) noexcept;

/// Pattern character to velocity (0 for a rest or an unknown character).
int velocityOf (char c) noexcept;

} // namespace sedef::dsp::drums
