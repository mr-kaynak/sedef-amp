// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DriveTestUtils.h"
#include "sedef/core/Oversampler.h"
#include "sedef/dsp/Sargasso.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <complex>

using namespace sedef;
using Catch::Approx;
using dsp::Sargasso;

namespace
{
constexpr double kFs = 48000.0;

void run (Sargasso& g, AudioView v)
{
    g.process (v);
}

/// |N(j w) / D(j w)| of the analogue tone network, in dB.
double networkDb (float tone, double hz)
{
    const auto net = Sargasso::toneNetwork (tone);
    const std::complex<double> s (0.0, kTwoPiD * hz);
    const auto poly = [&s] (const std::array<double, 3>& c) {
        return c[0] + c[1] * s + c[2] * s * s;
    };
    return test::db (std::abs (poly (net.numerator) / poly (net.denominator)));
}

/// The same network solved directly as a circuit (complex nodal equations, no algebra shared with
/// Sargasso::toneNetwork): low-pass branch R1/C1 to node L, high-pass branch C2/R2 to node H, pot
/// between them, wiper into the load.
double circuitDb (float tone, double hz)
{
    using C = std::complex<double>;
    using N = Sargasso::ToneNetwork;
    const C s (0.0, kTwoPiD * hz);
    const double t = std::clamp (static_cast<double> (tone), 1.0e-4, 1.0 - 1.0e-4);
    const C y1 = 1.0 / N::kLowPassR, yc1 = s * N::kLowPassC, yc2 = s * N::kHighPassC;
    const C y2 = 1.0 / N::kHighPassR, ya = 1.0 / (t * N::kPot), yb = 1.0 / ((1.0 - t) * N::kPot);
    const C yl = 1.0 / N::kLoad;
    // Unknowns L, H, W; source 1 V. Rows: KCL at L, H, W.
    C m[3][4] = { { y1 + yc1 + ya, 0.0, -ya, y1 },
                  { 0.0, yc2 + y2 + yb, -yb, yc2 },
                  { -ya, -yb, ya + yb + yl, 0.0 } };
    for (int col = 0; col < 3;
         ++col) // Gaussian elimination, no pivoting needed (diagonal dominant)
        for (int row = col + 1; row < 3; ++row)
        {
            const C f = m[row][col] / m[col][col];
            for (int k = col; k < 4; ++k)
                m[row][k] -= f * m[col][k];
        }
    C x[3];
    for (int row = 2; row >= 0; --row)
    {
        C acc = m[row][3];
        for (int k = row + 1; k < 3; ++k)
            acc -= m[row][k] * x[k];
        x[row] = acc / m[row][row];
    }
    return test::db (std::abs (x[2]));
}

/// Small-signal response of the whole pedal (sustain 0, tiny input: the clippers are linear).
double pedalDb (float tone, double hz)
{
    Sargasso g;
    g.prepare ({ kFs, 512, 1 });
    g.setSettings ({ 0.0f, tone, 0.5f });
    return test::measureGainDb (
        [&] (AudioView v) { test::processChunked (v, [&] (AudioView c) { g.process (c); }); }, hz,
        kFs, 32768, 1.0e-5f);
}

std::vector<float> decayThrough (float sustain)
{
    Sargasso g;
    g.prepare ({ kFs, 512, 1 });
    g.setSettings ({ sustain, 0.5f, 0.5f });
    test::Buffer b (1, static_cast<int> (5.0 * kFs));
    test::fillDecayingTone (b[0], 196.0, kFs, 0.3f, 0.4);
    test::processInBlocks (b, 512, [&] (AudioView v) { g.process (v); });
    return b[0];
}
} // namespace

TEST_CASE ("Sargasso output stays finite at every rate, block size and setting",
           "[pedals][sargasso]")
{
    test::checkFiniteEverywhere<Sargasso> (
        [] (Random& r) { return Sargasso::Settings { r.unipolar(), r.unipolar(), r.unipolar() }; });
}

TEST_CASE ("Sargasso passes silence as silence", "[pedals][sargasso]")
{
    CHECK (test::silenceOutputPeak<Sargasso> (Sargasso::Settings {}, run) == 0.0f);
    CHECK (test::silenceOutputPeak<Sargasso> (Sargasso::Settings { 1.0f, 1.0f, 1.0f }, run) ==
           0.0f);
}

TEST_CASE ("Sargasso does not depend on how the host slices the audio", "[pedals][sargasso]")
{
    for (double fs : test::kDriveRates)
        CHECK (test::blockSizeDeviation<Sargasso> ({ 0.7f, 0.4f, 0.5f }, fs) < 1.0e-5);
}

TEST_CASE ("Sargasso's tone network is the passive low-pass/high-pass blend", "[pedals][sargasso]")
{
    // The transfer function agrees with the circuit solved directly.
    for (float tone : { 0.0f, 0.2f, 0.5f, 0.8f, 1.0f })
        for (double hz : { 50.0, 100.0, 400.0, 1000.0, 3000.0, 8000.0, 15000.0 })
            CHECK (networkDb (tone, hz) == Approx (circuitDb (tone, hz)).margin (0.01));

    // Noon: a mid scoop centred near 1 kHz, about 9 dB below the shelves on either side.
    double minDb = 0.0, minHz = 0.0;
    for (double hz = 200.0; hz <= 6000.0; hz *= 1.01)
        if (networkDb (0.5f, hz) < minDb)
        {
            minDb = networkDb (0.5f, hz);
            minHz = hz;
        }
    const double shelves = 0.5 * (networkDb (0.5f, 100.0) + networkDb (0.5f, 8000.0));
    CHECK (minHz > 900.0);
    CHECK (minHz < 1150.0);
    CHECK (shelves - minDb > 8.0);
    CHECK (shelves - minDb < 10.0);
    CHECK (std::abs (networkDb (0.5f, 100.0) - networkDb (0.5f, 8000.0)) < 1.0);

    // The ends: the low-pass alone (bass) and the high-pass alone (treble).
    CHECK (networkDb (0.0f, 100.0) - networkDb (0.0f, 8000.0) > 20.0);
    CHECK (networkDb (1.0f, 8000.0) - networkDb (1.0f, 100.0) > 15.0);
    CHECK (networkDb (0.0f, 100.0) > -3.5);
    CHECK (networkDb (1.0f, 8000.0) > -1.0);
}

