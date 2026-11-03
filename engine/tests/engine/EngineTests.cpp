// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestUtils.h"
#include "sedef/Engine.h"
#include "sedef/dsp/cab/CabSim.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstdlib>
#include <new>
#include <vector>

// -------------------------------------------------------------------------------------------------
// Counting allocator: proves process() never allocates (ADR-0005). Only counts on threads that
// opted in, so Catch2's own allocations are ignored.

namespace
{
std::atomic<long> gAllocations { 0 };
thread_local bool gTrackAllocations = false;

void* countedAlloc (std::size_t n)
{
    if (gTrackAllocations)
        gAllocations.fetch_add (1, std::memory_order_relaxed);
    if (void* p = std::malloc (n == 0 ? 1 : n))
        return p;
    throw std::bad_alloc();
}

struct TrackAllocations
{
    TrackAllocations() { gTrackAllocations = true; }
    ~TrackAllocations() { gTrackAllocations = false; }
};
} // namespace

void* operator new (std::size_t n)
{
    return countedAlloc (n);
}
void* operator new[] (std::size_t n)
{
    return countedAlloc (n);
}
// The nothrow forms must come from the same allocator as the replaced deletes below (the standard
// library's temporary buffers use them), or sanitizers report an alloc/dealloc mismatch.
void* operator new (std::size_t n, const std::nothrow_t&) noexcept
{
    try
    {
        return countedAlloc (n);
    }
    catch (...)
    {
        return nullptr;
    }
}
void* operator new[] (std::size_t n, const std::nothrow_t&) noexcept
{
    try
    {
        return countedAlloc (n);
    }
    catch (...)
    {
        return nullptr;
    }
}
void operator delete (void* p) noexcept
{
    std::free (p);
}
void operator delete[] (void* p) noexcept
{
    std::free (p);
}
void operator delete (void* p, std::size_t) noexcept
{
    std::free (p);
}
void operator delete[] (void* p, std::size_t) noexcept
{
    std::free (p);
}

// -------------------------------------------------------------------------------------------------

using namespace sedef;

namespace
{
struct Rig
{
    Rig (double fs, int blockSize, int ins, int outs) : in (ins, 1 << 16), out (outs, 1 << 16)
    {
        engine.prepare (fs, blockSize, ins, outs);
        block = blockSize;
    }

    /// Processes `frames` samples of the input buffer in host blocks of `block` frames.
    void run (const ParamSnapshot& p, int frames, int offset = 0)
    {
        for (int s = offset; s < offset + frames; s += block)
        {
            const int n = std::min (block, offset + frames - s);
            std::array<const float*, 2> ip { in.ptrs[0] + s,
                                             in.channels() > 1 ? in.ptrs[1] + s : nullptr };
            std::array<float*, 2> op { out.ptrs[0] + s,
                                       out.channels() > 1 ? out.ptrs[1] + s : nullptr };
            engine.process (ip.data(), op.data(), n, p, transport);
        }
    }

    Engine engine;
    test::Buffer in, out;
    Transport transport;
    int block = 64;
};

ParamSnapshot randomSnapshot (Random& r)
{
    auto p = ParamSnapshot::defaults();
    for (std::size_t i = 0; i < kNumParams; ++i)
        p.values[i] = kParamSpecs[i].fromNormalised (r.unipolar());
    p[ParamId::inputGain] = 0.0f;
    p[ParamId::outputGain] = -6.0f;
    p[ParamId::metronomeEnabled] = 0.0f;
    return p;
}
} // namespace

TEST_CASE ("Engine renders every I/O layout with finite output", "[engine]")
{
    const auto [ins, outs] = GENERATE (std::pair { 1, 1 }, std::pair { 1, 2 }, std::pair { 2, 2 });
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    Rig rig (fs, 128, ins, outs);
    for (int c = 0; c < ins; ++c)
        test::fillNoise (rig.in[c], 0.3f, static_cast<std::uint32_t> (c + 1));

    Random r (42);
    for (int round = 0; round < 6; ++round)
    {
        auto p = randomSnapshot (r);
        if (ins == 2)
            p[ParamId::stereoMode] = static_cast<float> (round % 2);
        rig.run (p, 8192, round * 8192);
    }
    for (int c = 0; c < outs; ++c)
        CHECK (test::allFinite (rig.out[c]));
}

