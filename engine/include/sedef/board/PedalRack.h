// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/board/Board.h"
#include "sedef/core/AudioView.h"
#include "sedef/core/BypassFader.h"
#include "sedef/core/DelayLine.h"
#include "sedef/params/Parameters.h"

// Every module named in PedalList.inc.
#include "sedef/dsp/Anemone.h"
#include "sedef/dsp/Beacon.h"
#include "sedef/dsp/Breaker.h"
#include "sedef/dsp/Conch.h"
#include "sedef/dsp/Coral.h"
#include "sedef/dsp/Gyre.h"
#include "sedef/dsp/Jetty.h"
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

/// One copy of a pedal in a rack: the module, its click-free bypass, and a dry path delayed by the
/// module's latency, so switching the pedal never changes the chain's timing. One channel in
/// front of the amp, two after it.
template <typename Module>
struct PedalSlot
{
    Module module;
    BypassFader fader;
    std::array<DelayLine, kMaxChannels> dryDelay;
    std::array<std::vector<float>, kMaxChannels> dry;
    int latency = 0;
    int channels = 0;            // 0: never prepared (a pedal the rack can't hold)
    bool snapOnNextBlock = true; // first block after prepare/reset starts in its final state

    void prepare (const ProcessSpec& spec)
    {
        module.prepare (spec);
        if constexpr (requires { module.latencySamples(); })
            latency = module.latencySamples();
        channels = std::clamp (spec.numChannels, 1, kMaxChannels);
        for (int c = 0; c < channels; ++c)
        {
            dryDelay[static_cast<size_t> (c)].prepare (std::max (latency, 1));
            dry[static_cast<size_t> (c)].assign (static_cast<size_t> (spec.maxBlockSize), 0.0f);
        }
        fader.prepare (spec.sampleRate);
    }

    bool prepared() const noexcept { return channels > 0; }

    void reset() noexcept
    {
        if (!prepared())
            return;
        module.reset();
        for (int c = 0; c < channels; ++c)
            dryDelay[static_cast<size_t> (c)].reset();
        snapOnNextBlock = true;
    }

    /// Joins the board without a duck: from a clean state and bypassed, so a pedal that is on
    /// fades in like a footswitch press.
    void join() noexcept
    {
        module.reset();
        for (int c = 0; c < channels; ++c)
            dryDelay[static_cast<size_t> (c)].reset();
        fader.snap (false);
        snapOnNextBlock = false;
    }

    /// Returns true when the module ran (so its telemetry is meaningful).
    template <typename Settings, typename Run>
    bool process (const AudioView& io, bool enabled, const Settings& settings, Run&& run) noexcept
    {
        const int n = io.numFrames();
        const int numChannels = std::min (io.numChannels(), channels);
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
        for (int c = 0; c < numChannels; ++c)
        {
            float* x = io.channel (c);
            auto& line = dryDelay[static_cast<size_t> (c)];
            float* d = dry[static_cast<size_t> (c)].data();
            if (latency > 0)
            {
                for (int i = 0; i < n; ++i)
                {
                    line.push (x[i]);
                    if (needDry)
                        d[i] = line.read (latency);
                }
            }
            else if (needDry)
            {
                std::copy_n (x, n, d);
            }
        }

        if (fader.isFullyBypassed())
        {
            if (latency > 0)
                for (int c = 0; c < numChannels; ++c)
                    std::copy_n (dry[static_cast<size_t> (c)].data(), n, io.channel (c));
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

        const auto view = io.withChannels (numChannels);
        run (module, view);
        if (!fader.isFullyActive())
        {
            // One channel or both, spelt out: a view built with a channel count only known at run
            // time makes GCC 13 warn of reading past the pointers (-Wmaybe-uninitialized).
            static_assert (kMaxChannels == 2);
            if (numChannels > 1)
            {
                std::array<float*, kMaxChannels> dryPtrs { dry[0].data(), dry[1].data() };
                fader.mix (AudioView (dryPtrs.data(), kMaxChannels, n), view);
            }
            else
            {
                fader.mix (AudioView (dry[0].data(), n), view);
            }
        }
        return true;
    }
};

/// Every copy's settings and on/off switch, derived from the snapshot once per block, whether or
/// not it is on the board. The second copies read their own parameters (Board.h, kSecondCopy).
struct PedalSettings
{
    std::array<bool, kNumSlots> on {}; ///< by slotIndex()
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings)                            \
    std::array<Module::Settings, kCopies> id {};
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL

    static PedalSettings from (const ParamSnapshot& p) noexcept;
};

