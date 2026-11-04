// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PedalTestUtils.h"
#include "sedef/core/Oversampler.h"
#include "sedef/dsp/Kelp.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>

using namespace sedef;
using Catch::Approx;
using dsp::Kelp;

namespace
{
constexpr double kFs = 48000.0;

void run (Kelp& k, AudioView v)
{
    k.process (v);
}

/// Small-signal gain (dB) at one frequency: far below the diodes' knee, so the stage is linear.
double responseDb (const Kelp::Settings& s, double hz, double fs = kFs)
{
    Kelp k;
    k.prepare ({ fs, 512, 1 });
    k.setSettings (s);
    return test::measureGainDb (
        [&] (AudioView v) { test::processChunked (v, [&] (AudioView c) { k.process (c); }); }, hz,
        fs, 32768, 0.0003f);
}

struct Harmonics
{
    double h[8] {}; ///< magnitude of harmonics 1..7 (index = harmonic)
    float positivePeak = 0.0f, negativePeak = 0.0f;
    double thd = 0.0;
};

Harmonics harmonics (float drive, double hz = 440.0, float amp = 0.1f)
{
    Kelp k;
    k.prepare ({ kFs, 512, 1 });
    k.setSettings ({ drive, 1.0f, 0.5f });
    test::Buffer b (1, 49152);
    test::fillSine (b[0], hz, kFs, amp);
    test::processInBlocks (b, 512, [&] (AudioView v) { k.process (v); });
    Harmonics r;
    const float* d = b[0].data() + 16384;
    for (int n = 1; n <= 7; ++n)
        r.h[n] = test::toneMagnitude (d, 32768, n * hz, kFs);
    for (int i = 0; i < 32768; ++i)
    {
        r.positivePeak = std::max (r.positivePeak, d[i]);
        r.negativePeak = std::max (r.negativePeak, -d[i]);
    }
    r.thd = test::thd (d, 32768, hz, kFs);
    return r;
}

double levelDb (const Kelp::Settings& s)
{
    Kelp k;
    k.prepare ({ kFs, 512, 1 });
    k.setSettings (s);
    test::Buffer b (1, 48000);
    test::fillGuitarTone (b[0], kFs);
    const float in = test::rms (b[0], 24000);
    test::processInBlocks (b, 512, [&] (AudioView v) { k.process (v); });
    return test::db (test::rms (b[0], 24000) / in);
}

/// Frequency of the largest small-signal response between 200 Hz and 4 kHz (third-octave steps
/// refined to 1/24 octave).
double humpCentreHz (const Kelp::Settings& s)
{
    double best = 0.0, bestHz = 0.0;
    for (double hz = 200.0; hz <= 4000.0; hz *= std::pow (2.0, 1.0 / 24.0))
    {
        const double g = responseDb (s, hz);
        if (g > best || bestHz == 0.0)
        {
            best = g;
            bestHz = hz;
        }
    }
    return bestHz;
}
} // namespace

TEST_CASE ("Kelp output stays finite for random settings and full-scale noise", "[pedals][kelp]")
{
    test::checkFiniteWithRandomSettings<Kelp> (
        [] (Random& r) { return Kelp::Settings { r.unipolar(), r.unipolar(), r.unipolar() }; },
        run);
}

TEST_CASE ("Kelp passes silence as silence", "[pedals][kelp]")
{
    CHECK (test::silenceOutputPeak<Kelp> (Kelp::Settings {}, run) == 0.0f);
    CHECK (test::silenceOutputPeak<Kelp> (Kelp::Settings { 1.0f, 1.0f, 1.0f }, run) == 0.0f);
}

TEST_CASE ("Kelp's feedback diodes solve the diode law", "[pedals][kelp]")
{
    // v + a sinh v = w across six decades of w and the whole drive range, and the antiderivative
    // that the anti-aliasing uses really is the integral of the solution.
    for (float drive : { 0.0f, 0.5f, 1.0f })
    {
        const double a = 2.0 * 2.52e-9 * Kelp::feedbackResistance (drive) / (1.752 * 0.025852);
        for (double w = 1.0e-3; w < 1.0e4; w *= 1.7)
            for (double sign : { 1.0, -1.0 })
            {
                const double v = Kelp::FeedbackDiodes::solve (sign * w, a);
                CHECK (std::abs (v + a * std::sinh (v) - sign * w) < 1.0e-9 * w);
                const double h = 1.0e-4 * w;
                const double slope = (Kelp::FeedbackDiodes::antiderivative (sign * w + h, a) -
                                      Kelp::FeedbackDiodes::antiderivative (sign * w - h, a)) /
                                     (2.0 * h);
                CHECK (slope == Approx (v).epsilon (1.0e-6).margin (1.0e-9));
            }
    }
}

