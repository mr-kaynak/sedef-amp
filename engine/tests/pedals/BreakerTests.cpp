// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DriveTestUtils.h"
#include "sedef/core/Oversampler.h"
#include "sedef/dsp/Breaker.h"
#include "sedef/dsp/detail/Pickup.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <complex>

using namespace sedef;
using Catch::Approx;
using dsp::Breaker;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kTreble = Breaker::Treble;
constexpr int kFull = Breaker::Full;

void run (Breaker& b, AudioView v)
{
    b.process (v);
}

/// Small-signal gain of the pedal (dB).
double gainDb (float boost, int range, double hz, double fs = kFs)
{
    Breaker b;
    b.prepare ({ fs, 512, 1 });
    b.setSettings ({ boost, range });
    return test::measureGainDb (
        [&] (AudioView v) { test::processChunked (v, [&] (AudioView c) { b.process (c); }); }, hz,
        fs, 32768, 1.0e-4f);
}

/// The analogue input network's magnitude (dB).
double networkDb (int range, double hz)
{
    const auto net = Breaker::inputNetwork (range);
    const std::complex<double> s (0.0, kTwoPiD * hz);
    const auto poly = [&s] (const std::array<double, 3>& c) {
        return c[0] + c[1] * s + c[2] * s * s;
    };
    return test::db (std::abs (poly (net.numerator) / poly (net.denominator)));
}

struct Response
{
    double peakHz = 0.0, peakDb = -200.0, lowCornerHz = 0.0;
};

Response sweep (float boost, int range)
{
    Response r;
    std::vector<std::pair<double, double>> points;
    for (double hz = 80.0; hz <= 8000.0; hz *= std::pow (2.0, 1.0 / 24.0))
    {
        const double g = gainDb (boost, range, hz);
        points.emplace_back (hz, g);
        if (g > r.peakDb)
        {
            r.peakDb = g;
            r.peakHz = hz;
        }
    }
    for (const auto& [hz, g] : points)
        if (hz < r.peakHz && g < r.peakDb - 3.0)
            r.lowCornerHz = hz;
    return r;
}

test::Harmonics toneHarmonics (float boost, float amp, float* peakOut = nullptr)
{
    Breaker b;
    b.prepare ({ kFs, 512, 1 });
    b.setSettings ({ boost, kTreble });
    test::Buffer c (1, 65536);
    test::fillSine (c[0], 1500.0, kFs, amp);
    test::processInBlocks (c, 512, [&] (AudioView v) { b.process (v); });
    if (peakOut != nullptr)
        *peakOut = test::peak (std::vector<float> (c[0].begin() + 32768, c[0].end()));
    return test::harmonicsOf (c[0].data() + 32768, 32768, 1500.0, kFs);
}

double thdAt (float boost, float amp)
{
    const auto h = toneHarmonics (boost, amp);
    return std::sqrt (h.h[2] * h.h[2] + h.h[3] * h.h[3] + h.h[4] * h.h[4] + h.h[5] * h.h[5]);
}
} // namespace

TEST_CASE ("Breaker output stays finite at every rate, block size and setting", "[pedals][breaker]")
{
    test::checkFiniteEverywhere<Breaker> ([] (Random& r) {
        return Breaker::Settings { r.unipolar(), r.unipolar() < 0.5f ? kTreble : kFull };
    });
}

TEST_CASE ("Breaker passes silence as silence", "[pedals][breaker]")
{
    CHECK (test::silenceOutputPeak<Breaker> (Breaker::Settings { 1.0f, kTreble }, run) == 0.0f);
    CHECK (test::silenceOutputPeak<Breaker> (Breaker::Settings { 1.0f, kFull }, run) == 0.0f);
}

TEST_CASE ("Breaker does not depend on how the host slices the audio", "[pedals][breaker]")
{
    for (double fs : test::kDriveRates)
        for (int range : { kTreble, kFull })
            CHECK (test::blockSizeDeviation<Breaker> ({ 0.8f, range }, fs) < 1.0e-5);
}

TEST_CASE ("Breaker treble range keeps the lows out and peaks in the upper mids",
           "[pedals][breaker]")
{
    // The 4.7 nF input capacitor resonates with the pickup's 2.5 H near 1.47 kHz.
    const auto r = sweep (1.0f, kTreble);
    CHECK (r.peakHz > 1300.0);
    CHECK (r.peakHz < 1650.0);
    CHECK (r.lowCornerHz > 950.0);
    CHECK (r.lowCornerHz < 1250.0);
    CHECK (gainDb (1.0f, kTreble, 100.0) < r.peakDb - 24.0);
    CHECK (gainDb (1.0f, kTreble, 300.0) < r.peakDb - 15.0);
    CHECK (gainDb (1.0f, kTreble, 3000.0) > r.peakDb - 12.0);
}