TEST_CASE ("Engine::process never allocates after prepare", "[engine]")
{
    Rig rig (48000.0, 256, 2, 2);
    test::fillNoise (rig.in[0], 0.3f, 1);
    test::fillNoise (rig.in[1], 0.3f, 2);

    Random r (7);
    auto p = randomSnapshot (r);
    // Turn everything on so every module runs.
    for (auto id :
         { ParamId::gateEnabled, ParamId::tideEnabled, ParamId::pearlEnabled, ParamId::coralEnabled,
           ParamId::rippleEnabled, ParamId::doublerEnabled, ParamId::driftEnabled,
           ParamId::fathomEnabled, ParamId::roomAEnabled, ParamId::roomBEnabled })
        p[id] = 1.0f;
    p[ParamId::transpose] = -2.0f;
    rig.engine.setAnalysisEnabled (true);
    rig.run (p, 4096); // warm-up (first-use paths)

    gAllocations = 0;
    {
        TrackAllocations track;
        for (int model = 0; model < 3; ++model)
        {
            p[ParamId::ampModel] = static_cast<float> (model);
            p[ParamId::stereoMode] = static_cast<float> (model % 2);
            rig.run (p, 16384, 4096 + model * 16384);
        }
    }
    CHECK (gAllocations.load() == 0);
}

TEST_CASE ("Switching pedals does not change the chain's latency", "[engine]")
{
    // Render the same impulse with Pearl off and Pearl on at minimum gain; the outputs must line
    // up (the bypass path is delayed by the pedal's latency). Pearl's tone filters still shift the
    // correlation peak by their group delay (a few samples), while a missing compensation would
    // shift it by the whole oversampler latency (31-37 samples at 48 kHz).
    const auto render = [] (bool pearlOn) {
        Rig rig (48000.0, 64, 1, 1);
        rig.in[0][1000] = 0.5f;
        auto p = ParamSnapshot::defaults();
        p[ParamId::gateEnabled] = 0.0f;
        p[ParamId::ampModel] = 0.0f; // Lagoon, clean
        p[ParamId::lagoonVolume] = 0.2f;
        p[ParamId::lagoonMaster] = 0.2f;
        p[ParamId::pearlEnabled] = pearlOn ? 1.0f : 0.0f;
        p[ParamId::pearlGain] = 0.0f;
        rig.run (p, 8192);
        return rig.out[0];
    };
    const auto off = render (false);
    const auto on = render (true);

    // Lag with the strongest cross-correlation.
    int bestLag = 0;
    double best = -1.0;
    for (int lag = -64; lag <= 64; ++lag)
    {
        double acc = 0.0;
        for (int i = 900; i < 4000; ++i)
            acc += static_cast<double> (off[static_cast<size_t> (i)]) *
                   on[static_cast<size_t> (i + lag)];
        if (acc > best)
        {
            best = acc;
            bestLag = lag;
        }
    }
    CHECK (std::abs (bestLag) <= 4);
}

TEST_CASE ("Analysis ring is fed only while analysis is enabled; tuner mute silences", "[engine]")
{
    Rig rig (48000.0, 128, 1, 2);
    test::fillSine (rig.in[0], 110.0, 48000.0, 0.3f);
    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;

    rig.run (p, 4096);
    CHECK (rig.engine.analysisRing().available() == 0);

    rig.engine.setAnalysisEnabled (true);
    p[ParamId::tunerMute] = 1.0f;
    rig.run (p, 8192, 4096);
    CHECK (rig.engine.analysisRing().available() == 8192);
    // After the 10 ms fade, the output is silent.
    CHECK (test::rms (rig.out[0].data() + 4096 + 2048, 4096) < 1.0e-6f);
}

TEST_CASE ("Telemetry reports levels", "[engine]")
{
    Rig rig (48000.0, 128, 1, 2);
    test::fillSine (rig.in[0], 220.0, 48000.0, 0.25f);
    auto p = ParamSnapshot::defaults();
    rig.run (p, 4800);
    const auto& t = rig.engine.telemetry();
    CHECK (t.inputPeak[0].load() > 0.2f);
    CHECK (t.outputPeak[0].load() > 0.0f);
}

