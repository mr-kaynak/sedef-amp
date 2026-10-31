// SPDX-License-Identifier: AGPL-3.0-or-later
// Hidden benchmarks and measurements: run with `sedef_engine_tests "[reverb-bench]"`.
#include "ReverbTestUtils.h"
#include "TestUtils.h"
#include "sedef/dsp/Fathom.h"
#include "sedef/dsp/PitchShifter.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>

using namespace sedef;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 64;

template <typename Module>
double nsPerFrame (Module& m, double seconds)
{
    test::Buffer b (2, static_cast<int> (seconds * kFs));
    test::fillNoise (b[0], 0.3f, 1);
    test::fillNoise (b[1], 0.3f, 2);
    const auto t0 = std::chrono::steady_clock::now();
    for (int pos = 0; pos < b.frames(); pos += kBlock)
        m.process (b.view().subView (pos, std::min (kBlock, b.frames() - pos)));
    const auto t1 = std::chrono::steady_clock::now();
    REQUIRE (test::allFinite (b[0]));
    return std::chrono::duration<double, std::nano> (t1 - t0).count() / b.frames();
}

void report (const char* name, double ns)
{
    std::printf ("  %-34s %7.1f ns/frame  %5.2f %% of one core (48 kHz stereo)\n", name, ns,
                 ns * kFs * 1.0e-9 * 100.0);
}

dsp::Fathom::Settings wet (int mode, float decay)
{
    dsp::Fathom::Settings s;
    s.enabled = true;
    s.mode = mode;
    s.mix = 1.0f;
    s.decaySeconds = decay;
    s.preDelayMs = 1.0f;
    return s;
}
} // namespace

TEST_CASE ("Reverb and transpose benchmarks", "[.bench][reverb-bench]")
{
    std::printf ("\nCPU, 48 kHz, %d-sample blocks, stereo noise:\n", kBlock);
    for (int mode : { dsp::Fathom::Plate, dsp::Fathom::Hall })
    {
        dsp::Fathom f;
        auto s = wet (mode, 3.0f);
        s.mix = 0.3f;
        f.setSettings (s);
        f.prepare ({ kFs, kBlock, 2 });
        report (mode == dsp::Fathom::Plate ? "Fathom plate" : "Fathom hall", nsPerFrame (f, 20.0));
    }
    for (int semis : { -2, -12, 12 })
    {
        dsp::PitchShifter p;
        p.setSettings ({ semis });
        p.prepare ({ kFs, kBlock, 2 });
        char name[64];
        std::snprintf (name, sizeof (name), "PitchShifter %+d st", semis);
        report (name, nsPerFrame (p, 20.0));
    }
    {
        dsp::PitchShifter p;
        p.prepare ({ kFs, kBlock, 2 });
        report ("PitchShifter 0 st (bypass)", nsPerFrame (p, 20.0));
    }
}

