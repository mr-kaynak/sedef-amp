// SPDX-License-Identifier: AGPL-3.0-or-later
#include "board/BoardTestUtils.h"
#include "sedef/board/BoardSwitch.h"
#include "sedef/board/Duck.h"
#include "sedef/board/PedalRack.h"
#include "sedef/core/RtMailbox.h"

#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <set>
#include <string>
#include <thread>

using namespace sedef;
using namespace sedef::test;

// -------------------------------------------------------------------------------------------------
// The registry and the board

TEST_CASE ("Every pedal is declared once, with an on/off switch", "[board]")
{
    std::set<std::string_view> ids, names;
    for (std::size_t i = 0; i < kNumPedals; ++i)
    {
        const auto& p = kPedals[i];
        INFO (p.id);
        CHECK (ids.insert (p.id).second);
        CHECK (names.insert (p.name).second);
        CHECK (findPedal (p.id) == static_cast<PedalId> (i));
        const auto& enable = spec (p.enable);
        CHECK (enable.kind == ParamKind::Bool);
        CHECK (enable.automatable);
        CHECK (!categoryName (p.category).empty());
    }
    CHECK_FALSE (findPedal ("nothing").has_value());
    CHECK_FALSE (findPedal ("").has_value());
}

TEST_CASE ("The legacy board is 0.3.0's fixed chain", "[board]")
{
    const auto legacy = Board::legacy();
    REQUIRE (legacy.size() == 5);
    CHECK (legacy == boardOf ({ PedalId::gate, PedalId::tide, PedalId::pearl, PedalId::coral,
                                PedalId::ripple }));

    // A session from 0.3.0 has none of the newer pedals switched on: exactly the legacy board,
    // whichever of its own pedals it switches on or off.
    const auto defaults = ParamSnapshot::defaults();
    CHECK (Board::legacyFor ([&] (ParamId id) { return defaults.flag (id); }) == legacy);
    CHECK (Board::legacyFor ([] (ParamId) { return false; }) == legacy);
    // Every pedal it switches on is there, the legacy ones in their old order.
    const auto all = Board::legacyFor ([] (ParamId) { return true; });
    CHECK (all.size() == kNumPedals);
    for (std::size_t i = 0; i + 1 < legacy.size(); ++i)
        CHECK (*all.indexOf (legacy[i]) < *all.indexOf (legacy[i + 1]));
}

TEST_CASE ("Board edits keep every pedal once and in place", "[board]")
{
    Board b;
    CHECK (b.empty());
    CHECK (b.add (PedalId::ripple));
    CHECK (b.add (PedalId::gate)); // lower rank: in front
    CHECK (b.add (PedalId::pearl));
    CHECK (b == boardOf ({ PedalId::gate, PedalId::pearl, PedalId::ripple }));
    CHECK_FALSE (b.add (PedalId::pearl));
    CHECK_FALSE (b.insert (PedalId::gate, 2));
    CHECK_FALSE (b.insert (PedalId::Count, 0));

    // Add goes after the last pedal at or below its rank, even on a reordered board.
    CHECK (b.move (PedalId::ripple, 0));
    CHECK (b == boardOf ({ PedalId::ripple, PedalId::gate, PedalId::pearl }));
    CHECK (b.add (PedalId::coral));
    CHECK (b == boardOf ({ PedalId::ripple, PedalId::gate, PedalId::pearl, PedalId::coral }));
    CHECK (b.add (PedalId::tide));
    CHECK (b == boardOf ({ PedalId::ripple, PedalId::gate, PedalId::tide, PedalId::pearl,
                           PedalId::coral }));

    CHECK (b.move (PedalId::gate, 99)); // clamped to the end
    CHECK (b[b.size() - 1] == PedalId::gate);
    CHECK (b.remove (PedalId::gate));
    CHECK_FALSE (b.remove (PedalId::gate));
    CHECK_FALSE (b.move (PedalId::gate, 0));
    CHECK (b.size() == 4);

    const auto every = Board::everyPedal();
    CHECK (every.size() == kNumPedals);
    for (std::size_t i = 0; i + 1 < every.size(); ++i)
        CHECK (pedalInfo (every[i]).rank <= pedalInfo (every[i + 1]).rank);
    // Full: nothing more fits, and copies compare by content.
    auto full = every;
    CHECK_FALSE (full.insert (PedalId::gate, 0));
    CHECK (full == every);
    full.remove (PedalId::tide);
    CHECK (full != every);
}

