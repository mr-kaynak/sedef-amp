// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/board/PedalRack.h"

#include <cassert>
#include <limits>

namespace sedef
{

PedalSettings PedalSettings::from (const ParamSnapshot& p) noexcept
{
    using P = ParamId;
    PedalSettings s;
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings)                            \
    s.on[index (PedalId::id)] = p.flag (P::enable);                                                \
    s.id = settings;
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
    return s;
}

void PedalRack::prepare (const ProcessSpec& mono)
{
    totalLatency_ = 0;
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings)                            \
    slots_.id.prepare (mono);                                                                      \
    latency_[index (PedalId::id)] = slots_.id.latency;                                             \
    totalLatency_ += slots_.id.latency;
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL

    // A keyed pedal reads the key up to the whole rack's latency late, a block at a time.
    keyDelay_.prepare (totalLatency_ + mono.maxBlockSize);
    keyed_.assign (static_cast<size_t> (mono.maxBlockSize), 0.0f);
    ran_.fill (false);
    layout_ = layoutOf (order_); // which pedals have latency depends on the rate and the mode
}

void PedalRack::reset() noexcept
{
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings) slots_.id.reset();
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
    keyDelay_.reset();
    ran_.fill (false);
}

int PedalRack::latencyOf (const Board& board) const noexcept
{
    int sum = 0;
    for (const auto id : board)
        sum += latencyOf (id);
    return sum;
}

PedalRack::Layout PedalRack::layoutOf (const Board& board) const noexcept
{
    Layout layout;
    std::array<bool, kNumPedals> placed {};
    const auto delaysBefore = [&] (int rank) {
        // Board::everyPedal() is in rank order.
        for (const auto id : Board::everyPedal())
        {
            const auto i = index (id);
            if (pedalInfo (id).rank >= rank)
                break;
            if (!placed[i] && latency_[i] > 0 && !board.contains (id))
            {
                layout.steps[layout.size++] = { id, false };
                placed[i] = true;
            }
        }
    };
    for (const auto id : board)
    {
        delaysBefore (pedalInfo (id).rank);
        layout.steps[layout.size++] = { id, true };
    }
    delaysBefore (std::numeric_limits<int>::max());
    return layout;
}

bool PedalRack::needsDuck (const Board& next) const noexcept
{
    // Pedals without latency that join or leave can fade in or out where they are, as long as
    // nothing else moves: compare the two chains without them.
    const auto without = [&] (const Layout& layout) {
        Layout kept;
        for (const auto step : layout)
        {
            const bool fades =
                latencyOf (step.id) == 0 && order_.contains (step.id) != next.contains (step.id);
            if (!fades)
                kept.steps[kept.size++] = step;
        }
        return kept;
    };
    return !(without (layout_) == without (layoutOf (next)));
}

void PedalRack::rewire (const Board& order) noexcept
{
    // Pedals that stay keep their state: the signal faded out through them, so what they hold is
    // quiet, and a compressor that kept its envelope doesn't overshoot when the signal returns.
    // Joining pedals start clean; leaving ones are bypassed at once (their delay stays).
    for (std::size_t i = 0; i < kNumPedals; ++i)
    {
        const auto id = static_cast<PedalId> (i);
        const bool joins = order.contains (id) && !order_.contains (id);
        const bool leaves = !order.contains (id) && order_.contains (id);
        if (!joins && !leaves)
            continue;
        switch (id)
        {
#define SEDEF_PEDAL(pid, name, category, rank, enable, Module, settings)                           \
    case PedalId::pid:                                                                             \
        if (joins)                                                                                 \
            slots_.pid.reset();                                                                    \
        else                                                                                       \
            slots_.pid.fader.snap (false);                                                         \
        break;
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
        case PedalId::Count:
            break;
        }
    }
    order_ = order;
    layout_ = layoutOf (order_);
}

void PedalRack::adopt (const Board& order) noexcept
{
    assert (latencyOf (order) == latencyOf (order_));
    for (const auto id : order)
    {
        if (order_.contains (id))
            continue;
        switch (id)
        {
#define SEDEF_PEDAL(pid, name, category, rank, enable, Module, settings)                           \
    case PedalId::pid:                                                                             \
        slots_.pid.join();                                                                         \
        break;
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
        case PedalId::Count:
            break;
        }
    }
    order_ = order;
    layout_ = layoutOf (order_);
}

bool PedalRack::fullyBypassed (PedalId id) const noexcept
{
    switch (id)
    {
#define SEDEF_PEDAL(pid, name, category, rank, enable, Module, settings)                           \
    case PedalId::pid:                                                                             \
        return slots_.pid.fader.isFullyBypassed();
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
    case PedalId::Count:
        break;
    }
    return true;
}

const float* PedalRack::alignedKey (int delay, int n) noexcept
{
    if (delay <= 0)
        return key_;
    // After the block was pushed, read (0) is its last sample: sample i, `delay` late, is
    // read (delay + n - 1 - i).
    for (int i = 0; i < n; ++i)
        keyed_[static_cast<size_t> (i)] = keyDelay_.read (delay + n - 1 - i);
    return keyed_.data();
}

template <typename Module>
bool PedalRack::run (PedalSlot<Module>& slot, float* io, int n, bool enabled,
                     const typename Module::Settings& settings, int keyDelay) noexcept
{
    if constexpr (requires (Module& m, const AudioView& v, const float* k) { m.process (v, k); })
    {
        // A keyed pedal (the gate) listens to the instrument, lined up with the signal that
        // reaches it through the pedals before it.
        const float* key = alignedKey (keyDelay, n);
        return slot.process (io, n, enabled, settings,
                             [key] (Module& m, const AudioView& v) { m.process (v, key); });
    }
    else
    {
        return slot.process (io, n, enabled, settings,
                             [] (Module& m, const AudioView& v) { m.process (v); });
    }
}

void PedalRack::process (float* io, const float* key, int n, const PedalSettings& settings,
                         const std::array<bool, kNumPedals>& forcedOff) noexcept
{
    key_ = key;
    for (int i = 0; i < n; ++i)
        keyDelay_.push (key[i]);

    ran_.fill (false);
    int before = 0; // latency of the chain so far
    for (const auto step : layout_)
    {
        const auto id = step.id;
        const auto i = index (id);
        // A delay left by a pedal off the board: its bypass, a pure delay.
        const bool enabled = step.onBoard && settings.on[i] && !forcedOff[i];
        switch (id)
        {
#define SEDEF_PEDAL(pid, name, category, rank, enable, Module, unused)                             \
    case PedalId::pid:                                                                             \
        ran_[i] = run (slots_.pid, io, n, enabled, settings.pid, before);                          \
        break;
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
        case PedalId::Count:
            break;
        }
        before += latency_[i];
    }
}

} // namespace sedef
