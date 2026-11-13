// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TestUtils.h"
#include "pedals/PedalTestUtils.h"
#include "sedef/Engine.h"
#include "sedef/board/Board.h"
#include "sedef/board/BoardSwitch.h"
#include "sedef/board/PedalRack.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace sedef::test
{

/// An engine with a mono input and stereo output, processed in host blocks of `block` frames.
struct BoardRig
{
    BoardRig (double fs, int blockSize, int frames, int ins = 1, int outs = 2,
              OversamplingMode mode = OversamplingMode::lowLatency)
        : rate (fs),
          block (blockSize),
          in (ins, frames),
          out (outs, frames)
    {
        engine.setOversampling (mode);
        engine.prepare (fs, blockSize, ins, outs);
    }

    /// Processes [from, from + frames). `beforeBlock (start)` runs before each host block.
    void run (const ParamSnapshot& p, int from, int frames,
              const std::function<void (int)>& beforeBlock = {})
    {
        for (int s = from; s < from + frames; s += block)
        {
            const int n = std::min (block, from + frames - s);
            if (beforeBlock)
                beforeBlock (s);
            std::array<const float*, 2> ip { in.ptrs[0] + s,
                                             in.channels() > 1 ? in.ptrs[1] + s : nullptr };
            std::array<float*, 2> op { out.ptrs[0] + s,
                                       out.channels() > 1 ? out.ptrs[1] + s : nullptr };
            engine.process (ip.data(), op.data(), n, p, transport);
        }
    }

    double rate;
    int block;
    Engine engine;
    Buffer in, out;
    Transport transport;
};

/// Everything off but the pedals asked for, through a clean amp: Lagoon at low volume.
inline ParamSnapshot cleanRig (std::initializer_list<ParamId> on = {})
{
    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;
    p[ParamId::ampModel] = 0.0f;
    p[ParamId::lagoonVolume] = 0.25f;
    p[ParamId::lagoonMaster] = 0.3f;
    for (const auto id : on)
        p[id] = 1.0f;
    return p;
}

/// One side's order, first copies (a Board in front of the amp, where a Board is wanted).
inline PedalOrder boardOf (std::initializer_list<PedalRef> ids)
{
    PedalOrder b;
    for (const auto id : ids)
        b.insert (id, b.size());
    return b;
}

/// The board alone, as a chain plays it: duck, rack, duck, in chunks the switch allows.
struct BoardHarness
{
    BoardHarness (double fs, int maxBlockSize, const PedalOrder& start,
                  OversamplingMode mode = OversamplingMode::lowLatency)
        : maxBlock (maxBlockSize)
    {
        rack.prepare ({ fs, maxBlock, 1, mode });
        board.prepare (fs, maxBlock, rack.latencySamples());
        rack.reset();
        board.reset (start, racks);
    }

    /// Processes x in place in host blocks of `block` frames (at most the prepared size), with
    /// `key` as the raw input. `beforeBlock (start)` runs before each host block.
    /// `p` is read again every host block, so `beforeBlock` may change it.
    void run (float* x, const float* key, int frames, int block, const ParamSnapshot& p,
              const std::function<void (int)>& beforeBlock = {})
    {
        for (int s = 0; s < frames; s += block)
        {
            if (beforeBlock)
                beforeBlock (s);
            const auto settings = PedalSettings::from (p);
            const int end = std::min (frames, s + block);
            for (int c = s; c < end;)
            {
                const int n = board.step (std::min (maxBlock, end - c), racks);
                const auto gain = board.duckGains (n);
                BoardSwitch::apply (x + c, gain.input, n);
                rack.process (x + c, key + c, n, settings, board.leaving());
                BoardSwitch::apply (x + c, gain.output, n);
                c += n;
            }
        }
    }

    int maxBlock;
    PedalRack rack;
    BoardSwitch board;
    std::array<PedalRack*, 1> racks { &rack };
};

/// Both sides of the amp as the engine plays them, without the amp: the mono rack in front, its
/// output copied to both channels, then the stereo rack after it, each side with its own switch
/// and duck (docs/board.md §4.3).
struct SidesHarness
{
    SidesHarness (double fs, int maxBlockSize, const Board& start,
                  OversamplingMode mode = OversamplingMode::lowLatency)
        : maxBlock (maxBlockSize)
    {
        pre.prepare ({ fs, maxBlock, 1, mode });
        post.prepare ({ fs, maxBlock, 2, mode }, Side::post);
        preSwitch.prepare (fs, maxBlock, pre.latencySamples());
        postSwitch.prepare (fs, maxBlock, post.latencySamples());
        pre.reset();
        post.reset();
        preSwitch.reset (start.pre, preRacks);
        postSwitch.reset (start.post, postRacks);
    }

    void setTarget (const Board& b) noexcept
    {
        preSwitch.setTarget (b.pre);
        postSwitch.setTarget (b.post);
    }

    /// Plays mono `x` into `left` and `right` in host blocks of `block` frames (at most the
    /// prepared size). `beforeBlock (start)` runs before each host block, and may change `p`.
    void run (const std::vector<float>& x, std::vector<float>& left, std::vector<float>& right,
              int block, const ParamSnapshot& p, const std::function<void (int)>& beforeBlock = {})
    {
        const int frames = static_cast<int> (x.size());
        left = x;
        right.assign (x.size(), 0.0f);
        for (int s = 0; s < frames; s += block)
        {
            if (beforeBlock)
                beforeBlock (s);
            const auto settings = PedalSettings::from (p);
            const int end = std::min (frames, s + block);
            for (int c = s; c < end;)
            {
                int n = preSwitch.step (std::min (maxBlock, end - c), preRacks);
                n = postSwitch.step (n, postRacks);
                const auto front = preSwitch.duckGains (n);
                const auto back = postSwitch.duckGains (n);
                float* l = left.data() + c;
                float* r = right.data() + c;
                BoardSwitch::apply (l, front.input, n);
                pre.process (l, x.data() + c, n, settings, preSwitch.leaving());
                BoardSwitch::apply (l, front.output, n);
                std::copy_n (l, n, r);
                std::array<float*, 2> ptrs { l, r };
                for (auto* ch : ptrs)
                    BoardSwitch::apply (ch, back.input, n);
                post.process (AudioView (ptrs.data(), 2, n), settings, postSwitch.leaving());
                for (auto* ch : ptrs)
                    BoardSwitch::apply (ch, back.output, n);
                c += n;
            }
        }
    }

    int maxBlock;
    PedalRack pre, post;
    BoardSwitch preSwitch, postSwitch;
    std::array<PedalRack*, 1> preRacks { &pre };
    std::array<PedalRack*, 1> postRacks { &post };
};

/// A board from the pedals in front of the amp and the pedals after it.
inline Board sides (std::initializer_list<PedalRef> front, std::initializer_list<PedalRef> back)
{
    Board b;
    for (const auto r : front)
        b.pre.insert (r, b.pre.size());
    for (const auto r : back)
        b.post.insert (r, b.post.size());
    return b;
}

/// RMS of consecutive windows of `window` frames.
inline std::vector<float> windowedRms (const std::vector<float>& v, int window)
{
    std::vector<float> out;
    for (size_t s = 0; s + static_cast<size_t> (window) <= v.size();
         s += static_cast<size_t> (window))
        out.push_back (rms (v.data() + s, window));
    return out;
}

/// How a change from board A to board B at frame `at` came across, against renders that played
/// A and B throughout (1 ms windows, over 80 ms from the change).
struct Transient
{
    float dipDb = 0.0f;       ///< deepest window against the same window without the change
    float dipMs = 0.0f;       ///< time spent more than 1 dB below it
    float swellDb = 0.0f;     ///< loudest window against the same window without the change
    float slopeRatio = 0.0f;  ///< largest second difference against the references'
    float excessDb = -200.0f; ///< second difference above the references', against their peak
};

inline Transient compareTransient (const std::vector<float>& y, const std::vector<float>& refA,
                                   const std::vector<float>& refB, double fs, int at)
{
    const int ms1 = static_cast<int> (fs / 1000.0);
    const int span = static_cast<int> (0.08 * fs);
    Transient t;
    float deepest = 1.0f, loudest = 0.0f;
    for (int s = at - 10 * ms1; s + ms1 <= at + span; s += ms1)
    {
        const auto& ref = s < at ? refA : refB;
        const float ratio = rms (y.data() + s, ms1) / std::max (rms (ref.data() + s, ms1), 1e-9f);
        deepest = std::min (deepest, ratio);
        loudest = std::max (loudest, ratio);
        t.dipMs += ratio < 0.891f ? 1.0f : 0.0f;
    }
    t.dipDb = static_cast<float> (db (deepest));
    t.swellDb = static_cast<float> (db (loudest));

    const auto window = [&] (const std::vector<float>& v) {
        return std::vector<float> (v.begin() + at - 2, v.begin() + at + span);
    };
    const float slope = maxSecondDifference (window (y));
    const float refSlope =
        std::max (maxSecondDifference (window (refA)), maxSecondDifference (window (refB)));
    t.slopeRatio = slope / refSlope;
    if (slope > refSlope)
        t.excessDb = static_cast<float> (
            db ((slope - refSlope) / std::max (peak (window (refA)), peak (window (refB)))));
    return t;
}

inline std::vector<float> testSignal (bool guitar, double fs, int frames)
{
    std::vector<float> x (static_cast<size_t> (frames));
    if (guitar)
        fillGuitarTone (x, fs, 0.3f);
    else
        fillSine (x, 110.0, fs, 0.25f);
    return x;
}

/// The board alone playing `x`, starting on `from`; at frame `at` (a block boundary) it is asked
/// for `to`, when given.
inline std::vector<float> renderBoard (double fs, int block, const std::vector<float>& x,
                                       const ParamSnapshot& p, const PedalOrder& from,
                                       const PedalOrder* to = nullptr, int at = 0,
                                       const std::function<void (BoardHarness&, int)>& watch = {},
                                       OversamplingMode mode = OversamplingMode::lowLatency)
{
    BoardHarness h (fs, std::min (block, 512), from, mode);
    auto y = x;
    h.run (y.data(), x.data(), static_cast<int> (x.size()), block, p, [&] (int s) {
        if (to != nullptr && s == at)
            h.board.setTarget (*to);
        if (watch)
            watch (h, s);
    });
    return y;
}

/// The same through the whole engine (amp, cab and post effects after the board).
inline std::vector<float> renderEngine (double fs, int block, const std::vector<float>& x,
                                        const ParamSnapshot& p, const Board& from,
                                        const Board* to = nullptr, int at = 0,
                                        OversamplingMode mode = OversamplingMode::lowLatency)
{
    BoardRig rig (fs, block, static_cast<int> (x.size()), 1, 2, mode);
    rig.in[0] = x;
    rig.engine.setBoard (from);
    rig.engine.reset();
    rig.run (p, 0, static_cast<int> (x.size()), [&] (int s) {
        if (to != nullptr && s == at)
            rig.engine.setBoard (*to);
    });
    return rig.out[0];
}

inline std::vector<float> renderEngine (double fs, int block, const std::vector<float>& x,
                                        const ParamSnapshot& p, const PedalOrder& from,
                                        const PedalOrder* to, int at = 0,
                                        OversamplingMode mode = OversamplingMode::lowLatency)
{
    const Board next = to != nullptr ? Board (*to) : Board();
    return renderEngine (fs, block, x, p, Board (from), to != nullptr ? &next : nullptr, at, mode);
}

} // namespace sedef::test
