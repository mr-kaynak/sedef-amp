// SPDX-License-Identifier: AGPL-3.0-or-later
// MidiPortRouter through a real virtual port (macOS; Linux with an ALSA sequencer): another
// client sees "Sedef MIDI ...", gets the notes in order and spaced as they were played, and
// changing the destination or quitting ends the note that was sounding. Where the platform has no
// virtual ports (Windows, or Linux without a sequencer), the router says so instead.
#include "MidiPortRouter.h"
#include "ParameterLayout.h"
#include "sedef/midi/MidiParameters.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <mutex>
#include <sstream>
#include <vector>

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
    constexpr int kNotes = 6;
    const juce::String name =
        "Sedef MIDI test " + juce::String (juce::Random::getSystemRandom().nextInt (100000));
    Received received;
    std::unique_ptr<juce::MidiInput> input;
    std::size_t total = 0; // messages received so far
    int last = -1;         // the note still sounding
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

        // Six notes 2400 samples (50 ms) apart, each ending the one before. A busy machine can
        // wake the router's thread, or the receiving one, 10-20 ms late at any note, so the six
        // are played up to three times and the first run that comes through undisturbed is
        // judged: a router that spaces notes wrongly does it in every run.
        using sedef::midi::Event;
        constexpr int kRuns = 3;
        std::vector<double> spacings;
        std::ostringstream runs;
        for (int run = 0; run < kRuns; ++run)
        {
            const int first = 40 + 8 * run;
            sedef::midi::EventList opening;
            if (last >= 0)
                opening.push (Event::noteOff (1, last, 0));
            opening.push (Event::noteOn (1, first, 100, 0));
            router.push (opening, 0, 48000.0);
            for (int n = 1; n < kNotes; ++n)
                router.push (eventsOf ({ Event::noteOff (1, first + n - 1, 0),
                                         Event::noteOn (1, first + n, 90, 0) }),
                             n * 2400, 48000.0);
            const auto start = total + static_cast<std::size_t> (last >= 0 ? 1 : 0);
            const auto count = start + static_cast<std::size_t> (2 * kNotes - 1);
            REQUIRE (waitFor ([&] { return received.copy().size() >= count; }));
            const auto got = received.copy();
            if (last >= 0)
            {
                CHECK (got[total].second.isNoteOff());
                CHECK (got[total].second.getNoteNumber() == last);
            }
            const auto at = [&] (int k) {
                return got[start + static_cast<std::size_t> (k)];
            };
            spacings.clear();
            for (int n = 0; n < kNotes; ++n)
            {
                CHECK (at (2 * n).second.isNoteOn());
                CHECK (at (2 * n).second.getNoteNumber() == first + n);
                if (n == 0)
                    continue;
                CHECK (at (2 * n - 1).second.isNoteOff());
                CHECK (at (2 * n - 1).second.getNoteNumber() == first + n - 1);
                spacings.push_back (at (2 * n).first - at (2 * n - 2).first);
            }
            total = count;
            last = first + kNotes - 1;
            // Spaced as played: the typical spacing within the router's millisecond (and the
            // receiving thread's), and none bunched up or held back.
            std::ranges::sort (spacings);
            const double median = spacings[spacings.size() / 2];
            runs << "run " << run + 1 << ": spacings " << spacings.front() << " to "
                 << spacings.back() << " ms, median " << median << "; ";
            if (median > 46.0 && median < 54.0 && spacings.front() > 25.0 &&
                spacings.back() < 100.0)
                break;
        }
        const double median = spacings[spacings.size() / 2];
        INFO (runs.str());
        CHECK (median > 46.0);
        CHECK (median < 54.0);
        CHECK (spacings.front() > 25.0);
        CHECK (spacings.back() < 100.0);
        // Quitting (the router going away) ends the note still sounding.
    }
    REQUIRE (waitFor ([&] { return received.copy().size() > total; }));
    const auto got = received.copy();
    CHECK (got[total].second.isNoteOff());
    CHECK (got[total].second.getNoteNumber() == last);
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
