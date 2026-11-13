// SPDX-License-Identifier: AGPL-3.0-or-later
// The looper's footswitches in the live layer (docs/live.md §4.5): which messages are presses,
// and the double tap measured on the audio clock. What the engine makes of them is in
// tests/engine/EngineTests.cpp.

#include "live/LiveTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <tuple>
#include <vector>

using namespace sedef;
using namespace sedef::live;
using sedef::test::bindAction;
using sedef::test::cc;
using sedef::test::LiveHarness;

namespace
{
constexpr int kMain = 60, kStop = 61, kUndo = 62, kClear = 63;

Binding mainSwitch (bool doubleTap, SwitchMode mode = SwitchMode::toggle)
{
    auto b = bindAction (Action::looperMain, kMain, 0, mode);
    b.doubleTap = doubleTap;
    return b;
}

/// The presses of the last block.
std::vector<LooperPress> presses (const LiveControl& c)
{
    std::vector<LooperPress> out;
    for (int i = 0; i < c.numLooperPresses(); ++i)
        out.push_back (c.looperPress (i));
    return out;
}

using P = std::vector<LooperPress>;
} // namespace

TEST_CASE ("Looper footswitches press in Toggle and Latch; releases are ignored", "[live][looper]")
{
    LiveHarness h;
    h.bind (mainSwitch (false));
    h.bind (bindAction (Action::looperStop, kStop, 0, SwitchMode::latch));
    h.bind (bindAction (Action::looperUndo, kUndo));
    h.bind (bindAction (Action::looperClear, kClear));

    h.block ({ cc (kMain, 127) });
    CHECK (presses (h.control) == P { LooperPress::main });
    h.block ({ cc (kMain, 0) }); // a momentary switch's release
    CHECK (presses (h.control).empty());
    h.block();
    CHECK (presses (h.control).empty()); // presses last one block

    // Latch: each message is a press (a latching switch sends the other value every time).
    h.block ({ cc (kStop, 127) });
    CHECK (presses (h.control) == P { LooperPress::stop });
    h.block ({ cc (kStop, 0) });
    CHECK (presses (h.control) == P { LooperPress::stop });

    h.block ({ cc (kUndo, 127), cc (kUndo, 0), cc (kClear, 100) });
    CHECK (presses (h.control) == (P { LooperPress::undo, LooperPress::clear }));
    // Nothing reaches the parameters.
    h.tick();
    CHECK (h.events.empty());
    for (std::size_t i = 0; i < kNumParams; ++i)
        CHECK_FALSE (h.control.overridden (static_cast<ParamId> (i)));
}

TEST_CASE ("A double tap is two presses whose blocks start less than the window apart",
           "[live][looper]")
{
    const auto row = GENERATE (table<double, std::int64_t> ({
        { 44100.0, 17640 },
        { 48000.0, 19200 },
        { 88200.0, 35280 },
        { 96000.0, 38400 },
    }));
    const double fs = std::get<0> (row);
    const std::int64_t window = std::get<1> (row);
    CAPTURE (fs);
    CHECK (doubleTapSamples (fs) == window);

    const auto twoPresses = [fs] (std::int64_t gap) {
        LiveHarness h;
        h.control.prepare (fs);
        h.bind (mainSwitch (true));
        h.block(); // the clock starts at an arbitrary block, not at 0
        P out;
        h.block ({ cc (kMain, 127) }, static_cast<int> (gap)); // the next block starts `gap` later
        out = presses (h.control);
        h.block ({ cc (kMain, 0), cc (kMain, 127) });
        const auto second = presses (h.control);
        out.insert (out.end(), second.begin(), second.end());
        return out;
    };
    CHECK (twoPresses (window - 1) == (P { LooperPress::main, LooperPress::doubleTap }));
    CHECK (twoPresses (window) == (P { LooperPress::main, LooperPress::main }));
    CHECK (twoPresses (1) == (P { LooperPress::main, LooperPress::doubleTap }));
}