// -------------------------------------------------------------------------------------------------
// Primitives

TEST_CASE ("RtMailbox hands over the newest value", "[board]")
{
    RtMailbox<Board> box;
    Board got;
    CHECK_FALSE (box.read (got));
    box.write (boardOf ({ PedalId::pearl }));
    box.write (boardOf ({ PedalId::coral }));
    REQUIRE (box.read (got));
    CHECK (got == boardOf ({ PedalId::coral }));
    CHECK_FALSE (box.read (got));
    box.write (Board::legacy());
    REQUIRE (box.read (got));
    CHECK (got == Board::legacy());
}

TEST_CASE ("RtMailbox never tears a value across threads", "[board][concurrency]")
{
    struct Pair
    {
        int a = 0, b = 0;
    };
    RtMailbox<Pair> box;
    std::atomic<bool> done { false };
    constexpr int kWrites = 200000;
    std::thread producer ([&] {
        for (int i = 1; i <= kWrites; ++i)
            box.write ({ i, -i });
        done = true;
    });
    int last = 0, torn = 0, backwards = 0, reads = 0;
    Pair p;
    for (;;)
    {
        const bool finished = done.load(); // read before: a value written before it is seen below
        if (!box.read (p))
        {
            if (finished)
                break;
            continue;
        }
        ++reads;
        torn += p.a != -p.b ? 1 : 0;
        backwards += p.a < last ? 1 : 0;
        last = p.a;
    }
    producer.join();
    CHECK (reads > 0);
    CHECK (torn == 0);
    CHECK (backwards == 0);
    CHECK (last == kWrites); // the newest one always gets through
}

TEST_CASE ("The duck's gains are smooth and can turn round", "[board]")
{
    Duck duck;
    duck.prepare (48000.0, 0.006, 40);
    const int length = 288;
    CHECK (duck.idle());
    CHECK (duck.downSamples() == length + 40);
    std::vector<float> in (2000), out (2000);
    CHECK_FALSE (duck.render (in.data(), out.data(), 16)); // unity: nothing to do

    duck.fadeOut();
    CHECK (duck.samplesToSilence() == length + 40);
    REQUIRE (duck.render (in.data(), out.data(), length + 40));
    CHECK (duck.silent());
    CHECK (in[static_cast<size_t> (length - 1)] == Catch::Approx (0.0f).margin (1e-6));
    // The output trails the input by the lag.
    for (int i = 0; i < length; ++i)
        CHECK (out[static_cast<size_t> (i + 40)] == Catch::Approx (in[static_cast<size_t> (i)]));
    CHECK (out[static_cast<size_t> (39)] == 1.0f);
    // Smooth: a raised cosine never steps by more than pi / (2 * length).
    const float maxStep = 3.1416f / (2.0f * length) * 1.01f;
    for (int i = 1; i < length + 40; ++i)
    {
        CHECK (std::abs (in[static_cast<size_t> (i)] - in[static_cast<size_t> (i - 1)]) <= maxStep);
        CHECK (std::abs (out[static_cast<size_t> (i)] - out[static_cast<size_t> (i - 1)]) <=
               maxStep);
    }

    // Back up, turning round half way: from the gain it reached, at most a step away.
    duck.fadeIn();
    duck.render (in.data(), out.data(), 200);
    const float reached = duck.inputGain();
    duck.fadeOut();
    duck.render (in.data(), out.data(), 1);
    CHECK (std::abs (in[0] - reached) <= maxStep);
    duck.fadeIn();
    duck.render (in.data(), out.data(), 2000);
    CHECK (duck.idle());
    CHECK (in[1999] == 1.0f);
    CHECK (out[1999] == 1.0f);
}

