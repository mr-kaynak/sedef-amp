// SPDX-License-Identifier: AGPL-3.0-or-later
// The pedalboard in the plug-in: session state, presets, snapshots, A/B and Undo, and old
// sessions and presets that were saved before it.

#include "BoardText.h"
#include "ParameterLayout.h"
#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using sedef::plugin::SedefProcessor;

namespace
{
Board boardOf (std::initializer_list<PedalId> ids)
{
    Board b;
    for (const auto id : ids)
        b.insert (id, b.size());
    return b;
}

/// A JUCE environment (message manager, timers) and a scratch folder for presets and settings,
/// set before the processor that uses them is made.
struct Fixture
{
    Fixture()
    {
        root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("sedef-plugin-tests")
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

/// A session as 0.3.0 saved it: everything this build saves, without the board.
juce::MemoryBlock withoutBoard (const juce::MemoryBlock& state)
{
    auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(),
                                                       static_cast<int> (state.getSize()));
    REQUIRE (xml != nullptr);
    auto* extra = xml->getChildByName ("Extra");
    REQUIRE (extra != nullptr);
    REQUIRE (extra->hasAttribute ("board"));
    extra->removeAttribute ("board");
    juce::MemoryBlock out;
    juce::AudioProcessor::copyXmlToBinary (*xml, out);
    return out;
}

void setParam (SedefProcessor& p, ParamId id, float value)
{
    p.setParamValue (id, value);
}

/// Renders a second of a guitar-like tone through the processor, mono in, stereo out.
std::vector<float> render (SedefProcessor& p)
{
    p.setPlayConfigDetails (1, 2, 48000.0, 256);
    p.prepareToPlay (48000.0, 256);
    std::vector<float> out;
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    for (int block = 0; block < 188; ++block)
    {
        buffer.clear();
        for (int i = 0; i < 256; ++i)
        {
            const double t = (block * 256 + i) / 48000.0;
            float s = 0.0f;
            for (int k = 1; k <= 6; ++k)
                s += static_cast<float> (std::sin (2.0 * 3.14159265358979 * 147.0 * k * t) / k);
            buffer.setSample (0, i, 0.15f * s);
        }
        p.processBlock (buffer, midi);
        out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 256);
    }
    p.releaseResources();
    return out;
}

const Board kCustom = boardOf ({ PedalId::ripple, PedalId::coral, PedalId::tide });
} // namespace

TEST_CASE ("A new instance opens on Init's board: the pedals the defaults switch on")
{
    Fixture f;
    SedefProcessor p;
    // Only the Gate is on by default, so the board holds it alone, not the whole fixed chain.
    CHECK (p.board() == boardOf ({ PedalId::gate }));
    CHECK (p.board() == plugin::switchedOnBoardFor ({}));
    CHECK_FALSE (p.presets().isDirty());

    // The engine plays it from the first block, and a save keeps it.
    render (p);
    CHECK (p.engine().telemetry().pedalRan[index (PedalId::gate)].load());
    SedefProcessor restored;
    restored.setBoard (kCustom);
    restore (restored, stateOf (p));
    CHECK (restored.board() == boardOf ({ PedalId::gate }));
}

TEST_CASE ("The board text keeps what this build knows, in order, once")
{
    using namespace sedef::plugin;
    CHECK (boardFromText ("ripple,coral,tide") == kCustom);
    CHECK (boardFromText (" ripple , kelp-from-the-future,coral,ripple,tide") == kCustom);
    CHECK (boardFromText ("").empty());
    CHECK (boardToText (kCustom) == "ripple,coral,tide");
    CHECK (boardToText (Board {}).isEmpty());

    juce::Array<juce::var> ids { "pearl", "nothing", "gate", "pearl" };
    CHECK (boardFromVar (ids) == boardOf ({ PedalId::pearl, PedalId::gate }));
    CHECK_FALSE (boardFromVar (juce::var()).has_value());
    CHECK_FALSE (boardFromVar ("gate").has_value());
    CHECK (boardFromVar (boardToVar (kCustom)) == kCustom);
}

