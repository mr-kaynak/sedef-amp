// SPDX-License-Identifier: AGPL-3.0-or-later
// The live layer between MIDI and the parameter snapshot (docs/live.md §7): overrides and their
// acknowledgement, bindings, scenes switched on either thread, program changes, learn.

#include "live/LiveTestUtils.h"
#include "sedef/core/Random.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <map>

using namespace sedef;
using namespace sedef::live;
using sedef::test::bindAction;
using sedef::test::bindParam;
using sedef::test::cc;
using sedef::test::LiveHarness;

TEST_CASE ("A controller sets its parameter in the block of the message", "[live]")
{
    LiveHarness h;
    auto b = bindParam (ParamId::conchPosition, 11);
    h.bind (b);

    auto s = h.block ({ cc (11, 127) });
    CHECK (s[ParamId::conchPosition] == 1.0f);
    CHECK (h.control.overridden (ParamId::conchPosition));
    // The host's parameter has not moved yet: the override holds the value until the message
    // thread writes it.
    CHECK (h.store[ParamId::conchPosition] == spec (ParamId::conchPosition).defaultValue);
    s = h.block();
    CHECK (s[ParamId::conchPosition] == 1.0f);

    h.tick();
    CHECK (h.store[ParamId::conchPosition] == 1.0f);
    s = h.block();
    CHECK_FALSE (h.control.overridden (ParamId::conchPosition));
    CHECK (s[ParamId::conchPosition] == 1.0f);

    // Once it ended, the host's parameter is in charge again.
    h.store[ParamId::conchPosition] = 0.25f;
    CHECK (h.block()[ParamId::conchPosition] == 0.25f);
}

TEST_CASE ("An override ends only once its latest value is acknowledged", "[live]")
{
    LiveHarness h;
    h.bind (bindParam (ParamId::conchPosition, 11));
    h.block ({ cc (11, 0) });
    h.control.syncParameters ([&] (ParamId id, float v) { h.store[id] = v; });
    // A newer value arrives before the acknowledgement is read: the override stays, with it.
    auto s = h.block ({ cc (11, 127) });
    CHECK (s[ParamId::conchPosition] == 1.0f);
    s = h.block();
    CHECK (h.control.overridden (ParamId::conchPosition));
    CHECK (s[ParamId::conchPosition] == 1.0f);
    h.tick();
    h.block();
    CHECK_FALSE (h.control.overridden (ParamId::conchPosition));
}

TEST_CASE ("Footswitches toggle and latch through the live layer", "[live]")
{
    LiveHarness h;
    h.bind (bindParam (ParamId::pearlEnabled, 20, 0, SwitchMode::toggle));
    h.bind (bindParam (ParamId::coralEnabled, 21, 0, SwitchMode::latch));

    CHECK (h.block ({ cc (20, 127) })[ParamId::pearlEnabled] == 1.0f);
    CHECK (h.block ({ cc (20, 0) })[ParamId::pearlEnabled] == 1.0f); // release
    h.tick();
    CHECK (h.block ({ cc (20, 127) })[ParamId::pearlEnabled] == 0.0f);

    CHECK (h.block ({ cc (21, 127) })[ParamId::coralEnabled] == 1.0f);
    CHECK (h.block ({ cc (21, 0) })[ParamId::coralEnabled] == 0.0f);

    // Two presses in one block: the second reads the first's result, so they cancel out.
    const bool before = h.block()[ParamId::pearlEnabled] >= 0.5f;
    const auto s = h.block ({ cc (20, 127), cc (20, 0), cc (20, 127) });
    CHECK ((s[ParamId::pearlEnabled] >= 0.5f) == before);
}