// -------------------------------------------------------------------------------------------------
// The rack

TEST_CASE ("The rack plays its pedals in the board's order", "[board]")
{
    // Every legacy pedal on, in several orders: the rack's output is the pedals chained by hand
    // in that order, then the pad, sample for sample.
    auto p = cleanRig ({ ParamId::tideEnabled, ParamId::pearlEnabled, ParamId::coralEnabled,
                         ParamId::rippleEnabled });
    p[ParamId::gateEnabled] = 1.0f;
    p[ParamId::gateThreshold] = -50.0f;
    p[ParamId::rippleMode] = 1.0f;
    const auto settings = PedalSettings::from (p);
    const double fs = 48000.0;
    constexpr int block = 64, frames = 24000;
    const auto x = testSignal (true, fs, frames);

    const auto order = GENERATE (
        boardOf ({ PedalId::coral, PedalId::tide, PedalId::ripple, PedalId::gate }),
        boardOf ({ PedalId::ripple, PedalId::pearl, PedalId::coral }),
        boardOf ({ PedalId::gate, PedalId::tide, PedalId::pearl, PedalId::coral, PedalId::ripple }),
        boardOf ({ PedalId::pearl }), Board {});

    PedalRack rack;
    rack.prepare ({ fs, block, 1 });
    rack.rewire (order);
    rack.reset();
    auto y = x;
    const std::array<bool, kNumPedals> none {};
    for (int s = 0; s < frames; s += block)
        rack.process (y.data() + s, x.data() + s, block, settings, none);

    PedalRack::Slots hand;
    const ProcessSpec mono { fs, block, 1 };
    hand.gate.prepare (mono);
    hand.tide.prepare (mono);
    hand.pearl.prepare (mono);
    hand.coral.prepare (mono);
    hand.ripple.prepare (mono);
    DelayLine pad, keyLine;
    pad.prepare (rack.latencySamples() + 1);
    keyLine.prepare (rack.latencySamples() + 1);
    const int padLength = rack.latencySamples() - rack.latencyOf (order);
    // The gate's key: the input, as late as the pedals in front of the gate make the signal.
    int keyDelay = 0;
    for (const auto id : order)
    {
        if (id == PedalId::gate)
            break;
        keyDelay += rack.latencyOf (id);
    }
    auto z = x;
    std::vector<float> key (block);
    for (int s = 0; s < frames; s += block)
    {
        float* io = z.data() + s;
        for (int i = 0; i < block; ++i)
        {
            keyLine.push (x[static_cast<size_t> (s + i)]);
            key[static_cast<size_t> (i)] = keyLine.read (keyDelay);
        }
        const auto plain = [] (auto& m, const AudioView& v) {
            m.process (v);
        };
        for (const auto id : order)
            switch (id)
            {
            case PedalId::gate:
                hand.gate.process (
                    io, block, true, settings.gate,
                    [&] (dsp::NoiseGate& g, const AudioView& v) { g.process (v, key.data()); });
                break;
            case PedalId::tide:
                hand.tide.process (io, block, true, settings.tide, plain);
                break;
            case PedalId::pearl:
                hand.pearl.process (io, block, true, settings.pearl, plain);
                break;
            case PedalId::coral:
                hand.coral.process (io, block, true, settings.coral, plain);
                break;
            case PedalId::ripple:
                hand.ripple.process (io, block, true, settings.ripple, plain);
                break;
            default:
                FAIL ("a pedal this test doesn't know");
            }
        for (int i = 0; padLength > 0 && i < block; ++i)
        {
            pad.push (io[i]);
            io[i] = pad.read (padLength);
        }
    }
    CHECK (y == z);
}