TEST_CASE ("Kelp humps the mids and cuts the bass", "[pedals][kelp]")
{
    // Small-signal response at drive 0 (the clean boost) and noon, tone at noon: the 720 Hz
    // high-pass ahead of the clipping and the 723 Hz low-pass after it put the peak near 700 Hz to
    // 1 kHz, with 100 Hz at least 9 dB and 4 kHz at least 3 dB below it.
    for (float drive : { 0.0f, 0.5f })
    {
        INFO ("drive " << drive);
        const Kelp::Settings s { drive, 0.5f, 0.5f };
        const double centre = humpCentreHz (s);
        CHECK (centre > 600.0);
        CHECK (centre < 1200.0);
        const double peak = responseDb (s, centre);
        CHECK (responseDb (s, 100.0) < peak - 9.0);
        CHECK (responseDb (s, 4000.0) < peak - 3.0);
    }
}

TEST_CASE ("Kelp's tone brightens clockwise", "[pedals][kelp]")
{
    const auto highs = [] (float tone) {
        const Kelp::Settings s { 0.0f, tone, 0.5f };
        return responseDb (s, 4000.0) - responseDb (s, 800.0);
    };
    CHECK (highs (0.0f) < highs (0.5f) - 4.0);
    CHECK (highs (0.5f) < highs (1.0f) - 4.0);
}

TEST_CASE ("Kelp clips symmetrically and harder as the drive rises", "[pedals][kelp]")
{
    // Two matched diodes: the positive and negative halves clip alike, so even harmonics stay
    // 40 dB under the fundamental at every drive. THD rises with the drive and the clipping gets
    // harder: the 5th harmonic gains on the 3rd.
    double previousThd = 0.0, previousRatio = -1.0e9;
    for (float drive : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        INFO ("drive " << drive);
        const auto h = harmonics (drive);
        CHECK (test::db (h.h[2] / h.h[1]) < -40.0);
        CHECK (test::db (h.h[4] / h.h[1]) < -40.0);
        CHECK (h.positivePeak == Approx (h.negativePeak).epsilon (0.02));
        CHECK (h.thd > previousThd);
        const double ratio = test::db (h.h[5] / h.h[3]);
        CHECK (ratio > previousRatio);
        previousThd = h.thd;
        previousRatio = ratio;
    }
}

TEST_CASE ("Kelp at drive 0 is a mid-pushed boost that barely clips", "[pedals][kelp]")
{
    // A single note at an ordinary picking level (-26 dBFS peak, 50 mV): under 3 % THD at drive 0,
    // and Level up gives at least 12 dB of boost.
    const auto light = harmonics (0.0f, 330.0, 0.05f);
    CHECK (light.thd < 0.03);
    CHECK (levelDb ({ 0.0f, 0.5f, 1.0f }) > 12.0);
}

TEST_CASE ("Kelp level 0.5 is close to unity across the drive range", "[pedals][kelp]")
{
    for (float drive : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        INFO ("drive " << drive);
        CHECK (levelDb ({ drive, 0.5f, 0.5f }) == Approx (0.0).margin (1.5));
    }
}

TEST_CASE ("Kelp keeps aliasing low on high notes at full drive", "[pedals][kelp]")
{
    for (double f0 : { 1318.5, 2637.0 })
    {
        INFO ("note " << f0);
        Kelp m;
        m.prepare ({ kFs, 512, 1 });
        m.setSettings ({ 1.0f, 1.0f, 0.5f });
        test::Buffer b (1, 49152);
        test::fillSine (b[0], f0, kFs, 0.3f);
        test::processInBlocks (b, 512, [&] (AudioView v) { m.process (v); });
        CHECK (test::worstInharmonicDb (b[0].data() + 16384, 32768, f0, kFs) < -60.0);
    }
}

TEST_CASE ("Kelp reports the oversampler's latency", "[pedals][kelp]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        Oversampler os;
        os.prepare (512, 1, Oversampler::factorLog2For (fs));
        Kelp k;
        k.prepare ({ fs, 512, 1 });
        CHECK (k.latencySamples() == os.latencySamples());
    }
}

