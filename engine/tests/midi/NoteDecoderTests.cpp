// SPDX-License-Identifier: AGPL-3.0-or-later
// The note decoder (docs/design/guitar-to-midi.md, "Real playing"): what it believes follows
// from its model alone, so every wait here is computed from the model, not measured.
#include "sedef/midi/NoteDecoder.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

using sedef::midi::NoteDecoder;

namespace
{
constexpr double kHop = 0.0005;
constexpr int kA3 = 57, kB3 = 59, kE2 = 40;

/// A hop's evidence for a note whose period leaves `left`, by the model.
double evidence (const NoteDecoder::Model& m, double left)
{
    return std::log ((1.0 - m.outlier) * std::exp (-left / m.left) / m.left + m.outlier);
}

/// The share of one observation a hop is, for this note.
double share (const NoteDecoder::Model& m, int note)
{
    const double period = 1.0 / (440.0 * std::exp2 ((note - 69) / 12.0));
    return kHop /
           std::clamp (m.periodShare * period, 0.001 * m.windowMs, 0.001 * m.longestWindowMs);
}

/// Hops of a period leaving `left` until a transition costing `observations` is paid.
int hopsToPay (const NoteDecoder::Model& m, int note, double observations, double left)
{
    return static_cast<int> (std::ceil (m.patience * observations * evidence (m, 0.0) /
                                        (share (m, note) * evidence (m, left))));
}

/// One hop; `attack`: an attack began this hop (and is behind every note for its span).
int feed (NoteDecoder& d, int note, float value, bool attack, float overQuietDb = 20.0f)
{
    d.beginHop();
    if (note >= 0)
        d.addPeriod (static_cast<float> (note), value);
    return d.endHop (overQuietDb, attack ? 0.0 : 1.0);
}

/// Hops until the decoder first names `note`, fed a clean period of it; -1 if not within 400.
int hopsUntil (NoteDecoder& d, int note, bool attack)
{
    for (int k = 1; k <= 400; ++k)
        if (feed (d, note, 0.99f, attack) == note)
            return k;
    return -1;
}
} // namespace

TEST_CASE ("NoteDecoder believes a note after the observations its patience asks",
           "[midi][decoder]")
{
    for (float patience : { 1.0f, 2.0f })
        for (int note : { kE2, kA3 })
        {
            NoteDecoder d;
            d.prepare (kHop, 0.0);
            d.setPatience (patience);
            const auto& m = d.model;
            // With an attack behind it: `attack` observations; without: `bare`.
            const int withAttack = hopsToPay (m, note, m.attack, 0.01);
            const int k = hopsUntil (d, note, true);
            INFO ("note " << note << " patience " << patience << ": " << k << " hops, model "
                          << withAttack);
            CHECK (k >= withAttack);
            CHECK (k <= withAttack + 1);

            d.reset();
            const int bare = hopsToPay (m, note, m.bare, 0.01);
            const int b = hopsUntil (d, note, false);
            CHECK (b >= bare);
            CHECK (b <= bare + 1);
            CHECK (b > k);
        }
    // In time: an A3 with an attack, patience 1, is one window (4 ms) and the rounding.
    NoteDecoder d;
    d.prepare (kHop, 0.0);
    const int k = hopsUntil (d, kA3, true);
    CHECK (k * kHop >= 0.004);
    CHECK (k * kHop <= 0.0055);
}

TEST_CASE ("NoteDecoder keeps its note through a stray reading", "[midi][decoder]")
{
    NoteDecoder d;
    d.prepare (kHop, 0.0);
    REQUIRE (hopsUntil (d, kA3, true) > 0);
    for (int k = 0; k < 100; ++k)
        feed (d, kA3, 0.99f, false);
    // Another note read cleanly, with no attack: a leap must be paid for by `leap` observations,
    // less what the old note loses meanwhile (it leaves everything unexplained).
    const auto& m = d.model;
    const double perHop = share (m, kB3) * evidence (m, 0.01) - share (m, kA3) * evidence (m, 1.0);
    const int needed =
        static_cast<int> (std::ceil (m.patience * m.step * evidence (m, 0.0) / perHop));
    int changed = -1;
    for (int k = 1; k <= 200 && changed < 0; ++k)
        if (feed (d, kB3, 0.99f, false) == kB3)
            changed = k;
    INFO ("changed after " << changed << " hops, model " << needed);
    CHECK (changed >= needed);
    CHECK (changed <= needed + 1);
    CHECK (changed >= 4); // a reading of a millisecond or two changes nothing
}

TEST_CASE ("NoteDecoder gives a period's evidence to the octave above that explains as much",
           "[midi][decoder]")
{
    const auto run = [] (float upper) {
        NoteDecoder d;
        d.prepare (kHop, 0.0);
        int note = NoteDecoder::kSilence;
        for (int k = 0; k < 200; ++k)
        {
            d.beginHop();
            d.addPeriod (static_cast<float> (kA3), 0.99f);
            d.addPeriod (static_cast<float> (kA3 + 12), upper);
            note = d.endHop (20.0f, k < 20 ? 0.0 : 1.0);
        }
        return note;
    };
    // The input repeats at A4's period as well as at A3's: the note is A4, and A3's period is
    // only two of its periods.
    CHECK (run (0.98f) == kA3 + 12);
    // A string's own waveform does not repeat at half its period: the note is A3.
    CHECK (run (0.2f) == kA3);
}