TEST_CASE ("A keyed pedal hears the instrument lined up with its place on the board", "[board]")
{
    // Pearl switched off is a pure delay of its latency. With the gate after it, the gate must
    // gate the delayed signal exactly as it gates the signal in front of Pearl: its key is
    // delayed by Pearl's latency too.
    auto p = cleanRig ({ ParamId::gateEnabled });
    p[ParamId::gateThreshold] = -40.0f;
    const auto settings = PedalSettings::from (p);
    const double fs = 48000.0;
    constexpr int block = 64, frames = 48000;
    std::vector<float> x (frames);
    // Bursts that open and close the gate.
    for (int i = 0; i < frames; ++i)
        x[static_cast<size_t> (i)] = ((i / 4800) % 2 == 0 ? 0.2f : 0.0005f) *
                                     std::sin (6.283f * 220.0f * static_cast<float> (i) / 48000.0f);

    const auto play = [&] (const Board& order) {
        PedalRack rack;
        rack.prepare ({ fs, block, 1 });
        rack.rewire (order);
        rack.reset();
        auto y = x;
        const std::array<bool, kNumPedals> none {};
        for (int s = 0; s < frames; s += block)
            rack.process (y.data() + s, x.data() + s, block, settings, none);
        return y;
    };
    const auto gateFirst = play (boardOf ({ PedalId::gate, PedalId::pearl }));
    const auto gateAfter = play (boardOf ({ PedalId::pearl, PedalId::gate }));
    CHECK (gateFirst == gateAfter);
    // And the gate does work here: the quiet parts come out quieter than they went in.
    CHECK (rms (gateFirst.data() + 7200, 2000) < 0.7f * rms (x.data() + 7200, 2000));
}

TEST_CASE ("Only a new order or a pedal with latency needs the duck", "[board]")
{
    PedalRack rack;
    rack.prepare ({ 48000.0, 64, 1 });
    REQUIRE (rack.latencyOf (PedalId::pearl) > 0);
    REQUIRE (rack.latencyOf (PedalId::ripple) == 0);
    rack.rewire (Board::legacy());

    auto b = Board::legacy();
    CHECK_FALSE (rack.needsDuck (b));
    b.remove (PedalId::ripple); // no latency: a bypass fade
    CHECK_FALSE (rack.needsDuck (b));
    b.remove (PedalId::gate);
    CHECK_FALSE (rack.needsDuck (b));
    b = Board::legacy();
    b.remove (PedalId::pearl); // latency: the pad has to change
    CHECK (rack.needsDuck (b));
    b = Board::legacy();
    b.move (PedalId::ripple, 0); // a new order
    CHECK (rack.needsDuck (b));
    b = Board::legacy();
    b.move (PedalId::tide, 0); // swapping the first two is a new order too
    CHECK (rack.needsDuck (b));

    rack.rewire (boardOf ({ PedalId::pearl, PedalId::coral }));
    CHECK_FALSE (rack.needsDuck (boardOf (
        { PedalId::tide, PedalId::pearl, PedalId::ripple, PedalId::coral, PedalId::gate })));
    CHECK (rack.needsDuck (boardOf ({ PedalId::coral, PedalId::pearl })));
    CHECK (rack.needsDuck (boardOf ({ PedalId::pearl })));
}

// -------------------------------------------------------------------------------------------------
// Changing the board while it plays

namespace
{
const Board kSwapped =
    boardOf ({ PedalId::gate, PedalId::pearl, PedalId::tide, PedalId::coral, PedalId::ripple });

ParamSnapshot tideAndPearl()
{
    auto p = cleanRig ({ ParamId::tideEnabled, ParamId::pearlEnabled });
    p[ParamId::pearlGain] = 0.3f;
    return p;
}
} // namespace