/// Every pedal there is, prepared once, played in the order of one side of the board.
///
/// In front of the amp (mono) the rack holds both copies of every pedal. Pedals off the board are
/// not processed. A drive or fuzz with no copy on the board leaves its bypass delay in the chain
/// (a pure delay, no processing), where the fixed chain had it: before the first pedal on the
/// board that ranks after it, or at the end; each second copy of a drive or fuzz on the board
/// takes one such delay away, the last in default order first. So the rack's latency is always
/// that of every drive and fuzz once and never changes with the board, and a board in default
/// order plays exactly what the fixed chain played with the other pedals switched off.
///
/// After the amp (stereo) the rack holds only the pedals that fit there (fitsAfterAmp), which
/// have no latency.
///
/// The rack's owner (BoardSwitch) decides when the order may change: rewire() restarts the
/// pedals (at the bottom of a duck), adopt() takes in pedals without latency as they are.
class PedalRack
{
public:
    /// `spec.numChannels` is 1 in front of the amp and 2 after it, where only the pedals that fit
    /// there are prepared.
    void prepare (const ProcessSpec& spec, Side side = Side::pre);
    /// Clears every pedal and the key; the order stays.
    void reset() noexcept;

    Side side() const noexcept { return side_; }

    /// Every drive and fuzz's latency once: what the rack always delays by.
    int latencySamples() const noexcept { return totalLatency_; }
    int latencyOf (PedalRef r) const noexcept { return latency_[slotIndex (r)]; }
    /// Latency of the pedals in `order` together.
    int latencyOf (const PedalOrder& order) const noexcept;

    const PedalOrder& order() const noexcept { return order_; }

    /// One step of the chain the rack plays: a pedal on the board, or the delay a drive or fuzz
    /// leaves behind when it is off the board.
    struct Step
    {
        PedalRef ref {};
        bool onBoard = false;
        bool operator== (const Step&) const = default;
    };
    struct Layout
    {
        std::array<Step, kNumSlots + kNumPedals> steps {};
        std::size_t size = 0;
        const Step* begin() const noexcept { return steps.data(); }
        const Step* end() const noexcept { return steps.data() + size; }
        bool operator== (const Layout& o) const noexcept
        {
            return size == o.size && std::equal (begin(), end(), o.begin());
        }
    };
    /// The chain `order` plays: its pedals in order, with the delays of the drives and fuzzes
    /// that are off it (see the class comment).
    Layout layoutOf (const PedalOrder& order) const noexcept;
    const Layout& layout() const noexcept { return layout_; }

    /// True when going from the current order to `next` needs the signal ducked: anything that
    /// moves a pedal or a delay in the chain (pedals that stay change their relative order, a
    /// pedal with latency joins or leaves). Pedals without latency joining or leaving where
    /// nothing else moves are bypass fades (adopt()).
    bool needsDuck (const PedalOrder& next) const noexcept;
    /// A new order at silence: joining pedals start from a clean state (the next block snaps
    /// each to its switch), leaving ones turn into their delay at once. In front of the amp the
    /// others keep their state; after it they restart too (docs/board.md §4.3).
    void rewire (const PedalOrder& order) noexcept;
    /// A new order that only adds or drops pedals without latency, keeping the others' order.
    /// Joining pedals start bypassed and fade in when on; leaving ones must already be fully
    /// bypassed (the owner forces them off first).
    void adopt (const PedalOrder& order) noexcept;

    /// In front of the amp: processes io in place. `key` is the raw instrument input (the gate's
    /// detector), n frames. `forcedOff` bypasses pedals on their way off the board.
    void process (float* io, const float* key, int n, const PedalSettings& settings,
                  const std::array<bool, kNumSlots>& forcedOff) noexcept;
    /// After the amp: processes the stereo io in place.
    void process (const AudioView& io, const PedalSettings& settings,
                  const std::array<bool, kNumSlots>& forcedOff) noexcept;

    bool fullyBypassed (PedalRef r) const noexcept;
    /// The copy's module processed audio in the last block (telemetry is meaningful).
    bool ran (PedalRef r) const noexcept { return ran_[slotIndex (r)]; }

    struct Slots
    {
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, settings)                            \
    std::array<PedalSlot<Module>, kCopies> id;
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
    };
    const Slots& slots() const noexcept { return slots_; }

private:
    template <typename Module>
    bool run (PedalSlot<Module>& slot, const AudioView& io, bool enabled,
              const typename Module::Settings& settings, int keyDelay) noexcept;
    const float* alignedKey (int delay, int n) noexcept;
    void play (const AudioView& io, const PedalSettings& settings,
               const std::array<bool, kNumSlots>& forcedOff) noexcept;

    Side side_ = Side::pre;
    Slots slots_;
    std::array<int, kNumSlots> latency_ {};
    std::array<bool, kNumSlots> ran_ {};
    int totalLatency_ = 0;
    PedalOrder order_ = PedalOrder::legacy();

    Layout layout_;
    DelayLine keyDelay_; // the raw input, delayed to where a keyed pedal sits
    const float* key_ = nullptr;
    std::vector<float> keyed_;
};

} // namespace sedef
