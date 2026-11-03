// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/board/PedalRack.h"

#include <cassert>

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

    pad_.prepare (std::max (totalLatency_, 1));
    // A keyed pedal reads the key up to the whole rack's latency late, a block at a time.
    keyDelay_.prepare (totalLatency_ + mono.maxBlockSize);
    keyed_.assign (static_cast<size_t> (mono.maxBlockSize), 0.0f);
    ran_.fill (false);
    rewire (order_); // latencies depend on the sample rate
}

void PedalRack::reset() noexcept
{
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings) slots_.id.reset();
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
    pad_.reset();
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

bool PedalRack::needsDuck (const Board& next) const noexcept
{
    // Pedals that stay must keep their relative order...
    std::size_t j = 0;
    for (const auto id : order_)
    {
        if (!next.contains (id))
            continue;
        while (j < next.size() && !order_.contains (next[j]))
            ++j;
        if (j >= next.size() || next[j] != id)
            return true;
        ++j;
    }
    // ...and the latency may only move by the pad, which can't happen mid-signal.
    for (const auto id : order_)
        if (!next.contains (id) && latencyOf (id) > 0)
            return true;
    for (const auto id : next)
        if (!order_.contains (id) && latencyOf (id) > 0)
            return true;
    return false;
}

void PedalRack::rewire (const Board& order) noexcept
{
    order_ = order;
    for (const auto id : order_)
    {
        switch (id)
        {
#define SEDEF_PEDAL(pid, name, category, rank, enable, Module, settings)                           \
    case PedalId::pid:                                                                             \
        slots_.pid.reset();                                                                        \
        break;
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
        case PedalId::Count:
            break;
        }
    }
    padLength_ = totalLatency_ - latencyOf (order_);
    pad_.reset();
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
    int before = 0; // latency of the pedals so far
    for (const auto id : order_)
    {
        const auto i = index (id);
        const bool enabled = settings.on[i] && !forcedOff[i];
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

    if (padLength_ > 0)
        for (int i = 0; i < n; ++i)
        {
            pad_.push (io[i]);
            io[i] = pad_.read (padLength_);
        }
}

} // namespace sedef
