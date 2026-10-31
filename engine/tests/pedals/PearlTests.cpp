// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PedalTestUtils.h"
#include "sedef/core/Oversampler.h"
#include "sedef/dsp/Pearl.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;
using dsp::Pearl;

namespace
{
constexpr double kFs = 48000.0;

void run (Pearl& p, AudioView v)
{
    p.process (v);
}

double thdAt (float gain)
{
    Pearl p;
    p.prepare ({ kFs, 512, 1 });
    p.setSettings ({ gain, 1.0f, 0.5f, 0.0f });
    test::Buffer b (1, 32768);
    test::fillSine (b[0], 750.0, kFs, 0.2f);
    test::processInBlocks (b, 512, [&] (AudioView v) { p.process (v); });
    return test::thd (b[0].data() + 16384, 16384, 750.0, kFs);
}

double gainDb (const Pearl::Settings& s, double hz)
{
    Pearl p;
    p.prepare ({ kFs, 512, 1 });
    p.setSettings (s);
    return test::measureGainDb (
        [&] (AudioView v) { test::processChunked (v, [&] (AudioView c) { p.process (c); }); }, hz,
        kFs, 32768, 0.003f);
}
} // namespace

TEST_CASE ("Pearl output stays finite for random settings and full-scale noise", "[pedals][pearl]")
{
    test::checkFiniteWithRandomSettings<Pearl> (
        [] (Random& r) {
            return Pearl::Settings { r.unipolar(), r.unipolar(), r.unipolar(), r.unipolar() };
        },
        run);
}

TEST_CASE ("Pearl passes silence as silence", "[pedals][pearl]")
{
    CHECK (test::silenceOutputPeak<Pearl> (Pearl::Settings {}, run) == 0.0f);
    CHECK (test::silenceOutputPeak<Pearl> (Pearl::Settings { 1.0f, 1.0f, 1.0f, 1.0f }, run) ==
           0.0f);
}

TEST_CASE ("Pearl distortion rises with gain", "[pedals][pearl]")
{
    double previous = 0.0;
    for (float gain : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const double d = thdAt (gain);
        CHECK (d > previous);
        previous = d;
    }
    CHECK (thdAt (0.0f) < 0.1); // low gain stays nearly clean
    CHECK (thdAt (1.0f) > 0.2);
}

TEST_CASE ("Pearl keeps the low end clean and full", "[pedals][pearl]")
{
    // Only what the feedback network's high-pass lets through is clipped, so at moderate gain a
    // low note distorts far less than a mid-range one and keeps its fundamental.
    const auto measure = [] (double hz, float amp, double& fundamentalDb) {
        Pearl p;
        p.prepare ({ kFs, 512, 1 });
        p.setSettings ({ 0.5f, 1.0f, 0.5f, 0.0f });
        test::Buffer b (1, 32768);
        test::fillSine (b[0], hz, kFs, amp);
        test::processInBlocks (b, 512, [&] (AudioView v) { p.process (v); });
        fundamentalDb = test::db (test::toneMagnitude (b[0].data() + 16384, 16384, hz, kFs) / amp);
        return test::thd (b[0].data() + 16384, 16384, hz, kFs);
    };
    double lowDb = 0.0, midDb = 0.0;
    const double low = measure (82.0, 0.2f, lowDb);
    const double mid = measure (750.0, 0.2f, midDb);
    CHECK (low < 0.5 * mid);
    CHECK (lowDb > -3.0);
}

TEST_CASE ("Pearl is touch sensitive", "[pedals][pearl]")
{
    // Softer picking, cleaner sound.
    double previous = 0.0;
    for (float amp : { 0.03f, 0.1f, 0.3f })
    {
        Pearl p;
        p.prepare ({ kFs, 512, 1 });
        p.setSettings ({ 0.5f, 0.5f, 0.5f, 0.0f });
        test::Buffer b (1, 32768);
        test::fillSine (b[0], 196.0, kFs, amp);
        test::processInBlocks (b, 512, [&] (AudioView v) { p.process (v); });
        const double d = test::thd (b[0].data() + 16384, 16384, 196.0, kFs);
        CHECK (d > 1.5 * previous);
        previous = d;
    }
}

TEST_CASE ("Pearl mid boost lifts 1 kHz against 100 Hz", "[pedals][pearl]")
{
    const auto tilt = [] (float mid) {
        const Pearl::Settings s { 0.0f, 0.5f, 0.5f, mid };
        return gainDb (s, 1000.0) - gainDb (s, 100.0);
    };
    const double flat = tilt (0.0f);
    const double boosted = tilt (1.0f);
    CHECK (boosted - flat > 7.0);
    CHECK (boosted - flat < 11.0);
    CHECK (tilt (0.5f) - flat == Approx ((boosted - flat) / 2.0).margin (1.5));
}