TEST_CASE ("The board round-trips through the session byte for byte")
{
    Fixture f;
    SedefProcessor a;
    a.setBoard (kCustom);
    setParam (a, ParamId::coralEnabled, 1.0f);
    const auto saved = stateOf (a);

    SedefProcessor b;
    const int revision = b.boardRevision.load();
    restore (b, saved);
    CHECK (b.board() == kCustom);
    CHECK (b.boardRevision.load() != revision); // the editor hears of it
    CHECK (stateOf (b) == saved);

    // An empty board is a board too.
    a.setBoard (Board {});
    restore (b, stateOf (a));
    CHECK (b.board().empty());
}

TEST_CASE ("A session from 0.3.0 gets the legacy board and sounds the same")
{
    Fixture f;
    // A 0.3.0 session with every pedal of its day on.
    SedefProcessor source;
    for (const auto id : { ParamId::gateEnabled, ParamId::tideEnabled, ParamId::pearlEnabled,
                           ParamId::coralEnabled, ParamId::rippleEnabled })
        setParam (source, id, 1.0f);
    setParam (source, ParamId::pearlGain, 0.7f);
    setParam (source, ParamId::rippleMode, 1.0f);
    source.setBoard (kCustom); // whatever this instance had, 0.3.0 didn't save it
    const auto old = withoutBoard (stateOf (source));

    SedefProcessor restored;
    restored.setBoard (kCustom);
    restore (restored, old);
    CHECK (restored.board() == Board::legacy());
    // Saving it again adds the board, and nothing else changes.
    CHECK (withoutBoard (stateOf (restored)) == old);

    // It plays exactly what an instance on the legacy board plays.
    SedefProcessor reference;
    restore (reference, old);
    reference.setBoard (Board::legacy());
    const auto heard = render (restored);
    CHECK (heard == render (reference));
    float peak = 0.0f;
    for (const float s : heard)
        peak = std::max (peak, std::abs (s));
    CHECK (peak > 0.01f);
}

TEST_CASE ("A session without any parameters still gets a board")
{
    Fixture f;
    SedefProcessor p;
    p.setBoard (kCustom);
    juce::XmlElement root ("SedefState");
    root.setAttribute ("version", 1);
    root.createNewChildElement ("Extra")->setAttribute ("uiScale", 1.0);
    juce::MemoryBlock m;
    juce::AudioProcessor::copyXmlToBinary (root, m);
    restore (p, m);
    CHECK (p.board() == Board::legacy());
}

TEST_CASE ("The board reaches the engine")
{
    Fixture f;
    SedefProcessor p;
    for (const auto id : { ParamId::tideEnabled, ParamId::pearlEnabled, ParamId::coralEnabled })
        setParam (p, id, 1.0f);
    p.setBoard (boardOf ({ PedalId::coral }));
    render (p);
    const auto& ran = p.engine().telemetry().pedalRan;
    CHECK (ran[index (PedalId::coral)].load());
    CHECK_FALSE (ran[index (PedalId::tide)].load());
    CHECK_FALSE (ran[index (PedalId::pearl)].load());
}

TEST_CASE ("Presets carry the board; ones saved before it get the legacy board")
{
    Fixture f;
    SedefProcessor p;
    auto& presets = p.presets();

    // A user preset saved by a build before the board: the fixed chain it played.
    const auto old =
        plugin::PresetManager::userFolder().getChildFile ("Before the board.sedefpreset");
    REQUIRE (old.getParentDirectory().createDirectory());
    REQUIRE (old.replaceWithText (R"({ "format": "sedef-preset", "version": 1,
                                       "name": "Before the board",
                                       "parameters": { "tideEnabled": 1 } })"));
    presets.scan();
    REQUIRE (presets.load ("user:Before the board.sedefpreset", true));
    CHECK (p.board() == Board::legacy());
    CHECK_FALSE (presets.isDirty());

    // The board counts towards "dirty", and a user preset keeps it.
    p.setBoard (kCustom);
    CHECK (presets.isDirty());
    const auto id = presets.save ("Board test", "");
    REQUIRE (id.isNotEmpty());
    CHECK_FALSE (presets.isDirty());
    const auto file = plugin::PresetManager::userFolder().getChildFile ("Board test.sedefpreset");
    const auto json = juce::JSON::parse (file);
    CHECK (plugin::boardFromVar (json["board"]) == kCustom);

    REQUIRE (presets.load ("factory:riptide", true));
    CHECK (p.board() == boardOf ({ PedalId::gate }));
    REQUIRE (presets.load (id, true));
    CHECK (p.board() == kCustom);
    CHECK_FALSE (presets.isDirty());
    p.setBoard (Board::legacy());
    CHECK (presets.isDirty());
    p.setBoard (kCustom);
    CHECK_FALSE (presets.isDirty());

    // Init has no file, so nothing saved before the board: its board is the pedals its
    // defaults switch on, the Gate alone.
    presets.loadInit();
    CHECK (p.board() == boardOf ({ PedalId::gate }));
    CHECK_FALSE (presets.isDirty());
    p.setBoard (Board::legacy());
    CHECK (presets.isDirty());
}

