// SPDX-License-Identifier: AGPL-3.0-or-later
// The early reading (docs/design/guitar-to-midi.md, "Sooner than two periods"): in Fastest, a
// note is sent on the signal since its pick as soon as that alone names it, before the tracker
// has had its two periods. It must be sooner, and it must never be a wrong note.
#include "MidiTestSignals.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace sedef;
using namespace sedef::test::midi;
using midi::GuitarToMidi;
using midi::LowNote;

namespace
{
constexpr double kFs = 48000.0;

midi::Settings plain (LowNote low = LowNote::automatic)
{
    midi::Settings s;
    s.octave = 0;
    s.lowNote = low;
    return s;
}

/// A note-on sent on the early reading: when, which note, and the played note it answers.
struct EarlyStart
{
    double at = 0.0;
    int sent = -1, played = -1;
    bool right = false;
};

struct EarlyRun
{
    Run run;
    std::vector<EarlyStart> early;
};

/// convert(), also telling which note-ons went out on the early reading and whether each is the
/// note played (the latest one started, no more than 80 ms before).
EarlyRun convertEarly (const Scenario& sc, const midi::Settings& s, LowNote learned = LowNote::e2)
{
    GuitarToMidi g;
    g.setSettings (s);
    g.prepare (kFs);
    g.setLearnedLowNote (learned);
    EarlyRun out;
    midi::EventList events;
    std::vector<int> open (128, -1);
    std::uint32_t count = 0;
    const std::size_t block = 32;
    for (std::size_t start = 0; start < sc.signal.size(); start += block)
    {
        const int n = static_cast<int> (std::min (block, sc.signal.size() - start));
        events.clear();
        g.process (sc.signal.data() + start, n, events);
        std::vector<std::size_t> ons;
        for (const auto& e : events)
        {
            const double t =
                static_cast<double> (start + static_cast<std::size_t> (e.offset)) / kFs;
            out.run.events.push_back (e);
            out.run.eventTimes.push_back (t);
            if (e.isNoteOn())
            {
                open[static_cast<std::size_t> (e.note())] = static_cast<int> (out.run.notes.size());
                ons.push_back (out.run.notes.size());
                out.run.notes.push_back ({ e.note(), e.velocity(), e.channel(), t, -1.0 });
            }
            else if (e.isNoteOff())
            {
                auto& o = open[static_cast<std::size_t> (e.note())];
                if (o >= 0)
                    out.run.notes[static_cast<std::size_t> (o)].off = t;
                o = -1;
            }
        }
        out.run.maxSounding = std::max (out.run.maxSounding, g.soundingCount());
        // The block is under a millisecond: the note-ons of a block whose count moved are the
        // early ones (the last of them, if there are more note-ons than the count moved by).
        const auto moved = static_cast<std::size_t> (g.earlyNotes() - count);
        count = g.earlyNotes();
        for (std::size_t k = ons.size() - std::min (moved, ons.size()); k < ons.size(); ++k)
        {
            const auto& note = out.run.notes[ons[k]];
            EarlyStart e { note.on, note.pitch, -1, false };
            const RefNote* ref = nullptr;
            for (const auto& r : sc.notes)
                if (r.start <= note.on + 0.002 && (ref == nullptr || r.start > ref->start))
                    ref = &r;
            if (ref != nullptr)
            {
                e.played = ref->pitch;
                e.right = ref->pitch == note.pitch && note.on - ref->start < 0.08;
            }
            out.early.push_back (e);
        }
    }
    return out;
}

} // namespace

TEST_CASE ("Guitar to MIDI names a clean note from one period of it", "[midi][early][latency]")
{
    // Pick to note-on, the second of two picks, with a pick and with the fingers, on a bridge
    // and a neck pickup. A note with nothing an octave under it in the range (up to C#3 on a
    // six-string) goes out once its attack has come round and 0.6 of a period has repeated:
    // 1.6 periods and the attack's 2.5 ms, where the tracker needs two periods and the MIDI
    // layer took 3 ms more. A note above needs two of its own periods (5 ms at least) that
    // nothing else shares, where it waited one period of the low E. 2 ms of slack on each.
    std::printf ("note   Hz      two periods   pick to note-on, median (most), ms   early\n");
    for (int pitch : { 40, 43, 45, 47, 50, 55, 59, 64, 69, 76 })
    {
        const double period = 1000.0 / hzOf (pitch);
        const bool lowest = 2.0 * period > 1000.0 / 70.0;
        const double bound =
            GuitarToMidi::kAttackSkipSeconds * 1000.0 + 2.0 +
            (lowest ? (1.0 + GuitarToMidi::kEarlyShare) * period
                    : period + std::max (period, 1000.0 * GuitarToMidi::kEarlyAloneSeconds));
        std::vector<double> ms;
        int early = 0, runs = 0;
        for (auto pickup : { Pickup::bridge, Pickup::neck })
            for (bool fingers : { false, true })
                for (std::uint32_t seed = 1; seed <= 3; ++seed)
                {
                    if (!played (static_cast<int> (seed), 1, 3))
                        continue;
                    INFO ("note " << pitch << ", seed " << seed << (fingers ? ", fingers" : ""));
                    const auto sc = pickedTwice (pitch, pickup, seed, kFs, fingers);
                    const auto r = convertEarly (sc, plain());
                    ++runs;
                    REQUIRE (r.run.notes.size() == 2);
                    CHECK (r.run.notes[1].pitch == pitch);
                    ms.push_back (1000.0 * (r.run.notes[1].on - 1.1));
                    for (const auto& e : r.early)
                    {
                        CHECK (e.right);
                        early += e.at > 1.0 ? 1 : 0;
                    }
                }
        const double median = percentile (ms, 0.5), most = percentile (ms, 1.0);
        std::printf ("%3d  %6.1f     %5.1f            %5.1f (%5.1f)                     %d of %d\n",
                     pitch, hzOf (pitch), 2.0 * period, median, most, early, runs);
        INFO ("note " << pitch);
        CHECK (median <= bound);
        // Sooner than the tracker's two periods wherever those are the longer wait.
        if (lowest)
            CHECK (median < 2.0 * period);
        // And never later than before: two periods and the MIDI layer's 8 ms, or the old 16 ms.
        CHECK (most <= std::max (2.0 * period + 8.0, 18.5));
    }
}

