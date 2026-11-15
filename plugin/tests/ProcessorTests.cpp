// SPDX-License-Identifier: AGPL-3.0-or-later
// The processor against what it doesn't control (docs/robustness.md): a latency change while
// another thread plays, NaN from hosts and damaged sessions, sessions whose IR file is missing,
// and the tuner's mute across a device restart.

#include "IrLibrary.h"
#include "ParameterLayout.h"
#include "PluginProcessor.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <limits>
#include <thread>

using namespace sedef;
using sedef::plugin::IrLibrary;
using sedef::plugin::SedefProcessor;

namespace
{
struct Fixture
{
    Fixture()
    {
        root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("sedef-processor-tests")
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

std::unique_ptr<juce::XmlElement> xmlOf (const juce::MemoryBlock& m)
{
    auto xml = juce::AudioProcessor::getXmlFromBinary (m.getData(), static_cast<int> (m.getSize()));
    REQUIRE (xml != nullptr);
    return xml;
}

juce::MemoryBlock binaryOf (const juce::XmlElement& xml)
{
    juce::MemoryBlock m;
    juce::AudioProcessor::copyXmlToBinary (xml, m);
    return m;
}

/// `blocks` blocks of a 0.3 sine; the largest |sample|, or NaN if any sample is not finite.
float play (SedefProcessor& p, int blocks, int startBlock = 0)
{
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> buffer (2, kBlock);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
                buffer.setSample (
                    c, i,
                    0.3f * std::sin (0.05f * static_cast<float> ((startBlock + b) * kBlock + i)));
        juce::MidiBuffer midi;
        p.processBlock (buffer, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
            {
                const float x = buffer.getSample (c, i);
                if (!std::isfinite (x))
                    return std::numeric_limits<float>::quiet_NaN();
                peak = std::max (peak, std::abs (x));
            }
    }
    return peak;
}

/// Every pedal on the board and on, every post effect on: every parameter is in the signal path.
void switchEverythingOn (SedefProcessor& p)
{
    for (const auto& pedal : kPedals)
        p.setParamValue (pedal.enable, 1.0f);
    for (const auto id : { ParamId::doublerEnabled, ParamId::driftEnabled, ParamId::fathomEnabled,
                           ParamId::eqReefEnabled, ParamId::micBEnabled, ParamId::roomAEnabled })
        p.setParamValue (id, 1.0f);
    p.setBoard (Board::legacy());
}

juce::File writeIr (const juce::File& file)
{
    juce::WavAudioFormat wav;
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> out (file.createOutputStream().release());
    REQUIRE (out != nullptr);
    auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions {}
                                                .withSampleRate (48000.0)
                                                .withNumChannels (1)
                                                .withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    juce::AudioBuffer<float> ir (1, 480);
    ir.clear();
    ir.setSample (0, 0, 0.9f);
    ir.setSample (0, 10, -0.3f);
    REQUIRE (writer->writeFromAudioSampleBuffer (ir, 0, ir.getNumSamples()));
    return file;
}

juce::String irPathIn (const juce::MemoryBlock& state)
{
    const auto xml = xmlOf (state);
    if (const auto* irs = xml->getChildByName ("IRs"))
        if (const auto* slot = irs->getChildByName ("Slot"))
            return slot->getStringAttribute ("path");
    return {};
}
} // namespace

// 1. The hold ------------------------------------------------------------------------------------

TEST_CASE ("While the hold is raised a block plays silence and stays out of the engine", "[hold]")
{
    // The audio thread's side of the handshake (docs/robustness.md, section 1).
    Fixture f;
    SedefProcessor p;
    prepare (p);
    p.setParamValue (ParamId::gateEnabled, 0.0f);
    CHECK (play (p, 4) > 1.0e-3f);
    p.processHold().raise();
    CHECK (play (p, 4, 4) <= 0.0f); // silence: the block left without touching the engine
    CHECK (p.processHold().audioIsOut());
    p.processHold().lower();
    CHECK (play (p, 4, 8) > 1.0e-3f);
}

TEST_CASE ("A latency change waits for the block inside and never prepares under it", "[hold]")
{
    // The message thread's side: with a block inside processBlock (as a CLAP host's audio thread
    // can be), Settings > Latency gives up after the timeout instead of preparing the engine
    // under it, and applies on a later tick once the block has left.
    Fixture f;
    SedefProcessor p;
    prepare (p);
    REQUIRE (p.engine().oversampling() == OversamplingMode::lowLatency);
    REQUIRE (p.processHold().enter()); // a block is running
    p.setParamValue (ParamId::oversampling, 1.0f);
    p.tick();
    CHECK (p.engine().oversampling() == OversamplingMode::lowLatency); // not prepared under it
    CHECK_FALSE (p.processHold().isHeld());                            // and the hold lowered again
    p.processHold().leave();
    p.tick();
    CHECK (p.engine().oversampling() == OversamplingMode::linearPhase);
    CHECK (p.getLatencySamples() == p.engine().latencySamples());
}

TEST_CASE ("A latency change while another thread plays never meets a running block", "[hold]")
{
    // The CLAP wrapper calls processBlock without JUCE's callback lock, so the old
    // suspendProcessing() let Settings > Latency prepare the engine under a running block
    // (docs/robustness.md, section 1). Here an "audio thread" plays without pause.
    Fixture f;
    SedefProcessor p;
    prepare (p);
    switchEverythingOn (p);
    std::atomic<bool> done { false }, finite { true };
    std::atomic<int> blocks { 0 };
    std::thread audio ([&] {
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;
        for (int n = 0; !done.load(); ++n)
        {
            for (int i = 0; i < kBlock; ++i)
                buffer.setSample (0, i,
                                  0.3f * std::sin (0.05f * static_cast<float> (n * kBlock + i)));
            buffer.clear (1, 0, kBlock);
            p.processBlock (buffer, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < kBlock; ++i)
                    if (!std::isfinite (buffer.getSample (c, i)))
                        finite.store (false);
            blocks.fetch_add (1);
        }
    });

    for (int i = 0; i < 200; ++i)
    {
        for (const int seen = blocks.load(); blocks.load() < seen + 2;)
            std::this_thread::yield();
        p.setParamValue (ParamId::oversampling, static_cast<float> ((i + 1) % 2));
        p.tick(); // Settings > Latency applies here
        CHECK (p.engine().oversampling() ==
               ((i + 1) % 2 == 1 ? OversamplingMode::linearPhase : OversamplingMode::lowLatency));
    }
    done.store (true);
    audio.join();
    CHECK (finite.load());
    CHECK (p.getLatencySamples() == p.engine().latencySamples());
}

// 2. Numbers from hosts and sessions ---------------------------------------------------------

TEST_CASE ("A parameter saved as nan restores its default, plays and saves the default")
{
    // A session value of "nan" got past APVTS (jlimit lets NaN through) into the engine's
    // snapshot: the rig played NaN, and the filters kept it after the value was fixed.
    Fixture f;
    juce::MemoryBlock base;
    {
        SedefProcessor p;
        prepare (p);
        switchEverythingOn (p);
        base = stateOf (p);
    }
    const auto xml0 = xmlOf (base);
    SedefProcessor p;
    prepare (p);
    juce::StringArray wrong;
    for (auto* param : xml0->getChildByName ("SedefParameters")->getChildIterator())
    {
        const auto id = findParam (param->getStringAttribute ("id").toStdString());
        if (!id)
            continue;
        juce::XmlElement xml (*xml0);
        for (auto* p2 : xml.getChildByName ("SedefParameters")->getChildIterator())
            if (p2->getStringAttribute ("id") == param->getStringAttribute ("id"))
                p2->setAttribute ("value", "nan");
        restore (p, binaryOf (xml));
        const auto& s = spec (*id);
        const bool isDefault = juce::exactlyEqual (p.paramValue (*id), s.defaultValue) ||
                               std::abs (p.paramValue (*id) - s.defaultValue) <=
                                   1.0e-5f * (s.max - s.min); // the normalised round trip
        const bool finite = std::isfinite (play (p, 8));
        bool savedFinite = true;
        const auto resaved = xmlOf (stateOf (p));
        for (auto* p3 : resaved->getChildByName ("SedefParameters")->getChildIterator())
            if (p3->getStringAttribute ("id") == param->getStringAttribute ("id"))
                savedFinite = std::isfinite (p3->getDoubleAttribute ("value")) &&
                              !p3->getStringAttribute ("value").containsIgnoreCase ("nan");
        if (!isDefault || !finite || !savedFinite)
            wrong.add (param->getStringAttribute ("id"));
    }
    INFO ("wrong after a nan: " << wrong.joinIntoString (", "));
    CHECK (wrong.isEmpty());
}

TEST_CASE ("A host's NaN automation plays the default and is never saved")
{
    Fixture f;
    SedefProcessor p;
    prepare (p);
    switchEverythingOn (p);
    CHECK (std::isfinite (play (p, 4)));
    for (std::size_t i = 0; i < kNumParams; ++i)
        if (auto* param = p.state().getParameter (plugin::toJuce (kParamSpecs[i].id)))
            param->setValueNotifyingHost (std::numeric_limits<float>::quiet_NaN());
    CHECK (std::isfinite (play (p, 16, 4)));

    const auto saved = xmlOf (stateOf (p));
    for (auto* param : saved->getChildByName ("SedefParameters")->getChildIterator())
    {
        INFO (param->getStringAttribute ("id") << " = " << param->getStringAttribute ("value"));
        CHECK (std::isfinite (param->getDoubleAttribute ("value")));
        CHECK_FALSE (param->getStringAttribute ("value").containsIgnoreCase ("nan"));
    }
    // A preset saved now is readable JSON with finite values.
    const auto id = p.presets().save ("Saved during NaN", "");
    REQUIRE (id.isNotEmpty());
    REQUIRE (p.presets().load (id, false));
    CHECK (std::isfinite (play (p, 4, 20)));
}

TEST_CASE ("Preset values that are null or not numbers load as the default")
{
    const auto parameters = juce::JSON::parse (R"({ "inputGain": null, "outputGain": "3",
        "pearlEnabled": true, "abyssGain": 1e999, "reefGain": [], "driftMix": 0.25 })");
    const auto value = [&] (ParamId id) {
        return plugin::PresetManager::storedValue (parameters, index (id));
    };
    CHECK (juce::exactlyEqual (value (ParamId::inputGain), spec (ParamId::inputGain).defaultValue));
    CHECK (
        juce::exactlyEqual (value (ParamId::outputGain), spec (ParamId::outputGain).defaultValue));
    CHECK (juce::exactlyEqual (value (ParamId::pearlEnabled), 1.0f));
    CHECK (juce::exactlyEqual (value (ParamId::abyssGain), spec (ParamId::abyssGain).defaultValue));
    CHECK (juce::exactlyEqual (value (ParamId::reefGain), spec (ParamId::reefGain).defaultValue));
    CHECK (juce::exactlyEqual (value (ParamId::driftMix), 0.25f));
}

namespace
{
juce::String mutateValue (juce::Random& r, const juce::String& v)
{
    static const char* const kOdd[] = { "",       "nan", "inf",        "-inf", "1e309",
                                        "-1e309", "abc", "-1",         "9999", "0x10",
                                        "null",   "[]",  "{}",         ",,,",  "1,2",
                                        "true",   " ",   "2147483648", "-0",   "3.5" };
    switch (r.nextInt (4))
    {
    case 0:
        return kOdd[r.nextInt (static_cast<int> (std::size (kOdd)))];
    case 1:
        return v.substring (0, r.nextInt (v.length() + 1));
    case 2:
        return v + v;
    default:
        return juce::String (r.nextDouble() * 2.0e6 - 1.0e6);
    }
}

void mutate (juce::Random& r, juce::XmlElement& e)
{
    for (int i = 0; i < e.getNumAttributes(); ++i)
        if (r.nextInt (6) == 0)
            e.setAttribute (e.getAttributeName (i), mutateValue (r, e.getAttributeValue (i)));
    for (auto* child : e.getChildIterator())
        mutate (r, *child);
}
} // namespace

TEST_CASE ("A session whose attributes are damaged restores safely")
{
    // Every attribute (parameters; Extra: board, scenes, pattern, uiScale, preset; IRs; Capture)
    // replaced now and then by odd text. Restoring must not crash, the rig must stay finite and
    // the window must keep a usable size (a NaN uiScale gave a 0 x 0 window).
    Fixture f;
    juce::MemoryBlock base;
    {
        SedefProcessor p;
        prepare (p);
        REQUIRE (p.live().rememberInScenes (ParamId::pearlGain, true));
        p.live().selectScene (2);
        p.setBoard (Board::legacy());
        base = stateOf (p);
    }
    const auto xml0 = xmlOf (base);
    const auto seed = GENERATE (1234, 99, 7, 2024);
    juce::Random r (seed);
    SedefProcessor p;
    prepare (p);
    for (int round = 0; round < 300; ++round)
    {
        juce::XmlElement xml (*xml0);
        mutate (r, xml);
        restore (p, binaryOf (xml));
        p.live().tick();
        INFO ("seed " << seed << " round " << round << " uiScale " << p.uiScale());
        REQUIRE (std::isfinite (play (p, 1, round)));
        REQUIRE (p.uiScale() >= 0.5);
        REQUIRE (p.uiScale() <= 2.5);
        REQUIRE (stateOf (p).getSize() > 0);
    }
}

TEST_CASE ("The window scale from a session: only a finite number in range")
{
    Fixture f;
    SedefProcessor p;
    prepare (p);
    const auto xml0 = xmlOf (stateOf (p));
    for (const auto& [text, expected] :
         { std::pair { "nan", 1.0 }, std::pair { "inf", 1.0 }, std::pair { "abc", 1.0 },
           std::pair { "", 1.0 }, std::pair { "9", 2.5 }, std::pair { "0.1", 0.5 },
           std::pair { "1.25", 1.25 } })
    {
        juce::XmlElement xml (*xml0);
        xml.getChildByName ("Extra")->setAttribute ("uiScale", text);
        p.setUiScale (1.5);
        restore (p, binaryOf (xml));
        INFO ("uiScale=\"" << text << "\"");
        CHECK (juce::exactlyEqual (p.uiScale(), expected));
    }
    p.setUiScale (std::numeric_limits<double>::quiet_NaN());
    CHECK (juce::exactlyEqual (p.uiScale(), 1.0));
}

// 4. Missing files ----------------------------------------------------------------------------

TEST_CASE ("A session whose IR file is missing keeps it, and gets it back once it is there")
{
    // An external drive not mounted, a project opened on another computer: the next save dropped
    // the IR's path, so it never came back (docs/robustness.md, section 4).
    Fixture f;
    const auto drive = f.root.getChildFile ("ExternalDrive");
    drive.createDirectory();
    const auto irFile = writeIr (drive.getChildFile ("Cab.wav"));

    juce::MemoryBlock saved;
    {
        SedefProcessor p;
        prepare (p);
        REQUIRE (p.irs().load (0, 0, irFile).isEmpty());
        saved = stateOf (p);
    }
    REQUIRE (irPathIn (saved) == irFile.getFullPathName());

    // Unplugged: the project is opened and saved.
    const auto parked = f.root.getChildFile ("Parked.wav");
    REQUIRE (irFile.moveFileTo (parked));
    juce::MemoryBlock resaved;
    {
        SedefProcessor p;
        prepare (p);
        restore (p, saved);
        // What the message thread does after a restore (IrLibrary::loadAsync with this origin).
        CHECK (p.irs().load (0, 0, p.irs().fileFor (0, 0), IrLibrary::Origin::session) ==
               "The IR file was not found");
        CHECK (p.irs().fileFor (0, 0) == irFile);
        CHECK (p.irs().missingReason (0, 0) == "The IR file was not found");
        const auto slot = p.irs().stateAsVar()["slots"][0];
        CHECK (slot["name"].isVoid()); // nothing plays
        CHECK (slot["missing"]["fileName"] == "Cab.wav");
        CHECK (slot["missing"]["path"] == irFile.getFullPathName());
        resaved = stateOf (p);
    }
    CHECK (irPathIn (resaved) == irFile.getFullPathName());
    CHECK (resaved == saved); // byte for byte

    // Plugged in again: the session loads its IR.
    REQUIRE (parked.moveFileTo (irFile));
    SedefProcessor p;
    prepare (p);
    restore (p, resaved);
    CHECK (p.irs().load (0, 0, p.irs().fileFor (0, 0), IrLibrary::Origin::session).isEmpty());
    CHECK (p.irs().missingReason (0, 0).isEmpty());
    CHECK (p.irs().stateAsVar()["slots"][0]["name"] == "Cab.wav");
}

TEST_CASE ("Locating a missing IR, or a load that fails, behaves as the player expects")
{
    Fixture f;
    const auto irFile = writeIr (f.root.getChildFile ("Cab.wav"));
    const auto other = writeIr (f.root.getChildFile ("Other.wav"));
    const auto broken = f.root.getChildFile ("Broken.wav");
    REQUIRE (broken.replaceWithText ("not audio at all"));
    SedefProcessor p;
    prepare (p);

    // A load the player starts that fails leaves the slot as it was.
    REQUIRE (p.irs().load (1, 2, irFile).isEmpty());
    CHECK (p.irs().load (1, 2, broken) == "Unsupported audio file");
    CHECK (p.irs().fileFor (1, 2) == irFile);
    CHECK (p.irs().stateAsVar()["slots"][5]["name"] == "Cab.wav");
    CHECK (irPathIn (stateOf (p)) == irFile.getFullPathName());

    // A session's file that is there but isn't audio is kept as missing, with the reason.
    CHECK (p.irs().load (1, 2, broken, IrLibrary::Origin::session) == "Unsupported audio file");
    CHECK (p.irs().fileFor (1, 2) == broken);
    CHECK (p.irs().stateAsVar()["slots"][5]["missing"]["reason"] == "Unsupported audio file");
    // Locate: the player picks a file; the slot plays it.
    CHECK (p.irs().load (1, 2, other).isEmpty());
    CHECK (p.irs().missingReason (1, 2).isEmpty());
    CHECK (p.irs().stateAsVar()["slots"][5]["missing"].isVoid());
}

TEST_CASE ("Sessions whose IRs are there load and save as before")
{
    Fixture f;
    const auto a = writeIr (f.root.getChildFile ("A.wav"));
    const auto b = writeIr (f.root.getChildFile ("B.wav"));
    juce::MemoryBlock saved;
    {
        SedefProcessor p;
        prepare (p);
        REQUIRE (p.irs().load (0, 1, a).isEmpty());
        REQUIRE (p.irs().load (1, 0, b).isEmpty());
        saved = stateOf (p);
    }
    SedefProcessor p;
    prepare (p);
    restore (p, saved);
    CHECK (stateOf (p) == saved);
    for (const auto& [m, c, file] : { std::tuple { 0, 1, a }, std::tuple { 1, 0, b } })
    {
        CHECK (p.irs().load (m, c, p.irs().fileFor (m, c), IrLibrary::Origin::session).isEmpty());
        CHECK (p.irs().fileFor (m, c) == file);
        CHECK (p.irs().missingReason (m, c).isEmpty());
    }
    CHECK (stateOf (p) == saved);
    // A relative path from a hand-edited session is not a reference.
    auto xml = xmlOf (saved);
    xml->getChildByName ("IRs")->getChildByName ("Slot")->setAttribute ("path", "Cab.wav");
    restore (p, binaryOf (*xml));
    CHECK (p.irs().fileFor (0, 1) == juce::File());
}

// 6. The tuner --------------------------------------------------------------------------------

TEST_CASE ("The tuner's mute holds through a device restart")
{
    // prepareToPlay stops feeding the tracker until the message thread restarts it; the mute
    // followed that feed, so the guitar came through in between (docs/robustness.md, section 6).
    Fixture f;
    SedefProcessor p;
    prepare (p);
    p.setParamValue (ParamId::gateEnabled, 0.0f);
    p.setParamValue (ParamId::tunerMute, 1.0f);
    CHECK (play (p, 8) > 1.0e-3f); // tuner closed: the guitar plays
    p.analysis().setActive (true);
    play (p, 8, 8);
    CHECK (play (p, 4, 16) < 1.0e-6f);
    prepare (p); // the device restarts; engineWasPrepared() has not run yet
    CHECK (play (p, 8, 20) < 1.0e-6f);
    p.analysis().setActive (false);
    play (p, 8, 28);
    CHECK (play (p, 4, 36) > 1.0e-3f);
}