TEST_CASE ("Factory presets saved with a board load it")
{
    Fixture f;
    SedefProcessor p;
    auto& presets = p.presets();
    REQUIRE (presets.load ("factory:benthic", false));
    CHECK (p.board() == boardOf ({ PedalId::gate, PedalId::trench, PedalId::kelp }));
    CHECK_FALSE (presets.isDirty());
    REQUIRE (presets.load ("factory:holdfast", false));
    CHECK (p.board() == boardOf ({ PedalId::gate, PedalId::kelp }));
    REQUIRE (presets.load ("factory:thermocline", false));
    CHECK (p.board() == boardOf ({ PedalId::gate, PedalId::tide, PedalId::urchin }));
    REQUIRE (presets.load ("factory:nacre", false));
    CHECK (p.board() == boardOf ({ PedalId::gate, PedalId::tide, PedalId::ripple }));
    // Each holds exactly the pedals it switches on (the factory-preset-boards test also renders
    // each against the fixed chain).
    for (const auto* name : { "benthic", "brine", "coral-fuzz", "mariana", "riptide" })
    {
        REQUIRE (presets.load (juce::String ("factory:") + name, false));
        CHECK (p.board() ==
               Board::switchedOn ([&p] (ParamId id) { return p.paramValue (id) >= 0.5f; }));
    }
}

TEST_CASE ("A renamed factory preset keeps its id, and the library lists clean to ambient")
{
    Fixture f;
    SedefProcessor p;
    auto& presets = p.presets();

    // 0.5.0 called it Nacre; favourites, Recent and sessions name it by id.
    REQUIRE (presets.load ("factory:nacre", false));
    CHECK (presets.currentName() == "Chorus Clean");
    const auto state = presets.stateAsVar();
    CHECK (state["recent"][0].toString() == "factory:nacre");

    // A session saved by 0.5.0 names the preset as it was called then: the id finds it, and it
    // shows the name the library has now.
    {
        const auto lock = presets.lockParameters();
        presets.hostRestored ("factory:nacre", "Nacre");
    }
    presets.sync();
    CHECK (presets.currentId() == "factory:nacre");
    CHECK (presets.currentName() == "Chorus Clean");
    CHECK_FALSE (presets.isDirty());
    // A preset this machine doesn't have keeps the name the session gives it.
    {
        const auto lock = presets.lockParameters();
        presets.hostRestored ("user:Gone.sedefpreset", "Gone");
    }
    presets.sync();
    CHECK (presets.currentName() == "Gone");

    // The order the browser lists and previous/next step through.
    juce::StringArray folders;
    for (const auto& item : *state["items"].getArray())
        folders.addIfNotAlreadyThere (item["folder"].toString());
    CHECK (folders == juce::StringArray { "Clean", "Crunch", "High Gain", "Lead", "Ambient" });
    REQUIRE (presets.load ("factory:nacre", true));
    presets.step (-1);
    CHECK (presets.currentName() == "Auto-Wah Funk");
    presets.step (-1);
    CHECK (presets.currentName() == "Vibrato Swell Wash");
}

