// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PitchTestSignals.h"
#include "TestUtils.h"
#include "sedef/analysis/PitchDetector.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace sedef;
using analysis::PitchDetector;

namespace
{
constexpr double kNotesHz[] = { 30.87, 41.2,   82.41,  110.0,  146.83,
                                196.0, 246.94, 329.63, 659.26, 1318.5 };
} // namespace

TEST_CASE ("PitchDetector measures steady harmonic tones to a fraction of a cent",
           "[analysis][pitch]")
{
    // The tracker's analysis rates for 44.1 kHz (and its multiples) and 48 kHz (and 96 kHz).
    const double fs = GENERATE (11025.0, 12000.0);
    PitchDetector det;
    det.prepare (fs);
    const int window = det.windowSize();
    const int hop = static_cast<int> (0.005 * fs);

    for (double hz : kNotesHz)
    {
        CAPTURE (fs, hz);
        auto tone = test::harmonicTone (hz, fs, static_cast<size_t> (window + 40 * hop));
        test::addNoise (tone, -70.0f);

        double sum = 0.0, worst = 0.0;
        for (int i = 0; i < 40; ++i)
        {
            const auto r = det.detect (tone.data() + i * hop, window);
            REQUIRE (r.valid);
            CHECK (r.clarity > 0.95f);
            const double err = test::centsError (r.frequencyHz, hz);
            sum += err;
            worst = std::max (worst, std::abs (err));
        }
        CAPTURE (worst, sum / 40);
        CHECK (worst < 0.5);
        CHECK (std::abs (sum / 40) < 0.1);
    }
}

TEST_CASE ("PitchDetector short windows still find the period without octave errors",
           "[analysis][pitch]")
{
    // A tone whose even harmonics dominate tempts a short window into reporting the octave above.
    const double fs = 12000.0;
    PitchDetector det;
    det.prepare (fs);
    const double hz = 82.41;
    std::vector<float> tone (static_cast<size_t> (det.windowSize()));
    for (size_t i = 0; i < tone.size(); ++i)
    {
        const double ph = kTwoPiD * hz * static_cast<double> (i) / fs;
        tone[i] = static_cast<float> (0.3 * std::sin (ph) + 0.6 * std::sin (2.0 * ph + 0.3) +
                                      0.2 * std::sin (4.0 * ph + 1.0));
    }
    for (int n = 64; n <= det.windowSize(); n += 32)
    {
        const auto r = det.detect (tone.data(), n);
        if (r.valid)
        {
            CAPTURE (n, r.frequencyHz);
            CHECK (std::abs (test::centsError (r.frequencyHz, hz)) < 50.0);
        }
    }
    // Five periods are enough to commit.
    CHECK (det.detect (tone.data(), static_cast<int> (5.2 * fs / hz)).valid);
}

TEST_CASE ("PitchDetector rejects noise, silence and out-of-range input", "[analysis][pitch]")
{
    const double fs = 12000.0;
    PitchDetector det;
    det.prepare (fs);
    const auto n = static_cast<size_t> (det.windowSize());

    std::vector<float> silence (n, 0.0f);
    CHECK_FALSE (det.detect (silence.data(), static_cast<int> (n)).valid);

    for (std::uint32_t seed = 1; seed <= 20; ++seed)
    {
        std::vector<float> noise (n, 0.0f);
        test::addNoise (noise, -30.0f, seed);
        for (int len : { 256, 1024, static_cast<int> (n) })
        {
            CAPTURE (seed, len);
            CHECK_FALSE (det.detect (noise.data(), len).valid);
        }
    }

    // Above maxHz and below minHz.
    auto high = test::harmonicTone (2000.0, fs, n, 1);
    CHECK_FALSE (det.detect (high.data(), static_cast<int> (n)).valid);
    auto low = test::harmonicTone (15.0, fs, n, 1);
    CHECK_FALSE (det.detect (low.data(), static_cast<int> (n)).valid);

    CHECK_FALSE (det.detect (nullptr, 100).valid);
    CHECK_FALSE (det.detect (silence.data(), 0).valid);
}

TEST_CASE ("PitchDetector reports the fundamental partial of a stiff string", "[analysis][pitch]")
{
    // Upper partials stretched by inharmonicity pull the periodicity sharp; the precision stage
    // measures the fundamental itself.
    const double fs = 12000.0;
    PitchDetector det;
    det.prepare (fs);
    for (double hz : { 82.41, 196.0, 659.26 })
    {
        test::Pluck p;
        p.f1 = hz;
        p.inharmonicity = 4.0e-4;
        p.glideCents = 0.0;
        p.pickNoise = 0.0f;
        p.maxPartialHz = 5000.0;
        std::vector<float> sig (static_cast<size_t> (det.windowSize() + 4800), 0.0f);
        test::addPluck (sig, fs, 0, p);
        const auto r = det.detect (sig.data() + 2400, det.windowSize());
        CAPTURE (hz, r.frequencyHz);
        REQUIRE (r.valid);
        CHECK (std::abs (test::centsError (r.frequencyHz, hz)) < 0.1);
    }
}

TEST_CASE ("PitchDetector does not report the common period of two notes", "[analysis][pitch]")
{
    // E2 and A2 together repeat every 1 / 27.5 s, with nothing at 27.5 or 55 Hz.
    const double fs = 12000.0;
    PitchDetector det;
    det.prepare (fs);
    std::vector<float> mix (static_cast<size_t> (det.windowSize()), 0.0f);
    const auto e2 = test::harmonicTone (82.41, fs, mix.size(), 8, 0.15f, 1);
    const auto a2 = test::harmonicTone (110.0, fs, mix.size(), 8, 0.25f, 2);
    for (size_t i = 0; i < mix.size(); ++i)
        mix[i] = e2[i] + a2[i];
    for (int n : { 1200, 2400, det.windowSize() })
    {
        const auto r = det.detect (mix.data(), n);
        CAPTURE (n, r.valid, r.frequencyHz, r.clarity);
        if (r.valid)
            CHECK (std::abs (test::centsError (r.frequencyHz, 110.0)) < 50.0);
    }
}