TEST_CASE ("Kelp's clean and clipping paths are time-aligned", "[pedals][kelp]")
{
    // At low level both paths are linear and overlap below the hump; misaligned by the
    // oversampler's latency they would comb-filter. Aligned, the response is one smooth hump
    // (1/12-octave steps from 150 Hz to 6 kHz).
    for (double fs : { 48000.0, 96000.0 })
    {
        std::vector<double> response;
        for (double hz = 150.0; hz <= 6000.0; hz *= std::pow (2.0, 1.0 / 12.0))
            response.push_back (responseDb ({ 0.0f, 0.5f, 0.5f }, hz, fs));
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

TEST_CASE ("Kelp automation is click-free", "[pedals][kelp]")
{
    const auto check = [] (auto settingsAt) {
        const auto r = test::measureSweep<Kelp> (settingsAt, run);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return Kelp::Settings { t, 0.5f, 0.5f }; });
    check ([] (float t) { return Kelp::Settings { 0.5f, t, 0.5f }; });
    check ([] (float t) { return Kelp::Settings { 0.5f, 0.5f, t }; });
    check ([] (float t) { return Kelp::Settings { t, 1.0f - t, t }; });
}

TEST_CASE ("Kelp measurements", "[.report]")
{
    std::printf ("Kelp small-signal response (dB), tone noon, level noon\n");
    std::printf ("drive   100 Hz  200 Hz  400 Hz  720 Hz   1 kHz   2 kHz   4 kHz  centre\n");
    for (float drive : { 0.0f, 0.5f, 1.0f })
    {
        const Kelp::Settings s { drive, 0.5f, 0.5f };
        std::printf ("%4.2f ", static_cast<double> (drive));
        for (double hz : { 100.0, 200.0, 400.0, 720.0, 1000.0, 2000.0, 4000.0 })
            std::printf (" %+7.1f", responseDb (s, hz));
        std::printf ("  %4.0f Hz\n", humpCentreHz (s));
    }
    std::printf ("tone (drive 0)  4 kHz - 800 Hz\n");
    for (float tone : { 0.0f, 0.5f, 1.0f })
        std::printf ("%4.2f  %+6.1f dB\n", static_cast<double> (tone),
                     responseDb ({ 0.0f, tone, 0.5f }, 4000.0) -
                         responseDb ({ 0.0f, tone, 0.5f }, 800.0));
    std::printf ("Harmonics, 440 Hz sine at -20 dBFS peak, tone 1 (dB re fundamental)\n");
    std::printf ("drive    THD     H2      H3      H4      H5      H7   +peak/-peak\n");
    for (float drive : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const auto h = harmonics (drive);
        std::printf ("%4.2f  %5.1f %%  %6.1f  %6.1f  %6.1f  %6.1f  %6.1f   %.3f\n",
                     static_cast<double> (drive), 100.0 * h.thd, test::db (h.h[2] / h.h[1]),
                     test::db (h.h[3] / h.h[1]), test::db (h.h[4] / h.h[1]),
                     test::db (h.h[5] / h.h[1]), test::db (h.h[7] / h.h[1]),
                     static_cast<double> (h.positivePeak / h.negativePeak));
    }
    std::printf ("Level 0.5 vs input (reference tone), tone noon\n");
    for (float drive : { 0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f })
        std::printf ("drive %.3f: %+5.2f dB\n", static_cast<double> (drive),
                     levelDb ({ drive, 0.5f, 0.5f }));
    std::printf ("drive 0, 330 Hz at -26 dBFS peak: THD %.2f %%; level 1 boost %+5.1f dB\n",
                 100.0 * harmonics (0.0f, 330.0, 0.05f).thd, levelDb ({ 0.0f, 0.5f, 1.0f }));
    for (double f0 : { 1318.5, 2637.0 })
    {
        Kelp m;
        m.prepare ({ kFs, 512, 1 });
        m.setSettings ({ 1.0f, 1.0f, 0.5f });
        test::Buffer b (1, 49152);
        test::fillSine (b[0], f0, kFs, 0.3f);
        test::processInBlocks (b, 512, [&] (AudioView v) { m.process (v); });
        std::printf ("aliasing at full drive, %.1f Hz: %.1f dB\n", f0,
                     test::worstInharmonicDb (b[0].data() + 16384, 32768, f0, kFs));
    }
}