TEST_CASE ("Re-preparing with fewer inputs while in Dual mode is safe", "[engine]")
{
    Engine engine;
    engine.prepare (48000.0, 128, 2, 2);
    test::Buffer in (2, 128), out (2, 128);
    test::fillNoise (in[0], 0.2f, 1);
    test::fillNoise (in[1], 0.2f, 2);
    auto p = ParamSnapshot::defaults();
    p[ParamId::stereoMode] = 1.0f;
    Transport t;
    std::array<const float*, 2> ip { in.ptrs[0], in.ptrs[1] };
    for (int i = 0; i < 50; ++i) // long enough for the mode fade to complete
        engine.process (ip.data(), out.ptrs.data(), 128, p, t);

    engine.prepare (44100.0, 256, 1, 2);
    test::Buffer in1 (1, 256), out1 (2, 256);
    test::fillNoise (in1[0], 0.2f, 3);
    std::array<const float*, 2> ip1 { in1.ptrs[0], nullptr };
    for (int i = 0; i < 20; ++i)
        engine.process (ip1.data(), out1.ptrs.data(), 256, p, t);
    CHECK (test::allFinite (out1[0]));
    CHECK (test::allFinite (out1[1]));
}

// -------------------------------------------------------------------------------------------------
// Regression tests for engine review findings.

namespace
{
/// Silent input, everything off except the metronome (which only sounds when allowed).
ParamSnapshot metronomeOnly()
{
    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;
    p[ParamId::metronomeEnabled] = 1.0f;
    p[ParamId::metronomeSound] = 3.0f; // cowbell: long tail, no noise, so renders are repeatable
    p[ParamId::tempo] = 60.0f;
    return p;
}

/// RMS of 64-sample windows, for level-over-time comparisons.
std::vector<float> windowRms (const std::vector<float>& v, size_t from, size_t to)
{
    std::vector<float> out;
    for (size_t i = from; i + 64 <= to; i += 64)
        out.push_back (test::rms (v.data() + i, 64));
    return out;
}
} // namespace

TEST_CASE ("Disabling the metronome lets a sounding click ring out", "[engine]")
{
    const auto render = [] (bool switchOff) {
        Rig rig (48000.0, 64, 1, 2);
        rig.engine.setMetronomeAllowed (true);
        auto p = metronomeOnly();
        rig.run (p, 256); // the downbeat sounds at sample 0
        if (switchOff)
            p[ParamId::metronomeEnabled] = 0.0f;
        rig.run (p, 40000, 256); // the next click would be at 48000
        return rig.out[0];
    };
    const auto keptOn = render (false);
    const auto switchedOff = render (true);
    CHECK (test::peak (switchedOff) > 0.05f);
    // Switching off stops scheduling only: the click already sounding finishes untouched.
    CHECK (switchedOff == keptOn);
}

TEST_CASE ("A re-enabled metronome starts clean, with no stale click", "[engine]")
{
    Rig rig (48000.0, 64, 1, 2);
    rig.engine.setMetronomeAllowed (true);
    auto p = metronomeOnly();
    rig.run (p, 256);
    p[ParamId::metronomeEnabled] = 0.0f;
    rig.run (p, 30000, 256); // long enough for the click to ring out
    p[ParamId::metronomeEnabled] = 1.0f;
    rig.run (p, 24000, 30256);

    Rig fresh (48000.0, 64, 1, 2);
    fresh.engine.setMetronomeAllowed (true);
    fresh.run (metronomeOnly(), 24000);

    const std::vector<float> again (rig.out[0].begin() + 30256, rig.out[0].begin() + 54256);
    const std::vector<float> first (fresh.out[0].begin(), fresh.out[0].begin() + 24000);
    CHECK (test::peak (first) > 0.05f);
    CHECK (again == first);
}

