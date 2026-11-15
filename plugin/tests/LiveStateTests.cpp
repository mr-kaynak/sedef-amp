// SPDX-License-Identifier: AGPL-3.0-or-later
// Playing live in the plug-in (docs/live.md): MIDI through processBlock, learn, the MIDI setup in
// midi.json, scenes in presets, sessions, snapshots and A/B, program changes.

#include "LiveState.h"
#include "ParameterLayout.h"
#include "PluginProcessor.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <string>

using namespace sedef;
using sedef::plugin::SedefProcessor;

namespace
{
struct Fixture
{
    Fixture()
    {
        root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("sedef-live-tests")
                   .getNonexistentChildFile ("run", "");
        root.createDirectory();
        plugin::PresetManager::testRoot() = root;
    }
    ~Fixture()
    {
        plugin::PresetManager::testRoot() = juce::File();
        root.deleteRecursively();
    }

    juce::ScopedJuceInitialiser_GUI juce;
    juce::File root;
};

constexpr int kBlock = 256;
constexpr double kRate = 48000.0;

void prepare (SedefProcessor& p)
{
    p.setPlayConfigDetails (1, 2, kRate, kBlock);
    p.prepareToPlay (kRate, kBlock);
}

/// One block of silence carrying `messages` at its first sample, then optionally the message
/// thread's tick.
void play (SedefProcessor& p, std::initializer_list<juce::MidiMessage> messages = {},
           bool tick = true)
{
    juce::AudioBuffer<float> buffer (2, kBlock);
    buffer.clear();
    juce::MidiBuffer midi;
    for (const auto& m : messages)
        midi.addEvent (m, 0);
    p.processBlock (buffer, midi);
    if (tick)
        p.live().tick();
}

/// Exact float comparison (the values here are set, not computed).
bool is (float a, float b)
{
    return juce::exactlyEqual (a, b);
}

juce::MidiMessage cc (int controller, int value, int channel = 1)
{
    return juce::MidiMessage::controllerEvent (channel, controller, value);
}

/// The scenes as the live state holds them, every value listed.
live::SceneBank bankOf (SedefProcessor& p)
{
    return plugin::scenesFromVar (p.live().reference (false),
                                  [&p] (ParamId id) { return p.paramValue (id); });
}

float sceneValue (const live::SceneBank& bank, int scene, ParamId id)
{
    const int at = bank.find (id);
    REQUIRE (at >= 0);
    return bank.values[static_cast<size_t> (scene)][static_cast<size_t> (at)];
}

/// Learns `target` on a controller: press (and release, for a momentary switch).
void learn (SedefProcessor& p, const char* target, int controller, bool momentary)
{
    REQUIRE (p.live().learn (target));
    play (p, { cc (controller, 127) });
    if (momentary)
        play (p, { cc (controller, 0) });
    else
        for (int i = 0; i < static_cast<int> (live::kLearnWindowSeconds * kRate / kBlock) + 2; ++i)
            play (p, {}, i % 20 == 0);
    p.live().tick();
    p.live().cancelLearn(); // the panel dismisses the result
}

juce::MemoryBlock stateOf (SedefProcessor& p)
{
    juce::MemoryBlock m;
    p.getStateInformation (m);
    return m;
}

void restore (SedefProcessor& p, const juce::MemoryBlock& m)
{
    p.setStateInformation (m.getData(), static_cast<int> (m.getSize()));
}
} // namespace

TEST_CASE ("Learn binds a controller; a release tells a momentary switch from a latching one")
{
    Fixture f;
    SedefProcessor p;
    prepare (p);

    learn (p, "param:pearlEnabled", 20, true);
    learn (p, "param:coralEnabled", 21, false);
    learn (p, "param:conchPosition", 11, false);
    const auto& b = p.live().settings().bindings;
    REQUIRE (b.size() == 3);
    CHECK (b[0].controller == 20);
    CHECK (b[0].mode == live::SwitchMode::toggle);
    CHECK (b[1].mode == live::SwitchMode::latch);
    CHECK (b[2].target.param == ParamId::conchPosition);

    // The learning press itself switched nothing.
    CHECK (is (p.paramValue (ParamId::pearlEnabled), 0.0f));

    // Now they play: Pearl toggles on presses, Coral follows its switch, Conch follows the pedal.
    play (p, { cc (20, 127) });
    play (p, { cc (20, 0) });
    CHECK (is (p.paramValue (ParamId::pearlEnabled), 1.0f));
    play (p, { cc (21, 127) });
    CHECK (is (p.paramValue (ParamId::coralEnabled), 1.0f));
    play (p, { cc (21, 0) });
    CHECK (is (p.paramValue (ParamId::coralEnabled), 0.0f));
    play (p, { cc (11, 127) });
    CHECK (is (p.paramValue (ParamId::conchPosition), 1.0f));

    // Learning a target again moves it; learn ignores bank select and channel mode messages.
    REQUIRE (p.live().learn ("param:conchPosition"));
    play (p, { cc (0, 5), cc (121, 0), cc (7, 64, 3) });
    p.live().cancelLearn();
    REQUIRE (b.size() == 3);
    CHECK (b[2].controller == 7);
    CHECK (b[2].channel == 2);
}

