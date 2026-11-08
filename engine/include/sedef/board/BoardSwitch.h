// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/board/Board.h"
#include "sedef/board/Duck.h"
#include "sedef/board/PedalRack.h"

#include <array>
#include <span>
#include <vector>

namespace sedef
{

/// Takes the racks of every chain from the order they play to the newest board, at block
/// boundaries, without clicks:
///
/// - Pedals without latency join and leave through their own bypass fades. A leaving pedal is
///   switched off and leaves the order once its fade has ended; a joining one comes in bypassed
///   and fades in when it is on.
/// - Anything else (pedals that stay change their order, a pedal with latency joins or leaves)
///   happens at the bottom of a duck: the board's input fades to silence, its output follows
///   once the signal has left the pedals, the racks rewire, and both fade back in (Duck).
///
/// Audio thread, except prepare() and reset().
class BoardSwitch
{
public:
    /// Each fade of the duck.
    static constexpr double kDuckSeconds = 0.006;

    /// `latency`: the racks' (PedalRack::latencySamples()), how long the signal takes to leave.
    void prepare (double sampleRate, int maxBlockSize, int latency);
    /// Rewires the racks to `target` at once (audio stopped, or after a reset).
    void reset (const Board& target, std::span<PedalRack* const> racks) noexcept;

    void setTarget (const Board& target) noexcept { target_ = target; }
    const Board& target() const noexcept { return target_; }

    /// At a block boundary: moves the racks on, and returns how many of the next `n` frames to
    /// process before calling again (a fade to silence ends exactly on a boundary).
    int step (int n, std::span<PedalRack* const> racks) noexcept;
    struct Gains
    {
        const float* input = nullptr;  ///< for the board's input, null at unity
        const float* output = nullptr; ///< for its output, null at unity
    };
    /// The duck's gains for the n frames that follow step().
    Gains duckGains (int n) noexcept;

    /// Pedals on their way off the board: process them switched off.
    const std::array<bool, kNumPedals>& leaving() const noexcept { return leaving_; }
    /// A change is still on its way in.
    bool changing (const PedalRack& rack) const noexcept
    {
        return rack.order() != target_ || !duck_.idle();
    }
    const Duck& duck() const noexcept { return duck_; }

    /// x *= gain, when ducking.
    static void apply (float* x, const float* gain, int n) noexcept
    {
        if (gain != nullptr)
            for (int i = 0; i < n; ++i)
                x[i] *= gain[i];
    }

private:
    Board target_ = Board::legacy();
    std::array<bool, kNumPedals> leaving_ {};
    Duck duck_;
    std::vector<float> inputGains_, outputGains_;
};

} // namespace sedef
