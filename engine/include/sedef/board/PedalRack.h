// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/board/Board.h"
#include "sedef/core/AudioView.h"
#include "sedef/core/BypassFader.h"
#include "sedef/core/DelayLine.h"
#include "sedef/params/Parameters.h"

// Every module named in PedalList.inc.
#include "sedef/dsp/Breaker.h"
#include "sedef/dsp/Coral.h"
#include "sedef/dsp/Kelp.h"
#include "sedef/dsp/NoiseGate.h"
#include "sedef/dsp/Pearl.h"
#include "sedef/dsp/Remora.h"
#include "sedef/dsp/Ripple.h"
#include "sedef/dsp/Sargasso.h"
#include "sedef/dsp/Tide.h"
#include "sedef/dsp/Trench.h"
#include "sedef/dsp/Urchin.h"

#include <algorithm>
#include <array>
#include <vector>

namespace sedef
{

/// One pedal in the rack: the module, its click-free bypass, and a dry path delayed by the
/// module's latency, so switching the pedal never changes the chain's timing.
template <typename Module>
struct PedalSlot
{
    Module module;
    BypassFader fader;
    DelayLine dryDelay;
    std::vector<float> dry;
    int latency = 0;
    bool snapOnNextBlock = true; // first block after prepare/reset starts in its final state

    void prepare (const ProcessSpec& spec)
    {
        module.prepare (spec);
        if constexpr (requires { module.latencySamples(); })
            latency = module.latencySamples();
        dryDelay.prepare (std::max (latency, 1));
        dry.assign (static_cast<size_t> (spec.maxBlockSize), 0.0f);
        fader.prepare (spec.sampleRate);
    }

    void reset() noexcept
    {
        module.reset();
        dryDelay.reset();
        snapOnNextBlock = true;
    }

    /// Joins the board without a duck: from a clean state and bypassed, so a pedal that is on
    /// fades in like a footswitch press.
    void join() noexcept
    {
        module.reset();
        dryDelay.reset();
        fader.snap (false);
        snapOnNextBlock = false;
    }

    /// Returns true when the module ran (so its telemetry is meaningful).
    template <typename Settings, typename Run>
    bool process (float* io, int n, bool enabled, const Settings& settings, Run&& run) noexcept
    {
        // The first block after prepare/reset counts as coming out of bypass, so modules whose
        // reset() depends on their settings (the gate) get settings, reset, settings.
        const bool wasBypassed = snapOnNextBlock || fader.isFullyBypassed();
        if (snapOnNextBlock)
        {
            fader.snap (enabled);
            snapOnNextBlock = false;
        }
        fader.setEnabled (enabled);

        // The dry path is only needed while bypassed or crossfading, but a latency-bearing module
        // keeps its delay line warm so a later bypass lines up from the first sample.
        const bool needDry = !fader.isFullyActive();
        if (latency > 0)
        {
            for (int i = 0; i < n; ++i)
            {
                dryDelay.push (io[i]);
                if (needDry)
                    dry[static_cast<size_t> (i)] = dryDelay.read (latency);
            }
        }
        else if (needDry)
        {
            std::copy_n (io, n, dry.data());
        }

        if (fader.isFullyBypassed())
        {
            if (latency > 0)
                std::copy_n (dry.data(), n, io);
            return false; // settings wait until the module runs again
        }
        // Coming back from bypass, the module restarts from a clean state that lands directly on
        // the current settings: reset() re-arms each module's "first settings snap" and the
        // settings that follow consume it. The settings also go in once before reset() for
        // modules whose reset reads them (the gate starts open or closed by its threshold).
        if (wasBypassed)
        {
            module.setSettings (settings);
            module.reset();
        }
        module.setSettings (settings);

        run (module, AudioView (io, n));
        if (!fader.isFullyActive())
            fader.mix (AudioView (dry.data(), n), AudioView (io, n));
        return true;
    }
};

/// Every pedal's settings and on/off switch, derived from the snapshot once per block, whether
/// or not the pedal is on the board.
struct PedalSettings
{
    std::array<bool, kNumPedals> on {};
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings) Module::Settings id {};
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL

    static PedalSettings from (const ParamSnapshot& p) noexcept;
};

/// Every pedal there is, prepared once, played in the order of the board. Mono.
///
/// Pedals off the board are not processed at all. A pad delay stands in for their latency, so
/// the rack's latency is always that of every pedal together and never changes with the board.
/// The rack's owner (Engine) decides when the order may change: rewire() restarts the pedals
/// (at the bottom of a duck), adopt() takes in pedals without latency as they are.
class PedalRack
{
public:
    void prepare (const ProcessSpec& mono);
    /// Clears every pedal, the pad and the key; the order stays.
    void reset() noexcept;

    /// Every pedal's latency together: what the rack always delays by.
    int latencySamples() const noexcept { return totalLatency_; }
    int latencyOf (PedalId id) const noexcept { return latency_[index (id)]; }
    /// Latency of the pedals on `board` together.
    int latencyOf (const Board& board) const noexcept;

    const Board& order() const noexcept { return order_; }
    /// True when going from the current order to `next` needs the signal ducked: pedals that
    /// stay on the board change their relative order, or a pedal with latency joins or leaves.
    /// Anything else is bypass fades (adopt()).
    bool needsDuck (const Board& next) const noexcept;
    /// A new order at silence: joining pedals start from a clean state (the next block snaps
    /// each to its switch), the others keep theirs, and the pad takes up the latency of the
    /// pedals off the board.
    void rewire (const Board& order) noexcept;
    /// A new order that only adds or drops pedals without latency, keeping the others' order.
    /// Joining pedals start bypassed and fade in when on; leaving ones must already be fully
    /// bypassed (the owner forces them off first).
    void adopt (const Board& order) noexcept;

    /// Processes io in place through the board. `key` is the raw instrument input (the gate's
    /// detector), n frames. `forcedOff` bypasses pedals on their way off the board.
    void process (float* io, const float* key, int n, const PedalSettings& settings,
                  const std::array<bool, kNumPedals>& forcedOff) noexcept;

    bool fullyBypassed (PedalId id) const noexcept;
    /// The pedal's module processed audio in the last block (telemetry is meaningful).
    bool ran (PedalId id) const noexcept { return ran_[index (id)]; }

    struct Slots
    {
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings) PedalSlot<Module> id;
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
    };
    const Slots& slots() const noexcept { return slots_; }

private:
    template <typename Module>
    bool run (PedalSlot<Module>& slot, float* io, int n, bool enabled,
              const typename Module::Settings& settings, int keyDelay) noexcept;
    const float* alignedKey (int delay, int n) noexcept;

    Slots slots_;
    std::array<int, kNumPedals> latency_ {};
    std::array<bool, kNumPedals> ran_ {};
    int totalLatency_ = 0;
    Board order_ = Board::legacy();

    DelayLine pad_; // the latency of the pedals off the board
    int padLength_ = 0;
    DelayLine keyDelay_; // the raw input, delayed to where a keyed pedal sits
    const float* key_ = nullptr;
    std::vector<float> keyed_;
};

} // namespace sedef