TEST_CASE ("Starting in Dual mode plays from the first block without a gap", "[engine]")
{
    Rig rig (44100.0, 4096, 2, 2);
    test::fillSine (rig.in[0], 220.0, 44100.0, 0.2f);
    test::fillSine (rig.in[1], 330.0, 44100.0, 0.2f);
    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;
    p[ParamId::stereoMode] = 1.0f;
    rig.run (p, 3 * 4096);

    // Once the chains have warmed up (~1500 samples), every window carries signal on both sides.
    // Starting in Summed mode and switching at the next chunk left 4096 - 441 samples of silence.
    for (int c = 0; c < 2; ++c)
    {
        const auto w = windowRms (rig.out[c], 2048, 3 * 4096);
        const float steady = test::rms (rig.out[c].data() + 2 * 4096, 4096);
        INFO ("channel " << c);
        CHECK (steady > 0.01f);
        CHECK (*std::min_element (w.begin(), w.end()) > 0.3f * steady);
    }
}

TEST_CASE ("A stereo-mode switch dips for the fades only, even in long blocks", "[engine]")
{
    Rig rig (44100.0, 4096, 2, 2);
    test::fillSine (rig.in[0], 220.0, 44100.0, 0.2f);
    test::fillSine (rig.in[1], 220.0, 44100.0, 0.2f);
    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;
    rig.run (p, 2 * 4096);
    p[ParamId::stereoMode] = 1.0f;
    rig.run (p, 2 * 4096, 2 * 4096);

    const float steady = test::rms (rig.out[0].data() + 3 * 4096, 4096);
    const auto w = windowRms (rig.out[0], 2 * 4096, 3 * 4096);
    const auto quiet =
        std::count_if (w.begin(), w.end(), [steady] (float r) { return r < 0.05f * steady; });
    // 10 ms out, switch, 10 ms in (441 samples each): only the windows around the switch point
    // are near silence. Switching at the next 4096-frame chunk instead held ~83 ms of silence.
    CHECK (steady > 0.01f);
    CHECK (quiet <= 4);
}

TEST_CASE ("Switching to Dual mode does not replay stale doubler audio", "[engine]")
{
    Rig rig (48000.0, 256, 2, 2);
    test::fillNoise (rig.in[0], 0.3f, 1);
    test::fillNoise (rig.in[1], 0.3f, 2);
    const int switchAt = 24064; // block-aligned
    std::fill (rig.in[0].begin() + switchAt, rig.in[0].end(), 0.0f);
    std::fill (rig.in[1].begin() + switchAt, rig.in[1].end(), 0.0f);

    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;
    p[ParamId::doublerEnabled] = 1.0f; // the Doubler only runs in Summed mode
    rig.run (p, switchAt);
    p[ParamId::stereoMode] = 1.0f;
    rig.run (p, 9600, switchAt);

    // Silent input from the switch on: once the 10 ms fade-out has passed and the chains (and the
    // Doubler) are reset, nothing of the old signal may come back.
    const size_t after = static_cast<size_t> (switchAt + 480);
    for (int c = 0; c < 2; ++c)
    {
        const std::vector<float> tail (rig.out[c].begin() + static_cast<long> (after),
                                       rig.out[c].begin() + switchAt + 9600);
        INFO ("channel " << c);
        CHECK (test::peak (tail) < 1.0e-6f);
    }
}

namespace
{
/// Renders a 1 kHz tone with a pedal off, switches the pedal on, and changes one of its
/// parameters on the very next block. Returns the output and a reference with no change.
std::pair<std::vector<float>, std::vector<float>>
unbypassThenAutomate (ParamId enable, ParamId param, float from, float to)
{
    const auto render = [&] (bool automate) {
        Rig rig (48000.0, 64, 1, 2);
        test::fillSine (rig.in[0], 1000.0, 48000.0, 0.05f);
        auto p = ParamSnapshot::defaults();
        p[ParamId::gateEnabled] = 0.0f;
        p[ParamId::ampModel] = 0.0f; // Lagoon, clean
        p[enable] = 0.0f;
        p[param] = from;
        rig.run (p, 9600);
        p[enable] = 1.0f;
        rig.run (p, 64, 9600);
        if (automate)
            p[param] = to;
        rig.run (p, 9600, 9664);
        return rig.out[0];
    };
    return { render (true), render (false) };
}

/// Level of `out` relative to `ref` (dB) over 64 samples starting `ms` after the change.
float gainAfter (const std::pair<std::vector<float>, std::vector<float>>& r, double ms)
{
    const auto at = static_cast<size_t> (9664 + ms * 48.0);
    return 20.0f *
           std::log10 (test::rms (r.first.data() + at, 64) / test::rms (r.second.data() + at, 64));
}
} // namespace

