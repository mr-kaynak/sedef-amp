// SPDX-License-Identifier: AGPL-3.0-or-later
// Strings under a guitar's low E (docs/design/guitar-to-midi.md, "Down to F#1"): the Lowest Note
// ranges, what the automatic one learns, and what each costs the notes above.
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

midi::Settings range (LowNote low, midi::Timing timing = midi::Timing::fastest)
{
    midi::Settings s;
    s.octave = 0;
    s.lowNote = low;
    s.timing = timing;
    return s;
}

struct Totals
{
    Score score;
    int shorts = 0;
    bool oneAtATime = true;
};

void add (Totals& t, const Run& run, const Scenario& sc, bool judgeEnds, double window = 0.08)
{
    const auto s = score (run, sc, 0, window, judgeEnds);
    t.score.expected += s.expected;
    t.score.correct += s.correct;
    t.score.wrongPitch += s.wrongPitch;
    t.score.octaveErrors += s.octaveErrors;
    t.score.missed += s.missed;
    t.score.extra += s.extra;
    t.score.earlyOff += s.earlyOff;
    for (const auto& n : run.notes)
        t.shorts += n.off >= 0.0 && 1000.0 * (n.off - n.on) < 40.0 ? 1 : 0;
    t.oneAtATime = t.oneAtATime && run.maxSounding <= 1;
}

void report (const char* name, const Totals& t)
{
    std::printf ("%-34s %4d notes  %6.2f%% right  %d wrong (%d octave)  %d missed  %d extra  %d "
                 "under 40 ms\n",
                 name, t.score.expected, 100.0 * t.score.correctShare(), t.score.wrongPitch,
                 t.score.octaveErrors, t.score.missed, t.score.extra, t.shorts);
}

/// Pick to note-on of the second of two picks of `pitch`, in ms (-1: not played).
double secondPickMs (int pitch, const midi::Settings& s, Pickup pickup, std::uint32_t seed,
                     bool fingers)
{
    const auto sc = pickedTwice (pitch, pickup, seed, kFs, fingers);
    const auto run = convert (sc.signal, kFs, s);
    for (const auto& n : run.notes)
        if (n.pitch == pitch && n.on >= 1.098 && n.on <= 1.2)
            return 1000.0 * (n.on - 1.1);
    return -1.0;
}

/// The suite's lines on strings `shift` semitones lower, against the six-string bars.
void lowLines (int shift, LowNote low, const char* name)
{
    Totals bass, sloppy, fretted, silent;
    for (auto pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
    {
        for (std::uint32_t seed = 1; seed <= 6; ++seed)
        {
            const auto a = lowered (
                shift, [&] { return bassLine (seed, pickup, 120.0 + 4.0 * seed, kFs, 48, false); });
            add (bass, convert (a.signal, kFs, range (low)), a, true);
            const auto b = lowered (
                shift, [&] { return sloppyLine (seed, pickup, 110.0 + 5.0 * seed, kFs, 48); });
            add (sloppy, convert (b.signal, kFs, range (low)), b, false);
        }
        for (std::uint32_t seed = 1; seed <= 4; ++seed)
        {
            const auto c = lowered (shift, [&] { return fretThenPick (seed, pickup, kFs); });
            add (fretted, convert (c.signal, kFs, range (low)), c, false);
        }
    }
    for (int tuning = 0; tuning < 3; ++tuning)
        for (auto pickup : { Pickup::bridge, Pickup::neck })
        {
            const auto d = lowered (shift, [&] { return fromSilence (1, tuning, pickup, kFs); });
            add (silent, convert (d.signal, kFs, range (low)), d, true);
        }
    char label[64];
    std::snprintf (label, sizeof label, "%s: bass lines", name);
    report (label, bass);
    std::snprintf (label, sizeof label, "%s: sloppy lines", name);
    report (label, sloppy);
    std::snprintf (label, sizeof label, "%s: fretted first", name);
    report (label, fretted);
    std::snprintf (label, sizeof label, "%s: from silence", name);
    report (label, silent);
    CHECK (bass.score.correctShare() >= 0.99);
    CHECK (bass.score.extraPer100() <= 1.0);
    CHECK (sloppy.score.correctShare() >= 0.985);
    CHECK (sloppy.score.extraPer100() <= 2.5);
    CHECK (100.0 * sloppy.shorts / sloppy.score.expected <= 2.0);
    CHECK (fretted.score.correctShare() >= 0.98);
    CHECK (fretted.score.extraPer100() <= 2.0);
    // (One note of the 108 a fourth lower is missed: its finger's touch squeaks 9 ms before the
    // pluck, and the pick seen there is judged before the pluck's level has come.)
    CHECK (silent.score.correctShare() >= 0.98);
    CHECK (silent.score.extra == 0);
    // A low string's level swings with its long period: no note ends for that.
    CHECK (silent.score.earlyOff == 0);
    for (const auto* t : { &bass, &sloppy, &fretted, &silent })
    {
        CHECK (t->score.octaveErrors == 0);
        CHECK (t->oneAtATime);
    }
}
} // namespace