TEST_CASE ("One controller can drive several targets; channels must match", "[live]")
{
    LiveHarness h;
    h.bind (bindParam (ParamId::pearlEnabled, 20, 2));
    h.bind (bindParam (ParamId::driftEnabled, 20, 2));
    h.bind (bindParam (ParamId::driftMix, 20, 2));

    auto s = h.block ({ cc (20, 127, 3) }); // another channel
    CHECK_FALSE (h.control.overridden (ParamId::pearlEnabled));
    s = h.block ({ cc (20, 127, 2) });
    CHECK (s[ParamId::pearlEnabled] == 1.0f);
    CHECK (s[ParamId::driftEnabled] == 1.0f - spec (ParamId::driftEnabled).defaultValue);
    CHECK (s[ParamId::driftMix] == spec (ParamId::driftMix).max);
}

namespace
{
/// Scene 1 (index 1) differs from the preset: Pearl off, Coral and Drift on, Drift Mix 0.6.
void twoScenes (LiveHarness& h)
{
    h.store[ParamId::pearlEnabled] = 1.0f;
    h.store[ParamId::driftMix] = 0.2f;
    h.master = SceneBank::withSwitches (h.store);
    REQUIRE (h.master.addValue (ParamId::driftMix, 0.2f));
    auto& scene = h.master.values[1];
    scene[static_cast<size_t> (h.master.find (ParamId::pearlEnabled))] = 0.0f;
    scene[static_cast<size_t> (h.master.find (ParamId::coralEnabled))] = 1.0f;
    scene[static_cast<size_t> (h.master.find (ParamId::driftEnabled))] = 1.0f;
    scene[static_cast<size_t> (h.master.find (ParamId::driftMix))] = 0.6f;
    h.master.made = 0b11;
    h.control.setScenes (h.master);
    h.block();
}

void checkScene (const ParamSnapshot& s, const SceneBank& bank, int scene)
{
    for (int i = 0; i < bank.numParams; ++i)
    {
        const auto id = static_cast<ParamId> (bank.params[static_cast<size_t> (i)]);
        CAPTURE (spec (id).id, scene);
        CHECK (s[id] == bank.values[static_cast<size_t> (scene)][static_cast<size_t> (i)]);
    }
    CHECK (s[ParamId::scene] == static_cast<float> (scene));
}
} // namespace

TEST_CASE ("A scene switch sets exactly the stored state in the block of the message", "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.bind (bindAction (Action::scene, 30, 1));

    const auto before = h.block();
    const auto s = h.block ({ cc (30, 127) });
    checkScene (s, h.master, 1);
    CHECK (h.control.activeScene() == 1);
    // Nothing outside the scene moved.
    for (std::size_t i = 0; i < kNumParams; ++i)
        if (h.master.find (static_cast<ParamId> (i)) < 0 && i != index (ParamId::scene))
            CHECK (s.values[i] == before.values[i]);

    // The report carries the values the first scene was playing.
    h.tick();
    REQUIRE (h.events.size() == 1);
    const auto& r = h.events[0].scene;
    CHECK (r.from == 0);
    CHECK (r.to == 1);
    for (int i = 0; i < r.numParams; ++i)
        CHECK (r.values[static_cast<size_t> (i)] ==
               before.values[r.params[static_cast<size_t> (i)]]);
    CHECK (h.master.active == 1);
    CHECK (h.store[ParamId::scene] == 1.0f);
    CHECK (h.store[ParamId::driftMix] == 0.6f);
}

