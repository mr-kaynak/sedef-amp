// SPDX-License-Identifier: AGPL-3.0-or-later
// Held notes, bends, slides and vibrato (docs/design/guitar-to-midi.md, "Held notes, bends and
// slides"): a held note sends nothing but its note-on and note-off, Notes + Bend follows what the
// player does to the string and nothing else, and a slide is never a burst of short notes.
#include "MidiTestSignals.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace sedef;
using namespace sedef::test::midi;
using midi::GuitarToMidi;

namespace
{
constexpr double kFs = 48000.0;

midi::Settings settings (midi::Mode mode, int range = 2)
{
    midi::Settings s;
    s.octave = 0;
    s.mode = mode;
    s.bendRange = range;
    return s;
}

int bendCount (const Run& run)
{
    return static_cast<int> (std::count_if (run.events.begin(), run.events.end(),
                                            [] (const midi::Event& e) { return e.isPitchBend(); }));
}

/// Bends that are not the centre a note starts from: what the receiver hears as a detune.
int offCentreBends (const Run& run)
{
    return static_cast<int> (
        std::count_if (run.events.begin(), run.events.end(), [] (const midi::Event& e) {
            return e.isPitchBend() && e.bendValue() != midi::kBendCentre;
        }));
}

/// Every note-on comes right after a centred bend, at the same sample.
bool everyNoteStartsCentred (const Run& run)
{
    for (std::size_t k = 0; k < run.events.size(); ++k)
        if (run.events[k].isNoteOn())
        {
            // (A note-off of the note before may sit between the two.)
            bool centred = false;
            for (std::size_t j = k; j-- > 0 && run.eventTimes[j] == run.eventTimes[k];)
                if (run.events[j].isPitchBend())
                {
                    centred = run.events[j].bendValue() == midi::kBendCentre;
                    break;
                }
            if (!centred)
                return false;
        }
    return true;
}
} // namespace