TEST_CASE ("The MIDI setup is kept in midi.json and shared by every instance")
{
    Fixture f;
    SedefProcessor a;
    prepare (a);
    learn (a, "param:pearlEnabled", 20, true);
    learn (a, "scene:3", 30, true);
    a.live().updateBinding (0, juce::JSON::parse (R"({ "mode": "latch" })"));
    a.live().setProgram (4, juce::JSON::parse (R"({ "preset": "factory:mariana" })"));
    a.live().setProgram (9, juce::JSON::parse (R"({ "scene": 1 })"));
    a.live().setProgramChannel (2);

    const auto file = plugin::LiveSettings::file();
    REQUIRE (file.existsAsFile());
    const auto json = juce::JSON::parse (file);
    CHECK (json["bindings"][0]["target"] == "param:pearlEnabled");
    CHECK (json["bindings"][0]["channel"] == juce::var (1));
    CHECK (json["bindings"][0]["mode"] == "latch");
    CHECK (json["bindings"][1]["target"] == "scene:3");
    CHECK (json["programs"]["channel"] == juce::var (3));

    // A second instance (another track, the standalone app) reads the same setup.
    SedefProcessor b;
    CHECK (b.live().settings() == a.live().settings());
    CHECK (plugin::LiveSettings::fromVar (json) == a.live().settings());

    // Edits in one reach the other on its next once-a-second check.
    a.live().clearBindings();
    for (int i = 0; i < plugin::LiveState::kTickHz; ++i)
        b.live().tick();
    CHECK (b.live().settings().bindings.empty());
    CHECK (b.live().settings().programs.size() == 2);

    // A broken file is not read as "no bindings".
    REQUIRE (file.replaceWithText ("{ not json"));
    plugin::LiveSettings broken;
    broken.programChannel = 7;
    CHECK_FALSE (broken.load());
    CHECK (broken.programChannel == 7);
}

TEST_CASE ("midi.json from elsewhere is read safely")
{
    const auto s = plugin::LiveSettings::fromVar (juce::JSON::parse (R"({
        "bindings": [
            { "target": "param:pearlEnabled", "channel": 99, "controller": 20, "mode": "latch" },
            { "target": "param:noSuchParameter", "channel": 1, "controller": 21 },
            { "target": "param:conchPosition", "channel": 1, "controller": 0 },
            { "target": "param:conchPosition", "channel": 1, "controller": 11, "low": -3, "high": 0.4 },
            { "target": "param:conchPosition", "channel": 1, "controller": 12 },
            { "target": "scene:9", "controller": 30 },
            { "target": "param:presetNext", "controller": 31 }
        ],
        "programs": { "channel": 40, "assigned": [ { "program": 0, "preset": "x" },
                                                   { "program": 3, "scene": 12 },
                                                   { "program": 5, "scene": 2 } ] } })"));
    REQUIRE (s.bindings.size() == 3);
    CHECK (s.bindings[0].channel == 15);
    CHECK (s.bindings[0].mode == live::SwitchMode::latch);
    CHECK (s.bindings[1].controller == 11); // bank select (CC 0) is not a control
    CHECK (is (s.bindings[1].low, 0.0f));
    CHECK (is (s.bindings[1].high, 0.4f));
    CHECK (s.bindings[2].target.action == live::Action::presetNext);
    CHECK (s.programChannel == 15);
    REQUIRE (s.programs.size() == 2);
    CHECK (s.programs.at (2).scene == 7);
    CHECK (s.programs.at (4).scene == 1);
}

TEST_CASE ("midi.json numbers that are not numbers are absent")
{
    // docs/robustness.md, section 2: 1e999 parses as infinity, and converting it to int was
    // undefined behaviour; null, text and booleans are not numbers.
    const auto s = plugin::LiveSettings::fromVar (juce::JSON::parse (R"({
        "bindings": [
            { "target": "param:pearlEnabled", "channel": 1e999, "controller": 20 },
            { "target": "param:coralEnabled", "channel": 2, "controller": -1e999 },
            { "target": "param:driftEnabled", "channel": null, "controller": 118.6 },
            { "target": "param:conchPosition", "channel": "3", "controller": 11,
              "low": null, "high": "0.5" },
            { "target": "param:anemoneSensitivity", "channel": true, "controller": 12,
              "low": -1e999, "high": 1e999 }
        ],
        "programs": { "channel": -1e999, "assigned": [ { "program": 1e999, "preset": "x" },
                                                       { "program": 3, "scene": true },
                                                       { "program": 5, "scene": null },
                                                       { "program": 7.6, "scene": 2.4 } ] } })"));
    REQUIRE (s.bindings.size() == 4);
    CHECK (s.bindings[0].target.param == ParamId::pearlEnabled);
    CHECK (s.bindings[0].channel == 0); // the default channel 1
    CHECK (s.bindings[1].target.param == ParamId::driftEnabled);
    CHECK (s.bindings[1].controller == 119);
    CHECK (s.bindings[1].channel == 0);
    CHECK (s.bindings[2].channel == 0); // text is not a channel
    CHECK (is (s.bindings[2].low, 0.0f));
    CHECK (is (s.bindings[2].high, 1.0f));
    CHECK (s.bindings[3].channel == 0);
    CHECK (is (s.bindings[3].low, 0.0f));
    CHECK (is (s.bindings[3].high, 1.0f));
    CHECK (s.programChannel == -1);
    REQUIRE (s.programs.size() == 1);
    CHECK (s.programs.at (7).scene == 1); // 7.6 -> program 8, 2.4 -> scene 2
}

