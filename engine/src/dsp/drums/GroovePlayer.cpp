// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/drums/GroovePlayer.h"

#include <algorithm>

namespace sedef::dsp::drums
{
namespace
{
// Velocity scale per intensity level, in percent (docs/drums.md, section 4.3).
constexpr std::array<int, 5> kIntensityScale { 84, 88, 92, 96, 100 };
constexpr int kAccent = 124;

bool isTimekeeper (Instrument i) noexcept
{
    return i == Instrument::hatClosed || i == Instrument::hatOpen || i == Instrument::ride;
}
} // namespace

int GroovePlayer::scaleVelocity (int velocity, int intensity) noexcept
{
    const int scale = kIntensityScale[static_cast<size_t> (std::clamp (intensity, 1, 5) - 1)];
    return std::clamp ((velocity * scale + 50) / 100, 1, 127); // round half up
}

void GroovePlayer::setSettings (const Settings& s) noexcept
{
    pending_ = s;
    havePending_ = true;
}

void GroovePlayer::start() noexcept
{
    bar_ = -1;
    haveBar_ = false;
    fillBar_ = false;
    lastBarWasFill_ = false;
    fillsPlayed_ = 0;
    numEvents_ = 0;
    havePending_ = true;
}

int GroovePlayer::onStep (int barStep, int signature, StepEvent* out, int maxEvents) noexcept
{
    if (barStep == 0 || !haveBar_)
    {
        // A bar line, or the drums came in during a bar: the bar's events are built here, with the
        // settings chosen since the last bar line.
        if (barStep == 0 || bar_ < 0)
            ++bar_;
        if (havePending_)
        {
            current_ = pending_;
            havePending_ = false;
        }
        buildBar (signature);
    }

    const int first = barStep * slotsPerStep_;
    const int last = first + slotsPerStep_;
    int count = 0;
    for (int i = 0; i < numEvents_ && count < maxEvents; ++i)
    {
        const auto& e = events_[static_cast<size_t> (i)];
        if (e.slot < first || e.slot >= last)
            continue;
        out[count++] = { e.slot - first, slotsPerStep_, e.instrument, e.velocity, e.offsetMs };
    }
    return count;
}

void GroovePlayer::add (int slot, Instrument instrument, int velocity, float offsetMs) noexcept
{
    if (velocity <= 0 || numEvents_ >= kMaxBarEvents)
        return;
    events_[static_cast<size_t> (numEvents_++)] = { static_cast<std::int16_t> (slot), instrument,
                                                    static_cast<std::uint8_t> (velocity),
                                                    offsetMs };
}

void GroovePlayer::buildBar (int signature) noexcept
{
    groove_ = grooveFor (current_.groove, signature);
    const Groove& g = grooves()[static_cast<size_t> (groove_)];
    slotsPerStep_ = g.slotsPerStep;
    barSlots_ = g.lanes.empty() ? 0 : static_cast<int> (g.lanes[0].pattern.size());

    const int intensity = std::clamp (current_.intensity, 1, 5);
    const int every = std::max (0, current_.fillEvery);
    fillBar_ = every > 0 && bar_ % every == every - 1;
    const bool crash = every > 0 && bar_ > 0 && bar_ % every == 0 && lastBarWasFill_;
    const Fill& f = fill (g.fill, fillsPlayed_);
    const int fillSlots =
        fillBar_ ? std::min (barSlots_, intensity >= 3 ? f.longSlots : f.shortSlots) : 0;
    const int fillStart = barSlots_ - fillSlots;

    numEvents_ = 0;
    for (const auto& lane : g.lanes)
    {
        if (intensity < lane.lo || intensity > lane.hi)
            continue;
        const int n = std::min (barSlots_, static_cast<int> (lane.pattern.size()));
        for (int j = 0; j < std::min (n, fillStart); ++j)
        {
            if (crash && j == 0 && isTimekeeper (lane.instrument))
                continue; // the crash takes the hand that played the hat or ride on the "1"
            const int v = velocityOf (lane.pattern[static_cast<size_t> (j)]);
            if (v > 0)
                add (j, lane.instrument, scaleVelocity (v, intensity), lane.offsetMs);
        }
    }
    if (fillBar_)
    {
        for (const auto& lane : f.lanes)
        {
            const int skip = f.longSlots - fillSlots;
            for (int j = 0; j < fillSlots; ++j)
            {
                const auto at = static_cast<size_t> (skip + j);
                if (at >= lane.pattern.size())
                    break;
                const int v = velocityOf (lane.pattern[at]);
                if (v > 0)
                    add (fillStart + j, lane.instrument, scaleVelocity (v, intensity), 0.0f);
            }
        }
        ++fillsPlayed_;
    }
    if (crash)
    {
        add (0, Instrument::crash, scaleVelocity (kAccent, intensity), 0.0f);
        bool kicked = false;
        for (int i = 0; i < numEvents_; ++i)
        {
            auto& e = events_[static_cast<size_t> (i)];
            if (e.slot == 0 && e.instrument == Instrument::kick)
            {
                e.velocity = static_cast<std::uint8_t> (scaleVelocity (kAccent, intensity));
                kicked = true;
            }
        }
        if (!kicked)
            add (0, Instrument::kick, scaleVelocity (kAccent, intensity), 0.0f);
    }
    lastBarWasFill_ = fillBar_;

    // Slot order; events on one slot keep the order they were added in (stable).
    for (int i = 1; i < numEvents_; ++i)
    {
        const BarEvent e = events_[static_cast<size_t> (i)];
        int j = i - 1;
        while (j >= 0 && events_[static_cast<size_t> (j)].slot > e.slot)
        {
            events_[static_cast<size_t> (j + 1)] = events_[static_cast<size_t> (j)];
            --j;
        }
        events_[static_cast<size_t> (j + 1)] = e;
    }
    haveBar_ = true;
}

} // namespace sedef::dsp::drums
