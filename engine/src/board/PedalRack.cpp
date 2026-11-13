// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/board/PedalRack.h"

#include <cassert>
#include <limits>

namespace sedef
{
namespace
{
/// Reads the second copy's parameters where the settings expressions read the first copy's.
struct SecondCopyView
{
    const ParamSnapshot& p;
    float operator[] (ParamId id) const noexcept { return p[secondCopyOf (id)]; }
    bool flag (ParamId id) const noexcept { return p.flag (secondCopyOf (id)); }
    int choice (ParamId id) const noexcept { return p.choice (secondCopyOf (id)); }
};

template <typename Snapshot>
void fill (PedalSettings& s, std::size_t copy, const Snapshot& p) noexcept
{
    using P = ParamId;
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings)                            \
    s.on[slotIndex ({ PedalId::id, static_cast<int> (copy) })] = p.flag (P::enable);               \
    s.id[copy] = settings;
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
}
} // namespace

PedalSettings PedalSettings::from (const ParamSnapshot& p) noexcept
{
    PedalSettings s;
    fill (s, 0, p);
    fill (s, 1, SecondCopyView { p });
    return s;
}

void PedalRack::prepare (const ProcessSpec& spec, Side side)
{
    side_ = side;
    totalLatency_ = 0;
    latency_.fill (0);
    ProcessSpec slotSpec = spec;
    slotSpec.numChannels = side == Side::pre ? 1 : 2;
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings)                            \
    if (side == Side::pre || fitsAfterAmp (PedalId::id))                                           \
        for (std::size_t copy = 0; copy < kCopies; ++copy)                                         \
        {                                                                                          \
            slots_.id[copy].prepare (slotSpec);                                                    \
            latency_[slotIndex ({ PedalId::id, static_cast<int> (copy) })] =                       \
                slots_.id[copy].latency;                                                           \
        }                                                                                          \
    totalLatency_ += slots_.id[0].latency;
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
    // After the amp only pedals without latency stand (fitsAfterAmp).
    assert (side == Side::pre || totalLatency_ == 0);

    // A keyed pedal reads the key up to the whole rack's latency late, a block at a time.
    keyDelay_.prepare (totalLatency_ + spec.maxBlockSize);
    keyed_.assign (static_cast<size_t> (spec.maxBlockSize), 0.0f);
    ran_.fill (false);
    if (side == Side::post)
        order_ = {};
    layout_ = layoutOf (order_); // which pedals have latency depends on the rate and the mode
}

void PedalRack::reset() noexcept
{
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings)                            \
    for (auto& slot : slots_.id)                                                                   \
        slot.reset();
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
    keyDelay_.reset();
    ran_.fill (false);
}

int PedalRack::latencyOf (const PedalOrder& order) const noexcept
{
    int sum = 0;
    for (const auto r : order)
        sum += latencyOf (r);
    return sum;
}

PedalRack::Layout PedalRack::layoutOf (const PedalOrder& order) const noexcept
{
    // The delays the board needs: the rack's latency less what its pedals bring. They come from
    // the drives and fuzzes with no copy on it, in default order, the first ones first.
    int owed = totalLatency_ - latencyOf (order);
    std::array<bool, kNumPedals> delay {};
    for (const auto r : PedalOrder::everyPedal()) // in rank order
    {
        const auto i = index (r.id);
        if (owed > 0 && latency_[i] > 0 && !order.contains ({ r.id, 0 }) &&
            !order.contains ({ r.id, 1 }))
        {
            delay[i] = true;
            owed -= latency_[i];
        }
    }
    assert (owed == 0); // every drive and fuzz has the same latency (BoardTests)

    Layout layout;
    const auto delaysBefore = [&] (int rank) {
        for (const auto r : PedalOrder::everyPedal())
        {
            const auto i = index (r.id);
            if (pedalInfo (r.id).rank >= rank)
                break;
            if (delay[i])
            {
                layout.steps[layout.size++] = { r, false };
                delay[i] = false;
            }
        }
    };
    for (const auto r : order)
    {
        delaysBefore (pedalInfo (r.id).rank);
        layout.steps[layout.size++] = { r, true };
    }
    delaysBefore (std::numeric_limits<int>::max());
    return layout;
}