TEST_CASE ("Forgetting or removing another binding while a learn listens leaves both right")
{
    // docs/robustness.md, section 6 / bug scan: the learn kept its binding's index, so a binding
    // erased before it moved the learn's result onto another binding, or past the end.
    Fixture f;
    SedefProcessor p;
    prepare (p);
    learn (p, "param:coralEnabled", 21, true); // 0
    learn (p, "param:pearlEnabled", 20, true); // 1
    learn (p, "param:driftEnabled", 22, true); // 2
    const auto& b = p.live().settings().bindings;
    REQUIRE (b.size() == 3);

    const auto window = static_cast<int> (live::kLearnWindowSeconds * kRate / kBlock) + 2;
    const auto relearnPearlForgetting = [&] (const std::function<void()>& erase) {
        REQUIRE (p.live().learn ("param:pearlEnabled"));
        play (p, { cc (20, 127) }); // the first press: the classifier listens for 1.5 s
        CHECK (p.live().stateAsVar()["learn"]["binding"] ==
               juce::var (
                   p.live().settings().find (*plugin::LiveTarget::parse ("param:pearlEnabled"))));
        erase();
        for (int i = 0; i < window; ++i) // the window runs out: a latching switch
            play (p, {}, i % 20 == 0);
        p.live().tick();
        CHECK (p.live().stateAsVar()["learn"]["phase"] == "done");
        p.live().cancelLearn();
    };

    SECTION ("forget, from a control's menu")
    {
        relearnPearlForgetting ([&] { p.live().forget ("param:coralEnabled"); });
        REQUIRE (b.size() == 2);
        CHECK (b[0].target.param == ParamId::pearlEnabled);
        CHECK (b[0].mode == live::SwitchMode::latch); // what was learnt
        CHECK (b[1].target.param == ParamId::driftEnabled);
        CHECK (b[1].mode == live::SwitchMode::toggle); // untouched
    }
    SECTION ("remove, from the bindings list")
    {
        relearnPearlForgetting ([&] { p.live().removeBinding (2); });
        REQUIRE (b.size() == 2);
        CHECK (b[0].target.param == ParamId::coralEnabled);
        CHECK (b[0].mode == live::SwitchMode::toggle);
        CHECK (b[1].target.param == ParamId::pearlEnabled);
        CHECK (b[1].mode == live::SwitchMode::latch);
    }
    SECTION ("forgetting the binding being learnt ends the learn and leaves it forgotten")
    {
        REQUIRE (p.live().learn ("param:pearlEnabled"));
        play (p, { cc (20, 127) });
        p.live().forget ("param:pearlEnabled");
        CHECK (p.live().stateAsVar()["learn"].isVoid());
        for (int i = 0; i < window; ++i)
            play (p, {}, i % 20 == 0);
        REQUIRE (b.size() == 2);
        CHECK (p.live().settings().find (*plugin::LiveTarget::parse ("param:pearlEnabled")) < 0);
        CHECK (b[0].mode == live::SwitchMode::toggle);
        CHECK (b[1].mode == live::SwitchMode::toggle);
    }
}

TEST_CASE ("Two instances editing the MIDI setup within the one-second check keep both edits")
{
    // docs/robustness.md, section 3: each instance wrote its whole copy, so the later write
    // dropped what the other had just learnt.
    Fixture f;
    SedefProcessor a, b;
    prepare (a);
    prepare (b);
    learn (a, "param:pearlEnabled", 20, true);
    learn (b, "param:coralEnabled", 21, true); // b has not read a's edit yet
    auto file = plugin::LiveSettings::fromVar (juce::JSON::parse (plugin::LiveSettings::file()));
    REQUIRE (file.bindings.size() == 2);
    CHECK (file.find (*plugin::LiveTarget::parse ("param:pearlEnabled")) >= 0);
    CHECK (file.find (*plugin::LiveTarget::parse ("param:coralEnabled")) >= 0);

    // a's panel still shows only Pearl, at index 0: its edit reaches Pearl, not Coral.
    REQUIRE (a.live().settings().bindings.size() == 1);
    a.live().updateBinding (0, juce::JSON::parse (R"({ "mode": "latch" })"));
    b.live().setProgram (3, juce::JSON::parse (R"({ "scene": 2 })"));
    a.live().removeBinding (0); // Pearl, in a's view; Coral stays
    file = plugin::LiveSettings::fromVar (juce::JSON::parse (plugin::LiveSettings::file()));
    REQUIRE (file.bindings.size() == 1);
    CHECK (file.bindings[0].target.param == ParamId::coralEnabled);
    CHECK (file.bindings[0].mode == live::SwitchMode::toggle);
    CHECK (file.programs.at (3).scene == 2);
    CHECK (a.live().settings() == file);
}