TEST_CASE ("Scenes remember what changed in them, also between message-thread ticks", "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.bind (bindAction (Action::scene, 30, 0));
    h.bind (bindAction (Action::scene, 31, 1));
    h.bind (bindParam (ParamId::driftMix, 7));
    h.bind (bindParam (ParamId::gyreEnabled, 22));

    // 0 -> 1, change two things in scene 1, 1 -> 0 -> 1: all before the message thread runs.
    h.block ({ cc (31, 127) });
    h.block ({ cc (7, 127), cc (22, 127) });
    auto s = h.block ({ cc (30, 127) });
    CHECK (s[ParamId::driftMix] == 0.2f);
    CHECK (s[ParamId::gyreEnabled] == 0.0f);
    s = h.block ({ cc (31, 127) });
    CHECK (s[ParamId::driftMix] == 1.0f);
    CHECK (s[ParamId::gyreEnabled] == 1.0f);

    // The message thread then learns all three switches, and its bank agrees.
    h.tick();
    CHECK (h.master.active == 1);
    CHECK (h.master.reportsSeen == 3);
    const auto at = [&h] (ParamId id) {
        return static_cast<size_t> (h.master.find (id));
    };
    CHECK (h.master.values[1][at (ParamId::driftMix)] == 1.0f);
    CHECK (h.master.values[1][at (ParamId::gyreEnabled)] == 1.0f);
    CHECK (h.master.values[0][at (ParamId::driftMix)] == 0.2f);
    CHECK (h.master.values[0][at (ParamId::gyreEnabled)] == 0.0f);
}

TEST_CASE ("A scene not made yet starts as a copy of the scene being left", "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.bind (bindAction (Action::scene, 31, 1));
    h.bind (bindAction (Action::scene, 35, 5));
    h.bind (bindParam (ParamId::gyreEnabled, 22));

    // From scene 2 (made, Pearl off, Coral on) to scene 6 (not made): nothing changes.
    h.block ({ cc (31, 127) });
    h.block ({ cc (22, 127) }); // Gyre on in scene 2
    const auto before = h.block();
    const auto s = h.block ({ cc (35, 127) });
    CHECK (s.values == [&] {
        auto expected = before;
        expected[ParamId::scene] = 5.0f;
        return expected;
    }().values);
    CHECK (h.control.bank().isMade (5));
    h.tick();
    CHECK (h.master.isMade (5));
    CHECK (h.master.values[5] == h.master.values[1]);
    CHECK_FALSE (h.master.isMade (3));
}

TEST_CASE ("Next and previous scene wrap round; selecting the active scene does nothing", "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.bind (bindAction (Action::sceneNext, 40));
    h.bind (bindAction (Action::scenePrevious, 41));
    h.bind (bindAction (Action::scene, 42, 0));

    h.block ({ cc (41, 127) });
    CHECK (h.control.activeScene() == 7);
    h.block ({ cc (40, 127), cc (40, 0), cc (40, 127) }); // a release does nothing
    CHECK (h.control.activeScene() == 1);
    h.tick();
    const auto reports = h.events.size();
    h.block ({ cc (42, 127) });
    h.block ({ cc (42, 127) });
    h.tick();
    CHECK (h.events.size() == reports + 1);
    CHECK (h.control.activeScene() == 0);
}

TEST_CASE ("Latch scene targets select on every message", "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.bind (bindAction (Action::sceneNext, 40, 0, SwitchMode::latch));
    h.block ({ cc (40, 127) });
    h.block ({ cc (40, 0) }); // the next press of a latching switch
    CHECK (h.control.activeScene() == 2);
}

TEST_CASE ("A bank that hasn't taken in every report is not adopted", "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.bind (bindAction (Action::scene, 31, 1));
    h.block ({ cc (31, 127) });
    CHECK (h.control.activeScene() == 1);

    // The message thread publishes a bank before it read the report (an edit made meanwhile).
    auto stale = h.master;
    stale.applySwitch = false;
    h.control.setScenes (stale);
    h.block();
    CHECK (h.control.activeScene() == 1);

    // Once it took the report in, its bank is adopted.
    h.tick();
    h.block();
    CHECK (h.control.bank().reportsSeen == 1);
    CHECK (h.control.activeScene() == 1);
}