TEST_CASE ("Sargasso's digital tone control matches the analogue network", "[pedals][sargasso]")
{
    // Turning the tone knob changes the pedal's small-signal response by exactly what the network
    // changes; everything else in the chain cancels in the difference. The bilinear transform's
    // frequency warping (exact at 1 kHz) costs about half a dB at 6 kHz.
    for (float tone : { 0.0f, 0.25f, 0.75f, 1.0f })
        for (double hz : { 100.0, 400.0, 1000.0, 3000.0, 6000.0 })
        {
            const double pedal = pedalDb (tone, hz) - pedalDb (0.5f, hz);
            const double network = networkDb (tone, hz) - networkDb (0.5f, hz);
            CHECK (pedal == Approx (network).margin (hz > 4000.0 ? 0.75 : 0.3));
        }
}

TEST_CASE ("Sargasso's sustain holds a decaying note longer", "[pedals][sargasso]")
{
    // A plucked note (0.4 s decay): the clippers hold it at full level until the input falls below
    // their threshold, which more sustain pushes further down. Measured to 20 dB below the peak.
    double previous = 0.0;
    for (float sustain : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const double t = test::decaySeconds (decayThrough (sustain), kFs, 20.0);
        CHECK (t > previous + 0.2);
        previous = t;
    }
    CHECK (test::decaySeconds (decayThrough (1.0f), kFs, 20.0) >
           1.8 * test::decaySeconds (decayThrough (0.0f), kFs, 20.0));
    // The input alone falls 20 dB in 0.92 s; even at minimum the fuzz holds it longer.
    CHECK (test::decaySeconds (decayThrough (0.0f), kFs, 20.0) > 1.2);
}

TEST_CASE ("Sargasso's clipping is nearly symmetric: odd harmonics", "[pedals][sargasso]")
{
    for (float sustain : { 0.2f, 0.6f, 1.0f })
    {
        Sargasso g;
        g.prepare ({ kFs, 512, 1 });
        g.setSettings ({ sustain, 0.5f, 0.5f });
        test::Buffer b (1, 65536);
        test::fillSine (b[0], 196.0, kFs, 0.1f);
        test::processInBlocks (b, 512, [&] (AudioView v) { g.process (v); });
        const auto h = test::harmonicsOf (b[0].data() + 32768, 32768, 196.0, kFs);
        CHECK (h.h[3] > 0.1);
        CHECK (h.h[2] < 0.25 * h.h[3]);
    }
}

TEST_CASE ("Sargasso volume 0.5 is level-matched across the sustain range", "[pedals][sargasso]")
{
    for (float sustain : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        Sargasso g;
        g.prepare ({ kFs, 512, 1 });
        g.setSettings ({ sustain, 0.5f, 0.5f });
        test::Buffer b (1, 48000);
        test::fillGuitarTone (b[0], kFs);
        const float in = test::rms (b[0], 24000);
        test::processInBlocks (b, 512, [&] (AudioView v) { g.process (v); });
        CHECK (test::db (test::rms (b[0], 24000) / in) == Approx (0.0).margin (1.0));
    }
}

TEST_CASE ("Sargasso keeps aliasing low on high notes at full sustain", "[pedals][sargasso]")
{
    for (double fs : { 44100.0, 48000.0 })
        for (double f0 : { 1318.5, 2637.0 })
        {
            Sargasso m;
            m.prepare ({ fs, 512, 1 });
            m.setSettings ({ 1.0f, 0.5f, 0.5f });
            test::Buffer b (1, 49152);
            test::fillSine (b[0], f0, fs, 0.3f);
            test::processInBlocks (b, 512, [&] (AudioView v) { m.process (v); });
            CHECK (test::worstInharmonicDb (b[0].data() + 16384, 32768, f0, fs) < -60.0);
        }
}

TEST_CASE ("Sargasso reports the oversampler's latency", "[pedals][sargasso]")
{
    for (double fs : test::kDriveRates)
    {
        Oversampler os;
        os.prepare (512, 1, Oversampler::factorLog2For (fs));
        Sargasso g;
        g.prepare ({ fs, 512, 1 });
        CHECK (g.latencySamples() == os.latencySamples());
    }
}

TEST_CASE ("Sargasso automation is click-free", "[pedals][sargasso]")
{
    int which = 0;
    const auto check = [&which] (auto settingsAt) {
        const auto r = test::measureSweep<Sargasso> (settingsAt, run);
        INFO ("sweep " << which++);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return Sargasso::Settings { t, 0.5f, 0.5f }; });
    check ([] (float t) { return Sargasso::Settings { 0.5f, t, 0.5f }; });
    check ([] (float t) { return Sargasso::Settings { 0.5f, 0.5f, t }; });
    check ([] (float t) { return Sargasso::Settings { t, 1.0f - t, t }; });
}