TEST_CASE ("A footswitch switches a scene in one block, and the host's parameters follow")
{
    Fixture f;
    SedefProcessor p;
    prepare (p);
    auto& live = p.live();

    // A preset whose scene 2 has Pearl and Drift on, Drift Mix (picked) at 0.6.
    REQUIRE (live.rememberInScenes (ParamId::driftMix, true));
    CHECK_FALSE (live.rememberInScenes (ParamId::anemoneMode, true)); // a choice jumps
    CHECK_FALSE (live.rememberInScenes (ParamId::outputGain, true));  // global
    live.selectScene (1);
    p.setParamValue (ParamId::pearlEnabled, 1.0f);
    p.setParamValue (ParamId::driftEnabled, 1.0f);
    p.setParamValue (ParamId::driftMix, 0.6f);
    live.selectScene (0);
    CHECK (is (p.paramValue (ParamId::pearlEnabled), 0.0f));
    CHECK (is (p.paramValue (ParamId::scene), 0.0f));
    CHECK (p.presets().isDirty());
    REQUIRE (p.presets().save ("Two scenes", "").isNotEmpty());
    learn (p, "scene:2", 30, true);

    // The press: the audio thread plays scene 2 from this block on.
    play (p, { cc (30, 127) }, false);
    CHECK (live.control().activeScene() == 1);
    CHECK (live.control().overridden (ParamId::pearlEnabled));
    // The host's parameters catch up on the message thread's tick.
    live.tick();
    CHECK (is (p.paramValue (ParamId::pearlEnabled), 1.0f));
    CHECK (is (p.paramValue (ParamId::driftEnabled), 1.0f));
    CHECK (is (p.paramValue (ParamId::driftMix), 0.6f));
    CHECK (is (p.paramValue (ParamId::scene), 1.0f));
    CHECK (live.activeScene() == 1);
    play (p);
    CHECK_FALSE (live.control().overridden (ParamId::pearlEnabled));

    // Switching scenes is not an edit.
    CHECK_FALSE (p.presets().isDirty());
}

TEST_CASE ("Program changes load presets in library order, by assignment, or select scenes")
{
    Fixture f;
    SedefProcessor p;
    prepare (p);
    auto& presets = p.presets();

    play (p, { juce::MidiMessage::programChange (1, 2) });
    CHECK (presets.currentId() == presets.idAt (2));
    p.live().setProgram (2, juce::JSON::parse (R"({ "preset": "factory:mariana" })"));
    play (p, { juce::MidiMessage::programChange (1, 2) });
    CHECK (presets.currentId() == "factory:mariana");

    // Past the end of the list: nothing.
    play (p, { juce::MidiMessage::programChange (1, 127) });
    CHECK (presets.currentId() == "factory:mariana");

    p.live().setProgram (5, juce::JSON::parse (R"({ "scene": 3 })"));
    play (p, { juce::MidiMessage::programChange (1, 5) });
    CHECK (p.live().activeScene() == 3);
    CHECK (is (p.paramValue (ParamId::scene), 3.0f));
    CHECK (presets.currentId() == "factory:mariana");

    // Only the program channel, once one is set.
    p.live().setProgramChannel (4);
    play (p, { juce::MidiMessage::programChange (1, 0) });
    CHECK (presets.currentId() == "factory:mariana");
    play (p, { juce::MidiMessage::programChange (5, 0) });
    CHECK (presets.currentId() == presets.idAt (0));

    // Next and previous preset from a footswitch.
    learn (p, "preset:next", 50, true);
    const auto before = presets.currentId();
    play (p, { cc (50, 127) });
    CHECK (presets.currentId() == presets.idAt (1));
    CHECK (presets.currentId() != before);
}

TEST_CASE ("Presets carry their scenes; files without them load as before")
{
    Fixture f;
    SedefProcessor p;
    auto& presets = p.presets();
    auto& live = p.live();

    // A factory preset: only scene 1 is made; going to another makes it, as a copy, which is
    // not a change.
    REQUIRE (presets.load ("factory:mariana", true));
    const auto loaded = bankOf (p);
    CHECK (loaded.made == 1);
    CHECK (loaded.active == 0);
    const auto pearl = p.paramValue (ParamId::pearlEnabled);
    live.selectScene (4);
    CHECK (bankOf (p).made == (1 | 1 << 4));
    CHECK (is (p.paramValue (ParamId::pearlEnabled), pearl));
    CHECK_FALSE (presets.isDirty());

    // A change in a scene is an edit; going back to it brings it back.
    p.setParamValue (ParamId::pearlEnabled, 1.0f);
    CHECK (presets.isDirty());
    REQUIRE (live.rememberInScenes (ParamId::driftMix, true));
    p.setParamValue (ParamId::driftMix, 0.45f);
    live.selectScene (0);
    CHECK (is (p.paramValue (ParamId::pearlEnabled), 0.0f));
    CHECK (presets.isDirty());
    live.selectScene (4);
    CHECK (is (p.paramValue (ParamId::pearlEnabled), 1.0f));
    CHECK (is (p.paramValue (ParamId::driftMix), 0.45f));

    // Saved in scene 5: "parameters" are scene 5's sound, the others are what differs from it.
    const auto id = presets.save ("Two faces", "");
    REQUIRE (id.isNotEmpty());
    CHECK_FALSE (presets.isDirty());
    const auto json = juce::JSON::parse (
        plugin::PresetManager::userFolder().getChildFile ("Two faces.sedefpreset"));
    CHECK (is (static_cast<float> (json["parameters"]["pearlEnabled"]), 1.0f));
    CHECK_FALSE (json["parameters"].hasProperty ("scene")); // carried by the scenes object
    const auto& scenes = json["scenes"];
    CHECK (static_cast<int> (scenes["active"]) == 4);
    CHECK (scenes["values"][0] == "driftMix");
    REQUIRE (scenes["list"].size() == 5); // scenes 6-8 were never made
    CHECK (scenes["list"][4].getDynamicObject()->getProperties().isEmpty());
    CHECK (scenes["list"][1].isVoid());
    CHECK (is (static_cast<float> (scenes["list"][0]["pearlEnabled"]), 0.0f));
    CHECK_FALSE (scenes["list"][0].hasProperty ("gateEnabled")); // the same in both scenes

    // Back again: the scenes, the scene in use and nothing dirty.
    REQUIRE (presets.load ("factory:riptide", true));
    CHECK (live.activeScene() == 0);
    REQUIRE (presets.load (id, true));
    CHECK (live.activeScene() == 4);
    CHECK (is (p.paramValue (ParamId::scene), 4.0f));
    CHECK_FALSE (presets.isDirty());
    live.selectScene (0);
    CHECK (is (p.paramValue (ParamId::pearlEnabled), 0.0f));
    CHECK (is (p.paramValue (ParamId::driftMix), sceneValue (bankOf (p), 0, ParamId::driftMix)));
    CHECK_FALSE (presets.isDirty());

    // A preset whose scenes were never used is written as before.
    REQUIRE (presets.load ("factory:mariana", true));
    REQUIRE (presets.save ("Plain", "").isNotEmpty());
    CHECK_FALSE (
        juce::JSON::parse (plugin::PresetManager::userFolder().getChildFile ("Plain.sedefpreset"))
            .hasProperty ("scenes"));
}