TEST_CASE ("Guitar to MIDI's early reading is never a wrong note", "[midi][early][accuracy]")
{
    // Every note-on sent on the early reading, over the bass lines, the sloppy lines, the
    // fretted-first runs and the notes from silence: each is the note played. More than half
    // of all the notes go out that way.
    int notes = 0, early = 0, wrong = 0;
    const auto over = [&] (const Scenario& sc) {
        const auto r = convertEarly (sc, plain());
        notes += static_cast<int> (sc.notes.size());
        for (const auto& e : r.early)
        {
            ++early;
            if (!e.right)
            {
                ++wrong;
                std::printf ("  %s: at %.3f sent %d, played %d\n", sc.name.c_str(), e.at, e.sent,
                             e.played);
            }
        }
    };
    for (auto pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
    {
        for (std::uint32_t seed = 1; seed <= 6; ++seed)
        {
            if (!played (static_cast<int> (seed), 1, 6))
                continue;
            over (bassLine (seed, pickup, 120.0 + 4.0 * seed, kFs, 48, false));
            over (sloppyLine (seed, pickup, 110.0 + 5.0 * seed, kFs, 48));
        }
        for (std::uint32_t seed = 1; seed <= 4; ++seed)
            if (played (static_cast<int> (seed), 1, 4))
                over (fretThenPick (seed, pickup, kFs));
    }
    for (int tuning = 0; tuning < 3; ++tuning)
        for (auto pickup : { Pickup::bridge, Pickup::neck })
            for (std::uint32_t seed = 1; seed <= 3; ++seed)
                if (played (static_cast<int> (seed), 1, 3))
                    over (fromSilence (seed, tuning, pickup, kFs));
    std::printf ("early reading: %d of %d notes sent on it, %d wrong\n", early, notes, wrong);
    CHECK (wrong == 0);
    CHECK (early >= notes / 2);
    // Steady never uses it: its notes are those it always sent.
    auto steady = plain();
    steady.timing = midi::Timing::steady;
    CHECK (
        convertEarly (bassLine (1, Pickup::bridge, 124.0, kFs, 48, false), steady).early.empty());
    CHECK (convertEarly (sloppyLine (2, Pickup::neck, 120.0, kFs, 48), steady).early.empty());
}

TEST_CASE ("Guitar to MIDI's early reading on the low strings of seven and eight strings",
           "[midi][early][accuracy][range][long]")
{
    // The same on strings a fourth and a minor seventh lower, in their ranges: a low string's
    // long period gives a few milliseconds of it more chances to look like themselves at
    // another period, so the stretch that must repeat grows with the period.
    for (int shift : { -5, -10 })
    {
        const auto low = shift == -5 ? LowNote::b1 : LowNote::fSharp1;
        int notes = 0, early = 0, wrong = 0;
        const auto over = [&] (const Scenario& sc) {
            const auto r = convertEarly (sc, plain (low));
            notes += static_cast<int> (sc.notes.size());
            for (const auto& e : r.early)
            {
                ++early;
                wrong += e.right ? 0 : 1;
            }
        };
        for (auto pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
        {
            for (std::uint32_t seed = 1; seed <= 6; ++seed)
            {
                over (lowered (shift, [&] {
                    return bassLine (seed, pickup, 120.0 + 4.0 * seed, kFs, 48, false);
                }));
                over (lowered (
                    shift, [&] { return sloppyLine (seed, pickup, 110.0 + 5.0 * seed, kFs, 48); }));
            }
            for (std::uint32_t seed = 1; seed <= 4; ++seed)
                over (lowered (shift, [&] { return fretThenPick (seed, pickup, kFs); }));
        }
        std::printf ("early reading, strings %d semitones lower: %d of %d notes sent on it, %d "
                     "wrong\n",
                     -shift, early, notes, wrong);
        CHECK (wrong == 0);
        CHECK (early >= notes / 2);
    }
}

TEST_CASE ("Guitar to MIDI does not take a ringing note for the note picked over it",
           "[midi][early][ringing]")
{
    // The note before left ringing 6 dB under the new one, an octave or a twelfth under it or
    // over it, a fourth under, a fifth over: the input then repeats at the old note's period
    // too, or at a period neither note has. Two notes, each the one played, nothing else.
    Score total;
    for (double hz : { 110.0, 164.814, 220.0 })
        for (double cents : { -1200.0, 1200.0, -1900.0, 1900.0, -500.0, 700.0 })
        {
            const double old = hz * std::exp2 (cents / 1200.0);
            if (old < 82.0 || old > 700.0)
                continue;
            INFO (hz << " Hz over a note " << cents << " cents from it");
            const auto sc = overRinging (hz, cents, kFs);
            const auto r = convertEarly (sc, plain());
            const auto st = score (r.run, sc, 0);
            CHECK (st.correct == st.expected);
            CHECK (st.wrongPitch == 0);
            CHECK (st.extra == 0);
            for (const auto& e : r.early)
                CHECK (e.right);
            total.expected += st.expected;
            total.correct += st.correct;
        }
    CHECK (total.expected >= 24);
}

TEST_CASE ("Guitar to MIDI plays the same note picked again once each time, and sooner",
           "[midi][early][repick]")
{
    // The staple of a bass line: one note, eighths, each pick taking the string over at about
    // the level it rang at. Such a pick does not drown what rang, so a lower note could still
    // have been on its way, and the note used to wait for the lowest one: 33 ms, whatever the
    // note. Read against the note the string already had, it goes out once the stretch from
    // 5 ms after the pick has come round and 0.6 of a period has repeated. One note per pick,
    // none extra.
    std::printf ("the same note again: note, median and most pick to note-on (ms)\n");
    for (int pitch : { 40, 45, 50, 55 })
    {
        std::vector<double> ms;
        for (auto pickup : { Pickup::bridge, Pickup::neck })
            for (std::uint32_t seed = 1; seed <= 3; ++seed)
            {
                INFO ("note " << pitch << ", seed " << seed);
                const auto sc = repeated (pitch, pickup, seed, 6, 0.25, kFs);
                const auto r = convertEarly (sc, plain());
                const auto st = score (r.run, sc, 0);
                CHECK (st.correct == st.expected);
                CHECK (st.extra == 0);
                CHECK (r.run.maxSounding <= 1);
                for (const auto& e : r.early)
                    CHECK (e.right);
                for (std::size_t k = 1; k < sc.notes.size(); ++k)
                    for (const auto& n : r.run.notes)
                        if (n.on >= sc.notes[k].start - 0.002 && n.on <= sc.notes[k].start + 0.08)
                        {
                            ms.push_back (1000.0 * (n.on - sc.notes[k].start));
                            break;
                        }
            }
        const double period = 1000.0 / hzOf (pitch);
        const double bound = 1000.0 * GuitarToMidi::kEarlyHeldSkipSeconds + period +
                             std::max (1000.0 * GuitarToMidi::kEarlyWindowSeconds,
                                       GuitarToMidi::kEarlyShare * period) +
                             2.0;
        std::printf ("  %d: %.1f (%.1f)\n", pitch, percentile (ms, 0.5), percentile (ms, 1.0));
        REQUIRE (ms.size() == 30);
        CHECK (percentile (ms, 0.5) <= bound);
        CHECK (percentile (ms, 1.0) <= 34.0);
    }
}

TEST_CASE ("Guitar to MIDI waits out a fret that lands just after the pick", "[midi][early][fret]")
{
    // The string is picked while it still sounds the note before, and the fretting finger
    // lands a few milliseconds later: until then the string really has the old note. A fret
    // that lands within 6 ms is waited out: the note played is the fret, and the old note is
    // not sent again first. One that lands 9 ms after the pick is a hammer-on: now and then
    // the old note sounds for some 10 ms first (5 of these 48 cases; 4 before the early
    // reading).
    int late = 0;
    for (int pitch : { 45, 50, 55, 59 })
        for (double cents : { 100.0, -100.0, 200.0 })
            for (double after : { 0.004, 0.006, 0.009 })
                for (auto pickup : { Pickup::bridge, Pickup::neck })
                {
                    INFO ("note " << pitch << ", " << cents << " cents, " << 1000.0 * after
                                  << " ms after the pick");
                    const auto sc = fretAfterPick (pitch, cents, after, pickup, 3, kFs);
                    const auto r = convertEarly (sc, plain());
                    const auto st = score (r.run, sc, 0);
                    CHECK (st.correct == st.expected);
                    if (after < 0.008)
                    {
                        CHECK (st.extra == 0);
                        for (const auto& e : r.early)
                            CHECK (e.right);
                    }
                    else
                        late += st.extra;
                }
    std::printf ("a fret 9 ms after the pick: %d extra notes in 48 cases\n", late);
    CHECK (late <= 6);
}