TEST_CASE ("NoteDecoder hears no note in a quiet input, and lets a note go", "[midi][decoder]")
{
    NoteDecoder d;
    d.prepare (kHop, 0.0);
    // 10 dB under the quietest a note can be: the penalty is more than a clean period earns.
    for (int k = 0; k < 400; ++k)
        CHECK (feed (d, kA3, 0.99f, k < 20, -10.0f) == NoteDecoder::kSilence);

    d.reset();
    REQUIRE (hopsUntil (d, kA3, true) > 0);
    for (int k = 0; k < 100; ++k)
        feed (d, kA3, 0.99f, false);
    // Nothing repeats any more: the note ends once that has cost it `end` observations.
    const auto& m = d.model;
    const int needed = static_cast<int> (
        std::ceil (m.end * evidence (m, 0.0) / (-share (m, kA3) * evidence (m, 1.0))));
    int ended = -1;
    for (int k = 1; k <= 400 && ended < 0; ++k)
        if (feed (d, -1, 0.0f, false) == NoteDecoder::kSilence)
            ended = k;
    INFO ("ended after " << ended << " hops, model " << needed);
    CHECK (ended >= needed);
    CHECK (ended <= needed + 1);
}

TEST_CASE ("NoteDecoder with a lag never shows a note too short to pay for itself",
           "[midi][decoder]")
{
    // A3 held, six hops (3 ms) of a clean B3 in an attack, A3 again. Going there costs one
    // observation and coming back (no attack) two; six hops earn B3 less than that, so the best
    // path never leaves A3. Without a lag the best state now is B3 for a moment; ten
    // milliseconds later it was never there.
    const auto run = [] (double lagSeconds, bool& sawNow) {
        NoteDecoder d;
        d.prepare (kHop, 0.02);
        d.setLag (lagSeconds);
        bool sawLagged = false;
        sawNow = false;
        const auto step = [&] (int note, bool attack) {
            if (feed (d, note, 0.99f, attack) == kB3)
                sawLagged = true;
            if (d.now() == kB3)
                sawNow = true;
        };
        for (int k = 0; k < 200; ++k)
            step (kA3, k < 20);
        for (int k = 0; k < 6; ++k)
            step (kB3, true);
        for (int k = 0; k < 200; ++k)
            step (kA3, false);
        return sawLagged;
    };
    bool now = false;
    CHECK (run (0.0, now));
    CHECK (now);
    CHECK_FALSE (run (0.01, now));
    CHECK (now);
}

TEST_CASE ("NoteDecoder's answer depends on its input alone", "[midi][decoder]")
{
    // The same hops after reset() give the same answers (no state survives it).
    NoteDecoder d;
    d.prepare (kHop, 0.01);
    d.setLag (0.005);
    std::vector<int> first, second;
    for (int pass = 0; pass < 2; ++pass)
    {
        d.reset();
        auto& out = pass == 0 ? first : second;
        for (int k = 0; k < 300; ++k)
            out.push_back (feed (d, k < 150 ? kA3 : kB3, k % 37 == 0 ? 0.3f : 0.97f,
                                 k < 20 || (k >= 150 && k < 170)));
    }
    CHECK (first == second);
    CHECK (first.back() == kB3);
}

TEST_CASE ("NoteDecoder keeps an attack behind a note for as long as the note needs to show",
           "[midi][decoder]")
{
    // An attack is behind a note for 12 ms and two of its periods: a low E2 that shows its
    // period 30 ms after its pick still starts on one observation; 50 ms after, it is a note
    // without an attack and needs two. An A4 30 ms after an attack already is.
    NoteDecoder d;
    d.prepare (kHop, 0.0);
    CHECK (d.attackSpan (kE2) > 0.036);
    CHECK (d.attackSpan (kE2) < 0.037);
    CHECK (d.attackSpan (69) < 0.017);
    const auto hops = [&] (int note, double since) {
        d.reset();
        for (int k = 1; k <= 400; ++k)
        {
            d.beginHop();
            d.addPeriod (static_cast<float> (note), 0.99f);
            // The whole wait inside or outside the span: the attack does not age here.
            if (d.endHop (20.0f, since) == note)
                return k;
        }
        return -1;
    };
    const auto& m = d.model;
    CHECK (hops (kE2, 0.030) <= hopsToPay (m, kE2, m.attack, 0.01) + 1);
    CHECK (hops (kE2, 0.050) >= hopsToPay (m, kE2, m.bare, 0.01));
    CHECK (hops (69, 0.030) >= hopsToPay (m, 69, m.bare, 0.01));
    CHECK (hops (69, -1.0) >= hopsToPay (m, 69, m.bare, 0.01)); // no attack yet
}

TEST_CASE ("NoteDecoder cuts a low note's patience to what a budget leaves", "[midi][decoder]")
{
    // Patience 2 with 38 ms to spend: a note read after two periods and 5 ms has the rest. An
    // A3 (9 ms to read, 4 ms a window) has room for all of it; a low E2 (24.3 + 5 ms to read,
    // 6 ms a window) for (38 - 29.3) / 6 = 1.45; an E1 (53.5 ms to read) for none, and keeps 1.
    NoteDecoder d;
    d.prepare (kHop, 0.0);
    d.setPatience (2.0f, 0.038);
    CHECK (d.patienceOf (kA3) == 2.0f);
    CHECK (std::abs (d.patienceOf (kE2) - 1.45f) < 0.02f);
    CHECK (d.patienceOf (28) == 1.0f);
    // And the wait follows it.
    const auto& m = d.model;
    const int k = hopsUntil (d, kE2, true);
    const int model = static_cast<int> (std::ceil (
        d.patienceOf (kE2) * m.attack * evidence (m, 0.0) / (share (m, kE2) * evidence (m, 0.01))));
    CHECK (k >= model);
    CHECK (k <= model + 1);
    d.setPatience (2.0f);
    CHECK (d.patienceOf (kE2) == 2.0f);
}