TEST_CASE ("A switch made on screen lands in one block", "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.screenSwitch (1);
    // The audio thread may read the host's parameters before, during or after they are written:
    // the overrides fill in the rest, so the block plays the whole scene either way.
    auto half = h.store;
    for (std::size_t i = 0; i < kNumParams / 2; ++i)
        half.values[i] = ParamSnapshot::defaults().values[i];
    h.control.acknowledge();
    auto s = half;
    h.control.process (s, nullptr, 0, 64);
    checkScene (s, h.master, 1);
    CHECK (h.control.consumedSwitchCount() == h.master.switchCount);
}

TEST_CASE ("An on-screen switch withdrawn before the audio thread read it applies nothing",
           "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.screenSwitch (1);
    // No audio ran; the message thread withdraws the apply (docs/live.md §7.4), then the player
    // changes a scene value by hand.
    h.master.applySwitch = false;
    h.control.setScenes (h.master);
    h.store[ParamId::driftMix] = 0.9f;
    const auto s = h.block();
    CHECK (s[ParamId::driftMix] == 0.9f);
    CHECK_FALSE (h.control.overridden (ParamId::driftMix));
    CHECK (h.control.activeScene() == 1);
}

TEST_CASE ("The Scene parameter switches scenes", "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.store[ParamId::scene] = 1.0f; // host automation
    const auto s = h.block();
    checkScene (s, h.master, 1);
    h.tick();
    REQUIRE (h.events.size() == 1);
    CHECK (h.events[0].scene.to == 1);
    // The parameter following a switch made elsewhere is not a second switch.
    h.bind (bindAction (Action::scene, 30, 2));
    h.block ({ cc (30, 127) });
    h.tick();
    h.block();
    h.tick();
    CHECK (h.store[ParamId::scene] == 2.0f);
    CHECK (h.events.size() == 2);
}

TEST_CASE ("Program changes select scenes or ask for presets", "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.map.programScene[9] = 1;
    h.control.setMap (h.map);

    h.block ({ MidiMessage::programChange (0, 4) });
    h.block ({ MidiMessage::programChange (5, 9) });
    CHECK (h.control.activeScene() == 1);
    h.tick();
    REQUIRE (h.events.size() == 2);
    CHECK (h.events[0].type == LiveEvent::Type::program);
    CHECK (h.events[0].program == 4);
    CHECK (h.events[1].type == LiveEvent::Type::sceneSwitched);

    // One program channel: others are ignored.
    h.map.programChannel = 2;
    h.control.setMap (h.map);
    h.block ({ MidiMessage::programChange (0, 4) });
    h.block ({ MidiMessage::programChange (2, 5) });
    h.tick();
    REQUIRE (h.events.size() == 3);
    CHECK (h.events[2].program == 5);
}

TEST_CASE ("Preset next and previous go to the message thread", "[live]")
{
    LiveHarness h;
    h.bind (bindAction (Action::presetNext, 50));
    h.bind (bindAction (Action::presetPrevious, 51));
    h.block ({ cc (50, 127), cc (50, 0), cc (51, 127) });
    h.tick();
    REQUIRE (h.events.size() == 2);
    CHECK (h.events[0].step == 1);
    CHECK (h.events[1].step == -1);
}

TEST_CASE ("While learning, messages go to learn and act on nothing", "[live]")
{
    LiveHarness h;
    h.bind (bindParam (ParamId::pearlEnabled, 20));
    h.control.setLearning (true);
    h.block ({ cc (20, 127), MidiMessage::programChange (0, 3) }, 480);
    const auto s = h.block ({ cc (20, 0) });
    CHECK_FALSE (h.control.overridden (ParamId::pearlEnabled));
    LearnMessage l;
    REQUIRE (h.control.popLearn (l));
    CHECK (l.message.data1 == 20);
    CHECK (l.time == 0);
    REQUIRE (h.control.popLearn (l));
    CHECK (l.message.isProgramChange());
    REQUIRE (h.control.popLearn (l));
    CHECK (l.message.data2 == 0);
    CHECK (l.time == 480);
    CHECK_FALSE (h.control.popLearn (l));
    h.tick();
    CHECK (h.events.empty());
    CHECK (h.control.messageCount() == 3);
    CHECK (h.control.lastMessage() == (0xb0u << 16 | 20u << 8 | 0u));
    CHECK (s[ParamId::pearlEnabled] == 0.0f);
}