TEST_CASE ("A preset with scenes plays its saved scene in a build without scenes")
{
    // 0.9 reads only "parameters": they must be the sound of the scene the file names, and this
    // build must play the same from them, whatever a scene's entry holds.
    Fixture f;
    SedefProcessor p;
    const auto file = plugin::PresetManager::userFolder().getChildFile ("Hand made.sedefpreset");
    REQUIRE (file.getParentDirectory().createDirectory());
    REQUIRE (file.replaceWithText (R"({ "format": "sedef-preset", "version": 1, "name": "Hand made",
        "parameters": { "pearlEnabled": 1, "driftMix": 0.3 },
        "scenes": { "active": 1, "values": ["driftMix", "noSuchThing", "ampModel"],
                    "list": [ { "pearlEnabled": 0, "driftMix": 7, "fromTheFuture": 1 },
                              { "pearlEnabled": 0 } ] } })"));
    p.presets().scan();
    REQUIRE (p.presets().load ("user:Hand made.sedefpreset", true));
    CHECK (p.live().activeScene() == 1);
    // The active scene is "parameters", even where its entry says otherwise.
    CHECK (is (p.paramValue (ParamId::pearlEnabled), 1.0f));
    CHECK (is (p.paramValue (ParamId::driftMix), 0.3f));
    CHECK_FALSE (p.presets().isDirty());
    const auto bank = bankOf (p);
    CHECK (bank.numValues() == 1); // driftMix only: unknown ids and a choice are dropped
    CHECK (is (sceneValue (bank, 0, ParamId::pearlEnabled), 0.0f));
    CHECK (is (sceneValue (bank, 0, ParamId::driftMix), spec (ParamId::driftMix).max)); // clamped
    CHECK (bank.made == 0b11);
    CHECK (is (sceneValue (bank, 7, ParamId::pearlEnabled), 1.0f)); // not made: the preset's
}

TEST_CASE ("Transpose stays out of scenes: a scene switch never changes the latency")
{
    // Transpose adds latency while it shifts (docs/dsp/transpose.md), and the host is told on the
    // next timer tick: a scene that recalled it would move the latency under the player's foot.
    // It is global, so scenes never hold it, however a preset file asks (docs/live.md §6.2).
    Fixture f;
    SedefProcessor p;
    prepare (p);
    auto& live = p.live();
    CHECK_FALSE (live.rememberInScenes (ParamId::transpose, true));
    CHECK (bankOf (p).find (ParamId::transpose) < 0);

    const auto file = plugin::PresetManager::userFolder().getChildFile ("Shifted.sedefpreset");
    REQUIRE (file.getParentDirectory().createDirectory());
    REQUIRE (file.replaceWithText (R"({ "format": "sedef-preset", "version": 1, "name": "Shifted",
        "parameters": { "pearlEnabled": 1 },
        "scenes": { "active": 0, "values": ["transpose", "driftMix"],
                    "list": [ {}, { "pearlEnabled": 0, "transpose": 5 } ] } })"));
    p.presets().scan();
    REQUIRE (p.presets().load ("user:Shifted.sedefpreset", true));
    const auto bank = bankOf (p);
    CHECK (bank.numValues() == 1); // driftMix: Transpose is dropped
    CHECK (bank.find (ParamId::transpose) < 0);

    // The player transposes down; footswitches and the scene bar switch scenes, and Transpose
    // and the latency the engine reports stay where they are.
    p.setParamValue (ParamId::transpose, -2.0f);
    play (p);
    const auto latency = p.engine().latencySamples();
    learn (p, "scene:2", 30, true);
    for (const auto to : { 1, 0, 1 })
    {
        if (to == 1)
            play (p, { cc (30, 127) });
        else
            live.selectScene (to);
        play (p);
        CHECK (live.activeScene() == to);
        CHECK (is (p.paramValue (ParamId::transpose), -2.0f));
        CHECK (p.engine().latencySamples() == latency);
    }
    CHECK (is (p.paramValue (ParamId::pearlEnabled), 0.0f)); // scene 2 did play
}

