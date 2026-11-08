// SPDX-License-Identifier: AGPL-3.0-or-later
// MidiPortRouter through a real virtual port (macOS; Linux with an ALSA sequencer): another
// client sees "Sedef MIDI ...", gets the notes in order and spaced as they were played, and
// changing the destination or quitting ends the note that was sounding. Where the platform has no
// virtual ports (Windows, or Linux without a sequencer), the router says so instead.
#include "MidiPortRouter.h"
#include "ParameterLayout.h"
#include "sedef/midi/MidiParameters.h"

#include <catch2/catch_test_macros.hpp>
#include <mutex>

using sedef::midiplugin::MidiPortRouter;

namespace
{
struct Received final : juce::MidiInputCallback
{
    std::mutex lock;
    std::vector<std::pair<double, juce::MidiMessage>> messages;

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {
        const std::scoped_lock l (lock);
        messages.emplace_back (juce::Time::getMillisecondCounterHiRes(), m);
    }
    std::vector<std::pair<double, juce::MidiMessage>> copy()
    {
        const std::scoped_lock l (lock);
        return messages;
    }
};

bool waitFor (const std::function<bool()>& condition, int ms = 3000)
{
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (ms);
    while (!condition())
    {
        if (juce::Time::getMillisecondCounter() > end)
            return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    }
    return true;
}

sedef::midi::EventList eventsOf (std::initializer_list<sedef::midi::Event> list)
{
    sedef::midi::EventList out;
    for (const auto& e : list)
        out.push (e);
    return out;
}
} // namespace

TEST_CASE ("The virtual port carries the notes, spaced as played, and ends them", "[midi][ports]")
{
    const juce::String name =
        "Sedef MIDI test " + juce::String (juce::Random::getSystemRandom().nextInt (100000));
    Received received;
    std::unique_ptr<juce::MidiInput> input;
    {
        MidiPortRouter router (name);
        router.setDestination (MidiPortRouter::kVirtual);
        REQUIRE (waitFor ([&router] { return router.active() || router.status().isNotEmpty(); }));
        if (!MidiPortRouter::virtualSupported() || !router.active())
        {
            INFO (router.status().toStdString());
            CHECK (router.status().isNotEmpty());
            SKIP ("No virtual MIDI ports here: " << router.status());
        }
        juce::MidiDeviceInfo source;
        const auto listed = [] {
            juce::StringArray names;
            for (const auto& d : juce::MidiInput::getAvailableDevices())
                names.add (d.name + " [" + d.identifier + "]");
            return names.joinIntoString (", ").toStdString();
        };
        INFO ("inputs: " << listed());
        REQUIRE (waitFor ([&] {
            for (const auto& d : juce::MidiInput::getAvailableDevices())
                if (d.name == name)
                    source = d;
            return source.identifier.isNotEmpty();
        }));
        input = juce::MidiInput::openDevice (source.identifier, &received);
        REQUIRE (input != nullptr);
        input->start();

        // A note at sample 0, its note-off and the next note 2400 samples (50 ms) later.
        using sedef::midi::Event;
        router.push (eventsOf ({ Event::noteOn (1, 40, 100, 0) }), 0, 48000.0);
        router.push (eventsOf ({ Event::noteOff (1, 40, 0), Event::noteOn (1, 43, 90, 0) }), 2400,
                     48000.0);
        REQUIRE (waitFor ([&received] { return received.copy().size() >= 3; }));
        const auto got = received.copy();
        CHECK (got[0].second.isNoteOn());
        CHECK (got[0].second.getNoteNumber() == 40);
        CHECK (got[1].second.isNoteOff());
        CHECK (got[2].second.getNoteNumber() == 43);
        // Spaced as played, give or take the router's millisecond.
        const double spacing = got[1].first - got[0].first;
        INFO ("spacing " << spacing << " ms");
        CHECK (spacing > 46.0);
        CHECK (spacing < 54.0);
        // Quitting (the router going away) ends the note still sounding: 43.
    }
    REQUIRE (waitFor ([&received] { return received.copy().size() >= 4; }));
    const auto got = received.copy();
    CHECK (got[3].second.isNoteOff());
    CHECK (got[3].second.getNoteNumber() == 43);
    if (input != nullptr)
        input->stop();
}

TEST_CASE ("A destination that cannot open says why", "[midi][ports]")
{
    MidiPortRouter router ("Sedef MIDI test");
    router.setDestination (juce::String (MidiPortRouter::kDevicePrefix) + "no-such-device");
    REQUIRE (waitFor ([&router] { return router.status().isNotEmpty(); }));
    CHECK (!router.active());
    CHECK (router.destination() == "device:no-such-device");
    router.setDestination (MidiPortRouter::kOff);
    REQUIRE (waitFor ([&router] { return router.status().isEmpty(); }));
    CHECK (router.destination() == MidiPortRouter::kOff);
}

TEST_CASE ("Sedef MIDI's parameters read back from their own text", "[midi][params]")
{
    // What hosts and clap-validator check: value -> text -> value -> the same text.
    for (const auto& spec : sedef::midi::kParamSpecs)
        for (int i = 0; i <= 200; ++i)
        {
            const float value = spec.min + (spec.max - spec.min) * static_cast<float> (i) / 200.0f;
            const auto snapped = spec.sanitise (value);
            const auto text = sedef::plugin::formatValue (spec, snapped);
            const auto again =
                sedef::plugin::formatValue (spec, sedef::plugin::parseValue (spec, text));
            INFO (std::string (spec.id) << " " << snapped << " -> " << text << " -> " << again);
            CHECK (text == again);
        }
    CHECK (sedef::plugin::formatValue (sedef::midi::spec (sedef::midi::ParamId::bendRange), 2.0f) ==
           "2 st");
    CHECK (sedef::plugin::formatValue (sedef::midi::spec (sedef::midi::ParamId::channel), 10.0f) ==
           "10");
    CHECK (sedef::plugin::formatValue (sedef::midi::spec (sedef::midi::ParamId::octave), 0.0f) ==
           "-2");
}
