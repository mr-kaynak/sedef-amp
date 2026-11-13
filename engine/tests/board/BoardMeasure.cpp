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
const PedalOrder kBoardA = PedalOrder::legacy();
const PedalOrder kBoardB =
    boardOf ({ PedalId::gate, PedalId::pearl, PedalId::tide, PedalId::coral, PedalId::ripple });

ParamSnapshot tideAndPearl()
{
    auto p = cleanRig ({ ParamId::tideEnabled, ParamId::pearlEnabled });
    p[ParamId::pearlGain] = 0.3f;
    return p;
}

void print (const char* what, OversamplingMode mode, bool guitar, double fs, int block,
            const Transient& t)
{
    std::printf ("%-14s %-6s %-6s %6.0f Hz block %4d: dip %6.1f dB, %4.1f ms below -1 dB, swell "
                 "%+.1f dB, slope x%.2f (excess %.0f dB)\n",
                 what, mode == OversamplingMode::linearPhase ? "linear" : "low",
                 guitar ? "guitar" : "sine", fs, block, static_cast<double> (t.dipDb),
                 static_cast<double> (t.dipMs), static_cast<double> (t.swellDb),
                 static_cast<double> (t.slopeRatio), static_cast<double> (t.excessDb));
}
} // namespace

TEST_CASE ("Board reorder transient (measurement)", "[.board-measure]")
{
    const auto p = tideAndPearl();
    for (const auto mode : { OversamplingMode::lowLatency, OversamplingMode::linearPhase })
        for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
            for (const int block : { 1, 64, 512, 4096 })
                for (const bool guitar : { false, true })
                {
                    const auto x = testSignal (guitar, fs, static_cast<int> (1.6 * fs));
                    const int at = (static_cast<int> (fs) / block) * block;
                    const auto board = [&] (const PedalOrder& from, const PedalOrder* to) {
                        return renderBoard (fs, block, x, p, from, to, at, {}, mode);
                    };
                    const auto rig = [&] (const PedalOrder& from, const PedalOrder* to) {
                        return renderEngine (fs, block, x, p, from, to, at, mode);
                    };
                    print ("board reorder", mode, guitar, fs, block,
                           compareTransient (board (kBoardA, &kBoardB), board (kBoardA, nullptr),
                                             board (kBoardB, nullptr), fs, at));
                    print ("rig reorder", mode, guitar, fs, block,
                           compareTransient (rig (kBoardA, &kBoardB), rig (kBoardA, nullptr),
                                             rig (kBoardB, nullptr), fs, at));
                }

    // Pearl (latency) leaves while switched off: the duck alone, nothing else changes.
    for (const auto mode : { OversamplingMode::lowLatency, OversamplingMode::linearPhase })
        for (const double fs : { 48000.0, 96000.0 })
            for (const bool guitar : { false, true })
            {
                const auto p2 = cleanRig ({ ParamId::tideEnabled });
                auto without = kBoardA;
                without.remove (PedalId::pearl);
                const auto x = testSignal (guitar, fs, static_cast<int> (1.6 * fs));
                const int at = (static_cast<int> (fs) / 64) * 64;
                const auto a = renderBoard (fs, 64, x, p2, kBoardA, nullptr, 0, {}, mode);
                print (
                    "duck only", mode, guitar, fs, 64,
                    compareTransient (renderBoard (fs, 64, x, p2, kBoardA, &without, at, {}, mode),
                                      a, a, fs, at));
            }

    // Control: the same swap with no duck (the rack rewired mid-signal) shows what the measures
    // catch.
    const double fs = 48000.0;
    const auto x = testSignal (false, fs, static_cast<int> (1.6 * fs));
    const auto rackRender = [&] (const PedalOrder& from, const PedalOrder* to, int at) {
        PedalRack rack;
        rack.prepare ({ fs, 64, 1 });
        rack.rewire (from);
        rack.reset();
        auto y = x;
        const auto settings = PedalSettings::from (p);
        const std::array<bool, kNumSlots> none {};
        const int frames = static_cast<int> (y.size());
        for (int s = 0; s < frames; s += 64)
        {
            if (to != nullptr && s == at)
                rack.adopt (*to);
            rack.process (y.data() + s, x.data() + s, std::min (64, frames - s), settings, none);
        }
        return y;
    };
    print ("hard swap", OversamplingMode::lowLatency, false, fs, 64,
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
    const auto time = [&] (const PedalOrder& board, const ParamSnapshot& p) {
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
                 time (PedalOrder::everyPedal(), on), time (boardOf ({ PedalId::tide }), on),
                 time (PedalOrder {}, on), time (PedalOrder::everyPedal(), cleanRig()));
}

TEST_CASE ("Full board CPU through the rig (measurement)", "[.board-measure]")
{
    // The whole engine, 10 s at 48 kHz in 64-frame blocks, Low latency: the rig with nothing on
    // the board, 0.10's largest board (every pedal once, in front of the amp), and the largest
    // board there is (25: both copies of the nine pedals without latency, the seven drives and
    // fuzzes once), all in front of the amp and with the eight modulation copies after it. Every
    // copy on. In Summed mode (one input) and in Dual mode (two inputs, two chains in front).
    ScopedNoDenormals noDenormals; // as Engine::process
    const double fs = 48000.0;
    const int block = 64;
    const int frames = static_cast<int> (10.0 * fs);
    const auto x = testSignal (true, fs, frames);

    Board largest = Board::switchedOn ([] (ParamId) { return true; });
    REQUIRE (largest.size() == 25);
    Board split;
    for (const auto r : largest.pre)
        split.side (fitsAfterAmp (r.id) ? Side::post : Side::pre)
            .insert (r, split.side (fitsAfterAmp (r.id) ? Side::post : Side::pre).size());
    REQUIRE (split.post.size() == 8);

    auto every = cleanRig();
    for (std::size_t slot = 0; slot < kNumSlots; ++slot)
        every[enableOf (slotRef (slot))] = 1.0f;

    const auto time = [&] (const Board& board, bool dual) {
        BoardRig rig (fs, block, frames, dual ? 2 : 1, 2);
        rig.in[0] = x;
        if (dual)
            rig.in[1] = x;
        auto p = every;
        p[ParamId::stereoMode] = dual ? 1.0f : 0.0f;
        rig.engine.setBoard (board);
        rig.engine.reset();
        rig.run (p, 0, block); // the first block applies the board and the mode
        const auto start = std::chrono::steady_clock::now();
        rig.run (p, block, frames - block);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return std::chrono::duration<double, std::nano> (elapsed).count() / (frames - block);
    };
    const struct
    {
        const char* name;
        Board board;
    } rows[] = {
        { "nothing on the board", Board {} },
        { "every pedal once, in front (0.10's largest)", Board::everyPedal() },
        { "25, all in front of the amp", largest },
        { "25, the 8 modulation copies after the amp", split },
    };
    std::printf ("rig CPU at 48 kHz, ns per sample (%% of one core in real time):\n");
    for (const auto& row : rows)
    {
        const double summed = time (row.board, false);
        const double dual = time (row.board, true);
        std::printf ("  %-46s Summed %7.1f (%4.1f%%)  Dual %7.1f (%4.1f%%)\n", row.name, summed,
                     100.0 * summed / (1e9 / fs), dual, 100.0 * dual / (1e9 / fs));
    }
}
