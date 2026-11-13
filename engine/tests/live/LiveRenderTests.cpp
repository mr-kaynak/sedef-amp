// SPDX-License-Identifier: AGPL-3.0-or-later
// Scenes through the whole engine (docs/live.md §6): a switch is a block-aligned parameter change
// that goes through the modules' own fades and smoothing, nothing more.

#include "TestUtils.h"
#include "board/BoardTestUtils.h"
#include "live/LiveTestUtils.h"
#include "pedals/PedalTestUtils.h"
#include "sedef/Engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstdio>

using namespace sedef;
using namespace sedef::live;

namespace
{
struct Scenes
{
    ParamSnapshot first;
    ParamSnapshot second;
    SceneBank bank;
};

/// Scene 1: Pearl and Drift on, a quiet mix; scene 2: Pearl off, Coral and Gyre on, Drift off
/// with more mix and the amp a little louder.
Scenes makeScenes()
{
    Scenes s;
    s.first = test::cleanRig ({ ParamId::pearlEnabled, ParamId::driftEnabled });
    s.first[ParamId::driftMix] = 0.2f;
    s.bank = SceneBank::withSwitches (s.first);
    REQUIRE (s.bank.addValue (ParamId::driftMix, s.first[ParamId::driftMix]));
    REQUIRE (s.bank.addValue (ParamId::lagoonVolume, s.first[ParamId::lagoonVolume]));
    s.second = s.first;
    s.second[ParamId::pearlEnabled] = 0.0f;
    s.second[ParamId::coralEnabled] = 1.0f;
    s.second[ParamId::gyreEnabled] = 1.0f;
    s.second[ParamId::driftEnabled] = 0.0f;
    s.second[ParamId::driftMix] = 0.5f;
    s.second[ParamId::lagoonVolume] = 0.35f;
    s.second[ParamId::scene] = 1.0f;
    for (int i = 0; i < s.bank.numParams; ++i)
        s.bank.values[1][static_cast<size_t> (i)] =
            s.second.values[s.bank.params[static_cast<size_t> (i)]];
    return s;
}

/// Renders `x` through the engine in host blocks; `paramsFor (blockStart, snapshot)` sets the
/// block's parameters.
template <typename Params>
std::vector<float> render (double fs, int block, const std::vector<float>& x, Params&& paramsFor)
{
    Engine engine;
    engine.prepare (fs, block, 1, 2);
    engine.setBoard (
        test::boardOf ({ PedalId::gate, PedalId::pearl, PedalId::coral, PedalId::gyre }));
    engine.reset();
    std::vector<float> left (x.size()), right (x.size());
    Transport transport;
    const int frames = static_cast<int> (x.size());
    for (int s = 0; s < frames; s += block)
    {
        const int n = std::min (block, frames - s);
        ParamSnapshot p;
        paramsFor (s, p);
        std::array<const float*, 2> in { x.data() + s, nullptr };
        std::array<float*, 2> out { left.data() + s, right.data() + s };
        engine.process (in.data(), out.data(), n, p, transport);
    }
    return left;
}
} // namespace

TEST_CASE ("A scene switch plays exactly like the same change made as automation", "[live]")
{
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE (64, 333);
    CAPTURE (fs, block);
    const auto scenes = makeScenes();
    std::vector<float> x (static_cast<size_t> (fs * 1.2));
    test::fillGuitarTone (x, fs, 0.3f);
    const int at = (static_cast<int> (fs * 0.5) / block) * block;

    // Through the live layer: the host's parameters never move, a footswitch selects scene 2.
    LiveControl live;
    live.setScenes (scenes.bank);
    MidiMap map;
    map.add (test::bindAction (Action::scene, 30, 1));
    live.setMap (map);
    const auto switched = render (fs, block, x, [&] (int s, ParamSnapshot& p) {
        live.acknowledge();
        p = scenes.first;
        const auto press = test::cc (30, 127);
        live.process (p, &press, s == at ? 1 : 0, block);
    });

    // The same values as automation from that block on.
    const auto automated = render (
        fs, block, x, [&] (int s, ParamSnapshot& p) { p = s < at ? scenes.first : scenes.second; });

    REQUIRE (switched.size() == automated.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < switched.size(); ++i)
        differing += switched[i] != automated[i] ? 1 : 0;
    CHECK (differing == 0);
    CHECK (live.activeScene() == 1);
    CHECK (test::allFinite (switched));
    // No gap: the output keeps playing through the switch.
    const auto window = test::windowedRms (switched, static_cast<int> (fs * 0.005));
    const auto from = static_cast<size_t> (at / static_cast<int> (fs * 0.005));
    for (std::size_t w = from; w < from + 10 && w < window.size(); ++w)
        CHECK (window[w] > 0.25f * window[from - 2]);
}

TEST_CASE ("Scene switch measurements", "[.live-measure]")
{
    // The largest sample-to-sample step after the switch against the largest before it, at the
    // rig's output (docs/live.md, reported, not a gate).
    for (const double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const int block = 128;
        const auto scenes = makeScenes();
        std::vector<float> x (static_cast<size_t> (fs * 1.2));
        test::fillGuitarTone (x, fs, 0.3f);
        const int at = (static_cast<int> (fs * 0.5) / block) * block;
        const auto y = render (fs, block, x, [&] (int s, ParamSnapshot& p) {
            p = s < at ? scenes.first : scenes.second;
        });
        const auto span = static_cast<std::size_t> (fs * 0.05);
        float before = 0.0f, after = 0.0f;
        for (std::size_t i = static_cast<std::size_t> (at) - span;
             i < static_cast<std::size_t> (at); ++i)
            before = std::max (before, std::abs (y[i] - y[i - 1]));
        for (std::size_t i = static_cast<std::size_t> (at);
             i < static_cast<std::size_t> (at) + span; ++i)
            after = std::max (after, std::abs (y[i] - y[i - 1]));
        std::printf ("scene switch @ %.1f kHz: largest step %.4f before, %.4f after (%+.1f dB)\n",
                     fs / 1000.0, static_cast<double> (before), static_cast<double> (after),
                     20.0 * std::log10 (static_cast<double> (after / before)));
    }
}
