// SPDX-License-Identifier: AGPL-3.0-or-later
// Measurements behind the board's numbers in docs/architecture.md (hidden: run with
// sedef_engine_tests "[.board-measure]").
#include "board/BoardTestUtils.h"
#include "pedals/PedalTestUtils.h"
#include "sedef/board/PedalRack.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>

using namespace sedef;
using namespace sedef::test;

namespace
{
struct Transient
{
    float dipDb = 0.0f;       // deepest 1 ms window against the same window without the change
    float dipMs = 0.0f;       // time spent more than 1 dB below it
    float slopeRatio = 0.0f;  // largest second difference against the references'
    float swellDb = 0.0f;     // loudest 1 ms window against the same window without the change
    float worstAtMs = 0.0f;   // where the largest second difference is, from the change
    float excessDb = -200.0f; // second difference above the references', against their peak
};

/// `y` switched from board A to board B at `at`; refA and refB played A and B throughout.
Transient compare (const std::vector<float>& y, const std::vector<float>& refA,
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
    {
        const auto w = window (y);
        float m = 0.0f;
        for (size_t i = 2; i < w.size(); ++i)
        {
            const float d = std::abs (w[i] - 2.0f * w[i - 1] + w[i - 2]);
            if (d > m)
            {
                m = d;
                t.worstAtMs = static_cast<float> ((static_cast<double> (i) - 2.0) * 1000.0 / fs);
            }
        }
    }
    const float refSlope =
        std::max (maxSecondDifference (window (refA)), maxSecondDifference (window (refB)));
    t.slopeRatio = maxSecondDifference (window (y)) / refSlope;
    const float excess = maxSecondDifference (window (y)) - refSlope;
    if (excess > 0.0f)
        t.excessDb = static_cast<float> (
            db (excess / std::max (peak (window (refA)), peak (window (refB)))));
    return t;
}

const Board kBoardA = Board::legacy();
const Board kBoardB =
    boardOf ({ PedalId::gate, PedalId::pearl, PedalId::tide, PedalId::coral, PedalId::ripple });

std::vector<float> render (double fs, int block, const std::vector<float>& input,
                           const ParamSnapshot& p, const Board& from, const Board* to, int at)
{
    BoardRig rig (fs, block, static_cast<int> (input.size()));
    rig.in[0] = input;
    rig.engine.setBoard (from);
    rig.engine.reset();
    rig.run (p, 0, static_cast<int> (input.size()), [&] (int s) {
        if (to != nullptr && s == at)
            rig.engine.setBoard (*to);
    });
    return rig.out[0];
}
} // namespace