TEST_CASE ("Sessions keep the scenes byte for byte; older sessions have one scene")
{
    Fixture f;
    SedefProcessor a;
    REQUIRE (a.live().rememberInScenes (ParamId::gyreSpeed, true)); // a skewed range
    a.live().selectScene (2);
    a.setParamValue (ParamId::kelpEnabled, 1.0f);
    a.setParamValue (ParamId::gyreSpeed, 3.3f);
    a.live().selectScene (6);
    const auto saved = stateOf (a);

    SedefProcessor b;
    restore (b, saved);
    CHECK (b.live().activeScene() == 6);
    CHECK (is (b.paramValue (ParamId::scene), 6.0f));
    CHECK (is (sceneValue (bankOf (b), 2, ParamId::kelpEnabled), 1.0f));
    // What scene 3 kept is the parameter's value after its skewed round trip through the host's
    // 0-1 range, which differs from 3.3f in the last bits with the platform's pow(): compare with
    // what the first instance kept, exactly.
    CHECK (is (sceneValue (bankOf (b), 2, ParamId::gyreSpeed),
               sceneValue (bankOf (a), 2, ParamId::gyreSpeed)));
    CHECK (sceneValue (bankOf (b), 2, ParamId::gyreSpeed) == Catch::Approx (3.3f).margin (1e-4));
    CHECK (stateOf (b) == saved);
    b.live().selectScene (2);
    CHECK (is (b.paramValue (ParamId::kelpEnabled), 1.0f));

    // A session saved before scenes: no attribute, 8 copies of its sound.
    SedefProcessor plain;
    plain.setParamValue (ParamId::tideEnabled, 1.0f);
    const auto old = stateOf (plain);
    auto xml =
        juce::AudioProcessor::getXmlFromBinary (old.getData(), static_cast<int> (old.getSize()));
    REQUIRE (xml != nullptr);
    CHECK_FALSE (xml->getChildByName ("Extra")->hasAttribute ("scenes"));
    restore (b, old);
    CHECK (b.live().activeScene() == 0);
    const auto bank = bankOf (b);
    CHECK (bank.made == 1);
    CHECK (bank.numValues() == 0);
    CHECK (stateOf (b) == old);
}

TEST_CASE ("A Scene parameter saved before the switch was heard restores as saved")
{
    // A validator sets parameters, runs a block and saves, with no event loop: the audio thread
    // has switched, the message thread has not heard of it, and the session names scene 1 while
    // the parameter says 3. Reloaded, the parameter must read 3 and the state save the same bytes.
    Fixture f;
    SedefProcessor a;
    prepare (a);
    a.setParamValue (ParamId::scene, 2.0f);
    play (a, {}, false);
    const auto saved = stateOf (a);

    SedefProcessor b;
    prepare (b);
    restore (b, saved);
    CHECK (is (b.paramValue (ParamId::scene), 2.0f));
    CHECK (b.live().activeScene() == 2);
    CHECK (bankOf (b).made == 0b100); // the session had one scene; it still has one
    CHECK (stateOf (b) == saved);
    play (b);
    CHECK (b.live().activeScene() == 2);
    CHECK (is (b.paramValue (ParamId::scene), 2.0f));
    CHECK (stateOf (b) == saved);
}

TEST_CASE ("Snapshots, Undo and A/B bring the scenes back")
{
    Fixture f;
    SedefProcessor p;
    auto& presets = p.presets();
    auto& live = p.live();
    using Slot = plugin::PresetManager::Slot;

    live.selectScene (3);
    p.setParamValue (ParamId::coralEnabled, 1.0f);
    presets.snapshot (Slot::browse);
    REQUIRE (presets.load ("factory:mariana", true));
    CHECK (live.activeScene() == 0);
    REQUIRE (presets.restore (Slot::browse));
    CHECK (live.activeScene() == 3);
    CHECK (is (p.paramValue (ParamId::scene), 3.0f));
    CHECK (is (p.paramValue (ParamId::coralEnabled), 1.0f));
    live.selectScene (0);
    CHECK (is (p.paramValue (ParamId::coralEnabled), 0.0f));
    live.selectScene (3);

    presets.compareSwitch(); // to B, a copy of A
    live.selectScene (5);
    presets.compareSwitch(); // to A
    CHECK (live.activeScene() == 3);
    presets.compareSwitch(); // to B
    CHECK (live.activeScene() == 5);
}

