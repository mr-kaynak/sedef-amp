// SPDX-License-Identifier: AGPL-3.0-or-later
// Prints the kit's and the grooves' measurements (docs/drums.md). Hidden: run with
//   sedef_engine_tests "[drums-probe]"
#include "DrumsTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>

using namespace sedef;
using namespace sedef::test;
using namespace sedef::test::drums;

namespace
{
const char* nameOf (Instrument i)
{
    static const char* names[] = { "kick",  "snare",    "hat closed", "hat open", "ride",
                                   "crash", "tom high", "tom mid",    "tom floor" };
    return names[static_cast<int> (i)];
}

/// Seconds until the 1 ms peak envelope stays below `db` under the stroke's peak.
double decayTo (const std::vector<float>& v, double fs, double db)
{
    const float p = peak (v);
    const float limit = p * static_cast<float> (std::pow (10.0, db / 20.0));
    for (size_t i = v.size(); i-- > 0;)
        if (std::abs (v[i]) > limit)
            return static_cast<double> (i) / fs;
    return 0.0;
}

double centroid (const std::vector<float>& v, double fs, int frames)
{
    double num = 0.0, den = 0.0;
    for (double hz = 40.0; hz < 16000.0; hz *= 1.06)
    {
        const double m =
            toneMagnitude (v.data(), std::min (frames, static_cast<int> (v.size())), hz, fs);
        num += hz * m * m;
        den += m * m;
    }
    return den > 0.0 ? num / den : 0.0;
}
} // namespace

TEST_CASE ("Drum kit and groove measurements", "[.drums-probe]")
{
    const double fs = 48000.0;
    std::printf ("%-11s %4s %8s %8s %8s %9s %9s\n", "instrument", "vel", "peakL dB", "peakR dB",
                 "-40 dB s", "-60 dB s", "centroid");
    for (int i = 0; i < 9; ++i)
        for (int vel : { 124, 72, 40 })
        {
            const auto inst = static_cast<Instrument> (i);
            const auto r = strikeOnce (inst, vel, fs, static_cast<int> (fs * 4.0));
            std::vector<float> mono (r.left.size());
            for (size_t k = 0; k < mono.size(); ++k)
                mono[k] = r.left[k] + r.right[k];
            std::printf ("%-11s %4d %8.1f %8.1f %8.3f %9.3f %9.0f\n", nameOf (inst), vel,
                         gainToDb (peak (r.left)), gainToDb (peak (r.right)),
                         decayTo (mono, fs, -40.0), decayTo (mono, fs, -60.0),
                         centroid (mono, fs, 4800));
        }

    std::printf ("\n%-18s %3s %8s %8s %8s\n", "groove", "int", "peak dB", "rms dB", "events");
    const auto list = sedef::dsp::drums::grooves();
    for (int g = 0; g < static_cast<int> (list.size()); ++g)
        for (int intensity : { 1, 3, 5 })
        {
            GrooveSetup setup;
            setup.groove = g;
            setup.intensity = intensity;
            setup.fillEvery = 2;
            const int frames = barFrames (fs, 120.0, list[static_cast<size_t> (g)].signature, 4);
            const auto r = renderGroove (setup, frames, 128);
            std::printf ("%-18s %3d %8.1f %8.1f %8zu\n",
                         std::string (list[static_cast<size_t> (g)].name).c_str(), intensity,
                         gainToDb (std::max (peak (r.left), peak (r.right))),
                         gainToDb (std::max (rms (r.left), rms (r.right))), r.events.size());
        }

    // Cost: the busiest groove, best of three.
    GrooveSetup busy;
    busy.groove = 3;
    busy.intensity = 5;
    busy.fillEvery = 2;
    busy.bpm = 200.0f;
    const int frames = static_cast<int> (fs * 20.0);
    double best = 1.0e9;
    for (int run = 0; run < 3; ++run)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = renderGroove (busy, frames, 128);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min (best, std::chrono::duration<double> (t1 - t0).count());
        REQUIRE (allFinite (r.left));
    }
    std::printf ("\nbusiest groove at 200 bpm: %.3f %% of one core\n", 100.0 * best / 20.0);
}
