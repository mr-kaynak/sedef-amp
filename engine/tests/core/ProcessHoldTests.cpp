// SPDX-License-Identifier: AGPL-3.0-or-later
// The hold that keeps the audio thread out while the engine is prepared again
// (docs/robustness.md, section 1). The threaded cases are for the TSan job: a missing step in
// the protocol shows up there as a data race.
#include "sedef/Engine.h"
#include "sedef/core/ProcessHold.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <thread>
#include <vector>

using namespace sedef;

TEST_CASE ("ProcessHold: blocks enter while nothing holds", "[core][hold]")
{
    ProcessHold hold;
    CHECK (hold.audioIsOut());
    CHECK_FALSE (hold.isHeld());
    REQUIRE (hold.enter());
    CHECK_FALSE (hold.audioIsOut());
    hold.leave();
    CHECK (hold.audioIsOut());
}

TEST_CASE ("ProcessHold: a raised hold keeps blocks out until it is lowered", "[core][hold]")
{
    ProcessHold hold;
    hold.raise();
    CHECK (hold.isHeld());
    CHECK_FALSE (hold.enter()); // backs off, and leaves nothing counted
    CHECK (hold.audioIsOut());
    {
        const ProcessHold::Pass pass (hold);
        CHECK_FALSE (static_cast<bool> (pass));
    }
    CHECK (hold.audioIsOut());
    hold.lower();
    CHECK_FALSE (hold.isHeld());
    const ProcessHold::Pass pass (hold);
    CHECK (static_cast<bool> (pass));
}

TEST_CASE ("ProcessHold: holds nest", "[core][hold]")
{
    ProcessHold hold;
    hold.raise();
    hold.raise();
    hold.lower();
    CHECK_FALSE (hold.enter()); // one hold is still raised
    hold.lower();
    CHECK (hold.enter());
    hold.leave();
}

TEST_CASE ("ProcessHold: the holder waits for the block inside, and only for it", "[core][hold]")
{
    ProcessHold hold;
    REQUIRE (hold.enter()); // a block is running
    hold.raise();           // the message thread wants to prepare
    CHECK_FALSE (hold.audioIsOut());

    // The block ends during the third round of waiting.
    int rounds = 0;
    const bool out = hold.waitUntilOut ([&] (int round) {
        rounds = round + 1;
        if (round == 2)
            hold.leave();
        return true;
    });
    CHECK (out);
    CHECK (rounds == 3);
    // The next block stays out while the holder works.
    CHECK_FALSE (hold.enter());
    hold.lower();
    CHECK (hold.enter());
    hold.leave();
}

TEST_CASE ("ProcessHold: a holder that gives up reports the block still inside", "[core][hold]")
{
    ProcessHold hold;
    REQUIRE (hold.enter());
    hold.raise();
    CHECK_FALSE (hold.waitUntilOut ([] (int round) { return round < 5; }));
    CHECK_FALSE (hold.waitUntilOut (std::chrono::nanoseconds (0)));
    hold.lower();
    hold.leave();
    CHECK (hold.audioIsOut());
    {
        const ProcessHold::Scope scope (hold, std::chrono::milliseconds (0));
        CHECK (scope.audioIsOut()); // nothing inside: no wait at all
        CHECK_FALSE (hold.enter());
    }
    CHECK_FALSE (hold.isHeld());
}

TEST_CASE ("ProcessHold: a holder's writes never meet a block's reads", "[core][hold][concurrency]")
{
    // What the "engine" holds: two fields a prepare writes and a block reads. A block that saw
    // them half-written, or a data race on them (TSan), fails.
    struct State
    {
        int a = 0;
        int b = 0;
    };
    State state;
    ProcessHold hold;
    std::atomic<bool> done { false };
    std::atomic<int> torn { 0 };
    std::atomic<int> blocks { 0 }, heldBlocks { 0 };

    std::thread audio ([&] {
        while (!done.load())
        {
            const ProcessHold::Pass pass (hold);
            if (!pass)
            {
                heldBlocks.fetch_add (1);
                std::this_thread::yield();
                continue;
            }
            if (state.a != state.b)
                torn.fetch_add (1);
            blocks.fetch_add (1);
        }
    });

    constexpr int kPrepares = 2000;
    int prepared = 0;
    for (int i = 0; i < kPrepares; ++i)
    {
        // Every other round, let a block in first, so holds meet blocks in every phase.
        if (i % 2 == 0)
            for (const int seen = blocks.load(); blocks.load() == seen;)
                std::this_thread::yield();
        const ProcessHold::Scope scope (hold, std::chrono::seconds (5));
        if (!scope.audioIsOut())
            continue;
        state.a = i + 1;
        std::this_thread::yield(); // widen the window a block would see
        state.b = i + 1;
        ++prepared;
    }
    done.store (true);
    audio.join();
    CHECK (prepared == kPrepares);
    CHECK (torn.load() == 0);
    CHECK (blocks.load() >= kPrepares / 2);
}

TEST_CASE ("ProcessHold: an engine prepared again while another thread processes it",
           "[core][hold][concurrency]")
{
    // Settings > Latency in a host whose wrapper does not keep the audio thread out
    // (docs/robustness.md, section 1): prepare() on one thread, process() on another.
    Engine engine;
    engine.prepare (48000.0, 256, 1, 2);
    ProcessHold hold;
    std::atomic<bool> done { false };
    std::atomic<bool> finite { true };
    std::atomic<int> blocks { 0 };

    std::thread audio ([&] {
        std::vector<float> in (256), left (256), right (256);
        const float* inputs[] = { in.data() };
        float* outputs[] = { left.data(), right.data() };
        const auto params = ParamSnapshot::defaults();
        int n = 0;
        while (!done.load())
        {
            for (auto& x : in)
                x = 0.25f * std::sin (0.031f * static_cast<float> (n++ % 4096));
            const ProcessHold::Pass pass (hold);
            if (!pass)
            {
                std::this_thread::yield();
                continue;
            }
            engine.process (inputs, outputs, 256, params, {});
            for (int i = 0; i < 256; ++i)
                if (!std::isfinite (left[static_cast<std::size_t> (i)]))
                    finite.store (false);
            blocks.fetch_add (1);
        }
    });

    for (int i = 0; i < 6; ++i)
    {
        // Let a few blocks run between prepares.
        const int seen = blocks.load();
        while (blocks.load() < seen + 3)
            std::this_thread::yield();
        const ProcessHold::Scope scope (hold, std::chrono::seconds (10));
        REQUIRE (scope.audioIsOut());
        engine.setOversampling (i % 2 == 0 ? OversamplingMode::linearPhase
                                           : OversamplingMode::lowLatency);
        engine.prepare (48000.0, 256, 1, 2);
    }
    done.store (true);
    audio.join();
    CHECK (finite.load());
    CHECK (engine.oversampling() == OversamplingMode::lowLatency);
}