TEST_CASE ("Pearl tone darkens counter-clockwise", "[pedals][pearl]")
{
    const auto highs = [] (float tone) {
        const Pearl::Settings s { 0.3f, tone, 0.5f, 0.0f };
        return gainDb (s, 6000.0) - gainDb (s, 500.0);
    };
    CHECK (highs (0.0f) < highs (0.5f) - 3.0);
    CHECK (highs (0.5f) < highs (1.0f) - 3.0);
}

TEST_CASE ("Pearl level 0.5 is close to unity across the gain range", "[pedals][pearl]")
{
    for (float gain : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        Pearl p;
        p.prepare ({ kFs, 512, 1 });
        p.setSettings ({ gain, 0.5f, 0.5f, 0.0f });
        test::Buffer b (1, 48000);
        test::fillGuitarTone (b[0], kFs);
        const float in = test::rms (b[0], 24000);
        test::processInBlocks (b, 512, [&] (AudioView v) { p.process (v); });
        CHECK (test::db (test::rms (b[0], 24000) / in) == Approx (0.0).margin (1.5));
    }
}

TEST_CASE ("Pearl keeps aliasing low on high notes at full gain", "[pedals][pearl]")
{
    // Oversampling plus antiderivative anti-aliasing: inharmonic products of a 24th-fret note
    // (and an octave above it) stay 60 dB below the fundamental.
    for (double f0 : { 1318.5, 2637.0 })
    {
        Pearl m;
        m.prepare ({ kFs, 512, 1 });
        m.setSettings ({ 1.0f, 1.0f, 0.5f, 0.0f });
        test::Buffer b (1, 49152);
        test::fillSine (b[0], f0, kFs, 0.3f);
        test::processInBlocks (b, 512, [&] (AudioView v) { m.process (v); });
        CHECK (test::worstInharmonicDb (b[0].data() + 16384, 32768, f0, kFs) < -60.0);
    }
}

TEST_CASE ("Pearl reports the oversampler's latency", "[pedals][pearl]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        Oversampler os;
        os.prepare (512, 1, Oversampler::factorLog2For (fs));
        Pearl p;
        p.prepare ({ fs, 512, 1 });
        CHECK (p.latencySamples() == os.latencySamples());
    }
}

TEST_CASE ("Pearl's clean and clipping paths are time-aligned", "[pedals][pearl]")
{
    // At low level both paths are linear and overlap above ~650 Hz. Misaligned by the
    // oversampler's latency they would comb-filter (notches every ~1.3 kHz at 48 kHz); aligned,
    // the response is a smooth hump: it turns at most twice between 500 Hz and 8 kHz.
    for (double fs : { 48000.0, 96000.0 })
    {
        std::vector<double> response;
        for (double hz = 500.0; hz <= 8000.0; hz += 100.0)
        {
            Pearl p;
            p.prepare ({ fs, 512, 1 });
            p.setSettings ({ 0.0f, 1.0f, 0.5f, 0.0f });
            response.push_back (test::measureGainDb (
                [&] (AudioView v) {
                    test::processChunked (v, [&] (AudioView c) { p.process (c); });
                },
                hz, fs, 8192, 0.001f));
        }
        int turns = 0;
        double maxStep = 0.0;
        for (size_t i = 1; i < response.size(); ++i)
        {
            maxStep = std::max (maxStep, std::abs (response[i] - response[i - 1]));
            if (i >= 2 &&
                (response[i] - response[i - 1]) * (response[i - 1] - response[i - 2]) < 0.0)
                ++turns;
        }
        CHECK (turns <= 2);
        CHECK (maxStep < 1.0);
    }
}

TEST_CASE ("Pearl automation is click-free", "[pedals][pearl]")
{
    const auto check = [] (auto settingsAt) {
        const auto r = test::measureSweep<Pearl> (settingsAt, run);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return Pearl::Settings { t, 0.5f, 0.5f, 0.0f }; });
    check ([] (float t) { return Pearl::Settings { 0.5f, t, 0.5f, 0.0f }; });
    check ([] (float t) { return Pearl::Settings { 0.5f, 0.5f, t, 0.0f }; });
    check ([] (float t) { return Pearl::Settings { 0.5f, 0.5f, 0.5f, t }; });
    check ([] (float t) { return Pearl::Settings { t, 1.0f - t, t, 1.0f - t }; });
}