TEST_CASE ("Guitar to MIDI plays a seven-string's low strings", "[midi][range][long]")
{
    // The suite's lines a fourth lower (B1, E2 and A2 where a guitar has E2, A2 and D3), in the
    // B1 range, against the bars of the six-string sets.
    lowLines (-5, LowNote::b1, "B1 range, a fourth lower");
}

TEST_CASE ("Guitar to MIDI plays an eight-string's low strings", "[midi][range][long]")
{
    // The same a minor seventh lower (F#1, B1 and E2), in the F#1 range.
    lowLines (-10, LowNote::fSharp1, "F#1 range, a seventh lower");
}

TEST_CASE ("Guitar to MIDI's automatic range learns the low strings it hears",
           "[midi][range][long]")
{
    // Automatic, starting as a six-string each time: the eight-string lines, the first low note
    // included, against the same bars but one more extra note per 100 on the bass lines (the
    // notes before the range has learned); the range is F#1 at the end of every line.
    Totals bass, sloppy;
    for (auto pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
        for (std::uint32_t seed = 1; seed <= 6; ++seed)
        {
            const auto a = lowered (
                -10, [&] { return bassLine (seed, pickup, 120.0 + 4.0 * seed, kFs, 48, false); });
            const auto ra = convert (a.signal, kFs, range (LowNote::automatic));
            add (bass, ra, a, true);
            CHECK (ra.range == LowNote::fSharp1);
            const auto b = lowered (
                -10, [&] { return sloppyLine (seed, pickup, 110.0 + 5.0 * seed, kFs, 48); });
            const auto rb = convert (b.signal, kFs, range (LowNote::automatic));
            add (sloppy, rb, b, false);
            CHECK (rb.range == LowNote::fSharp1);
        }
    report ("Automatic, a seventh lower: bass", bass);
    report ("Automatic, a seventh lower: sloppy", sloppy);
    CHECK (bass.score.correctShare() >= 0.99);
    CHECK (bass.score.extraPer100() <= 2.0);
    CHECK (bass.score.octaveErrors == 0);
    CHECK (sloppy.score.correctShare() >= 0.98);
    CHECK (sloppy.score.extraPer100() <= 2.5);
    CHECK (sloppy.score.octaveErrors == 0);
}

TEST_CASE ("Guitar to MIDI's automatic range is a six-string's until it hears a lower string",
           "[midi][range]")
{
    // On a six-string guitar Automatic sends the notes the E2 range sends, 99 in 100 within 3 ms
    // of where that range puts them (it looks for longer periods too, and one may hold a
    // reading back for a moment), and never widens: clean lines, sloppy lines, the fretting hand
    // first, and strummed chords (two strings ringing together repeat at a long period too).
    double worst = 0.0;
    int notes = 0, moved = 0;
    const int length = kThinned ? 16 : 48; // shorter lines under a sanitizer
    for (auto pickup : { Pickup::bridge, Pickup::piezo })
        for (std::uint32_t seed = 1; seed <= 2; ++seed)
        {
            const Scenario sets[] = { bassLine (seed, pickup, 124.0, kFs, length, false),
                                      sloppyLine (seed, pickup, 115.0, kFs, length),
                                      fretThenPick (seed, pickup, kFs) };
            for (const auto& sc : sets)
            {
                INFO (sc.name << " " << seed);
                const auto a = convert (sc.signal, kFs, range (LowNote::automatic));
                const auto e = convert (sc.signal, kFs, range (LowNote::e2));
                CHECK (a.range == LowNote::e2);
                REQUIRE (a.notes.size() == e.notes.size());
                for (std::size_t k = 0; k < a.notes.size(); ++k)
                {
                    CHECK (a.notes[k].pitch == e.notes[k].pitch);
                    const double apart = 1000.0 * std::abs (a.notes[k].on - e.notes[k].on);
                    CHECK (apart <= 40.0);
                    moved += apart > 3.0 ? 1 : 0;
                    ++notes;
                    worst = std::max (worst, apart);
                }
            }
            const auto chords = strummedChords (seed, pickup, kFs);
            CHECK (convert (chords.signal, kFs, range (LowNote::automatic)).range == LowNote::e2);
        }
    std::printf ("Automatic against the E2 range on a six-string: %d of %d note-ons more than "
                 "3 ms apart, at most %.1f ms\n",
                 moved, notes, worst);
    CHECK (100 * moved <= notes);
}

TEST_CASE ("Guitar to MIDI hears the notes of its range", "[midi][range]")
{
    // A1 (55 Hz) and F#1 (46 Hz), each picked twice. The E2 range plays neither as itself; B1 plays
    // A1; F#1 plays both; Automatic plays both from the first pick and has learned the string by
    // the second. A learned range survives reset() and prepare(); forgetLowNote() drops it.
    const struct
    {
        int pitch;
        LowNote learns;
    } notes[] = { { 33, LowNote::b1 }, { 30, LowNote::fSharp1 } };
    for (const auto& n : notes)
        for (auto pickup : { Pickup::bridge, Pickup::neck })
            for (bool fingers : { false, true })
            {
                INFO ("note " << n.pitch << (fingers ? ", fingers" : ", pick"));
                const auto sc = pickedTwice (n.pitch, pickup, 2, kFs, fingers);
                // (What the E2 range makes of a string under it is one of its partials.)
                for (const auto& sent : convert (sc.signal, kFs, range (LowNote::e2)).notes)
                    CHECK (sent.pitch > 35);
                const auto b1 = convert (sc.signal, kFs, range (LowNote::b1));
                CHECK (b1.notes.size() == (n.pitch == 33 ? 2u : 0u));
                for (const auto low : { LowNote::fSharp1, LowNote::automatic })
                {
                    const auto run = convert (sc.signal, kFs, range (low));
                    REQUIRE (run.notes.size() == 2);
                    CHECK (run.notes[0].pitch == n.pitch);
                    CHECK (run.notes[1].pitch == n.pitch);
                    // Each sounds until it is damped, not cut by its own level's swing.
                    CHECK (run.notes[0].off - run.notes[0].on > 0.35);
                    CHECK (run.notes[1].off - run.notes[1].on > 0.5);
                    if (low == LowNote::automatic)
                        CHECK (run.range == n.learns);
                }
            }
    GuitarToMidi g;
    g.setSettings (range (LowNote::automatic));
    g.prepare (kFs);
    CHECK (g.lowNoteInEffect() == LowNote::e2);
    const auto sc = pickedTwice (30, Pickup::bridge, 1, kFs);
    midi::EventList out;
    for (std::size_t start = 0; start + 64 <= sc.signal.size(); start += 64)
    {
        out.clear();
        g.process (sc.signal.data() + start, 64, out);
    }
    CHECK (g.lowNoteInEffect() == LowNote::fSharp1);
    g.reset();
    g.prepare (44100.0);
    CHECK (g.lowNoteInEffect() == LowNote::fSharp1);
    g.setSettings (range (LowNote::b1));
    out.clear();
    g.process (sc.signal.data(), 64, out);
    CHECK (g.lowNoteInEffect() == LowNote::b1);
    g.setSettings (range (LowNote::automatic));
    out.clear();
    g.process (sc.signal.data(), 64, out);
    CHECK (g.lowNoteInEffect() == LowNote::fSharp1);
    g.forgetLowNote();
    CHECK (g.lowNoteInEffect() == LowNote::e2);
}

TEST_CASE ("Guitar to MIDI's low ranges: every note within two of its periods plus 8 ms",
           "[midi][range][latency]")
{
    // From the pick to the note-on, for every note from E1 to E2 in the F#1 range and from G#1
    // in the B1 range, with a pick and with the fingers, on a bridge and a neck pickup: two
    // periods (what telling a pitch takes) plus 8 ms. And what the low ranges cost the notes
    // above: at most 6 ms over the E2 range, up to E4.
    std::printf ("note   two periods   B1 range   F#1 range   (median ms)\n");
    for (int pitch = 28; pitch <= 40; ++pitch)
    {
        if (!played (pitch, 28, 40))
            continue;
        const double two = 2000.0 / hzOf (pitch);
        double medians[2] = { -1.0, -1.0 };
        int k = 0;
        for (const auto low : { LowNote::b1, LowNote::fSharp1 })
        {
            std::vector<double> ms;
            if (low == LowNote::fSharp1 || pitch >= 32)
                for (auto pickup : { Pickup::bridge, Pickup::neck })
                    for (bool fingers : { false, true })
                        for (std::uint32_t seed = 1; seed <= 2; ++seed)
                        {
                            INFO ("note " << pitch << ", seed " << seed);
                            const double t =
                                secondPickMs (pitch, range (low), pickup, seed, fingers);
                            CHECK (t >= 0.0);
                            CHECK (t <= two + 8.0);
                            ms.push_back (t);
                        }
            medians[k++] = ms.empty() ? -1.0 : percentile (ms, 0.5);
        }
        std::printf ("%3d    %6.1f        %6.1f     %6.1f\n", pitch, two, medians[0], medians[1]);
    }
    double most = 0.0;
    for (int pitch = 41; pitch <= 64; ++pitch)
        for (std::uint32_t seed = 1; seed <= 2; ++seed)
        {
            if (!played (pitch, 41, 64))
                continue;
            INFO ("note " << pitch << ", seed " << seed);
            const double e2 =
                secondPickMs (pitch, range (LowNote::e2), Pickup::bridge, seed, false);
            REQUIRE (e2 >= 0.0);
            for (const auto low : { LowNote::b1, LowNote::fSharp1 })
            {
                const double t = secondPickMs (pitch, range (low), Pickup::bridge, seed, false);
                CHECK (t >= 0.0);
                CHECK (t <= e2 + 6.0);
                most = std::max (most, t - e2);
            }
        }
    std::printf ("F2 to E4 in the low ranges: at most %.1f ms later than in the E2 range\n", most);
}
