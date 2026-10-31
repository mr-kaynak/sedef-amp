// SPDX-License-Identifier: AGPL-3.0-or-later
// Hidden benchmark: sedef_engine_tests "[.bench]"
#include "AmpTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>

using namespace sedef;
using dsp::AmpSection;

namespace
{
double nsPerSample (int model, int channels, double fs, bool switching)
{
    const int block = 64;
    const auto source = test::guitarStrum (fs, 4.0);
    AmpSection amp;
    AmpSection::Settings s;
    s.model = model;
    test::prepareAmp (amp, s, fs, block);
    amp.prepare ({ fs, block, channels });

    test::Buffer buf (channels, block);
    const auto run = [&] (size_t frames)
    {
        size_t pos = 0;
        int blockIndex = 0;
        for (size_t done = 0; done < frames; done += block, ++blockIndex)
        {
            if (switching && blockIndex % 16 == 0)
            {
                // Keep a crossfade running permanently (30 ms fade, a new target every ~21 ms).
                s.model = (s.model + 1) % 3;
                amp.setSettings (s);
            }
            for (int c = 0; c < channels; ++c)
                std::copy_n (source.begin() + static_cast<long> (pos), block, buf[c].begin());
            pos = (pos + block) % (source.size() - block);
            amp.process (buf.view());
        }
    };
    run (static_cast<size_t> (fs)); // warm up
    const auto frames = static_cast<size_t> (fs * 10.0);
    const auto t0 = std::chrono::steady_clock::now();
    run (frames);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano> (t1 - t0).count() /
           static_cast<double> (frames);
}
} // namespace

TEST_CASE ("Amp section CPU cost", "[.bench]")
{
    const char* names[] = { "Lagoon", "Reef", "Abyss" };
    for (double fs : { 48000.0, 96000.0 })
    {
        for (int channels : { 1, 2 })
            for (int m = 0; m < 3; ++m)
            {
                const double ns = nsPerSample (m, channels, fs, false);
                std::printf (
                    "%-6s %.0f Hz %s: %6.1f ns/sample  (%.2f%% of one core in real time)\n",
                    names[m], fs, channels == 1 ? "mono  " : "stereo", ns, ns * fs * 1.0e-7);
            }
        const double ns = nsPerSample (0, 1, fs, true);
        std::printf ("crossfading %.0f Hz mono: %6.1f ns/sample  (%.2f%%)\n", fs, ns,
                     ns * fs * 1.0e-7);
    }
}
