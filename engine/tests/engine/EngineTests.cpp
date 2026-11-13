// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestUtils.h"
#include "capture/CaptureTestUtils.h"
#include "live/LiveTestUtils.h"
#include "midi/MidiTestSignals.h"
#include "sedef/Engine.h"
#include "sedef/dsp/Looper.h"
#include "sedef/dsp/PitchShifter.h"
#include "sedef/dsp/cab/CabSim.h"
#include "sedef/dsp/capture/CaptureStage.h"

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
    Rig (double fs, int blockSize, int ins, int outs,
         OversamplingMode mode = OversamplingMode::lowLatency)
        : in (ins, 1 << 16),
          out (outs, 1 << 16)
    {
        engine.setOversampling (mode);
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
    for (auto id : { ParamId::gateEnabled, ParamId::tideEnabled, ParamId::pearlEnabled,
                     ParamId::coralEnabled, ParamId::rippleEnabled, ParamId::doublerEnabled,
                     ParamId::driftEnabled, ParamId::fathomEnabled, ParamId::roomAEnabled,
                     ParamId::roomBEnabled, ParamId::trenchEnabled, ParamId::urchinEnabled,
                     ParamId::kelpEnabled, ParamId::conchEnabled, ParamId::anemoneEnabled,
                     ParamId::gyreEnabled, ParamId::jettyEnabled, ParamId::beaconEnabled })
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
            // Transpose changes direction, turns off and on: new voices and a new model.
            p[ParamId::transpose] = model == 1 ? 7.0f : (model == 2 ? 0.0f : -2.0f);
            rig.run (p, 16384, 4096 + model * 16384);
        }
        p[ParamId::transpose] = -12.0f;
        rig.run (p, 8192, 4096 + 3 * 16384);
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