TEST_CASE ("A new order ducks briefly and without a click", "[board]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    const int block = GENERATE (1, 64, 4096);
    const bool guitar = GENERATE (false, true);
    CAPTURE (fs, block, guitar);

    const auto p = tideAndPearl();
    const auto x = testSignal (guitar, fs, static_cast<int> (1.4 * fs));
    const int at = (static_cast<int> (0.8 * fs) / block) * block;
    int rewiredAt = -1;
    const auto y =
        renderBoard (fs, block, x, p, Board::legacy(), &kSwapped, at, [&] (BoardHarness& h, int s) {
            if (rewiredAt < 0 && h.rack.order() == kSwapped)
                rewiredAt = s;
        });
    const auto t = compareTransient (y, renderBoard (fs, block, x, p, Board::legacy()),
                                     renderBoard (fs, block, x, p, kSwapped), fs, at);

    PedalRack rack;
    rack.prepare ({ fs, 64, 1 });
    const double lagMs = 1000.0 * rack.latencySamples() / fs;
    const double fadeMs = 1000.0 * BoardSwitch::kDuckSeconds;
    Duck duck;
    duck.prepare (fs, BoardSwitch::kDuckSeconds, rack.latencySamples());
    const int down = duck.downSamples();
    // A dip of two fades and the lag (1 ms windows: one more at each end)...
    CHECK (t.dipMs <= 2.0 * fadeMs + lagMs + 2.0);
    CHECK (t.dipMs >= 2.0 * fadeMs * 0.7);
    // ...to silence...
    CHECK (t.dipDb < -40.0f);
    // ...back to the same level (Tide catching up after the dip),...
    CHECK (t.swellDb < 4.5f);
    // ...with no step: a hard swap puts its second difference 20 dB under the signal's peak, the
    // duck far below that.
    CHECK (t.excessDb < -50.0f);
    // The order changed at the silence.
    REQUIRE (rewiredAt >= 0);
    CHECK (rewiredAt - at >= down);
    CHECK (rewiredAt - at <= down + block);
}

TEST_CASE ("Through the whole rig a new order stays click-free", "[board]")
{
    const double fs = GENERATE (48000.0, 96000.0);
    const bool guitar = GENERATE (false, true);
    CAPTURE (fs, guitar);
    const auto p = tideAndPearl();
    const auto x = testSignal (guitar, fs, static_cast<int> (1.4 * fs));
    const int at = (static_cast<int> (0.8 * fs) / 64) * 64;
    const auto t = compareTransient (renderEngine (fs, 64, x, p, Board::legacy(), &kSwapped, at),
                                     renderEngine (fs, 64, x, p, Board::legacy()),
                                     renderEngine (fs, 64, x, p, kSwapped), fs, at);
    // The repo's click bound for automation sweeps (PedalTestUtils.h).
    CHECK (t.slopeRatio <= (guitar ? 1.2f : 1.6f));
}