TEST_CASE ("Guitar to MIDI: a held note sends its note-on and its note-off, nothing else",
           "[midi][bend][held]")
{
    // Strings left alone for 3 s: the pick's own glide (8 cents, gone in 150 ms), a pitch that
    // sinks 5 cents a second as the note decays, a string tuned 12 cents flat or 18 sharp, on
    // three pickups. Notes mode: two events. Notes + Bend: those two and the centred bend the
    // note starts from; never a bend off centre.
    for (double hz : { kE2, kA2, 195.998, kE4 })
        for (double drift : { 0.0, -5.0 })
            for (double tuning : { -12.0, 18.0 })
                for (auto pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
                {
                    INFO (hz << " Hz, drift " << drift << ", tuned " << tuning);
                    const auto m = heldString (hz, 3.0, drift, kFs, pickup, tuning);
                    const auto notes = convert (m.sc.signal, kFs, settings (midi::Mode::notes));
                    CHECK (notes.events.size() == 2);
                    REQUIRE (notes.notes.size() == 1);
                    CHECK (notes.notes[0].off - notes.notes[0].on > 2.8);
                    const auto bend =
                        convert (m.sc.signal, kFs, settings (midi::Mode::notesAndBend));
                    CHECK (bend.notes.size() == 1);
                    CHECK (bend.events.size() == 3);
                    CHECK (offCentreBends (bend) == 0);
                    CHECK (everyNoteStartsCentred (bend));
                }
    // Vibrato narrower than the dead band is the note, in both modes.
    for (double hz : { kE2, kA2, 195.998 })
        for (double cents : { 10.0, 15.0 })
            for (double rate : { 4.0, 6.5 })
            {
                INFO (hz << " Hz, vibrato +-" << cents << " at " << rate);
                const auto m = vibratoString (hz, cents, rate, kFs);
                CHECK (convert (m.sc.signal, kFs, settings (midi::Mode::notes)).events.size() == 2);
                CHECK (offCentreBends (
                           convert (m.sc.signal, kFs, settings (midi::Mode::notesAndBend))) == 0);
            }
}

TEST_CASE ("Guitar to MIDI: Notes + Bend follows a bend and comes back to the centre",
           "[midi][bend]")
{
    // A string bent a semitone in 80 to 600 ms or a whole step in 150 to 600, held, and let down
    // again: one
    // note; the bend follows within 45 cents (allowing 60 ms), 12 cents rms; it moves at most
    // its slew per message; it holds the bend to 3 cents, and after the release it is exactly
    // centred.
    const double slew = static_cast<double> (GuitarToMidi::kBendSlewCentsPerSecond) *
                        GuitarToMidi::kBendIntervalSeconds;
    double worst = 0.0, worstRms = 0.0;
    int most = 0;
    for (double hz : { kE2, kA2, 195.998 })
        for (double cents : { 100.0, 200.0 })
            for (double seconds : { 0.08, 0.15, 0.3, 0.6 })
                for (bool release : { false, true })
                {
                    // (Faster than a whole step in 120 ms, the pitch moves like a fret taken at
                    // once, and comes out as notes.)
                    if (cents / seconds > 1700.0)
                        continue;
                    INFO (hz << " Hz, " << cents << " cents in " << seconds << " s"
                             << (release ? ", released" : ""));
                    const auto m = bentString (hz, cents, seconds, release, kFs);
                    const auto run =
                        convert (m.sc.signal, kFs, settings (midi::Mode::notesAndBend));
                    const auto f = following (run, m, 2, m.pickAt + 0.1, m.muteAt - 0.01);
                    CHECK (f.notes == 1);
                    CHECK (f.silent < 0.002);
                    CHECK (f.worst <= 45.0);
                    CHECK (f.rms <= 12.0);
                    CHECK (f.largestStep <= slew + 0.5);
                    CHECK (std::abs (f.last - (release ? 0.0 : cents)) <= (release ? 0.0 : 3.0));
                    CHECK (everyNoteStartsCentred (run));
                    worst = std::max (worst, f.worst);
                    worstRms = std::max (worstRms, f.rms);
                    most = std::max (most, f.bends);
                }
    std::printf ("bends: worst %.1f cents from the string (60 ms allowed), worst rms %.1f, at most "
                 "%d messages\n",
                 worst, worstRms, most);
    // A quarter-tone bend: followed, and held within 12 cents (the note's centre creeps towards
    // a slow one for a moment before it counts as a bend).
    for (double hz : { kE2, kA2, 195.998 })
        for (double seconds : { 0.08, 0.3, 0.6 })
        {
            const auto m = bentString (hz, 50.0, seconds, false, kFs);
            const auto run = convert (m.sc.signal, kFs, settings (midi::Mode::notesAndBend));
            const auto f = following (run, m, 2, m.pickAt + 0.1, m.muteAt - 0.01);
            CHECK (f.notes == 1);
            CHECK (std::abs (f.last - 50.0) <= 12.0);
        }
    // Wide vibrato is followed.
    for (double hz : { kE2, kA2, 195.998 })
        for (double rate : { 4.0, 6.5 })
        {
            INFO (hz << " Hz, vibrato +-50 at " << rate);
            const auto m = vibratoString (hz, 50.0, rate, kFs);
            const auto run = convert (m.sc.signal, kFs, settings (midi::Mode::notesAndBend));
            const auto f = following (run, m, 2, m.pickAt + 0.45, m.muteAt - 0.01);
            CHECK (f.notes == 1);
            CHECK (f.rms <= 6.0);
            CHECK (f.largestStep <= slew + 0.5);
        }
    // In Notes mode the same bends give the notes they pass, and no bend.
    {
        const auto m = bentString (kA2, 200.0, 0.3, true, kFs);
        const auto run = convert (m.sc.signal, kFs, settings (midi::Mode::notes));
        CHECK (bendCount (run) == 0);
        REQUIRE (run.notes.size() == 5);
        const int expected[] = { 45, 46, 47, 46, 45 };
        for (std::size_t k = 0; k < 5; ++k)
            CHECK (run.notes[k].pitch == expected[k]);
        CHECK (run.maxSounding <= 1);
    }
}

TEST_CASE ("Guitar to MIDI: a slide gives its frets, never a burst of notes", "[midi][bend][slide]")
{
    // A slide over 2, 5 or 12 frets, a fret every 15 to 120 ms. Both modes: the first fret is a
    // note as soon as it is reached, the frets after it wait until one is held 30 ms, so a fast
    // slide is its first fret and the fret it stops on (and one note about every 80 ms on the
    // way), a slow one every fret; the last note is the fret the slide stopped at; no gap, one
    // note at a time. Notes + Bend: each of those notes starts from a centred bend and sends no
    // other (a fret taken at once is a note, not a bend: see bendReaches()).
    for (const auto mode : { midi::Mode::notes, midi::Mode::notesAndBend })
        for (double hz : { kE2, kA2, 195.998 })
            for (double every : { 0.015, 0.03, 0.06, 0.12 })
                for (int frets : { 2, -2, 5, 12 })
                {
                    INFO (hz << " Hz, " << frets << " frets, one every " << every << " s"
                             << (mode == midi::Mode::notes ? "" : ", Notes + Bend"));
                    const auto m = slidString (frets > 0 ? hz : 2.0 * hz, frets, every, kFs);
                    const auto run = convert (m.sc.signal, kFs, settings (mode));
                    const auto f = following (run, m, 2, m.pickAt + 0.1, m.muteAt - 0.01);
                    REQUIRE (!run.notes.empty());
                    CHECK (run.notes.back().pitch == m.sc.notes[0].pitch + frets);
                    CHECK (run.maxSounding <= 1);
                    CHECK (f.silent < 0.002);
                    CHECK (offCentreBends (run) == 0);
                    if (mode == midi::Mode::notesAndBend)
                        CHECK (everyNoteStartsCentred (run));
                    else
                        CHECK (bendCount (run) == 0);
                    for (std::size_t k = 2; k < run.notes.size(); ++k)
                        CHECK (1000.0 * (run.notes[k].on - run.notes[k - 1].on) >= 25.0);
                    if (every >= 0.06)
                        CHECK (static_cast<int> (run.notes.size()) == std::abs (frets) + 1);
                    else if (frets >= 5)
                        CHECK (static_cast<int> (run.notes.size()) <= 2 + frets / 2);
                }
}

TEST_CASE ("Guitar to MIDI: Notes + Bend plays lines as Notes does, with the wheel at rest",
           "[midi][bend][accuracy]")
{
    // Bass lines and the fretting hand moving before the pick, in Notes + Bend: the same bars as
    // in Notes, every note starts from a centred bend, and the bend leaves the centre for fewer
    // than 3 notes in 100.
    Score lines, fretted;
    int notes = 0, bent = 0;
    const auto count = [&] (const Run& run) {
        CHECK (everyNoteStartsCentred (run));
        CHECK (run.maxSounding <= 1);
        notes += static_cast<int> (run.notes.size());
        for (std::size_t k = 0; k < run.events.size(); ++k)
            if (run.events[k].isNoteOn())
                for (std::size_t j = k + 1; j < run.events.size() && !run.events[j].isNoteOn(); ++j)
                    if (run.events[j].isPitchBend() &&
                        run.events[j].bendValue() != midi::kBendCentre)
                    {
                        ++bent;
                        break;
                    }
    };
    const auto add = [] (Score& t, const Score& s) {
        t.expected += s.expected;
        t.correct += s.correct;
        t.extra += s.extra;
        t.octaveErrors += s.octaveErrors;
    };
    for (auto pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
    {
        for (std::uint32_t seed = 1; seed <= 3; ++seed)
        {
            const auto sc = bassLine (seed, pickup, 120.0 + 4.0 * seed, kFs, 48, false);
            const auto run = convert (sc.signal, kFs, settings (midi::Mode::notesAndBend));
            add (lines, score (run, sc, 0));
            count (run);
        }
        for (std::uint32_t seed = 1; seed <= 4; ++seed)
        {
            const auto sc = fretThenPick (seed, pickup, kFs);
            const auto run = convert (sc.signal, kFs, settings (midi::Mode::notesAndBend));
            add (fretted, score (run, sc, 0, 0.08, false));
            count (run);
        }
    }
    std::printf ("Notes + Bend: bass lines %.2f %% right, %d extra; fretted first %.2f %% right, "
                 "%d extra; %d of %d notes bent\n",
                 100.0 * lines.correctShare(), lines.extra, 100.0 * fretted.correctShare(),
                 fretted.extra, bent, notes);
    CHECK (lines.correctShare() >= 0.99);
    CHECK (lines.extraPer100() <= 1.0);
    CHECK (lines.octaveErrors == 0);
    CHECK (fretted.correctShare() >= 0.98);
    CHECK (fretted.extraPer100() <= 2.0);
    CHECK (100.0 * bent / std::max (1, notes) <= 3.0);
}

TEST_CASE ("Guitar to MIDI: changing the mode or the bend range leaves the wheel centred",
           "[midi][bend][stuck]")
{
    // In the middle of a bend, the mode goes back to Notes (or the range changes): the bent
    // note ends, the wheel is centred, and nothing is left sounding.
    for (int change = 0; change < 2; ++change)
    {
        const auto m = bentString (kA2, 200.0, 0.15, false, kFs);
        GuitarToMidi g;
        g.setSettings (settings (midi::Mode::notesAndBend));
        g.prepare (kFs);
        midi::EventList out;
        int lastBend = midi::kBendCentre, on = 0;
        const auto switchAt = static_cast<std::size_t> ((m.pickAt + 0.6) * kFs);
        bool bentBefore = false;
        for (std::size_t start = 0; start < m.sc.signal.size(); start += 64)
        {
            if (start <= switchAt && switchAt < start + 64)
            {
                bentBefore = lastBend != midi::kBendCentre;
                g.setSettings (change == 0 ? settings (midi::Mode::notes)
                                           : settings (midi::Mode::notesAndBend, 12));
            }
            const int n = static_cast<int> (std::min<std::size_t> (64, m.sc.signal.size() - start));
            out.clear();
            g.process (m.sc.signal.data() + start, n, out);
            for (const auto& e : out)
            {
                if (e.isPitchBend())
                    lastBend = e.bendValue();
                on += e.isNoteOn() ? 1 : (e.isNoteOff() ? -1 : 0);
            }
            if (start > switchAt + 64 && start < switchAt + 4800)
                CHECK (lastBend == midi::kBendCentre);
        }
        CHECK (bentBefore);
        CHECK (on == 0);
        CHECK (g.soundingCount() == 0);
    }
}