TEST_CASE ("Switching Kelp, Urchin or Trench does not change the chain's latency", "[engine]")
{
    // Same check for the newer pedals: each one on at a setting where it is close to linear
    // (Kelp at drive 0, Urchin's plain fuzz at a low level, Trench's direct path), against all off.
    // Kelp and Urchin are oversampled; without the bypass delay their output would sit 31-37
    // samples late.
    const auto render = [] (ParamId pedal, std::initializer_list<std::pair<ParamId, float>> set) {
        Rig rig (48000.0, 64, 1, 1);
        rig.in[0][1000] = 0.02f;
        auto p = ParamSnapshot::defaults();
        p[ParamId::gateEnabled] = 0.0f;
        p[ParamId::ampModel] = 0.0f; // Lagoon, clean
        p[ParamId::lagoonVolume] = 0.2f;
        p[ParamId::lagoonMaster] = 0.2f;
        p[pedal] = 1.0f;
        for (const auto& [id, value] : set)
            p[id] = value;
        rig.run (p, 8192);
        return rig.out[0];
    };
    const auto off = render (ParamId::kelpEnabled, { { ParamId::kelpEnabled, 0.0f } });
    REQUIRE (test::peak (off) > 0.0f);

    const auto lagOf = [&off] (const std::vector<float>& on) {
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
        return bestLag;
    };
    CHECK (std::abs (lagOf (render (ParamId::kelpEnabled, { { ParamId::kelpDrive, 0.0f } }))) <= 4);
    CHECK (std::abs (
               lagOf (render (ParamId::urchinEnabled, { { ParamId::urchinFuzz, 0.0f },
                                                        { ParamId::urchinOctave, 0.0f } }))) <= 8);
    CHECK (lagOf (render (ParamId::trenchEnabled, {})) == 0);
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
    // are near silence, plus the rig's latency while the fresh chains fill (their delay lines
    // start empty). Switching at the next 4096-frame chunk instead held ~83 ms of silence.
    const auto latencyWindows = (rig.engine.latencySamples() + 63) / 64;
    CHECK (steady > 0.01f);
    CHECK (quiet <= 2 + latencyWindows);
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

TEST_CASE ("Transpose's latency is reported and follows its direction", "[engine][transpose]")
{
    // docs/dsp/transpose.md, section 5: 3 ms down, 10 ms up, nothing at 0; steps that keep the
    // direction keep the latency.
    const double fs = GENERATE (44100.0, 48000.0);
    Rig rig (fs, 128, 1, 2);
    test::fillNoise (rig.in[0], 0.2f, 5);
    auto p = ParamSnapshot::defaults();
    rig.run (p, 1024);
    const int base = rig.engine.latencySamples();
    const auto down = static_cast<int> (std::lround (0.003 * fs));
    const auto up = static_cast<int> (std::lround (0.010 * fs));
    int offset = 1024;
    for (const auto& [st, added] :
         { std::pair { -2, down }, std::pair { -5, down }, std::pair { 7, up },
           std::pair { 12, up }, std::pair { 0, 0 }, std::pair { -12, down } })
    {
        CAPTURE (st);
        p[ParamId::transpose] = static_cast<float> (st);
        rig.run (p, 1024, offset);
        offset += 1024;
        CHECK (rig.engine.latencySamples() == base + added);
    }

    // Before the first block after prepare(), the host can already be told.
    rig.engine.prepare (fs, 128, 1, 2);
    rig.engine.expectTranspose (5.0f);
    CHECK (rig.engine.latencySamples() == base + up);
}

TEST_CASE ("The gate follows Transpose's latency", "[engine][transpose]")
{
    // The gate listens to the DI (before input gain), delayed by Transpose's latency, so it
    // closes when the shifted note has faded, not that much earlier. A decaying tone into a clean
    // amp: the time the gated output falls 30 dB under the ungated one moves by the latency.
    const double fs = 48000.0;
    const auto gatedEnd = [fs] (int semitones) {
        const auto render = [&] (bool gate) {
            Rig rig (fs, 64, 1, 1);
            for (int i = 0; i < static_cast<int> (0.6 * fs); ++i)
            {
                const double t = i / fs;
                const double level = t < 0.2 ? 0.3 : 0.3 * std::exp (-(t - 0.2) / 0.05);
                rig.in[0][static_cast<size_t> (i)] =
                    static_cast<float> (level * std::sin (kTwoPiD * 220.0 * t));
            }
            auto p = ParamSnapshot::defaults();
            p[ParamId::gateEnabled] = gate ? 1.0f : 0.0f;
            p[ParamId::gateThreshold] = -40.0f;
            p[ParamId::ampModel] = 0.0f;
            p[ParamId::transpose] = static_cast<float> (semitones);
            rig.run (p, static_cast<int> (0.6 * fs));
            return rig.out[0];
        };
        const auto open = render (false), gated = render (true);
        const auto energy = [] (const std::vector<float>& x, size_t i) {
            double e = 0.0;
            for (size_t k = i; k < i + 96; ++k)
                e += static_cast<double> (x[k]) * x[k];
            return e;
        };
        for (auto i = static_cast<size_t> (0.25 * fs); i < static_cast<size_t> (0.55 * fs); ++i)
            if (energy (gated, i) < 1.0e-3 * energy (open, i))
                return static_cast<double> (i);
        return -1.0;
    };
    const double off = gatedEnd (0);
    REQUIRE (off > 0.0);
    for (int st : { -3, 5 })
    {
        CAPTURE (st);
        const double latency = dsp::PitchShifter::latencyFor (st, fs);
        // Without the delay the gate would close the whole latency early. (The shifted tone meets
        // the amp at another pitch, so the crossing moves by a little more or less.)
        CHECK (std::abs (gatedEnd (st) - off - latency) <= std::max (48.0, 0.3 * latency));
    }
}

TEST_CASE ("Engine::process never allocates while captures play, switch and swap",
           "[engine][capture]")
{
    namespace nt = sedef::test::nam;
    const double fs = GENERATE (44100.0, 48000.0);
    Rig rig (fs, 256, 2, 2);
    test::fillNoise (rig.in[0], 0.3f, 1);
    test::fillNoise (rig.in[1], 0.3f, 2);

    // One capture of every architecture; the Linear one is long enough for the FFT path.
    std::vector<float> ir (4096);
    for (std::size_t i = 0; i < ir.size(); ++i)
        ir[i] = 0.2f * std::exp (-static_cast<float> (i) / 600.0f) * (i % 3 == 0 ? 1.0f : -0.5f);
    const std::vector<std::string> texts {
        nt::smallWaveNet (2),
        nt::file ("LSTM", nt::lstmConfig (2, 12),
                  nt::randomWeights (static_cast<std::size_t> (nt::lstmWeights (2, 12)), 3, 0.3f)),
        nt::file ("ConvNet", nt::convNetConfig (6, { 1, 2, 4, 8, 16 }, true),
                  nt::convNetWeightsFor (6, 5, true, 4)),
        nt::linear (ir),
    };
    const auto publish = [&rig] (const std::shared_ptr<const dsp::CaptureFile>& f) {
        for (int c = 0; c < rig.engine.numCabs(); ++c)
            rig.engine.capture (c).setModel (f->instantiate (rig.engine.capture (c).modelSpec()));
    };

    auto p = ParamSnapshot::defaults();
    p[ParamId::ampSource] = 1.0f;
    p[ParamId::outputGain] = -6.0f;
    int offset = 0;
    const auto run = [&] (int frames) {
        rig.run (p, frames, offset % 32768);
        offset += frames;
    };
    for (const auto& text : texts)
    {
        const auto f = nt::parseOrFail (text);
        publish (f); // allocates, on this (non-audio) thread
        run (2048);  // the stage picks it up and crossfades
        long allocations = 0;
        {
            gAllocations = 0;
            TrackAllocations track;
            for (int round = 0; round < 8; ++round)
            {
                p[ParamId::ampSource] = static_cast<float> ((round + 1) % 2);
                p[ParamId::captureCab] = static_cast<float> (round / 2 % 2);
                p[ParamId::stereoMode] = static_cast<float> (round / 4 % 2);
                p[ParamId::captureInput] = round % 3 == 0 ? 6.0f : -3.0f;
                run (4096);
            }
            allocations = gAllocations.load();
        }
        CAPTURE (f->info().architecture, fs);
        CHECK (allocations == 0);
        for (int c = 0; c < rig.engine.numCabs(); ++c)
            rig.engine.capture (c).collectGarbage();
    }
    for (int c = 0; c < 2; ++c)
        CHECK (test::allFinite (rig.out[c]));
}

TEST_CASE ("Engine::process never allocates while the pedalboard changes", "[engine][board]")
{
    // New orders, pedals joining and leaving with and without latency, changes of mind half way
    // through a duck, at every block size and rate the engine supports.
    const double fs = GENERATE (44100.0, 96000.0, 192000.0);
    const int block = GENERATE (1, 37, 512, 4096);
    CAPTURE (fs, block);
    Rig rig (fs, block, 2, 2);
    test::fillNoise (rig.in[0], 0.3f, 1);
    test::fillNoise (rig.in[1], 0.3f, 2);

    Random r (11);
    auto p = randomSnapshot (r);
    // Both copies of every pedal on; boards with pedals on both sides of the amp and second
    // copies (docs/board.md), so changes cross the amp, add copies and reorder both sides.
    for (std::size_t slot = 0; slot < kNumSlots; ++slot)
        p[enableOf (slotRef (slot))] = 1.0f;
    const auto randomBoard = [&r] {
        Board b;
        for (std::size_t slot = 0; slot < kNumSlots; ++slot)
            if (r.unipolar() < 0.6f)
            {
                const auto ref = slotRef (slot);
                const auto side =
                    fitsAfterAmp (ref.id) && r.unipolar() < 0.5f ? Side::post : Side::pre;
                b.place (ref, side,
                         static_cast<std::size_t> (r.unipolar() *
                                                   static_cast<float> (b.side (side).size())));
            }
        return b;
    };
    rig.run (p, 4096); // warm-up (first-use paths)

    const int frames = std::max (8192, 16 * block);
    gAllocations = 0;
    {
        TrackAllocations track;
        for (int round = 0; round < 24; ++round)
        {
            rig.engine.setBoard (randomBoard()); // the producer side doesn't allocate either
            p[ParamId::stereoMode] = static_cast<float> (round / 8 % 2);
            // Some changes land in the middle of the last one's duck.
            const int length = round % 3 == 0 ? std::max (block, 64) : frames / 4;
            rig.run (p, std::min (length, 60000), (round * 4096) % 32768);
        }
    }
    CHECK (gAllocations.load() == 0);
    for (int c = 0; c < 2; ++c)
        CHECK (test::allFinite (rig.out[c]));
}

// -------------------------------------------------------------------------------------------------
// Fuzzes and booster: Remora, Sargasso, Breaker.

TEST_CASE ("Engine::process never allocates with the fuzzes and booster on", "[engine][drives]")
{
    Rig rig (48000.0, 256, 1, 2);
    test::fillNoise (rig.in[0], 0.3f, 3);
    Random r (11);
    auto p = randomSnapshot (r);
    for (auto id : { ParamId::remoraEnabled, ParamId::sargassoEnabled, ParamId::breakerEnabled })
        p[id] = 1.0f;
    rig.run (p, 4096);

    gAllocations = 0;
    {
        TrackAllocations track;
        for (int round = 0; round < 4; ++round)
        {
            // Switches move, knobs glide: every curve blend and crossfade runs.
            p[ParamId::remoraTransistor] = static_cast<float> (round % 2);
            p[ParamId::breakerRange] = static_cast<float> (round / 2 % 2);
            p[ParamId::remoraBias] = round % 2 == 0 ? 0.1f : 0.9f;
            p[ParamId::breakerBoost] = round % 2 == 0 ? 1.0f : 0.2f;
            p[ParamId::sargassoTone] = round % 2 == 0 ? 0.0f : 1.0f;
            rig.run (p, 8192, 4096 + round * 8192);
        }
    }
    CHECK (gAllocations.load() == 0);
    CHECK (test::allFinite (rig.out[0]));
}

TEST_CASE ("Switching the fuzzes on does not change the chain's latency", "[engine][drives]")
{
    // As for Pearl: an impulse with the pedal off and on (in a nearly linear setting) must line
    // up within the few samples its filters shift the correlation peak, not the oversampler's
    // 31-37 samples.
    const auto render = [] (ParamId enable, bool on) {
        Rig rig (48000.0, 64, 1, 1);
        rig.in[0][1000] = 0.002f;
        auto p = ParamSnapshot::defaults();
        p[ParamId::gateEnabled] = 0.0f;
        p[ParamId::ampModel] = 0.0f; // Lagoon, clean
        p[ParamId::lagoonVolume] = 0.2f;
        p[ParamId::lagoonMaster] = 0.2f;
        p[ParamId::remoraFuzz] = 0.0f;
        p[ParamId::remoraGuitarVolume] = 0.3f; // rolled back: the input network is nearly flat
        p[ParamId::sargassoSustain] = 0.0f;
        p[enable] = on ? 1.0f : 0.0f;
        rig.run (p, 8192);
        return rig.out[0];
    };
    for (auto enable : { ParamId::remoraEnabled, ParamId::sargassoEnabled })
    {
        const auto off = render (enable, false);
        const auto on = render (enable, true);
        int bestLag = 0;
        double best = -1.0;
        for (int lag = -64; lag <= 64; ++lag)
        {
            double acc = 0.0;
            for (int i = 900; i < 4000; ++i)
                acc += static_cast<double> (off[static_cast<size_t> (i)]) *
                       on[static_cast<size_t> (i + lag)];
            acc = std::abs (acc); // the pedals may invert relative to the dry path's filters
            if (acc > best)
            {
                best = acc;
                bestLag = lag;
            }
        }
        CAPTURE (static_cast<int> (enable));
        CHECK (std::abs (bestLag) <= 6);
    }
}

TEST_CASE ("Switching the fuzzes and booster on and off is click-free", "[engine][drives]")
{
    // The slot crossfades over 10 ms between the (latency-aligned) dry path and the pedal, which
    // restarts from a clean state. The switch must sound like that crossfade and nothing more: no
    // thump from the restart, no comb from a misaligned dry path. Reference: the steady "off" and
    // "on" renders, crossfaded ideally. A guitar-like tone (147 Hz and seven harmonics).
    constexpr int kSwitch = 24000, kFade = 480;
    const auto oversampling =
        GENERATE (OversamplingMode::lowLatency, OversamplingMode::linearPhase);
    for (auto enable :
         { ParamId::remoraEnabled, ParamId::sargassoEnabled, ParamId::breakerEnabled })
    {
        const auto render = [enable, oversampling] (int mode) { // 0 off, 1 on, 2 on at kSwitch
            Rig rig (48000.0, 64, 1, 2, oversampling);
            for (size_t i = 0; i < rig.in[0].size(); ++i)
            {
                double v = 0.0;
                for (int k = 1; k <= 8; ++k)
                    v += std::sin (kTwoPiD * 147.0 * k * static_cast<double> (i) / 48000.0) / k;
                rig.in[0][i] = 0.05f * static_cast<float> (v);
            }
            auto p = ParamSnapshot::defaults();
            p[ParamId::gateEnabled] = 0.0f;
            p[ParamId::ampModel] = 0.0f; // Lagoon, clean, so the crossfade passes it unchanged
            p[ParamId::lagoonVolume] = 0.2f;
            p[ParamId::lagoonMaster] = 0.3f;
            p[ParamId::breakerBoost] = 0.2f;
            p[enable] = mode == 1 ? 1.0f : 0.0f;
            rig.run (p, kSwitch);
            if (mode == 2)
                p[enable] = 1.0f;
            rig.run (p, 9600, kSwitch);
            return rig.out[0];
        };
        const auto off = render (0);
        const auto on = render (1);
        const auto switched = render (2);
        const int latency = [oversampling] {
            Engine e;
            e.setOversampling (oversampling);
            e.prepare (48000.0, 64, 1, 2);
            return e.latencySamples();
        }();

        // Where the fade shows up in the output depends on the latency after the pedal, and on
        // the amp's and cab's own delay (its voicing filters, the mic's distance from the cone);
        // find it.
        const int search = latency + 192;
        const size_t from = kSwitch - 64, to = static_cast<size_t> (kSwitch + search + 3 * kFade);
        std::vector<float> ideal (switched.size());
        float worstError = 1.0e9f;
        for (int start = 0; start <= search; ++start)
        {
            std::vector<float> candidate (switched.size());
            float error = 0.0f;
            for (size_t i = from - 2; i < to; ++i)
            {
                const int k = static_cast<int> (i) - kSwitch - start;
                const float a = std::clamp (static_cast<float> (k) / kFade, 0.0f, 1.0f);
                candidate[i] = off[i] + a * (on[i] - off[i]);
                error = std::max (error, std::abs (switched[i] - candidate[i]));
            }
            if (error < worstError)
            {
                worstError = error;
                ideal = candidate;
            }
        }
        float worstEdge = 0.0f, idealEdge = 0.0f, top = 0.0f;
        for (size_t i = from; i < to; ++i)
        {
            const auto d2 = [] (const std::vector<float>& v, size_t j) {
                return std::abs (v[j] - 2.0f * v[j - 1] + v[j - 2]);
            };
            worstEdge = std::max (worstEdge, d2 (switched, i));
            idealEdge = std::max (idealEdge, d2 (ideal, i));
            top = std::max (top, std::abs (ideal[i]));
        }
        CAPTURE (static_cast<int> (oversampling), static_cast<int> (enable), worstEdge, idealEdge,
                 worstError, top);
        CHECK (worstEdge <= 1.25f * idealEdge);
        // The restarted pedal settles within the fade (its filters and bias start from rest), so
        // the switch is not exactly the ideal crossfade; a dry path off by the oversampler's
        // latency would miss it by about the signal's whole size.
        CHECK (worstError <= 0.2f * top);
    }
}

TEST_CASE ("GuitarToMidi::process never allocates after prepare", "[engine][midi]")
{
    // A bass line through every mode and timing, with settings changes, All Notes Off and a
    // panic between blocks.
    const double fs = 48000.0;
    const auto sc = test::midi::bassLine (2, test::midi::Pickup::bridge, 132.0, fs, 24);
    midi::GuitarToMidi g;
    g.prepare (fs);
    midi::EventList out;
    midi::Settings s;
    g.setSettings (s);
    const auto run = [&] (std::size_t from, std::size_t to) {
        for (std::size_t i = from; i + 256 <= to; i += 256)
        {
            out.clear();
            g.process (sc.signal.data() + i, 256, out);
        }
    };
    const std::size_t quarter = sc.signal.size() / 4;
    run (0, quarter); // warm-up (first-use paths)

    gAllocations = 0;
    {
        TrackAllocations track;
        s.timing = midi::Timing::steady;
        s.mode = midi::Mode::notesAndBend;
        g.setSettings (s);
        run (quarter, 2 * quarter);
        s.channel = 5;
        s.octave = -2;
        g.setSettings (s);
        out.clear();
        g.allNotesOff (out, 0);
        run (2 * quarter, 3 * quarter);
        out.clear();
        g.panic (out, 17);
        g.reset();
        run (3 * quarter, sc.signal.size());
    }
    CHECK (gAllocations.load() == 0);
    CHECK (g.notesSent() > 10);
}

TEST_CASE ("The live layer never allocates", "[engine][live]")
{
    // Controllers, toggles, scene switches by controller, program change and the Scene
    // parameter, on-screen switches, learning and the message thread's tick (docs/live.md §7.3).
    test::LiveHarness h;
    h.store[ParamId::pearlEnabled] = 1.0f;
    h.master = live::SceneBank::withSwitches (h.store);
    REQUIRE (h.master.addValue (ParamId::driftMix, 0.2f));
    h.control.setScenes (h.master);
    h.bind (test::bindParam (ParamId::conchPosition, 11));
    h.bind (test::bindParam (ParamId::pearlEnabled, 20));
    h.bind (test::bindAction (live::Action::sceneNext, 40));
    h.bind (test::bindAction (live::Action::presetNext, 41));
    h.map.programScene[3] = 2;
    h.control.setMap (h.map);
    h.events.reserve (4096);

    const std::array<live::MidiMessage, 6> messages { test::cc (11, 0),
                                                      test::cc (20, 127),
                                                      test::cc (40, 127),
                                                      test::cc (41, 127),
                                                      live::MidiMessage::programChange (0, 3),
                                                      live::MidiMessage::programChange (0, 7) };
    auto snapshot = h.store;
    live::LiveEvent event;
    live::LearnMessage learnt;
    h.control.acknowledge(); // warm-up
    h.control.process (snapshot, messages.data(), 1, 64);

    gAllocations = 0;
    {
        TrackAllocations track;
        for (int round = 0; round < 400; ++round)
        {
            h.control.setLearning (round % 50 == 49);
            h.control.acknowledge();
            snapshot = h.store;
            snapshot[ParamId::scene] = static_cast<float> ((round / 30) % live::kNumScenes);
            h.control.process (snapshot, messages.data(), 1 + round % 6, 64);
            if (round % 3 == 0)
            {
                h.control.syncParameters ([&h] (ParamId id, float v) { h.store[id] = v; });
                while (h.control.popEvent (event))
                    ;
                while (h.control.popLearn (learnt))
                    ;
            }
        }
    }
    CHECK (gAllocations.load() == 0);
    CHECK (h.control.messageCount() > 400);
}

// -------------------------------------------------------------------------------------------------
// The looper in the rig (docs/looper.md; the looper's own timing is in tests/looper/).

namespace
{
/// The engine in host blocks over long buffers, with looper presses before given blocks.
struct LoopRig
{
    LoopRig (double fs, int blockSize, int frames)
        : in (1, frames),
          out (2, frames),
          block (blockSize)
    {
        engine.prepare (fs, blockSize, 1, 2);
    }

    /// Processes everything; `presses` are (sample, command), each acting at the first block
    /// that starts at or after its sample.
    void run (const ParamSnapshot& p,
              const std::vector<std::pair<int, LooperCommand>>& presses = {})
    {
        std::size_t next = 0;
        for (int s = 0; s < in.frames(); s += block)
        {
            while (next < presses.size() && presses[next].first <= s)
                engine.looperCommand (presses[next++].second);
            const int n = std::min (block, in.frames() - s);
            std::array<const float*, 2> ip { in.ptrs[0] + s, nullptr };
            std::array<float*, 2> op { out.ptrs[0] + s, out.ptrs[1] + s };
            engine.process (ip.data(), op.data(), n, p, transport);
        }
    }

    Engine engine;
    test::Buffer in, out;
    Transport transport;
    int block;
};

ParamSnapshot quietRig()
{
    auto p = ParamSnapshot::defaults();
    p[ParamId::gateEnabled] = 0.0f;
    return p;
}
} // namespace

TEST_CASE ("Engine: the looper plays back the rig's output, a press acting at the next block",
           "[engine][looper]")
{
    const double fs = 48000.0;
    const int frames = 160000;
    // Transpose adds its own latency, which the looper allows for as well.
    const int semitones = GENERATE (0, 7, -5);
    INFO ("transpose " << semitones);
    LoopRig plain (fs, 64, frames);
    test::fillNoise (plain.in[0], 0.3f, 5);
    std::fill (plain.in[0].begin() + 60000, plain.in[0].end(), 0.0f);
    auto p = quietRig();
    p[ParamId::transpose] = static_cast<float> (semitones);
    plain.run (p);

    LoopRig looped (fs, 64, frames);
    looped.in = plain.in;
    REQUIRE (looped.engine.prepareLooper());
    // Presses land between blocks: the take starts at 6400, closes at 6400 + 40000.
    looped.run (p, { { 6400, LooperCommand::main }, { 46400, LooperCommand::main } });
    const auto& t = looped.engine.telemetry();
    CHECK (t.looperState.load() == static_cast<int> (dsp::Looper::State::playing));
    CHECK (t.looperLength.load() == 40000);

    // Recorded before the click and the output gain, the rig's latency earlier than it arrived.
    const int R = looped.engine.latencySamples();
    if (semitones != 0)
    {
        LoopRig untransposed (fs, 64, 64);
        CHECK (R > untransposed.engine.latencySamples()); // the case does test Transpose's share
    }
    const int X = 480;
    for (int c = 0; c < 2; ++c)
    {
        double worst = 0.0;
        for (int i = X; i < 40000 - X; ++i)
        {
            const auto now = static_cast<std::size_t> (46400 + i);
            const float added = looped.out[c][now] - plain.out[c][now];
            const float recorded = plain.out[c][static_cast<std::size_t> (6400 + R + i)];
            worst = std::max (worst, static_cast<double> (std::abs (added - recorded)));
        }
        INFO ("channel " << c);
        CHECK (worst < 1.0e-6);
    }
    // Before the loop closed, the rig sounded exactly as without the looper.
    for (int c = 0; c < 2; ++c)
        CHECK (
            std::equal (plain.out[c].begin(), plain.out[c].begin() + 46400, looped.out[c].begin()));
}

TEST_CASE ("Engine: the click is never recorded, and a take in bars starts on its downbeat",
           "[engine][looper]")
{
    const double fs = 48000.0;
    const int frames = 300000;
    auto p = metronomeOnly();
    p[ParamId::tempo] = 240.0f; // 4/4: a bar is 48000 samples
    LoopRig plain (fs, 64, frames);
    plain.engine.setMetronomeAllowed (true);
    plain.run (p);

    LoopRig looped (fs, 64, frames);
    looped.engine.setMetronomeAllowed (true);
    REQUIRE (looped.engine.prepareLooper());
    // Armed at 6400, the take starts on the downbeat at 48000; the close at 66560 rounds up to
    // the downbeat at 96000. Then it plays: silence plus nothing of the click.
    looped.run (p, { { 6400, LooperCommand::main }, { 66560, LooperCommand::main } });
    const auto& t = looped.engine.telemetry();
    CHECK (t.looperState.load() == static_cast<int> (dsp::Looper::State::playing));
    CHECK (t.looperLength.load() == 48000);
    CHECK (t.looperBars.load() == 1);
    CHECK ((t.looperFlags.load() & Engine::kLooperSynced) != 0);
    float added = 0.0f;
    for (std::size_t i = 0; i < static_cast<std::size_t> (frames); ++i)
        added = std::max (added, std::abs (looped.out[0][i] - plain.out[0][i]));
    CHECK (test::peak (plain.out[0]) > 0.05f); // the click sounds
    CHECK (added < 1.0e-6f);                   // and was not recorded
}

TEST_CASE ("Engine: preparing again at the same rate keeps the loop, a new rate clears it",
           "[engine][looper]")
{
    LoopRig rig (48000.0, 128, 80000);
    test::fillNoise (rig.in[0], 0.3f, 9);
    REQUIRE (rig.engine.prepareLooper());
    rig.run (quietRig(), { { 1280, LooperCommand::main }, { 31360, LooperCommand::main } });
    const auto& t = rig.engine.telemetry();
    REQUIRE (t.looperLength.load() == 30080);

    const auto block = [&] {
        std::array<const float*, 2> ip { rig.in.ptrs[0], nullptr };
        std::array<float*, 2> op { rig.out.ptrs[0], rig.out.ptrs[1] };
        rig.engine.process (ip.data(), op.data(), 128, quietRig(), rig.transport);
    };
    rig.engine.prepare (48000.0, 512, 1, 2); // a new buffer size, or the Latency setting
    block();
    CHECK (t.looperState.load() == static_cast<int> (dsp::Looper::State::stopped));
    CHECK (t.looperLength.load() == 30080);
    rig.engine.prepare (44100.0, 512, 1, 2);
    block();
    CHECK (t.looperState.load() == static_cast<int> (dsp::Looper::State::empty));
    CHECK (t.looperLength.load() == 0);
}

TEST_CASE ("Engine::process never allocates while the looper runs", "[engine][looper]")
{
    const double fs = GENERATE (44100.0, 96000.0);
    const int block = GENERATE (1, 64, 512);
    CAPTURE (fs, block);
    LoopRig rig (fs, block, 120000);
    test::fillNoise (rig.in[0], 0.3f, 4);
    rig.engine.setMetronomeAllowed (true);
    rig.engine.setLooperDeviceLatency (300);
    REQUIRE (rig.engine.prepareLooper()); // reserves, on this thread
    auto p = quietRig();
    p[ParamId::metronomeEnabled] = 1.0f;
    p[ParamId::tempo] = 400.0f;
    p[ParamId::metronomeSignature] = 0.0f; // 2/4: bars of 0.3 s, so takes close quickly
    p[ParamId::looperCountIn] = 1.0f;
    p[ParamId::looperFeedback] = 70.0f;

    using C = LooperCommand;
    const auto at = [fs] (double seconds) {
        return static_cast<int> (seconds * fs);
    };
    // Count-in, take, overdub, undo, redo, stop, play, overdub, clear, undo the clear, play.
    const std::vector<std::pair<int, C>> presses {
        { at (0.05), C::main },  { at (1.10), C::main }, { at (1.50), C::main },
        { at (1.70), C::main },  { at (1.80), C::undo }, { at (1.85), C::redo },
        { at (1.90), C::stop },  { at (1.95), C::main }, { at (2.10), C::main },
        { at (2.20), C::clear }, { at (2.30), C::undo }, { at (2.40), C::main },
    };
    gAllocations = 0;
    {
        TrackAllocations track;
        rig.run (p, presses); // the presses are pushed on this thread too
    }
    CHECK (gAllocations.load() == 0);
    CHECK (test::allFinite (rig.out[0]));
    CHECK (rig.engine.telemetry().looperLength.load() > 0);
}

TEST_CASE ("Engine: an idle looper leaves the rig's output bit for bit", "[engine][looper]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    Random r (21);
    auto p = randomSnapshot (r);
    p[ParamId::metronomeEnabled] = 1.0f; // its downbeats are read, nothing else changes
    const auto render = [&] (bool withLooper) {
        LoopRig rig (fs, 97, 60000);
        test::fillNoise (rig.in[0], 0.3f, 8);
        rig.engine.setMetronomeAllowed (true);
        if (withLooper)
            rig.engine.prepareLooper();
        rig.run (p);
        return rig.out.data;
    };
    CHECK (render (true) == render (false));
}