TEST_CASE ("Automation right after un-bypassing a post effect glides", "[engine]")
{
    // A +12 dB move on the 1 kHz band of the Lagoon EQ, one block after switching the EQ on.
    const auto r =
        unbypassThenAutomate (ParamId::eqLagoonEnabled, ParamId::eqLagoonBand5, 0.0f, 12.0f);
    CHECK (gainAfter (r, 150.0) == Catch::Approx (12.0f).margin (0.5f)); // it gets there...
    CHECK (gainAfter (r, 12.0) < 6.0f); // ...over the 50 ms glide (a snap is +12 dB here)
}

TEST_CASE ("Automation right after un-bypassing a pedal glides", "[engine]")
{
    // Tide's level from noon to full, one block after switching Tide on. With the default blend
    // part of the signal stays dry, so the full change is about +10 dB rather than +12.
    const auto r = unbypassThenAutomate (ParamId::tideEnabled, ParamId::tideLevel, 0.5f, 1.0f);
    const float full = gainAfter (r, 150.0);
    CHECK (full > 6.0f);
    CHECK (gainAfter (r, 11.0) < full - 3.0f); // a third into the 30 ms ramp; a snap is all of it
}

TEST_CASE ("Custom IRs installed before a two-input prepare reach both chains", "[engine]")
{
    Engine engine;
    engine.prepare (48000.0, 256, 1, 2);
    REQUIRE (engine.numCabs() == 2);
    CHECK (&engine.cab (0) != &engine.cab (1));
    CHECK (&engine.cab (7) == &engine.cab (1));
    CHECK (&engine.cab (-3) == &engine.cab (0));

    // A short decaying noise IR on mic A of cab 1, installed the way the plugin does it.
    std::vector<float> ir (2048);
    Random r (5);
    for (size_t i = 0; i < ir.size(); ++i)
        ir[i] = r.bipolar() * std::exp (-static_cast<float> (i) / 300.0f);
    for (int c = 0; c < engine.numCabs(); ++c)
        engine.cab (c).setCustomIr (0, 0,
                                    dsp::CabSim::makeKernel (ir.data(),
                                                             static_cast<int> (ir.size()), 48000.0,
                                                             48000.0, 256));

    engine.prepare (48000.0, 256, 2, 2);
    test::Buffer in (2, 256), out (2, 256);
    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;
    p[ParamId::stereoMode] = 1.0f;
    p[ParamId::ampModel] = 0.0f;
    p[ParamId::micACab1Type] = static_cast<float> (dsp::CabSim::kCustomIrType);
    p[ParamId::micBEnabled] = 0.0f;
    Transport t;
    std::vector<float> left, right;
    for (int b = 0; b < 200; ++b)
    {
        test::fillNoise (in[0], 0.2f, static_cast<std::uint32_t> (b + 1));
        in[1] = in[0]; // the same signal into both chains
        std::array<const float*, 2> ip { in.ptrs[0], in.ptrs[1] };
        engine.process (ip.data(), out.ptrs.data(), 256, p, t);
        left.insert (left.end(), out[0].begin(), out[0].end());
        right.insert (right.end(), out[1].begin(), out[1].end());
    }
    CHECK (test::peak (left) > 0.01f);
    CHECK (left == right); // both chains convolve with the IR
}

TEST_CASE ("The transpose parameter is ignored", "[engine]")
{
    // Transpose was removed in 0.3.0 (docs/dsp/transpose.md); old sessions may still set it.
    const auto render = [] (float semitones) {
        Rig rig (48000.0, 64, 1, 2);
        test::fillNoise (rig.in[0], 0.2f, 5);
        auto p = ParamSnapshot::defaults();
        p[ParamId::transpose] = semitones;
        rig.run (p, 1 << 15);
        return rig.out.data;
    };
    const auto dry = render (0.0f);
    CHECK (render (-5.0f) == dry);
    CHECK (render (12.0f) == dry);
}
