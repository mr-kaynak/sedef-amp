// SPDX-License-Identifier: AGPL-3.0-or-later
// The recorder and re-amp in an offline rig (docs/recorder.md, section 7): the deck around an
// engine, run in host blocks of chosen sizes, with the writer's work done by the test exactly
// when the test says (a stalled disk is a writer that is not called).
#pragma once

#include "TestUtils.h"
#include "sedef/Engine.h"
#include "sedef/record/Deck.h"
#include "sedef/record/ReampFeeder.h"
#include "sedef/record/TakeFiles.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <vector>

namespace sedef::test::rec
{

using namespace sedef::record;

/// Host block sizes: one size, or a cycle of sizes.
struct Blocks
{
    std::vector<int> sizes;
    std::size_t next = 0;
    int operator()() { return sizes[next++ % sizes.size()]; }
};

inline std::vector<std::vector<int>> blockPatterns()
{
    return { { 1 }, { 64 }, { 4096 }, { 3, 17, 255, 1000, 31, 4096, 1, 511 } };
}

/// What the writer thread does, by hand.
struct Writer
{
    explicit Writer (TakeRecorder& r) : recorder (r) {}

    bool start (const TakeRecorder::Config& config)
    {
        di = MemorySink();
        out = MemorySink();
        files = std::make_unique<TakeFiles> (di, out, config);
        return files->begin() && recorder.start (config);
    }

    int drain()
    {
        return recorder.drain ([this] (const TakeChunk& c) { REQUIRE (files->write (c)); });
    }

    /// After the recorder finished.
    void finish (const TakeInfo& info = { "take DI.wav", "take Output.wav", "2026-11-13",
                                          "Sedef Amp test" })
    {
        REQUIRE (recorder.phase() == TakeRecorder::Phase::finished);
        drain();
        REQUIRE (files->finish (recorder, info));
        recorder.release();
        REQUIRE (WavFloatFile::parse (di.bytes, diFile));
        REQUIRE (WavFloatFile::parse (out.bytes, outFile));
    }

    TakeRecorder& recorder;
    MemorySink di, out;
    std::unique_ptr<TakeFiles> files;
    WavFloatFile diFile, outFile;
};

/// An engine and a deck over long buffers. `in` is the live input (re-amp changes it in place,
/// so it ends up holding what the rig heard); `out` collects the output stream.
struct DeckRig
{
    DeckRig (double sampleRate, int ins, int outs, int frames,
             OversamplingMode mode = OversamplingMode::lowLatency, int maxBlock = 4096)
        : fs (sampleRate),
          numIns (ins),
          numOuts (outs),
          in (ins, frames),
          out (outs, frames)
    {
        engine.setOversampling (mode);
        engine.prepare (fs, maxBlock, ins, outs);
        deck.prepare (fs, ins, maxBlock);
    }

    /// Processes n frames from `at`.
    void process (int at, int n, const ParamSnapshot& p)
    {
        std::array<float*, 2> ip { in.ptrs[0] + at, numIns > 1 ? in.ptrs[1] + at : nullptr };
        std::array<float*, 2> op { out.ptrs[0] + at, numOuts > 1 ? out.ptrs[1] + at : nullptr };
        deck.process (engine, ip.data(), op.data(), numIns, numOuts, n, p, transport);
    }

    TakeRecorder::Config takeConfig() const
    {
        TakeRecorder::Config c;
        c.sampleRate = fs;
        c.diChannels = numIns;
        c.outChannels = numOuts;
        return c;
    }

    double fs;
    int numIns, numOuts;
    Engine engine;
    Deck deck;
    Buffer in, out;
    Transport transport;
};

/// A test signal with a guitar's kind of movement: decaying plucks over noise, seeded.
inline std::vector<float> plucks (int frames, double fs, std::uint32_t seed)
{
    std::vector<float> v (static_cast<std::size_t> (frames));
    fillNoise (v, 0.001f, seed);
    Random r (seed * 7919u + 1u);
    const int spacing = static_cast<int> (fs * 0.25);
    for (int start = 0; start < frames; start += spacing)
    {
        const double hz = 82.4 * (1.0 + 3.0 * r.unipolar());
        const float amp = 0.1f + 0.3f * r.unipolar();
        for (int i = 0; start + i < frames && i < spacing; ++i)
            v[static_cast<std::size_t> (start + i)] +=
                amp *
                static_cast<float> (std::exp (-i / (0.08 * fs)) * std::sin (kTwoPiD * hz * i / fs));
    }
    return v;
}

} // namespace sedef::test::rec
