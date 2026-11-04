// SPDX-License-Identifier: AGPL-3.0-or-later
// The measurements behind the board's numbers in docs/architecture.md. Hidden; run with
//   sedef_engine_tests "[.board-measure]"
#include "board/BoardTestUtils.h"
#include "sedef/board/PedalRack.h"
#include "sedef/core/Denormals.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>

using namespace sedef;
using namespace sedef::test;

namespace
{
const Board kBoardA = Board::legacy();
const Board kBoardB =
    boardOf ({ PedalId::gate, PedalId::pearl, PedalId::tide, PedalId::coral, PedalId::ripple });

ParamSnapshot tideAndPearl()
{
    auto p = cleanRig ({ ParamId::tideEnabled, ParamId::pearlEnabled });
    p[ParamId::pearlGain] = 0.3f;
    return p;
}

void print (const char* what, bool guitar, double fs, int block, const Transient& t)
{
    std::printf ("%-14s %-6s %6.0f Hz block %4d: dip %6.1f dB, %4.1f ms below -1 dB, swell "
                 "%+.1f dB, slope x%.2f (excess %.0f dB)\n",
                 what, guitar ? "guitar" : "sine", fs, block, static_cast<double> (t.dipDb),
                 static_cast<double> (t.dipMs), static_cast<double> (t.swellDb),
                 static_cast<double> (t.slopeRatio), static_cast<double> (t.excessDb));
}
} // namespace

TEST_CASE ("Board reorder transient (measurement)", "[.board-measure]")
{
    const auto p = tideAndPearl();
    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const int block : { 1, 64, 512, 4096 })
            for (const bool guitar : { false, true })
            {
                const auto x = testSignal (guitar, fs, static_cast<int> (1.6 * fs));
                const int at = (static_cast<int> (fs) / block) * block;
                print ("board reorder", guitar, fs, block,
                       compareTransient (renderBoard (fs, block, x, p, kBoardA, &kBoardB, at),
                                         renderBoard (fs, block, x, p, kBoardA),
                                         renderBoard (fs, block, x, p, kBoardB), fs, at));
                print ("rig reorder", guitar, fs, block,
                       compareTransient (renderEngine (fs, block, x, p, kBoardA, &kBoardB, at),
                                         renderEngine (fs, block, x, p, kBoardA),
                                         renderEngine (fs, block, x, p, kBoardB), fs, at));
            }

    // Pearl (latency) leaves while switched off: the duck alone, nothing else changes.
    for (const double fs : { 48000.0, 96000.0 })
        for (const bool guitar : { false, true })
        {
            const auto p2 = cleanRig ({ ParamId::tideEnabled });
            auto without = kBoardA;
            without.remove (PedalId::pearl);
            const auto x = testSignal (guitar, fs, static_cast<int> (1.6 * fs));
            const int at = (static_cast<int> (fs) / 64) * 64;
            const auto a = renderBoard (fs, 64, x, p2, kBoardA);
            print ("duck only", guitar, fs, 64,
                   compareTransient (renderBoard (fs, 64, x, p2, kBoardA, &without, at), a, a, fs,
                                     at));
        }

    // Control: the same swap with no duck (the rack rewired mid-signal) shows what the measures
    // catch.
    const double fs = 48000.0;
    const auto x = testSignal (false, fs, static_cast<int> (1.6 * fs));
    const auto rackRender = [&] (const Board& from, const Board* to, int at) {
        PedalRack rack;
        rack.prepare ({ fs, 64, 1 });
        rack.rewire (from);
        rack.reset();
        auto y = x;
        const auto settings = PedalSettings::from (p);
        const std::array<bool, kNumPedals> none {};
        const int frames = static_cast<int> (y.size());
        for (int s = 0; s < frames; s += 64)
        {
            if (to != nullptr && s == at)
                rack.adopt (*to);
            rack.process (y.data() + s, x.data() + s, std::min (64, frames - s), settings, none);
        }
        return y;
    };
    print ("hard swap", false, fs, 64,
           compareTransient (rackRender (kBoardA, &kBoardB, 48000),
                             rackRender (kBoardA, nullptr, 0), rackRender (kBoardB, nullptr, 0), fs,
                             48000));
}

TEST_CASE ("Board CPU (measurement)", "[.board-measure]")
{
    ScopedNoDenormals noDenormals; // as Engine::process
    const double fs = 48000.0;
    const int frames = static_cast<int> (10.0 * fs);
    const auto x = testSignal (true, fs, frames);
    const auto time = [&] (const Board& board, const ParamSnapshot& p) {
        BoardHarness h (fs, 64, board);
        auto y = x;
        const auto start = std::chrono::steady_clock::now();
        h.run (y.data(), x.data(), frames, 64, p);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return std::chrono::duration<double, std::nano> (elapsed).count() / frames;
    };
    // Every pedal switched on; what the board holds decides what runs.
    auto on = cleanRig();
    for (std::size_t i = 0; i < kNumPedals; ++i)
        on[kPedals[i].enable] = 1.0f;
    std::printf ("board CPU at 48 kHz, ns per sample: every pedal on the board and on %.1f; "
                 "Tide alone on the board %.1f; nothing on the board %.1f; every pedal on the "
                 "board, switched off %.1f\n",
                 time (Board::everyPedal(), on), time (boardOf ({ PedalId::tide }), on),
                 time (Board {}, on), time (Board::everyPedal(), cleanRig()));
}