TEST_CASE ("A preset file without a board gets the pedals it switches on")
{
    // As a preset written by hand or by a build before the board: the legacy board, plus any
    // other pedal its parameters switch on, at that pedal's default position.
    Fixture f;
    juce::var parameters (new juce::DynamicObject());
    parameters.getDynamicObject()->setProperty ("coralEnabled", 1.0);
    CHECK (plugin::legacyBoardFor (parameters) == Board::legacy());
    // Only those pedals (the defaults switch the Gate on), for Init and a fresh instance.
    CHECK (plugin::switchedOnBoardFor (parameters) == boardOf ({ PedalId::gate, PedalId::coral }));
    CHECK (plugin::switchedOnBoardFor ({}) == boardOf ({ PedalId::gate }));
    for (std::size_t i = 0; i < kNumPedals; ++i)
        parameters.getDynamicObject()->setProperty (plugin::toJuce (spec (kPedals[i].enable).id),
                                                    1.0);
    const auto all = plugin::legacyBoardFor (parameters);
    CHECK (all.size() == kNumPedals);
    for (const auto id : Board::legacy())
        CHECK (all.contains (id));
}

TEST_CASE ("Snapshots, Undo and A/B bring the board back")
{
    Fixture f;
    SedefProcessor p;
    auto& presets = p.presets();
    using Slot = plugin::PresetManager::Slot;

    // The browser: audition a preset (it has its own board), then Esc back.
    p.setBoard (kCustom);
    presets.snapshot (Slot::browse);
    REQUIRE (presets.load ("factory:mariana", true));
    CHECK (p.board() == boardOf ({ PedalId::gate }));
    REQUIRE (presets.restore (Slot::browse));
    CHECK (p.board() == kCustom);
    CHECK (presets.isDirty());

    // Undo of a load that dropped unsaved edits.
    presets.snapshot (Slot::undo);
    REQUIRE (presets.load ("factory:riptide", true));
    REQUIRE (presets.restore (Slot::undo));
    CHECK (p.board() == kCustom);

    // A/B: B starts as A, gets its own board, and each side keeps its own.
    presets.compareSwitch(); // to B
    CHECK (p.board() == kCustom);
    const auto boardB = boardOf ({ PedalId::pearl });
    p.setBoard (boardB);
    presets.compareSwitch(); // to A
    CHECK (p.board() == kCustom);
    presets.compareSwitch(); // to B
    CHECK (p.board() == boardB);
    presets.compareCopy(); // the other side (A) becomes B
    presets.compareSwitch();
    CHECK (p.board() == boardB);
}

TEST_CASE ("A host restore replaces the board and the snapshots taken before it")
{
    Fixture f;
    SedefProcessor p;
    auto& presets = p.presets();
    presets.snapshot (plugin::PresetManager::Slot::browse);

    SedefProcessor other;
    other.setBoard (kCustom);
    restore (p, stateOf (other));
    CHECK (p.board() == kCustom);
    CHECK_FALSE (presets.restore (plugin::PresetManager::Slot::browse));
    CHECK (p.board() == kCustom);
}

TEST_CASE ("The browser lists a preset's pedals in its board's order")
{
    Fixture f;
    SedefProcessor p;
    auto& presets = p.presets();
    setParam (p, ParamId::tideEnabled, 1.0f);
    setParam (p, ParamId::rippleEnabled, 1.0f);
    setParam (p, ParamId::gateEnabled, 1.0f);
    p.setBoard (boardOf ({ PedalId::ripple, PedalId::tide, PedalId::coral }));
    const auto id = presets.save ("Board order", "");
    REQUIRE (id.isNotEmpty());
    const auto state = presets.stateAsVar();
    juce::StringArray blocks;
    for (const auto& item : *state["items"].getArray())
        if (item["id"].toString() == id)
            for (const auto& b : *item["blocks"].getArray())
                blocks.add (b.toString());
    // The gate is on but not on the board, so it isn't listed.
    CHECK (blocks.joinIntoString (",").startsWith ("ripple,tide"));
    CHECK_FALSE (blocks.contains ("gate"));
}