TEST_CASE ("Board reorder transient (measurement)", "[.board-measure]")
{
    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const int block : { 1, 64, 512, 4096 })
            for (const bool guitar : { false, true })
            {
                const int frames = static_cast<int> (1.6 * fs);
                std::vector<float> x (static_cast<size_t> (frames));
                if (guitar)
                    fillGuitarTone (x, fs, 0.3f);
                else
                    fillSine (x, 110.0, fs, 0.25f);
                auto p = cleanRig ({ ParamId::tideEnabled, ParamId::pearlEnabled });
                p[ParamId::pearlGain] = 0.3f;
                const int at = (static_cast<int> (fs) / block) * block;
                const auto y = render (fs, block, x, p, kBoardA, &kBoardB, at);
                const auto a = render (fs, block, x, p, kBoardA, nullptr, at);
                const auto b = render (fs, block, x, p, kBoardB, nullptr, at);
                const auto t = compare (y, a, b, fs, at);
                std::printf ("reorder %-6s %6.0f Hz block %4d: dip %6.1f dB, %4.1f ms below -1 dB, "
                             "slope x%.2f, swell %+.1f dB\n",
                             guitar ? "guitar" : "sine", fs, block, static_cast<double> (t.dipDb),
                             static_cast<double> (t.dipMs), static_cast<double> (t.slopeRatio),
                             static_cast<double> (t.swellDb));
            }

    // The board on its own (no amp after it): the same swap.
    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const int block : { 1, 64, 4096 })
            for (const bool guitar : { false, true })
            {
                const int frames = static_cast<int> (1.6 * fs);
                std::vector<float> x (static_cast<size_t> (frames));
                if (guitar)
                    fillGuitarTone (x, fs, 0.3f);
                else
                    fillSine (x, 110.0, fs, 0.25f);
                auto p = cleanRig ({ ParamId::tideEnabled, ParamId::pearlEnabled });
                p[ParamId::pearlGain] = 0.3f;
                const int at = (static_cast<int> (fs) / block) * block;
                const auto boardRender = [&] (const Board& from, const Board* to) {
                    BoardHarness h (fs, std::min (block, 512), from);
                    auto y = x;
                    h.run (y.data(), x.data(), frames, block, p, [&] (int s) {
                        if (to != nullptr && s == at)
                            h.board.setTarget (*to);
                    });
                    return y;
                };
                const auto t =
                    compare (boardRender (kBoardA, &kBoardB), boardRender (kBoardA, nullptr),
                             boardRender (kBoardB, nullptr), fs, at);
                std::printf (
                    "board reorder %-6s %6.0f Hz block %4d: dip %6.1f dB, %4.1f ms below -1 dB, "
                    "slope x%.2f at %.2f ms (excess %.0f dB), swell %+.1f dB\n",
                    guitar ? "guitar" : "sine", fs, block, static_cast<double> (t.dipDb),
                    static_cast<double> (t.dipMs), static_cast<double> (t.slopeRatio),
                    static_cast<double> (t.worstAtMs), static_cast<double> (t.excessDb),
                    static_cast<double> (t.swellDb));
            }

    // The duck alone: Coral, switched off, leaves the board (a pedal with latency, so the
    // board ducks), while what plays stays the same.
    for (const double fs : { 48000.0, 96000.0 })
    {
        const int block = 64;
        const int frames = static_cast<int> (1.6 * fs);
        for (const bool guitar : { false, true })
        {
            std::vector<float> x (static_cast<size_t> (frames));
            if (guitar)
                fillGuitarTone (x, fs, 0.3f);
            else
                fillSine (x, 110.0, fs, 0.25f);
            for (const bool tide : { true, false })
            {
                auto p = cleanRig ({ ParamId::pearlEnabled });
                p[ParamId::tideEnabled] = tide ? 1.0f : 0.0f;
                p[ParamId::pearlGain] = 0.3f;
                auto without = kBoardA;
                without.remove (PedalId::coral);
                const int at = (static_cast<int> (fs) / block) * block;
                const auto y = render (fs, block, x, p, kBoardA, &without, at);
                const auto a = render (fs, block, x, p, kBoardA, nullptr, at);
                const auto t = compare (y, a, a, fs, at);
                std::printf (
                    "duck only %s %-6s %6.0f Hz block %4d: dip %6.1f dB, %4.1f ms below -1 dB, "
                    "slope x%.2f, swell %+.1f dB\n",
                    tide ? "tide+pearl" : "pearl", guitar ? "guitar" : "sine", fs, block,
                    static_cast<double> (t.dipDb), static_cast<double> (t.dipMs),
                    static_cast<double> (t.slopeRatio), static_cast<double> (t.swellDb));
            }
        }
    }

    // Control: the same swap without the duck (the rack rewired mid-signal, nothing reset),
    // against the same rack playing A and B throughout.
    const double fs = 48000.0;
    const int frames = static_cast<int> (1.6 * fs);
    const auto p = [] {
        auto q = cleanRig ({ ParamId::tideEnabled, ParamId::pearlEnabled });
        q[ParamId::pearlGain] = 0.3f;
        return q;
    }();
    const auto rackRender = [&] (const Board& from, const Board* to, int at) {
        PedalRack rack;
        rack.prepare ({ fs, 64, 1 });
        rack.rewire (from);
        rack.reset();
        std::vector<float> x (static_cast<size_t> (frames));
        fillSine (x, 110.0, fs, 0.25f);
        const auto key = x;
        const auto settings = PedalSettings::from (p);
        const std::array<bool, kNumPedals> none {};
        for (int s = 0; s < frames; s += 64)
        {
            if (to != nullptr && s == at)
                rack.adopt (*to);
            rack.process (x.data() + s, key.data() + s, std::min (64, frames - s), settings, none);
        }
        return x;
    };
    const int at = 48000;
    const auto t = compare (rackRender (kBoardA, &kBoardB, at), rackRender (kBoardA, nullptr, at),
                            rackRender (kBoardB, nullptr, at), fs, at);
    std::printf ("hard swap sine 48000 Hz block 64: dip %6.1f dB, %4.1f ms below -1 dB, slope "
                 "x%.2f (excess %.0f dB)\n",
                 static_cast<double> (t.dipDb), static_cast<double> (t.dipMs),
                 static_cast<double> (t.slopeRatio), static_cast<double> (t.excessDb));
}