TEST_CASE ("Double taps: off by default, a third press starts over, one block, a new map",
           "[live][looper]")
{
    SECTION ("Without the option every press is the main action")
    {
        LiveHarness h;
        h.bind (mainSwitch (false));
        h.block ({ cc (kMain, 127), cc (kMain, 0), cc (kMain, 127) });
        CHECK (presses (h.control) == (P { LooperPress::main, LooperPress::main }));
        h.block ({ cc (kMain, 127) });
        CHECK (presses (h.control) == P { LooperPress::main });
    }
    SECTION ("A third press is a first press again")
    {
        LiveHarness h;
        h.bind (mainSwitch (true));
        h.block ({ cc (kMain, 127) }, 4800);
        h.block ({ cc (kMain, 127) }, 4800);
        CHECK (presses (h.control) == P { LooperPress::doubleTap });
        h.block ({ cc (kMain, 127) }, 4800);
        CHECK (presses (h.control) == P { LooperPress::main });
        h.block ({ cc (kMain, 127) }, 4800);
        CHECK (presses (h.control) == P { LooperPress::doubleTap });
    }
    SECTION ("Two presses in one block")
    {
        LiveHarness h;
        h.bind (mainSwitch (true));
        h.block ({ cc (kMain, 127), cc (kMain, 0), cc (kMain, 127) });
        CHECK (presses (h.control) == (P { LooperPress::main, LooperPress::doubleTap }));
    }
    SECTION ("Latch: a latching switch's presses send 127 and 0 in turn")
    {
        LiveHarness h;
        h.bind (mainSwitch (true, SwitchMode::latch));
        h.block ({ cc (kMain, 127) }, 4800);
        h.block ({ cc (kMain, 0) });
        CHECK (presses (h.control) == P { LooperPress::doubleTap });
    }
    SECTION ("A new map: no press waits for its second")
    {
        LiveHarness h;
        h.bind (mainSwitch (true));
        h.block ({ cc (kMain, 127) });
        h.bind (bindAction (Action::looperStop, kStop));
        h.block ({ cc (kMain, 127) });
        CHECK (presses (h.control) == P { LooperPress::main });
    }
    SECTION ("While learning nothing presses, and nothing waits")
    {
        LiveHarness h;
        h.bind (mainSwitch (true));
        h.control.setLearning (true);
        h.block ({ cc (kMain, 127) });
        CHECK (presses (h.control).empty());
        h.control.setLearning (false);
        h.block ({ cc (kMain, 127) });
        CHECK (presses (h.control) == P { LooperPress::main });
    }
    SECTION ("At most eight presses a block")
    {
        LiveHarness h;
        h.bind (bindAction (Action::looperStop, kStop, 0, SwitchMode::latch));
        std::vector<MidiMessage> many (12, cc (kStop, 127));
        h.block (many);
        CHECK (h.control.numLooperPresses() == kMaxLooperPresses);
    }
}

TEST_CASE ("Footswitch taps read the same at any block size", "[live][looper]")
{
    // Taps 150 ms (a double tap) and 700 ms (two presses) apart, far from the window at every
    // block size: each block applies the messages of its frames at its start.
    constexpr double fs = 48000.0;
    const std::vector<int> gaps { 7200, 33600, 7200, 7200, 33600, 33600, 7200 };
    std::vector<int> times;
    for (int t = 1000; const int g : gaps)
        times.push_back (t += g);

    const auto run = [&times] (int block) {
        LiveHarness h;
        h.control.prepare (fs);
        h.bind (mainSwitch (true));
        P out;
        std::size_t next = 0;
        for (int start = 0; start < times.back() + block; start += block)
        {
            std::vector<MidiMessage> messages;
            while (next < times.size() && times[next] < start + block)
            {
                messages.push_back (cc (kMain, 127));
                messages.push_back (cc (kMain, 0));
                ++next;
            }
            h.block (messages, block);
            const auto p = presses (h.control);
            out.insert (out.end(), p.begin(), p.end());
        }
        return out;
    };
    const auto reference = run (1);
    REQUIRE (reference.size() == times.size());
    for (const int block : { 7, 64, 333, 4096 })
    {
        CAPTURE (block);
        CHECK (run (block) == reference);
    }
}
