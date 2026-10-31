// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestUtils.h"
#include "sedef/Engine.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdlib>
#include <new>

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
    Rig (double fs, int block, int ins, int outs) : in (ins, 1 << 16), out (outs, 1 << 16)
    {
        engine.prepare (fs, block, ins, outs);
        this->block = block;
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
