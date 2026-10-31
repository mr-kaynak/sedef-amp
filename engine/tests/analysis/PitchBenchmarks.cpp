// SPDX-License-Identifier: AGPL-3.0-or-later
// Hidden: run with "[.bench]" or "[.report]".
#include "PitchTestSignals.h"
#include "TestUtils.h"
#include "sedef/analysis/PitchDetector.h"
#include "sedef/analysis/PitchTracker.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>

using namespace sedef;

namespace
{
using Clock = std::chrono::steady_clock;

double secondsSince (Clock::time_point t0)
{
    return std::chrono::duration<double> (Clock::now() - t0).count();
}
} // namespace

TEST_CASE ("PitchDetector and PitchTracker cost", "[.bench][pitch-bench]")
{
    const double fs = 12000.0;
    analysis::PitchDetector det;
    det.prepare (fs);
    std::printf ("\nPitchDetector::detect at %.0f Hz\n", fs);
    for (double hz : { 30.87, 82.41, 196.0, 659.26 })
    {
        auto tone = test::harmonicTone (hz, fs, static_cast<size_t> (det.windowSize()) * 2);
        for (int n : { 480, 1200, det.windowSize() })
        {
            const int reps = 2000;
            volatile float sink = 0.0f;
            const auto t0 = Clock::now();
            for (int i = 0; i < reps; ++i)
                sink = sink + det.detect (tone.data() + (i % 64), n).frequencyHz;
            std::printf ("  %7.2f Hz  window %4d: %6.2f us\n", hz, n,
                         1.0e6 * secondsSince (t0) / reps);
        }
    }

    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        // Ten seconds of re-plucked strings so every code path (onsets, settling) runs.
        std::vector<float> sig (static_cast<size_t> (10.0 * rate), 0.0f);
        const double notes[] = { 82.41,  110.0,  146.83, 196.0, 246.94,
                                 329.63, 659.26, 41.2,   30.87, 1318.5 };
        for (int i = 0; i < 10; ++i)
        {
            test::Pluck p;
            p.f1 = notes[i];
            p.maxPartialHz = 5000.0;
            test::addPluck (sig, rate, static_cast<size_t> (i * rate), p);
        }
        analysis::PitchTracker tracker;
        tracker.prepare (rate);
        const int block = 256;
        const auto t0 = Clock::now();
        for (size_t s = 0; s + static_cast<size_t> (block) <= sig.size();
             s += static_cast<size_t> (block))
            tracker.push (sig.data() + s, block);
        const double elapsed = secondsSince (t0);
        const double hops = 10.0 / 0.005; // detections at ~200 per second
        std::printf (
            "PitchTracker at %6.0f Hz: %.2f%% of one core (%.1f us per hop incl. decimation)\n",
            rate, 100.0 * elapsed / 10.0, 1.0e6 * elapsed / hops);
    }
}

TEST_CASE ("PitchTracker accuracy report", "[.report][pitch-report]")
{
    struct Note
    {
        const char* name;
        double hz, b;
    };
    const Note notes[] = { { "B0", 30.87, 1.0e-4 },  { "E1", 41.2, 1.0e-4 },
                           { "E2", 82.41, 1.0e-4 },  { "A2", 110.0, 1.0e-4 },
                           { "D3", 146.83, 1.0e-4 }, { "G3", 196.0, 1.0e-4 },
                           { "B3", 246.94, 1.0e-4 }, { "E4", 329.63, 1.0e-4 },
                           { "E5", 659.26, 4.0e-4 }, { "E6", 1318.5, 4.0e-4 } };
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        std::printf ("\nfs = %.0f Hz (pluck +9 cents glide, B as listed, noise -75 dBFS)\n", fs);
        std::printf ("note      Hz      B  firstValid  firstStable  err@first  max|err|  err@end  "
                     "std@end  right-note\n");
        for (const auto& n : notes)
        {
            test::Pluck p;
            p.f1 = n.hz;
            p.inharmonicity = n.b;
            p.glideCents = 9.0;
            p.decaySeconds = n.hz > 300.0 ? 1.2 : 2.0;
            p.seed = static_cast<std::uint32_t> (n.hz);
            std::vector<float> sig (static_cast<size_t> (2.2 * fs), 0.0f);
            test::addPluck (sig, fs, static_cast<size_t> (0.2 * fs), p);
            test::addNoise (sig, -75.0f);
            analysis::PitchTracker tracker;
            tracker.prepare (fs);
            const auto run = test::runTracker (tracker, sig, fs, 0.2, n.hz);
            std::printf (
                "%-4s %8.2f %6.0e  %7.1f ms  %8.1f ms  %+8.3f  %8.3f  %+7.3f  %7.3f  %5.1f%%\n",
                n.name, n.hz, n.b, 1000.0 * run.firstValid, 1000.0 * run.firstStable,
                run.firstStableCents, run.maxStableCents, run.lastStableCents, run.lastStableStd,
                run.validAfterSkip > 0 ? 100.0 * run.rightNoteAfterSkip / run.validAfterSkip : 0.0);
        }
    }

    // Single-shot detector error on steady harmonic tones at the analysis rates.
    for (double fs : { 11025.0, 12000.0 })
    {
        analysis::PitchDetector det;
        det.prepare (fs);
        std::printf (
            "\nPitchDetector, full window, fs = %.0f Hz: worst / mean error (cents) over 40 hops\n",
            fs);
        for (double hz :
             { 30.87, 41.2, 82.41, 110.0, 146.83, 196.0, 246.94, 329.63, 659.26, 1318.5 })
        {
            const int hop = static_cast<int> (0.005 * fs);
            auto tone =
                test::harmonicTone (hz, fs, static_cast<size_t> (det.windowSize() + 40 * hop));
            test::addNoise (tone, -70.0f);
            double worst = 0.0, sum = 0.0;
            for (int i = 0; i < 40; ++i)
            {
                const double err = test::centsError (
                    det.detect (tone.data() + i * hop, det.windowSize()).frequencyHz, hz);
                worst = std::max (worst, std::abs (err));
                sum += err;
            }
            std::printf ("  %8.2f Hz  %.4f  %+.4f\n", hz, worst, sum / 40);
        }
    }
}