TEST_CASE ("A switch that can't be reported doesn't happen", "[live]")
{
    LiveHarness h;
    twoScenes (h);
    h.bind (bindAction (Action::sceneNext, 40));
    for (int i = 0; i < LiveControl::kEventCapacity + 5; ++i)
        h.block ({ cc (40, 127) }); // the message thread is stalled: nothing drains
    CHECK (h.control.activeScene() == LiveControl::kEventCapacity % kNumScenes);
    h.tick();
    CHECK (h.events.size() == static_cast<size_t> (LiveControl::kEventCapacity));
    CHECK (h.master.active == h.control.activeScene());
}

TEST_CASE ("The same MIDI gives the same state at any block size", "[live]")
{
    // Messages at fixed sample times from a fixed generator; each block applies the messages of
    // its frames at its start. Reference: blocks of one frame.
    constexpr int kFrames = 24000;
    struct Timed
    {
        int time;
        MidiMessage message;
    };
    std::vector<Timed> stream;
    Random r (2026);
    for (int t = 0; t < kFrames;)
    {
        t += 1 + static_cast<int> (r.unipolar() * 400.0f);
        const float pick = r.unipolar();
        const int value = static_cast<int> (r.unipolar() * 128.0f) & 0x7f;
        if (pick < 0.3f)
            stream.push_back ({ t, cc (11, value) });
        else if (pick < 0.55f)
            stream.push_back ({ t, cc (20, value < 64 ? 0 : 127) });
        else if (pick < 0.75f)
            stream.push_back ({ t, cc (40, 127) });
        else if (pick < 0.85f)
            stream.push_back ({ t, cc (21, value) });
        else
            stream.push_back ({ t, MidiMessage::programChange (0, value % 3) });
    }

    const auto run = [&stream] (int block, int tickEvery) {
        LiveHarness h;
        twoScenes (h);
        h.map.programScene.fill (-1);
        h.map.programScene[0] = 0;
        h.map.programScene[1] = 1;
        h.map.programScene[2] = 3;
        h.bind (bindParam (ParamId::conchPosition, 11));
        h.bind (bindParam (ParamId::pearlEnabled, 20));
        h.bind (bindParam (ParamId::driftMix, 21));
        h.bind (bindAction (Action::sceneNext, 40));
        std::vector<ParamSnapshot> states; // per frame: the state its block played
        std::size_t next = 0;
        int sinceTick = 0;
        for (int s = 0; s < kFrames; s += block)
        {
            const int n = std::min (block, kFrames - s);
            std::vector<MidiMessage> messages;
            while (next < stream.size() && stream[next].time < s + n)
                messages.push_back (stream[next++].message);
            const auto snap = h.block (messages, n);
            for (int i = 0; i < n; ++i)
                states.push_back (snap);
            if ((sinceTick += n) >= tickEvery)
            {
                h.tick();
                sinceTick = 0;
            }
        }
        h.tick();
        return std::pair { states, h.master };
    };

    const auto [reference, referenceBank] = run (1, 1);
    const int block = GENERATE (7, 64, 333, 4096);
    const int tickEvery = GENERATE (64, 960);
    CAPTURE (block, tickEvery);
    const auto [states, bank] = run (block, tickEvery);
    // Each block plays the state after the messages of all its frames.
    for (int s = 0; s < kFrames; s += block)
    {
        const int last = std::min (s + block, kFrames) - 1;
        CAPTURE (s);
        CHECK (states[static_cast<size_t> (s)].values ==
               reference[static_cast<size_t> (last)].values);
    }
    CHECK (bank.active == referenceBank.active);
    CHECK (bank.values == referenceBank.values);
}
