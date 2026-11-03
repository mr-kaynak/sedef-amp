// SPDX-License-Identifier: AGPL-3.0-or-later
// Hidden: "[.bench]" prints the cost of a standard WaveNet capture (docs/dsp/nam.md, Performance).

#include "CaptureTestUtils.h"
#include "sedef/dsp/capture/ReferenceStrum.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <ctime>

using namespace sedef;
namespace nt = sedef::test::nam;

namespace
{
double secondsSince (std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
}

/// Processor time of this (single-threaded) test: robust on a machine busy with other work.
double cpuSeconds()
{
    return static_cast<double> (std::clock()) / CLOCKS_PER_SEC;
}
} // namespace

TEST_CASE ("Capture benchmark", "[.bench][capture]")
{
    auto t0 = std::chrono::steady_clock::now();
    double c0 = cpuSeconds();
    const auto strum = dsp::capture::referenceStrum (48000.0, 6.0);
    std::printf ("reference strum, 6 s at 48 kHz: %.1f ms (cpu)\n", 1000.0 * (cpuSeconds() - c0));

    const auto text = nt::standardWaveNet (1);
    c0 = cpuSeconds();
    const auto f = nt::parseOrFail (text);
    std::printf ("parse + validate + build + measure, standard WaveNet: %.1f ms (cpu) "
                 "(measured %.1f dBFS, normalised by %+.1f dB)\n",
                 1000.0 * (cpuSeconds() - c0), static_cast<double> (f->measuredDbfs()),
                 static_cast<double> (f->normalisationDb()));

    for (const double fs : { 48000.0, 44100.0, 96000.0 })
        for (const int block : { 64, 256 })
        {
            nt::StageRig rig (fs, block);
            t0 = std::chrono::steady_clock::now();
            rig.load (f);
            const double instantiateMs = 1000.0 * secondsSince (t0);
            rig.settings.useCapture = true;
            std::vector<float> v (static_cast<std::size_t> (fs * 4.0));
            test::fillNoise (v, 0.3f, 1);
            rig.run (v, block); // warm-up
            test::fillNoise (v, 0.3f, 2);
            const double start = cpuSeconds();
            rig.run (v, block);
            const double s = cpuSeconds() - start;
            const double nsPerSample = 1.0e9 * s / static_cast<double> (v.size());
            std::printf (
                "standard WaveNet at %.1f kHz, %d-sample blocks: %.0f ns/sample, %.2f%% of "
                "one core (instantiate %.1f ms)\n",
                fs / 1000.0, block, nsPerSample, 100.0 * nsPerSample * fs * 1.0e-9, instantiateMs);
        }
}