TEST_CASE ("Pedals without latency join and leave through their bypass fade", "[board]")
{
    // Ripple (chorus, on) leaves and comes back: no duck, and it stays in the order until its
    // fade has ended.
    const double fs = 48000.0;
    const int block = GENERATE (1, 64, 512);
    CAPTURE (block);
    auto p = cleanRig ({ ParamId::tideEnabled, ParamId::rippleEnabled });
    p[ParamId::rippleMode] = 1.0f;
    const auto x = testSignal (false, fs, 72000);
    auto without = Board::legacy();
    without.remove (PedalId::ripple);
    const int leaveAt = (24000 / block) * block, joinAt = (48000 / block) * block;

    BoardHarness h (fs, std::min (block, 512), Board::legacy());
    auto y = x;
    bool ducked = false;
    int leftAt = -1, joinedAt = -1;
    h.run (y.data(), x.data(), 72000, block, p, [&] (int s) {
        if (s == leaveAt)
            h.board.setTarget (without);
        if (s == joinAt)
            h.board.setTarget (Board::legacy());
        ducked = ducked || !h.board.duck().idle();
        if (leftAt < 0 && s > leaveAt && !h.rack.order().contains (PedalId::ripple))
            leftAt = s;
        if (joinedAt < 0 && s > joinAt && h.rack.order().contains (PedalId::ripple))
            joinedAt = s;
    });
    CHECK_FALSE (ducked);
    // The bypass fade is 10 ms; the pedal leaves at the first block boundary after it.
    CHECK (leftAt - leaveAt >= 480);
    CHECK (leftAt - leaveAt <= 480 + 2 * block);
    CHECK (joinedAt - joinAt <= block);

    const auto with = renderBoard (fs, block, x, p, Board::legacy());
    const auto withoutRipple = renderBoard (fs, block, x, p, without);
    // Leaving: no dip below the quieter of the two sounds, no step.
    const auto out = compareTransient (y, with, withoutRipple, fs, leaveAt);
    CHECK (out.dipDb > -3.5f); // a chorus crossfading with its dry signal
    CHECK (out.excessDb < -50.0f);
    // Joining is the pedal's own footswitch: it comes out of bypass from a clean state, exactly
    // as when it is switched on where it stands.
    auto off = p;
    off[ParamId::rippleEnabled] = 0.0f;
    BoardHarness toggled (fs, std::min (block, 512), Board::legacy());
    auto z = x;
    toggled.run (z.data(), x.data(), 72000, block, off, [&] (int s) {
        if (s == joinAt)
            off[ParamId::rippleEnabled] = 1.0f;
    });
    // (Not sample for sample: Ripple's control-rate counter isn't part of its reset.)
    const auto joined = compareTransient (y, withoutRipple, with, fs, joinAt);
    const auto switched = compareTransient (z, withoutRipple, with, fs, joinAt);
    CHECK (joined.excessDb <= switched.excessDb + 1.0f);
    CHECK (joined.dipDb >= switched.dipDb - 0.5f);
    CHECK (joined.swellDb <= switched.swellDb + 0.5f);
}

TEST_CASE ("A pedal with latency leaves under the duck, and the timing stays", "[board]")
{
    // Pearl, switched on, leaves; everything else is off, so the board is then a pure delay of
    // the rack's latency, exactly as with Pearl switched off on the legacy board.
    const double fs = GENERATE (44100.0, 96000.0);
    CAPTURE (fs);
    const int block = 64;
    const auto x = testSignal (true, fs, static_cast<int> (fs));
    const auto on = cleanRig ({ ParamId::pearlEnabled });
    auto without = Board::legacy();
    without.remove (PedalId::pearl);
    const int at = (static_cast<int> (0.5 * fs) / block) * block;
    bool ducked = false;
    const auto y =
        renderBoard (fs, block, x, on, Board::legacy(), &without, at,
                     [&] (BoardHarness& h, int) { ducked = ducked || !h.board.duck().idle(); });
    CHECK (ducked);
    PedalRack rack;
    rack.prepare ({ fs, block, 1 });
    const int latency = rack.latencySamples();
    // Once the duck is over, the output is the input `latency` samples late, sample for sample.
    const int after = at + static_cast<int> (0.03 * fs);
    for (int i = after; i < static_cast<int> (x.size()); ++i)
        REQUIRE (y[static_cast<size_t> (i)] == x[static_cast<size_t> (i - latency)]);
}

TEST_CASE ("A change of mind on the way down comes back up without a rewire", "[board]")
{
    const double fs = 48000.0;
    const auto p = tideAndPearl();
    const auto x = testSignal (true, fs, 48000);
    const int at = 24000;
    bool rewired = false;
    float lowest = 1.0f;
    const auto y =
        renderBoard (fs, 1, x, p, Board::legacy(), &kSwapped, at, [&] (BoardHarness& h, int s) {
            if (s == at + 100) // well before the silence
                h.board.setTarget (Board::legacy());
            rewired = rewired || h.rack.order() == kSwapped;
            lowest = std::min (lowest, h.board.duck().inputGain());
        });
    CHECK_FALSE (rewired);
    CHECK (lowest < 0.95f);
    CHECK (lowest > 0.5f);
    const auto ref = renderBoard (fs, 1, x, p, Board::legacy());
    const auto t = compareTransient (y, ref, ref, fs, at);
    CHECK (t.excessDb < -50.0f);
}