TEST_CASE ("No Sedef parameter id falls among JUCE's VST3 controller parameters")
{
    // With MIDI input, JUCE's VST3 wrapper adds 16 x 130 controller parameters from a fixed id;
    // Sedef's ids are hashes of its string ids (JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS clears
    // the top bit), and none may land there (docs/live.md §2).
    constexpr std::uint32_t kFirst = 0x6d636d00; // JuceAudioProcessor::paramMidiControllerOffset
    constexpr std::uint32_t kCount = 16 * 130;
    for (const auto& s : kParamSpecs)
    {
        const auto hash =
            static_cast<std::uint32_t> (plugin::toJuce (s.id).hashCode()) & 0x7fffffffu;
        CAPTURE (s.id);
        CHECK ((hash < kFirst || hash >= kFirst + kCount));
        CHECK (hash != 0x62797073u); // the wrapper's own bypass
        CHECK (hash != 0x70727374u); // and program parameters
    }
}

// The looper's footswitches (docs/live.md §4.5) -----------------------------------------------

namespace
{
/// A processor made as the standalone app makes it (the looper is the app's).
struct StandaloneProcessor
{
    StandaloneProcessor()
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Standalone);
        p = std::make_unique<SedefProcessor>();
        juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Undefined);
    }
    std::unique_ptr<SedefProcessor> p;
};

/// The looper's state by the name the editor sends (in dsp::Looper::State's order).
std::string looperState (SedefProcessor& p)
{
    static constexpr const char* kStates[] = { "unavailable", "empty",   "armed",       "recording",
                                               "closing",     "playing", "overdubbing", "stopped" };
    return kStates[std::clamp (p.engine().telemetry().looperState.load(), 0, 7)];
}

/// Blocks of silence for `seconds`.
void wait (SedefProcessor& p, double seconds)
{
    for (int i = 0; i < static_cast<int> (seconds * kRate / kBlock); ++i)
        play (p);
}
} // namespace

TEST_CASE ("midi.json keeps the looper's targets; only the main one takes a double tap")
{
    const auto s = plugin::LiveSettings::fromVar (juce::JSON::parse (R"({
        "bindings": [
            { "target": "looper:main", "controller": 60, "mode": "toggle", "doubleTap": true },
            { "target": "looper:stop", "controller": 61, "mode": "latch", "doubleTap": true },
            { "target": "looper:undo", "controller": 62 },
            { "target": "looper:clear", "controller": 63 },
            { "target": "looper:pause", "controller": 64 }
        ] })"));
    REQUIRE (s.bindings.size() == 4);
    CHECK (s.bindings[0].target.action == live::Action::looperMain);
    CHECK (s.bindings[0].doubleTap);
    CHECK (s.bindings[1].target.action == live::Action::looperStop);
    CHECK (s.bindings[1].mode == live::SwitchMode::latch);
    CHECK_FALSE (s.bindings[1].doubleTap);
    CHECK (s.bindings[2].target.action == live::Action::looperUndo);
    CHECK (s.bindings[3].target.action == live::Action::looperClear);
    for (const auto& b : s.bindings)
        CHECK (b.target.isSwitch());

    const auto out = s.toVar();
    CHECK (out["bindings"][0]["target"] == "looper:main");
    CHECK (out["bindings"][0]["doubleTap"] == juce::var (true));
    CHECK_FALSE (out["bindings"][1].hasProperty ("doubleTap"));
    CHECK (plugin::LiveSettings::fromVar (out) == s);

    const auto map = s.toMap();
    REQUIRE (map.numBindings == 4);
    CHECK (map.bindings[0].doubleTap);
    CHECK_FALSE (map.bindings[1].doubleTap);
}

TEST_CASE ("In the standalone app a footswitch plays the looper, and a double tap stops it")
{
    Fixture f;
    StandaloneProcessor app;
    auto& p = *app.p;
    prepare (p);
    REQUIRE_FALSE (p.engine().looperReady()); // nothing asked for the looper yet

    learn (p, "looper:main", 60, true);
    // The binding reserves the memory, so the first press records at once.
    CHECK (p.engine().looperReady());
    learn (p, "looper:undo", 62, true);
    p.live().updateBinding (0, juce::JSON::parse (R"({ "doubleTap": true })"));
    const auto json = juce::JSON::parse (plugin::LiveSettings::file());
    CHECK (json["bindings"][0]["target"] == "looper:main");
    CHECK (json["bindings"][0]["doubleTap"] == juce::var (true));
    CHECK (p.live().stateAsVar()["bindings"][0]["doubleTap"] == juce::var (true));

    play (p, { cc (60, 127) });
    CHECK (looperState (p) == "recording");
    play (p, { cc (60, 0) }); // the release does nothing
    wait (p, 1.0);
    play (p, { cc (60, 127) }); // closes the loop: it plays
    play (p, { cc (60, 0) });
    CHECK (looperState (p) == "playing");
    CHECK (p.engine().telemetry().looperLength.load() > 0);

    wait (p, 0.6);
    play (p, { cc (60, 127) }); // overdub
    play (p, { cc (60, 0) });
    CHECK (looperState (p) == "overdubbing");
    wait (p, 0.2);
    play (p, { cc (60, 127) }); // the second tap, 0.2 s later: stop, and the layer is undone
    wait (p, 0.05);
    CHECK (looperState (p) == "stopped");
    const int flags = p.engine().telemetry().looperFlags.load();
    CHECK ((flags & Engine::kLooperCanRedo) != 0);
    CHECK ((flags & Engine::kLooperCanUndo) == 0);

    // The undo switch puts the layer back (one switch for undo and redo).
    play (p, { cc (62, 127) });
    wait (p, 0.05);
    CHECK ((p.engine().telemetry().looperFlags.load() & Engine::kLooperCanUndo) != 0);
}

