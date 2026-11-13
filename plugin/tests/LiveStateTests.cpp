// SPDX-License-Identifier: AGPL-3.0-or-later
// Playing live in the plug-in (docs/live.md): MIDI through processBlock, learn, the MIDI setup in
// midi.json, scenes in presets, sessions, snapshots and A/B, program changes.

#include "LiveState.h"
#include "ParameterLayout.h"
#include "PluginProcessor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