bool PedalRack::needsDuck (const PedalOrder& next) const noexcept
{
    // Pedals without latency that join or leave can fade in or out where they are, as long as
    // nothing else moves: compare the two chains without them.
    const auto without = [&] (const Layout& layout) {
        Layout kept;
        for (const auto step : layout)
        {
            const bool fades =
                latencyOf (step.ref) == 0 && order_.contains (step.ref) != next.contains (step.ref);
            if (!fades)
                kept.steps[kept.size++] = step;
        }
        return kept;
    };
    return !(without (layout_) == without (layoutOf (next)));
}

void PedalRack::rewire (const PedalOrder& order) noexcept
{
    // In front of the amp, pedals that stay keep their state: the signal faded out through them,
    // so what they hold is quiet, and a compressor that kept its envelope doesn't overshoot when
    // the signal returns. After the amp they restart with the joining ones: they are delay lines
    // and sweeps, whose lines still hold the last milliseconds before the duck, and rerouted
    // they would step (docs/board.md §4.3). Joining pedals start clean; leaving ones are
    // bypassed at once (their delay stays).
    const bool restart = side_ == Side::post;
    for (std::size_t slot = 0; slot < kNumSlots; ++slot)
    {
        const auto r = slotRef (slot);
        const bool joins = order.contains (r) && (restart || !order_.contains (r));
        const bool leaves = !order.contains (r) && order_.contains (r);
        if (!joins && !leaves)
            continue;
        switch (r.id)
        {
#define SEDEF_PEDAL(pid, name, category, rank, enable, Module, settings)                           \
    case PedalId::pid:                                                                             \
        if (joins)                                                                                 \
            slots_.pid[r.instance].reset();                                                        \
        else                                                                                       \
            slots_.pid[r.instance].fader.snap (false);                                             \
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

void PedalRack::adopt (const PedalOrder& order) noexcept
{
    assert (latencyOf (order) == latencyOf (order_));
    for (const auto r : order)
    {
        if (order_.contains (r))
            continue;
        switch (r.id)
        {
#define SEDEF_PEDAL(pid, name, category, rank, enable, Module, settings)                           \
    case PedalId::pid:                                                                             \
        slots_.pid[r.instance].join();                                                             \
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

bool PedalRack::fullyBypassed (PedalRef r) const noexcept
{
    switch (r.id)
    {
#define SEDEF_PEDAL(pid, name, category, rank, enable, Module, settings)                           \
    case PedalId::pid:                                                                             \
        return slots_.pid[r.instance].fader.isFullyBypassed();
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
bool PedalRack::run (PedalSlot<Module>& slot, const AudioView& io, bool enabled,
                     const typename Module::Settings& settings, int keyDelay) noexcept
{
    if constexpr (requires (Module& m, const AudioView& v, const float* k) { m.process (v, k); })
    {
        // A keyed pedal (the gate) listens to the instrument, lined up with the signal that
        // reaches it through the pedals before it. It only stands in front of the amp.
        assert (key_ != nullptr);
        const float* key = alignedKey (keyDelay, io.numFrames());
        return slot.process (io, enabled, settings,
                             [key] (Module& m, const AudioView& v) { m.process (v, key); });
    }
    else
    {
        return slot.process (io, enabled, settings,
                             [] (Module& m, const AudioView& v) { m.process (v); });
    }
}

void PedalRack::process (float* io, const float* key, int n, const PedalSettings& settings,
                         const std::array<bool, kNumSlots>& forcedOff) noexcept
{
    key_ = key;
    for (int i = 0; i < n; ++i)
        keyDelay_.push (key[i]);
    play (AudioView (io, n), settings, forcedOff);
}

void PedalRack::process (const AudioView& io, const PedalSettings& settings,
                         const std::array<bool, kNumSlots>& forcedOff) noexcept
{
    key_ = nullptr;
    play (io, settings, forcedOff);
}

void PedalRack::play (const AudioView& io, const PedalSettings& settings,
                      const std::array<bool, kNumSlots>& forcedOff) noexcept
{
    ran_.fill (false);
    int before = 0; // latency of the chain so far
    for (const auto step : layout_)
    {
        const auto r = step.ref;
        const auto i = slotIndex (r);
        // A delay left by a pedal off the board: its bypass, a pure delay.
        const bool enabled = step.onBoard && settings.on[i] && !forcedOff[i];
        switch (r.id)
        {
#define SEDEF_PEDAL(pid, name, category, rank, enable, Module, unused)                             \
    case PedalId::pid:                                                                             \
        ran_[i] = run (slots_.pid[r.instance], io, enabled, settings.pid[r.instance], before);     \
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