TEST_CASE ("The latency never changes with the board", "[board]")
{
    // With every pedal switched off, any board is the same pure delay: an impulse comes out at
    // the same sample, at the same height, through the whole engine.
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    CAPTURE (fs);
    const auto boards = { Board::legacy(), Board {}, Board::everyPedal(),
                          boardOf ({ PedalId::ripple, PedalId::coral }),
                          boardOf ({ PedalId::pearl, PedalId::tide, PedalId::gate }) };
    std::vector<float> first;
    int latency = -1;
    for (const auto& board : boards)
    {
        BoardRig rig (fs, 128, 8192);
        rig.engine.setBoard (board);
        rig.engine.reset();
        if (latency < 0)
            latency = rig.engine.latencySamples();
        CHECK (rig.engine.latencySamples() == latency);
        rig.in[0][1000] = 0.5f;
        rig.run (cleanRig(), 0, 8192);
        if (first.empty())
            first = rig.out[0];
        else
            CHECK (rig.out[0] == first);
    }
}

TEST_CASE ("Pedals off the board are not processed", "[board]")
{
    // Every pedal switched on, only some on the board: the others never run.
    BoardRig rig (48000.0, 256, 48000);
    fillSine (rig.in[0], 220.0, 48000.0, 0.2f);
    auto p = cleanRig ({ ParamId::gateEnabled, ParamId::tideEnabled, ParamId::pearlEnabled,
                         ParamId::coralEnabled, ParamId::rippleEnabled });
    const auto check = [&] (const Board& board) {
        rig.engine.setBoard (board);
        rig.run (p, 0, 24000); // well past any fade or duck
        CHECK_FALSE (rig.engine.telemetry().boardChanging.load());
        for (std::size_t i = 0; i < kNumPedals; ++i)
        {
            const auto id = static_cast<PedalId> (i);
            INFO (pedalInfo (id).id);
            CHECK (rig.engine.telemetry().pedalRan[i].load() == board.contains (id));
        }
    };
    check (boardOf ({ PedalId::tide }));
    check (boardOf ({ PedalId::ripple, PedalId::gate, PedalId::coral }));
    check (Board {});
    check (Board::everyPedal());
}

TEST_CASE ("A board set before prepare plays from the first block", "[board]")
{
    Engine engine;
    engine.setBoard (boardOf ({ PedalId::coral }));
    engine.prepare (48000.0, 64, 1, 2);
    std::vector<float> in (64, 0.1f), l (64), r (64);
    const float* ins[1] = { in.data() };
    float* outs[2] = { l.data(), r.data() };
    auto p = cleanRig ({ ParamId::tideEnabled, ParamId::coralEnabled });
    Transport transport;
    engine.process (ins, outs, 64, p, transport);
    CHECK (engine.telemetry().pedalRan[index (PedalId::coral)].load());
    CHECK_FALSE (engine.telemetry().pedalRan[index (PedalId::tide)].load());
    CHECK_FALSE (engine.telemetry().boardChanging.load());
}

TEST_CASE ("Both chains play the board in Dual mode", "[board]")
{
    // The same input on both sides: the two chains must come out identical through a board
    // change (they rewire together).
    BoardRig rig (48000.0, 64, 48000, 2, 2);
    fillGuitarTone (rig.in[0], 48000.0, 0.3f);
    rig.in[1] = rig.in[0];
    auto p = tideAndPearl();
    p[ParamId::stereoMode] = 1.0f; // Dual
    p[ParamId::coralEnabled] = 1.0f;
    rig.run (p, 0, 48000, [&] (int s) {
        if (s == 12000)
            rig.engine.setBoard (kSwapped);
        if (s == 24000)
            rig.engine.setBoard (boardOf ({ PedalId::coral, PedalId::tide }));
    });
    CHECK (rig.out[0] == rig.out[1]);
    CHECK (rms (rig.out[0], 40000) > 1.0e-3f);
}
