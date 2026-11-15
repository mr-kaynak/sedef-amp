// SPDX-License-Identifier: AGPL-3.0-or-later
// Notes nobody saw picked, and the decoder as a second opinion
// (docs/design/guitar-to-midi.md, "Real playing").
#include "MidiTestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

using namespace sedef;
using namespace sedef::test::midi;
using sedef::test::addNoise;
using sedef::test::addPluck;
using sedef::test::Pluck;

namespace
{
constexpr double kFs = 48000.0;

struct Outcome
{
    Run run;
    std::uint32_t heard = 0;
};

Outcome play (const Scenario& sc, midi::Timing timing)
{
    midi::Settings s;
    s.octave = 0;
    s.timing = timing;
    midi::GuitarToMidi g;
    g.setSettings (s);
    g.prepare (sc.fs);
    Outcome o;
    midi::EventList out;
    std::vector<int> open (128, -1);
    for (std::size_t start = 0; start < sc.signal.size(); start += 64)
    {
        const int n = static_cast<int> (std::min<std::size_t> (64, sc.signal.size() - start));
        out.clear();
        g.process (sc.signal.data() + start, n, out);
        for (const auto& e : out)
        {
            const double t =
                static_cast<double> (start + static_cast<std::size_t> (e.offset)) / sc.fs;
            if (e.isNoteOn())
            {
                open[static_cast<std::size_t> (e.note())] = static_cast<int> (o.run.notes.size());
                o.run.notes.push_back ({ e.note(), e.velocity(), e.channel(), t, -1.0 });
            }
            else if (e.isNoteOff() && open[static_cast<std::size_t> (e.note())] >= 0)
            {
                o.run.notes[static_cast<std::size_t> (open[static_cast<std::size_t> (e.note())])]
                    .off = t;
                open[static_cast<std::size_t> (e.note())] = -1;
            }
        }
        o.run.maxSounding = std::max (o.run.maxSounding, g.soundingCount());
    }
    o.heard = g.heardNotes();
    return o;
}
} // namespace

TEST_CASE ("Guitar to MIDI plays a soft note that follows a damped louder one", "[midi][heard]")
{
    // 96 notes in each timing: half of them 6 or 9 dB under the note before, 25 ms after its
    // mute, with no click. Before the decoder these had no pick and were not played.
    for (const auto timing : { midi::Timing::fastest, midi::Timing::steady })
    {
        int notes = 0, right = 0, extra = 0, wrong = 0;
        std::uint32_t heard = 0;
        for (const auto pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
            for (float softDb : { -6.0f, -9.0f })
            {
                if (!played (pickup == Pickup::bridge ? 0 : (pickup == Pickup::neck ? 1 : 2), 0, 2))
                    continue;
                const auto sc = softAfterLoud (softDb < -8.0f ? 2u : 1u, pickup, softDb, kFs);
                const auto o = play (sc, timing);
                const auto sco = score (o.run, sc, 0, 0.12, false);
                notes += sco.expected;
                right += sco.correct;
                extra += sco.extra;
                wrong += sco.wrongPitch;
                heard += o.heard;
                CHECK (o.run.maxSounding <= 1);
            }
        INFO ((timing == midi::Timing::steady ? "Steady: " : "Fastest: ")
              << right << " of " << notes << " right, " << wrong << " wrong, " << extra
              << " extra, " << heard << " started without a pick");
        CHECK (right * 100 >= notes * 97);
        CHECK (wrong == 0);
        CHECK (extra * 100 <= notes * 2);
        // The soft notes are the ones started without a pick: without that path they are missed.
        CHECK (heard * 5 >= static_cast<std::uint32_t> (notes));
    }
}
