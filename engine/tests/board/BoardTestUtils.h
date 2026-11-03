// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TestUtils.h"
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
    BoardRig (double fs, int blockSize, int frames, int ins = 1, int outs = 2)
        : rate (fs),
          block (blockSize),
          in (ins, frames),
          out (outs, frames)
    {
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

inline Board boardOf (std::initializer_list<PedalId> ids)
{
    Board b;
    for (const auto id : ids)
        b.insert (id, b.size());
    return b;
}

/// The board alone, as a chain plays it: duck, rack, duck, in chunks the switch allows.
struct BoardHarness
{
    BoardHarness (double fs, int maxBlockSize, const Board& start) : maxBlock (maxBlockSize)
    {
        rack.prepare ({ fs, maxBlock, 1 });
        board.prepare (fs, maxBlock, rack.latencySamples());
        rack.reset();
        board.reset (start, racks);
    }

    /// Processes x in place in host blocks of `block` frames (at most the prepared size), with
    /// `key` as the raw input. `beforeBlock (start)` runs before each host block.
    void run (float* x, const float* key, int frames, int block, const ParamSnapshot& p,
              const std::function<void (int)>& beforeBlock = {})
    {
        const auto settings = PedalSettings::from (p);
        for (int s = 0; s < frames; s += block)
        {
            if (beforeBlock)
                beforeBlock (s);
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

/// RMS of consecutive windows of `window` frames.
inline std::vector<float> windowedRms (const std::vector<float>& v, int window)
{
    std::vector<float> out;
    for (size_t s = 0; s + static_cast<size_t> (window) <= v.size();
         s += static_cast<size_t> (window))
        out.push_back (rms (v.data() + s, window));
    return out;
}

} // namespace sedef::test