TEST_CASE ("A plug-in learns no looper targets, and a looper binding from the app does nothing")
{
    Fixture f;
    {
        StandaloneProcessor app;
        prepare (*app.p);
        learn (*app.p, "looper:main", 60, true);
    }
    SedefProcessor plugin; // reads the same midi.json
    prepare (plugin);
    CHECK_FALSE (plugin.live().learn ("looper:stop"));
    REQUIRE (plugin.live().settings().bindings.size() == 1);
    CHECK (plugin.live().settings().bindings[0].target.isLooper());
    play (plugin, { cc (60, 127) });
    CHECK_FALSE (plugin.engine().looperReady());
    CHECK (looperState (plugin) == "unavailable");
}

TEST_CASE ("midi.json: switches keep no range, and only a boolean true turns a double tap on")
{
    plugin::LiveSettings s;
    plugin::LiveTarget pearl;
    pearl.param = ParamId::pearlEnabled;
    plugin::LiveTarget conch;
    conch.param = ParamId::conchPosition;
    plugin::LiveTarget main;
    main.action = live::Action::looperMain;
    s.bind (pearl, 0, 20, live::SwitchMode::toggle);
    s.bind (conch, 0, 11, live::SwitchMode::toggle);
    s.bind (main, 0, 60, live::SwitchMode::toggle);

    // As 0.10 wrote them: a switch has a mode, a continuous target a range, and a looper switch
    // with the option off says nothing about it.
    auto out = s.toVar();
    CHECK (out["bindings"][0].hasProperty ("mode"));
    CHECK_FALSE (out["bindings"][0].hasProperty ("low"));
    CHECK_FALSE (out["bindings"][0].hasProperty ("high"));
    CHECK (out["bindings"][1].hasProperty ("low"));
    CHECK_FALSE (out["bindings"][1].hasProperty ("mode"));
    CHECK_FALSE (out["bindings"][2].hasProperty ("doubleTap"));
    CHECK_FALSE (out["bindings"][2].hasProperty ("low"));

    s.bindings[2].doubleTap = true;
    out = s.toVar();
    CHECK (out["bindings"][2]["doubleTap"] == juce::var (true));
    CHECK (out["bindings"][2]["mode"] == "toggle");
    CHECK_FALSE (out["bindings"][2].hasProperty ("low"));
    CHECK (plugin::LiveSettings::fromVar (out) == s);

    // An edited file: a string, a number or a null is not a yes.
    for (const char* value : { R"("true")", "1", "null", "[]" })
    {
        CAPTURE (value);
        const auto edited = plugin::LiveSettings::fromVar (juce::JSON::parse (
            juce::String (R"({ "bindings": [ { "target": "looper:main", "controller": 60,
                              "doubleTap": )") +
            value + " } ] }"));
        REQUIRE (edited.bindings.size() == 1);
        CHECK_FALSE (edited.bindings[0].doubleTap);
    }
}

TEST_CASE ("A looper footswitch is forgotten and learnt again; the double tap is the player's")
{
    Fixture f;
    StandaloneProcessor app;
    auto& p = *app.p;
    prepare (p);
    const auto saved = [] {
        return juce::JSON::parse (plugin::LiveSettings::file())["bindings"];
    };

    learn (p, "looper:main", 60, true);
    p.live().updateBinding (0, juce::JSON::parse (R"({ "doubleTap": true })"));
    REQUIRE (p.live().settings().bindings[0].doubleTap);
    // A change that is not a boolean leaves it as it is.
    p.live().updateBinding (0, juce::JSON::parse (R"({ "doubleTap": null })"));
    p.live().updateBinding (0, juce::JSON::parse (R"({ "doubleTap": "false" })"));
    CHECK (p.live().settings().bindings[0].doubleTap);

    // Learnt again on another switch: the binding moves, the option stays.
    learn (p, "looper:main", 64, false);
    REQUIRE (p.live().settings().bindings.size() == 1);
    CHECK (p.live().settings().bindings[0].controller == 64);
    CHECK (p.live().settings().bindings[0].mode == live::SwitchMode::latch);
    CHECK (p.live().settings().bindings[0].doubleTap);
    CHECK (saved()[0]["controller"] == juce::var (64));

    // Forgotten: gone from midi.json, and the old switch does nothing.
    p.live().forget ("looper:main");
    CHECK (p.live().settings().bindings.empty());
    CHECK (saved().size() == 0);
    play (p, { cc (64, 127) });
    CHECK (looperState (p) == "empty");

    // Learnt afresh: a new binding, the option off, and the switch records.
    learn (p, "looper:main", 60, true);
    REQUIRE (p.live().settings().bindings.size() == 1);
    CHECK_FALSE (p.live().settings().bindings[0].doubleTap);
    CHECK (saved()[0]["target"] == "looper:main");
    CHECK_FALSE (saved()[0].hasProperty ("doubleTap"));
    play (p, { cc (60, 127) });
    CHECK (looperState (p) == "recording");

    // Another instance reads the same file.
    StandaloneProcessor other;
    prepare (*other.p);
    REQUIRE (other.p->live().settings().bindings.size() == 1);
    CHECK (other.p->live().settings().bindings[0].target.action == live::Action::looperMain);
}
