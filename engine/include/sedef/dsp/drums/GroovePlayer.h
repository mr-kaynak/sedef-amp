// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/dsp/drums/Grooves.h"

#include <array>
#include <cstdint>

namespace sedef::dsp::drums
{

/// What a groove plays, bar by bar and step by step (docs/drums.md, section 4). No audio and no
/// clock: the metronome tells it when a step sounds and places the events in samples.
class GroovePlayer
{
public:
    static constexpr int kMaxBarEvents = 192;
    static constexpr int kMaxStepEvents = 64;

    struct Settings
    {
        int groove = 0;
        int intensity = 3; ///< 1..5
        int fillEvery = 4; ///< bars; 0 = no fills
    };

    /// One event of the step that just sounded.
    struct StepEvent
    {
        int slot = 0; ///< slot within the step (0 = on the step)
        int slotsPerStep = 1;
        Instrument instrument = Instrument::kick;
        int velocity = 0; ///< 1..127, already scaled by the intensity
        float offsetMs = 0.0f;
    };

    /// Takes effect at the next bar line, or at once before the first bar.
    void setSettings (const Settings& s) noexcept;
    /// The next step that sounds is the first of a new run (bars count from 0 again).
    void start() noexcept;

    /// The metronome sounded `barStep` of a bar in `signature`. Writes the step's events, in slot
    /// then lane order, and returns how many.
    int onStep (int barStep, int signature, StepEvent* out, int maxEvents) noexcept;

    /// Bars since start (the bar playing now), -1 before the first.
    int bar() const noexcept { return bar_; }
    bool fillBar() const noexcept { return fillBar_; }
    /// The groove playing in the current bar.
    int groove() const noexcept { return groove_; }

    /// Velocity after the intensity's scaling (docs/drums.md, section 4.3).
    static int scaleVelocity (int velocity, int intensity) noexcept;

private:
    struct BarEvent
    {
        std::int16_t slot = 0;
        Instrument instrument = Instrument::kick;
        std::uint8_t velocity = 0;
        float offsetMs = 0.0f;
    };

    void buildBar (int signature) noexcept;
    void add (int slot, Instrument instrument, int velocity, float offsetMs) noexcept;

    Settings pending_ {};
    Settings current_ {};
    bool havePending_ = true;
    bool haveBar_ = false;
    int bar_ = -1;
    int groove_ = 0;
    int slotsPerStep_ = 4;
    int barSlots_ = 0;
    bool fillBar_ = false;
    bool lastBarWasFill_ = false;
    int fillsPlayed_ = 0;
    std::array<BarEvent, kMaxBarEvents> events_ {};
    int numEvents_ = 0;
};

} // namespace sedef::dsp::drums
