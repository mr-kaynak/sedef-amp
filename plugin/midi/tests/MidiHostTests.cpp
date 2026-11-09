// SPDX-License-Identifier: AGPL-3.0-or-later
// The built Sedef MIDI VST3, hosted the way a DAW hosts it: its MIDI output is the engine's,
// event for event and sample for sample, whatever the block size; bypass, a stopped transport
// and a reset end the sounding note; its state round-trips.
#include "midi/MidiTestSignals.h"
#include "sedef/midi/GuitarToMidi.h"
#include "sedef/midi/MidiParameters.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_processors/juce_audio_processors.h>

namespace
{
using sedef::test::midi::bassLine;
using sedef::test::midi::Pickup;

constexpr double kFs = 48000.0;

struct Sent
{
    std::int64_t sample = 0;
    std::uint8_t bytes[3] {};
    int size = 0;
    bool operator== (const Sent& o) const
    {
        return sample == o.sample && size == o.size && bytes[0] == o.bytes[0] &&
               bytes[1] == o.bytes[1] && (size < 3 || bytes[2] == o.bytes[2]);
    }
};

std::string describe (const std::vector<Sent>& v, std::size_t i)
{
    if (i >= v.size())
        return "(none)";
    const auto& s = v[i];
    return std::to_string (s.sample) + ": " + std::to_string (s.bytes[0]) + " " +
           std::to_string (s.bytes[1]) + " " + std::to_string (s.bytes[2]);
}

/// Loads the VST3 prepared for blocks of up to `maxBlock` samples. A host never sends more than
/// it prepared for: the wrappers size their buffers by it.
std::unique_ptr<juce::AudioPluginInstance> load (int maxBlock = 256)
{
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, SEDEF_MIDI_VST3_PATH);
    REQUIRE (!types.isEmpty());
    juce::String error;
    auto plugin = format.createInstanceFromDescription (*types[0], kFs, maxBlock, error);
    INFO (error);
    REQUIRE (plugin != nullptr);
    plugin->setPlayConfigDetails (1, 2, kFs, maxBlock);
    plugin->prepareToPlay (kFs, maxBlock);
    return plugin;
}

/// A host's transport: playing or stopped.
struct Transport final : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        return info;
    }
};

std::vector<Sent> run (juce::AudioPluginInstance& plugin, const std::vector<float>& x,
                       std::size_t from, std::size_t to, int block)
{
    std::vector<Sent> out;
    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    for (std::size_t i = from; i < to; i += static_cast<std::size_t> (block))
    {
        const int n =
            static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (block), to - i));
        buffer.setSize (2, n, false, false, true);
        buffer.clear();
        buffer.copyFrom (0, 0, x.data() + i, n);
        midi.clear();
        plugin.processBlock (buffer, midi);
        for (const auto m : midi)
        {
            Sent s;
            s.sample = static_cast<std::int64_t> (i) + m.samplePosition;
            s.size = m.numBytes;
            std::copy (m.data, m.data + std::min (3, m.numBytes), s.bytes);
            out.push_back (s);
        }
    }
    return out;
}

std::vector<Sent> engineReference (const std::vector<float>& x, int block)
{
    sedef::midi::GuitarToMidi g;
    g.prepare (kFs);
    g.setSettings (sedef::midi::Settings {}); // the plug-in's defaults
    sedef::midi::EventList events;
    std::vector<Sent> out;
    for (std::size_t i = 0; i < x.size(); i += static_cast<std::size_t> (block))
    {
        const int n = static_cast<int> (
            std::min<std::size_t> (static_cast<std::size_t> (block), x.size() - i));
        events.clear();
        g.process (x.data() + i, n, events);
        for (const auto& e : events)
        {
            Sent s;
            s.sample = static_cast<std::int64_t> (i) + e.offset;
            s.size = e.size;
            std::copy (e.bytes, e.bytes + 3, s.bytes);
            out.push_back (s);
        }
    }
    return out;
}

int sounding (const std::vector<Sent>& events)
{
    int on = 0;
    for (const auto& e : events)
    {
        if ((e.bytes[0] & 0xF0) == 0x90 && e.bytes[2] > 0)
            ++on;
        else if ((e.bytes[0] & 0xF0) == 0x80 || ((e.bytes[0] & 0xF0) == 0x90 && e.bytes[2] == 0))
            --on;
    }
    return on;
}
} // namespace