TEST_CASE ("Reverb measurements", "[.bench][reverb-bench]")
{
    std::printf ("\nFathom RT60 (T30, 350-1400 Hz band, 48 kHz):\n");
    for (int mode : { dsp::Fathom::Plate, dsp::Fathom::Hall })
        for (float decay : { 1.0f, 2.0f, 5.0f, 10.0f })
        {
            dsp::Fathom f;
            f.setSettings (wet (mode, decay));
            f.prepare ({ kFs, 512, 2 });
            test::Buffer b (2, static_cast<int> ((1.3 * decay + 0.3) * kFs));
            b[0][0] = b[1][0] = 1.0f;
            for (int pos = 0; pos < b.frames(); pos += 512)
                f.process (b.view().subView (pos, std::min (512, b.frames() - pos)));
            test::bandLimit (b[0], kFs, 350.0, 1400.0);
            test::bandLimit (b[1], kFs, 350.0, 1400.0);
            const double rt = test::rt60FromDecay (b[0], b[1], kFs);
            std::printf ("  %-5s set %5.1f s  measured %6.2f s  (%+5.1f %%)\n",
                         mode == 0 ? "plate" : "hall", static_cast<double> (decay), rt,
                         100.0 * (rt / decay - 1.0));
        }

    std::printf ("\nFathom wet level and stereo correlation (mono noise in, rms 0.29):\n");
    for (int mode : { dsp::Fathom::Plate, dsp::Fathom::Hall })
        for (float decay : { 1.0f, 2.5f, 10.0f })
        {
            dsp::Fathom f;
            f.setSettings (wet (mode, decay));
            f.prepare ({ kFs, 256, 2 });
            test::Buffer b (2, static_cast<int> (4.0 * kFs));
            test::fillNoise (b[0], 0.5f, 5);
            std::copy (b[0].begin(), b[0].end(), b[1].begin());
            const float in = test::rms (b[0]);
            for (int pos = 0; pos < b.frames(); pos += 256)
                f.process (b.view().subView (pos, 256));
            const auto start = static_cast<size_t> (2.0 * kFs);
            const float outL = test::rms (b[0], start), outR = test::rms (b[1], start);
            const double rho = test::correlation (b[0].data() + start, b[1].data() + start,
                                                  b.frames() - static_cast<int> (start));
            std::printf ("  %-5s decay %4.1f s  wet/in %+5.1f dB (L) %+5.1f dB (R)  corr %+.3f\n",
                         mode == 0 ? "plate" : "hall", static_cast<double> (decay),
                         20.0 * std::log10 (outL / in), 20.0 * std::log10 (outR / in), rho);
        }

    std::printf ("\nPitchShifter, 220 Hz sine (mono, 48 kHz):\n");
    for (int semis : { -12, -7, -5, -3, -2, -1, 1, 2, 3, 5, 7, 12 })
    {
        dsp::PitchShifter p;
        p.setSettings ({ semis });
        p.prepare ({ kFs, 256, 1 });
        test::Buffer b (1, static_cast<int> (2.5 * kFs));
        test::fillSine (b[0], 220.0, kFs, 0.5f);
        for (int pos = 0; pos < b.frames(); pos += 256)
            p.process (b.view().subView (pos, std::min (256, b.frames() - pos)));
        const int fft = 131072;
        const auto start = static_cast<size_t> (0.5 * kFs);
        const auto mag =
            test::magnitudeSpectrum (b[0].data() + start, static_cast<int> (2.0 * kFs), fft);
        const double expected = 220.0 * std::exp2 (semis / 12.0);
        const auto peak = test::strongestPeak (mag, kFs, fft, 40.0, 4000.0);
        double inBand = 0.0, total = 0.0;
        for (size_t k = 1; k < mag.size(); ++k)
        {
            const double hz = static_cast<double> (k) * kFs / fft;
            total += mag[k] * mag[k];
            if (std::abs (hz - expected) < 0.03 * expected)
                inBand += mag[k] * mag[k];
        }
        std::printf ("  %+3d st  expected %7.2f Hz  peak %7.2f Hz  (%+.3f %%)  "
                     "energy within 3 %%: %.2f %%\n",
                     semis, expected, peak.hz, 100.0 * (peak.hz / expected - 1.0),
                     100.0 * inBand / total);
    }

    // Splice artefacts on polyphonic material: a G major triad of sawtooth-like notes (8
    // harmonics each, 1/k amplitudes). Energy away from the expected partials (more than 0.6 %
    // off any of them), relative to the total.
    std::printf ("\nPitchShifter, G major triad with harmonics: residue away from the partials\n");
    const std::vector<double> chord { 196.0, 246.94, 293.66 };
    for (int semis : { -12, -5, -2, -1, 2, 5, 12 })
    {
        dsp::PitchShifter p;
        p.setSettings ({ semis });
        p.prepare ({ kFs, 256, 1 });
        test::Buffer b (1, static_cast<int> (3.0 * kFs));
        std::vector<float> partial (b[0].size());
        std::vector<double> expected;
        const double ratio = std::exp2 (semis / 12.0);
        for (double f0 : chord)
            for (int k = 1; k <= 8; ++k)
            {
                test::fillSine (partial, f0 * k, kFs, 0.15f / static_cast<float> (k), 0.7 * k + f0);
                for (size_t i = 0; i < partial.size(); ++i)
                    b[0][i] += partial[i];
                expected.push_back (f0 * k * ratio);
            }
        for (int pos = 0; pos < b.frames(); pos += 256)
            p.process (b.view().subView (pos, std::min (256, b.frames() - pos)));
        const int fft = 131072;
        const auto start = static_cast<size_t> (0.5 * kFs);
        const auto mag =
            test::magnitudeSpectrum (b[0].data() + start, static_cast<int> (2.5 * kFs), fft);
        double residue = 0.0, total = 0.0;
        for (size_t k = 1; k < mag.size(); ++k)
        {
            const double hz = static_cast<double> (k) * kFs / fft;
            const double e = mag[k] * mag[k];
            total += e;
            bool near = false;
            for (double f : expected)
                near = near || std::abs (hz - f) < 0.006 * f + 1.0;
            if (!near)
                residue += e;
        }
        std::printf ("  %+3d st  residue %6.1f dB\n", semis, 10.0 * std::log10 (residue / total));
    }
}
