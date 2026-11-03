// SPDX-License-Identifier: AGPL-3.0-or-later
// The mods pedals (Conch, Anemone, Gyre, Jetty, Beacon) inside the engine: wired to their
// parameters, in their chain positions, latency-free and click-free to switch.
#include "TestUtils.h"
#include "pedals/PedalTestUtils.h"
#include "sedef/Engine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

using namespace sedef;

namespace
{
constexpr double kFs = 48000.0;

constexpr ParamId kModsPedals[] = { ParamId::conchEnabled, ParamId::anemoneEnabled,
                                    ParamId::gyreEnabled, ParamId::jettyEnabled,
                                    ParamId::beaconEnabled };

/// A clean, quiet rig: no gate, Lagoon at low volume, so the amp stays nearly linear and the
/// pedals' own behaviour shows through.
ParamSnapshot cleanRig()
{
    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;
    p[ParamId::ampModel] = 0.0f;
    p[ParamId::lagoonVolume] = 0.2f;
    p[ParamId::lagoonMaster] = 0.3f;
    return p;
}

/// Renders mono `in` through the engine in 64-sample blocks; `perBlock(start, params)` may change
/// the parameters before each block.
template <typename PerBlock>
std::vector<float> render (const std::vector<float>& in, ParamSnapshot p, PerBlock&& perBlock,
                           int block = 64)
{
    Engine engine;
    engine.prepare (kFs, block, 1, 1);
    Transport transport;
    std::vector<float> out (in.size());
    for (size_t s = 0; s < in.size(); s += static_cast<size_t> (block))
    {
        const int n = static_cast<int> (std::min (in.size() - s, static_cast<size_t> (block)));
        perBlock (static_cast<int> (s), p);
        const float* ip[] = { in.data() + s };
        float* op[] = { out.data() + s };
        engine.process (ip, op, n, p, transport);
    }
    return out;
}

std::vector<float> render (const std::vector<float>& in, const ParamSnapshot& p)
{
    return render (in, p, [] (int, ParamSnapshot&) {});
}
} // namespace

TEST_CASE ("The mods pedals add no latency", "[engine][mods]")
{
    Engine engine;
    engine.prepare (kFs, 64, 1, 1);
    const int base = engine.latencySamples();
    // Latency is fixed by prepare(); render an impulse with each pedal on and off and check the
    // output's onset does not move.
    std::vector<float> impulse (8192, 0.0f);
    impulse[1000] = 0.5f;
    const auto onset = [] (const std::vector<float>& v) {
        for (size_t i = 0; i < v.size(); ++i)
            if (std::abs (v[i]) > 1.0e-4f)
                return static_cast<int> (i);
        return -1;
    };
    const int off = onset (render (impulse, cleanRig()));
    REQUIRE (off >= 1000);
    for (ParamId id : kModsPedals)
    {
        auto p = cleanRig();
        p[id] = 1.0f;
        p[ParamId::anemoneMode] = 2.0f; // high-pass: a low-pass at 200 Hz would rise too slowly
        INFO (std::string (spec (id).id));
        // A pedal may filter the onset's first sample away, but it can't delay it: the response
        // starts within a couple of samples of the bypassed one.
        CHECK (std::abs (onset (render (impulse, p)) - off) <= 2);
    }
    CHECK (engine.latencySamples() == base);
}

TEST_CASE ("Each mods pedal is wired to its parameters", "[engine][mods]")
{
    // Turning a pedal on changes the sound, and so does moving its main control.
    std::vector<float> in (static_cast<size_t> (2.0 * kFs));
    test::fillGuitarTone (in, kFs, 0.2f);
    const auto dry = render (in, cleanRig());
    const auto differs = [&] (const std::vector<float>& a, const std::vector<float>& b) {
        std::vector<float> d (a.size());
        for (size_t i = 0; i < a.size(); ++i)
            d[i] = a[i] - b[i];
        return test::rms (d, a.size() / 2) / test::rms (b, b.size() / 2);
    };
    struct Case
    {
        ParamId enable, control;
        float a, b;
    };
    const Case cases[] = {
        { ParamId::conchEnabled, ParamId::conchPosition, 0.1f, 0.9f },
        { ParamId::anemoneEnabled, ParamId::anemoneRange, 0.1f, 0.9f },
        { ParamId::gyreEnabled, ParamId::gyreDepth, 0.0f, 1.0f },
        { ParamId::jettyEnabled, ParamId::jettyDelay, 0.5f, 6.0f },
        { ParamId::beaconEnabled, ParamId::beaconMode, 0.0f, 1.0f },
    };
    for (const auto& c : cases)
    {
        INFO (std::string (spec (c.enable).id));
        auto p = cleanRig();
        p[c.enable] = 1.0f;
        p[c.control] = c.a;
        const auto a = render (in, p);
        p[c.control] = c.b;
        const auto b = render (in, p);
        CHECK (differs (a, dry) > 0.05f);
        CHECK (differs (b, a) > 0.05f);
    }
}

TEST_CASE ("Switching a mods pedal on and off is click-free", "[engine][mods]")
{
    // A 220 Hz tone; the pedal toggles on at 0.5 s and off at 1.0 s. The click indicator (max
    // second difference) around the switches stays within the steady on and off states' own.
    std::vector<float> in (static_cast<size_t> (1.5 * kFs));
    test::fillSine (in, 220.0, kFs, 0.2f);
    for (ParamId id : kModsPedals)
    {
        INFO (std::string (spec (id).id));
        auto on = cleanRig();
        on[id] = 1.0f;
        const float steady = std::max (test::maxSecondDifference (render (in, cleanRig()), 4800),
                                       test::maxSecondDifference (render (in, on), 4800));
        const auto toggled = render (in, cleanRig(), [&] (int start, ParamSnapshot& p) {
            p[id] = start >= static_cast<int> (0.5 * kFs) && start < static_cast<int> (kFs) ? 1.0f
                                                                                            : 0.0f;
        });
        CHECK (test::allFinite (toggled));
        CHECK (test::maxSecondDifference (toggled, 4800) <= 1.5f * steady);
    }
}
