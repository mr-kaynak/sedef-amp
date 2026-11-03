// SPDX-License-Identifier: AGPL-3.0-or-later
// The transpose evaluation harness (TransposeEval.h) must read zero error for the ideal output and
// the right amount for artefacts of known size, or its verdicts mean nothing.
#include "reverb/TransposeEval.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
namespace tp = sedef::test::transpose;
using Catch::Approx;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kRatio = 0.74915353843834; // -5 semitones

std::vector<float> withAm (std::vector<float> x, double hz, double index)
{
    for (size_t i = 0; i < x.size(); ++i)
        x[i] *= static_cast<float> (
            1.0 + index * std::sin (kTwoPiD * hz * static_cast<double> (i) / kFs));
    return x;
}

std::vector<float> mix (const std::vector<float>& a, float ga, const std::vector<float>& b,
                        float gb)
{
    std::vector<float> y (a.size());
    for (size_t i = 0; i < a.size(); ++i)
        y[i] = ga * a[i] + gb * b[i];
    return y;
}
} // namespace

TEST_CASE ("Transpose metrics read no error for the ideal output", "[reverb][transpose]")
{
    {
        const auto score = tp::scores::chord();
        const auto ref = tp::render (score, kFs, kRatio);
        const auto m = tp::sustainMetrics (score, ref, ref, kFs, kRatio);
        CHECK (m.purityDb == Approx (m.purityRefDb));
        CHECK (m.purityRefDb < -38.0); // the measurement floor is well below the -35 dB bar
        CHECK (m.timbreDb < 0.01);
    }
    {
        const auto score = tp::scores::chordD();
        const auto ref = tp::render (score, kFs, kRatio);
        const auto w = tp::warbleMetrics (score, ref, ref, kFs, kRatio, 0.5 * 146.8324);
        CHECK (w.partials >= 8);
        CHECK (w.partialDb < 0.01);
        CHECK (w.envelopeDb < 0.01);
    }
    {
        const auto score = tp::scores::hits();
        const auto ref = tp::render (score, kFs, kRatio);
        const auto t = tp::transientMetrics (score, ref, ref, kFs, 0.0);
        CHECK (t.latencyMs.size() == score.onsets.size());
        CHECK (std::abs (t.latencyMeanMs) < 0.01);
        CHECK (t.latencyRangeMs < 0.01);
        CHECK (t.preEchoDb < -100.0);
        CHECK (t.flamDb < 0.01);
        CHECK (t.attackDb < 0.01);
    }
    {
        const auto score = tp::scores::line();
        const auto ref = tp::render (score, kFs, kRatio);
        const auto p = tp::pitchMetrics (score, ref, ref, kFs);
        CHECK (p.frames > 100);
        CHECK (p.p95Cents < 0.01);
    }
}

TEST_CASE ("Transpose metrics measure artefacts of known size", "[reverb][transpose]")
{
    SECTION ("6 Hz amplitude modulation, index 0.05 (0.31 dB RMS, sidebands at -29 dB)")
    {
        const auto score = tp::scores::chordD();
        const auto ref = tp::render (score, kFs, kRatio);
        const auto out = withAm (ref, 6.0, 0.05);
        const auto w = tp::warbleMetrics (score, out, ref, kFs, kRatio, 0.5 * 146.8324);
        const double expected = 20.0 / std::log (10.0) * 0.05 / std::sqrt (2.0);
        CHECK (w.partialDb == Approx (expected).margin (0.03));
        CHECK (w.envelopeDb == Approx (expected).margin (0.03));
        const auto s = tp::sustainMetrics (score, out, ref, kFs, kRatio);
        CHECK (s.purityDb == Approx (10.0 * std::log10 (0.05 * 0.05 / 2.0)).margin (1.0));
    }

    SECTION ("a constant 5 ms delay")
    {
        const auto score = tp::scores::riff();
        const auto ref = tp::render (score, kFs, kRatio);
        const auto out = tp::advance (ref, -0.005 * kFs);
        const auto t = tp::transientMetrics (score, out, ref, kFs, 0.005 * kFs);
        CHECK (t.latencyMeanMs == Approx (5.0).margin (0.05));
        CHECK (t.latencyRangeMs < 0.05);
    }

    SECTION ("a doubled attack 15 ms late, and a -20 dB copy 10 ms early")
    {
        const auto score = tp::scores::hits();
        const auto ref = tp::render (score, kFs, kRatio);
        const auto flam = tp::transientMetrics (
            score, mix (ref, 0.7f, tp::advance (ref, -0.015 * kFs), 0.7f), ref, kFs, 0.0);
        CHECK (flam.flamDb > 6.0);
        CHECK (std::abs (flam.latencyMeanMs) < 0.5); // the first attack is the one that counts
        const auto pre = tp::transientMetrics (
            score, mix (ref, 1.0f, tp::advance (ref, 0.010 * kFs), 0.1f), ref, kFs, 0.0);
        CHECK (pre.preEchoDb > -30.0);
        CHECK (pre.preEchoDb < -22.0);
    }

    SECTION ("10 cents sharp")
    {
        const auto score = tp::scores::line();
        const auto ref = tp::render (score, kFs, kRatio);
        const auto out = tp::render (score, kFs, kRatio * std::exp2 (10.0 / 1200.0));
        const auto p = tp::pitchMetrics (score, out, ref, kFs);
        CHECK (p.medianCents == Approx (10.0).margin (0.5));
    }
}

TEST_CASE ("Transpose warble metrics ignore the phases of the partials", "[reverb][transpose]")
{
    // Two ideal outputs that differ only in partial phases: a shifter is free to choose them.
    const auto score = tp::scores::chordD();
    auto other = score;
    for (auto& n : other.notes)
        n.seed += 1000;
    for (double ratio : { kRatio, 1.0, 2.0 })
    {
        const auto ref = tp::render (score, kFs, ratio);
        const auto out = tp::render (other, kFs, ratio);
        const auto w = tp::warbleMetrics (score, out, ref, kFs, ratio, 0.5 * 146.8324);
        CAPTURE (ratio);
        CHECK (w.partialDb < 0.02);
        CHECK (w.envelopeDb < 0.02);
    }
}