TEST_CASE ("The Sedef MIDI VST3 sends the engine's notes at their samples", "[midi][host]")
{
    const auto sc = bassLine (3, Pickup::bridge, 132.0, kFs, 24);
    for (int block : { 64, 256, 333 })
    {
        INFO ("block " << block);
        auto plugin = load (block);
        CHECK (plugin->getName() == "Sedef MIDI");
        CHECK (plugin->producesMidi());
        CHECK (plugin->getParameters().size() == static_cast<int> (sedef::midi::kNumParams) + 1);
        const auto got = run (*plugin, sc.signal, 0, sc.signal.size(), block);
        // The engine in 512-sample chunks at most, like the plug-in: the result does not depend
        // on how the blocks fall.
        const auto want = engineReference (sc.signal, 64);
        REQUIRE (want.size() > 30);
        std::size_t i = 0;
        while (i < want.size() && i < got.size() && got[i] == want[i])
            ++i;
        INFO ("first difference at event " << i << ": got " << describe (got, i) << ", want "
                                           << describe (want, i));
        CHECK (got.size() == want.size());
        CHECK (i == want.size());
        plugin->releaseResources();
    }
}

TEST_CASE ("Bypass, a stopped transport and a reset end the Sedef MIDI note", "[midi][host]")
{
    const auto sc = bassLine (5, Pickup::bridge, 120.0, kFs, 12);
    // Run until a note sounds, then end it each way; every note-on gets its note-off.
    const auto untilSounding = [&sc] (juce::AudioPluginInstance& p, std::vector<Sent>& all) {
        for (std::size_t at = 0; at + 256 <= sc.signal.size(); at += 256)
        {
            const auto e = run (p, sc.signal, at, at + 256, 256);
            all.insert (all.end(), e.begin(), e.end());
            if (sounding (all) > 0)
                return at + 256;
        }
        return sc.signal.size();
    };

    SECTION ("bypass")
    {
        auto plugin = load();
        std::vector<Sent> all;
        const auto at = untilSounding (*plugin, all);
        REQUIRE (sounding (all) == 1);
        auto* bypass = plugin->getBypassParameter();
        REQUIRE (bypass != nullptr);
        bypass->setValueNotifyingHost (1.0f);
        const auto e = run (*plugin, sc.signal, at, at + 256, 256);
        all.insert (all.end(), e.begin(), e.end());
        CHECK (sounding (all) == 0);
    }
    SECTION ("transport stop")
    {
        auto plugin = load();
        Transport transport;
        plugin->setPlayHead (&transport);
        std::vector<Sent> all;
        const auto at = untilSounding (*plugin, all);
        REQUIRE (sounding (all) == 1);
        transport.playing = false;
        const auto e = run (*plugin, sc.signal, at, at + 256, 256);
        all.insert (all.end(), e.begin(), e.end());
        CHECK (sounding (all) == 0);
        plugin->setPlayHead (nullptr);
    }
    SECTION ("reset")
    {
        auto plugin = load();
        std::vector<Sent> all;
        const auto at = untilSounding (*plugin, all);
        REQUIRE (sounding (all) == 1);
        plugin->reset();
        const auto e = run (*plugin, sc.signal, at, at + 256, 256);
        all.insert (all.end(), e.begin(), e.end());
        CHECK (sounding (all) == 0);
    }
}

TEST_CASE ("Sedef MIDI's state round-trips", "[midi][host]")
{
    auto a = load();
    juce::Random random (99);
    for (auto* p : a->getParameters())
        p->setValueNotifyingHost (random.nextFloat());
    juce::MemoryBlock saved;
    a->getStateInformation (saved);
    auto b = load();
    b->setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    juce::MemoryBlock again;
    b->getStateInformation (again);
    CHECK (again == saved);
    for (int i = 0; i < a->getParameters().size(); ++i)
        CHECK (a->getParameters()[i]->getCurrentValueAsText() ==
               b->getParameters()[i]->getCurrentValueAsText());
}
