// SPDX-License-Identifier: AGPL-3.0-or-later
// CPU cost of the cab simulator. Hidden: sedef_engine_tests "[.bench]"
#include "CabTestUtils.h"
#include "sedef/core/Denormals.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>

using namespace sedef;
using namespace sedef::test;
using namespace sedef::test::cab;

namespace
{
double nsPerSample (CabSim& cab, double fs, int block, double seconds)
{
    ScopedNoDenormals noDenormals;
    std::vector<float> x (static_cast<size_t> (block));
    Buffer out (2, block);
    Random r (1);
    const int blocks = static_cast<int> (seconds * fs / block);
    // Warm up (crossfades settle, caches fill).
    for (int b = 0; b < 200; ++b)
    {
        for (auto& v : x)
            v = 0.3f * r.bipolar();
        cab.process (x.data(), out.view());
    }
    const auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
    {
        for (auto& v : x)
            v = 0.3f * r.bipolar();
        cab.process (x.data(), out.view());
    }
    const auto ns =
        std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - start).count();
    return ns / (static_cast<double> (blocks) * block);
}

void report (const char* what, double ns, double fs)
{
    // Share of one core in real time: ns per sample x samples per second.
    std::printf ("%-52s %7.1f ns/sample  %5.2f %% of a core at %.0f Hz\n", what, ns, ns * fs * 1e-7,
                 fs);
}
} // namespace

TEST_CASE ("CabSim benchmark", "[.bench]")
{
    const double fs = 48000.0;
    const int block = 64;
    const double seconds = 20.0;

    std::vector<float> ir (static_cast<size_t> (fs));
    fillNoise (ir, 1.0f, 3);
    for (size_t i = 0; i < ir.size(); ++i)
        ir[i] *= std::exp (-static_cast<float> (i) / (0.2f * static_cast<float> (fs))); // full 1 s

    CabSim::Settings procedural;
    procedural.cab = 2;
    procedural.mics[0].type = 0;
    procedural.mics[1].type = 8;
    procedural.mics[1].distance = 0.4f;
    procedural.mics[0].roomEnabled = true;
    procedural.mics[1].roomEnabled = true;

    {
        CabSim cab;
        cab.prepare ({ fs, block, 2 });
        cab.setSettings (procedural);
        cab.reset();
        report ("2 procedural mics + rooms", nsPerSample (cab, fs, block, seconds), fs);
    }
    {
        CabSim cab;
        cab.prepare ({ fs, block, 2 });
        auto s = procedural;
        s.mics[0].roomEnabled = s.mics[1].roomEnabled = false;
        cab.setSettings (s);
        cab.reset();
        report ("2 procedural mics, no room", nsPerSample (cab, fs, block, seconds), fs);
    }
    {
        CabSim cab;
        cab.prepare ({ fs, block, 2 });
        auto s = procedural;
        s.mics[0].position = 0.5f;
        cab.setSettings (s);
        cab.reset();
        // Continuous automation of the whole pose: around the cone, across, back and turning.
        ScopedNoDenormals noDenormals;
        std::vector<float> x (static_cast<size_t> (block), 0.1f);
        Buffer out (2, block);
        const int blocks = static_cast<int> (seconds * fs / block);
        const auto start = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
        {
            const float t = static_cast<float> (b) / 300.0f;
            s.mics[0].position = 0.5f + 0.5f * std::sin (t);
            s.mics[0].azimuthDeg = 90.0f * t;
            s.mics[1].distance = 0.5f + 0.5f * std::sin (1.3f * t);
            s.mics[1].offAxisDeg = 30.0f + 30.0f * std::sin (0.7f * t);
            cab.setSettings (s);
            cab.process (x.data(), out.view());
        }
        const auto ns =
            std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - start)
                .count();
        report ("2 procedural mics + rooms, automating the pose",
                ns / (static_cast<double> (blocks) * block), fs);
    }
    {
        CabSim cab;
        cab.prepare ({ fs, block, 2 });
        auto s = procedural;
        s.mics[0].type = CabSim::kCustomIrType;
        s.mics[0].roomEnabled = s.mics[1].roomEnabled = false;
        s.mics[1].enabled = false;
        cab.setCustomIr (
            0, 2, CabSim::makeKernel (ir.data(), static_cast<int> (ir.size()), fs, fs, block));
        cab.setSettings (s);
        cab.reset();
        report ("1 custom IR (1 s), nothing else", nsPerSample (cab, fs, block, seconds), fs);
    }
    {
        CabSim cab;
        cab.prepare ({ fs, block, 2 });
        auto s = procedural;
        s.mics[0].type = CabSim::kCustomIrType;
        s.mics[1].type = CabSim::kCustomIrType;
        cab.setCustomIr (
            0, 2, CabSim::makeKernel (ir.data(), static_cast<int> (ir.size()), fs, fs, block));
        cab.setCustomIr (
            1, 2, CabSim::makeKernel (ir.data(), static_cast<int> (ir.size()), fs, fs, block));
        cab.setSettings (s);
        cab.reset();
        report ("2 custom IRs (1 s) + rooms", nsPerSample (cab, fs, block, seconds), fs);
    }
    {
        CabSim cab;
        cab.prepare ({ fs, 1, 2 });
        auto s = procedural;
        s.mics[0].type = CabSim::kCustomIrType;
        cab.setCustomIr (0, 2,
                         CabSim::makeKernel (ir.data(), static_cast<int> (ir.size()), fs, fs, 1));
        cab.setSettings (s);
        cab.reset();
        report ("custom IR + procedural + rooms, 1-sample blocks", nsPerSample (cab, fs, 1, 5.0),
                fs);
    }
    {
        const auto start = std::chrono::steady_clock::now();
        auto k = CabSim::makeKernel (ir.data(), static_cast<int> (ir.size()), 44100.0, fs, block);
        const auto ms =
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start)
                .count();
        std::printf ("%-52s %7.1f ms (off the audio thread)\n", "makeKernel 1 s, 44.1k -> 48k", ms);
    }
}
