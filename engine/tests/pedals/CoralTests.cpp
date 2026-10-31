// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PedalTestUtils.h"
#include "sedef/core/Oversampler.h"
#include "sedef/dsp/Coral.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;
using dsp::Coral;

namespace
{
constexpr double kFs = 48000.0;

void run (Coral& c, AudioView v)
{
    c.process (v);
}

std::vector<float> render (const Coral::Settings& s, const std::vector<float>& input)
{
    Coral c;
    c.prepare ({ kFs, 512, 1 });
    c.setSettings (s);
    test::Buffer b (1, static_cast<int> (input.size()));
    b[0] = input;
    test::processInBlocks (b, 512, [&] (AudioView v) { c.process (v); });
    return b[0];
}
} // namespace

TEST_CASE ("Coral output stays finite for random settings and full-scale noise", "[pedals][coral]")
{
    test::checkFiniteWithRandomSettings<Coral> (
        [] (Random& r) { return Coral::Settings { r.unipolar(), r.unipolar(), r.unipolar() }; },
        run);
}

TEST_CASE ("Coral passes silence as silence", "[pedals][coral]")
{
    CHECK (test::silenceOutputPeak<Coral> (Coral::Settings {}, run) == 0.0f);
    CHECK (test::silenceOutputPeak<Coral> (Coral::Settings { 1.0f, 0.0f, 1.0f }, run) == 0.0f);
}

TEST_CASE ("Coral cut removes highs as it turns up", "[pedals][coral]")
{
    std::vector<float> tone (48000);
    test::fillGuitarTone (tone, kFs);
    const auto highs = [&] (float cut) {
        return test::rmsAbove (render ({ 0.7f, cut, 0.5f }, tone), 5000.0, kFs, 24000);
    };
    const float bright = highs (0.0f), mid = highs (0.5f), dark = highs (1.0f);
    CHECK (mid < bright);
    CHECK (dark < mid);
    CHECK (test::db (dark / bright) < -15.0);
}

TEST_CASE ("Coral distortion rises with gain", "[pedals][coral]")
{
    std::vector<float> sine (32768);
    test::fillSine (sine, 750.0, kFs, 0.1f);
    double previous = 0.0;
    for (float gain : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const auto out = render ({ gain, 0.0f, 0.5f }, sine);
        const double d = test::thd (out.data() + 16384, 16384, 750.0, kFs);
        CHECK (d > previous);
        previous = d;
    }
    CHECK (previous > 0.3);
}

TEST_CASE ("Coral output peak is bounded whatever goes in", "[pedals][coral]")
{
    std::vector<float> loud (48000);
    test::fillNoise (loud, 1.0f, 4);
    std::vector<float> huge (48000);
    test::fillSine (huge, 110.0, kFs, 10.0f);
    Random r (8);
    for (int k = 0; k < 12; ++k)
    {
        const Coral::Settings s { r.unipolar(), r.unipolar(), 0.5f };
        CHECK (test::peak (render (s, loud)) < 0.6f);
        CHECK (test::peak (render (s, huge)) < 0.6f);
        CHECK (test::peak (render ({ s.gain, s.cut, 1.0f }, huge)) < 6.0f);
    }
}

TEST_CASE ("Coral level 0.5 is close to unity across the gain range", "[pedals][coral]")
{
    std::vector<float> tone (48000);
    test::fillGuitarTone (tone, kFs);
    const float in = test::rms (tone, 24000);
    for (float gain : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        CHECK (test::db (test::rms (render ({ gain, 0.5f, 0.5f }, tone), 24000) / in) ==
               Approx (0.0).margin (1.5));
}

TEST_CASE ("Coral keeps aliasing low on high notes at full gain", "[pedals][coral]")
{
    // Oversampling plus antiderivative anti-aliasing: inharmonic products of a 24th-fret note
    // (and an octave above it) stay 60 dB below the fundamental.
    for (double f0 : { 1318.5, 2637.0 })
    {
        Coral m;
        m.prepare ({ kFs, 512, 1 });
        m.setSettings ({ 1.0f, 0.0f, 0.5f });
        test::Buffer b (1, 49152);
        test::fillSine (b[0], f0, kFs, 0.3f);
        test::processInBlocks (b, 512, [&] (AudioView v) { m.process (v); });
        CHECK (test::worstInharmonicDb (b[0].data() + 16384, 32768, f0, kFs) < -60.0);
    }
}

TEST_CASE ("Coral reports the oversampler's latency", "[pedals][coral]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        Oversampler os;
        os.prepare (512, 1, Oversampler::factorLog2For (fs));
        Coral c;
        c.prepare ({ fs, 512, 1 });
        CHECK (c.latencySamples() == os.latencySamples());
    }
}

TEST_CASE ("Coral automation is click-free", "[pedals][coral]")
{
    const auto check = [] (auto settingsAt) {
        const auto r = test::measureSweep<Coral> (settingsAt, run);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return Coral::Settings { t, 0.5f, 0.5f }; });
    check ([] (float t) { return Coral::Settings { 0.5f, t, 0.5f }; });
    check ([] (float t) { return Coral::Settings { 0.5f, 0.5f, t }; });
    check ([] (float t) { return Coral::Settings { t, 1.0f - t, t }; });
}