TEST_CASE ("Breaker full range lets the lows in", "[pedals][breaker]")
{
    // 47 nF resonates near 460 Hz: a broad boost from the low mids up.
    const auto r = sweep (1.0f, kFull);
    CHECK (r.peakHz > 380.0);
    CHECK (r.peakHz < 560.0);
    CHECK (r.lowCornerHz > 160.0);
    CHECK (r.lowCornerHz < 250.0);
    CHECK (gainDb (1.0f, kFull, 100.0) > r.peakDb - 9.0);
    CHECK (gainDb (1.0f, kFull, 100.0) - gainDb (1.0f, kTreble, 100.0) > 15.0);
}

TEST_CASE ("Breaker boost sets the gain at the peak", "[pedals][breaker]")
{
    // At its centre the input network passes Rin / (Rin + Rpickup); the stage adds stageGainDb.
    const double centre = test::db (Breaker::kInputResistance /
                                    (Breaker::kInputResistance + dsp::detail::Pickup::kResistance));
    for (int range : { kTreble, kFull })
        for (float boost : { 0.0f, 0.5f, 1.0f })
        {
            const double hz = Breaker::inputNetwork (range).centreHz;
            CHECK (gainDb (boost, range, hz) ==
                   Approx (Breaker::stageGainDb (boost) + centre).margin (0.5));
        }
    CHECK (gainDb (0.0f, kTreble, 1470.0) == Approx (2.9).margin (1.0));
    CHECK (gainDb (1.0f, kTreble, 1470.0) == Approx (30.9).margin (1.0));
}

TEST_CASE ("Breaker's digital input network matches the analogue one", "[pedals][breaker]")
{
    // The stage is flat and linear for small signals: the pedal's shape is the network's, times
    // the transistor's first-order roll-off at 12 kHz.
    const auto expectedDb = [] (int range, double hz) {
        return networkDb (range, hz) - 10.0 * std::log10 (1.0 + (hz / 12000.0) * (hz / 12000.0));
    };
    for (int range : { kTreble, kFull })
    {
        const double ref = gainDb (0.5f, range, 1000.0) - expectedDb (range, 1000.0);
        for (double hz : { 100.0, 300.0, 700.0, 1500.0, 3000.0, 6000.0 })
            CHECK (gainDb (0.5f, range, hz) - expectedDb (range, hz) == Approx (ref).margin (0.5));
    }
}

TEST_CASE ("Breaker clips softly and lopsidedly when pushed", "[pedals][breaker]")
{
    // Boost bypasses the emitter feedback: more gain and the junction's exponential exposed.
    CHECK (thdAt (0.0f, 0.1f) < 0.005); // clean at minimum boost
    double previous = 0.0;
    for (float boost : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const double d = thdAt (boost, 0.1f);
        CHECK (d > previous);
        previous = d;
    }
    // Soft: distortion grows gradually with the level, dominated by the second harmonic.
    CHECK (thdAt (1.0f, 0.01f) < thdAt (1.0f, 0.03f));
    CHECK (thdAt (1.0f, 0.03f) < thdAt (1.0f, 0.1f));
    const auto h = toneHarmonics (1.0f, 0.03f);
    CHECK (h.h[2] > 0.03);
    CHECK (h.h[2] < 0.2);
    CHECK (h.h[2] > 3.0 * h.h[3]);
    // Bounded by the transistor's swing: 2.4 V to cutoff, 5.5 V to saturation.
    float peak = 0.0f;
    toneHarmonics (1.0f, 0.5f, &peak);
    CHECK (peak < 5.6f);
    CHECK (peak > 4.0f);
}

TEST_CASE ("Breaker keeps aliasing low on high notes at full boost", "[pedals][breaker]")
{
    for (double fs : { 44100.0, 48000.0 })
        for (double f0 : { 1318.5, 2637.0 })
        {
            Breaker m;
            m.prepare ({ fs, 512, 1 });
            m.setSettings ({ 1.0f, kTreble });
            test::Buffer b (1, 49152);
            test::fillSine (b[0], f0, fs, 0.3f);
            test::processInBlocks (b, 512, [&] (AudioView v) { m.process (v); });
            CHECK (test::worstInharmonicDb (b[0].data() + 16384, 32768, f0, fs) < -60.0);
        }
}

TEST_CASE ("Breaker reports the oversampler's latency", "[pedals][breaker]")
{
    for (double fs : test::kDriveRates)
    {
        Oversampler os;
        os.prepare (512, 1, Oversampler::factorLog2For (fs));
        Breaker b;
        b.prepare ({ fs, 512, 1 });
        CHECK (b.latencySamples() == os.latencySamples());
    }
}

TEST_CASE ("Breaker automation and the range switch are click-free", "[pedals][breaker]")
{
    int which = 0;
    const auto check = [&which] (auto settingsAt) {
        const auto r = test::measureSweep<Breaker> (settingsAt, run, 48000.0, 330.0, 0.1f);
        INFO ("sweep " << which++);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return Breaker::Settings { t, kTreble }; });
    check ([] (float t) { return Breaker::Settings { t, kFull }; });
    check ([] (float t) { return Breaker::Settings { 0.7f, t < 0.5f ? kTreble : kFull }; });
    check ([] (float t) { return Breaker::Settings { 0.7f, t < 0.5f ? kFull : kTreble }; });
}
