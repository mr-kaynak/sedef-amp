// SPDX-License-Identifier: AGPL-3.0-or-later
// The scoring of real playing (MidiScore.h): each definition on a case small enough to check by
// hand.
#include "MidiScore.h"

#include <catch2/catch_test_macros.hpp>

using namespace sedef::test::midi::scoring;

TEST_CASE ("score: a note is right once, within its window", "[midi][score]")
{
    const std::vector<Truth> truth { { 1.0, 1.5, 60.0, 2 }, { 2.0, 2.5, 62.1, 2 } };
    // The first note comes 20 ms early and again 200 ms in; the second 119 ms late.
    const std::vector<Sent> sent { { 0.98, 1.2, 60 }, { 1.2, 1.5, 60 }, { 2.119, 2.5, 62 } };
    const auto r = judge (truth, sent);
    CHECK (r.verdict[0] == Verdict::right);
    CHECK (r.servedBy[0] == 0);
    CHECK (r.verdict[1] == Verdict::right);
    CHECK (r.extra[0] == Extra::none);
    CHECK (r.extra[1] == Extra::split); // the same note started a second time
    CHECK (r.extra[2] == Extra::none);

    // 121 ms late is not the note any more; nothing sounded: missed, and the note is extra.
    const auto late = judge ({ { 2.0, 2.5, 62.0, 2 } }, { { 2.121, 2.5, 62 } });
    CHECK (late.verdict[0] == Verdict::missedSilent);
    CHECK (late.extra[0] == Extra::split);
    // 51 ms early is not it either.
    const auto early = judge ({ { 2.0, 2.5, 62.0, 2 } }, { { 1.949, 2.5, 62 } });
    CHECK (early.verdict[0] == Verdict::missedHeld); // its pitch sounded through it
}

TEST_CASE ("score: octave errors, wrong pitches and missed notes", "[midi][score]")
{
    const std::vector<Truth> truth {
        { 1.0, 1.4, 52.0, 1 }, { 2.0, 2.4, 55.0, 1 }, { 3.0, 3.4, 57.0, 1 }, { 4.0, 4.4, 59.0, 1 }
    };
    const std::vector<Sent> sent {
        { 1.01, 1.4, 40 },  // an octave low
        { 2.01, 2.4, 56 },  // a semitone high
        { 2.9, 3.6, 50 },   // another pitch sounding through the third note
    };
    const auto r = judge (truth, sent);
    CHECK (r.verdict[0] == Verdict::octave);
    CHECK (r.verdict[1] == Verdict::wrong);
    CHECK (r.verdict[2] == Verdict::missedOther);
    CHECK (r.verdict[3] == Verdict::missedSilent);
    // Each of the three served or sounded for a reference note; only the third is extra.
    CHECK (r.extra[0] == Extra::none);
    CHECK (r.extra[1] == Extra::none);
    CHECK (r.extra[2] != Extra::none);
}

TEST_CASE ("score: extra notes by kind", "[midi][score]")
{
    // E3 on one string, then G3 on the next while the E3 rings on, then a rest.
    const std::vector<Truth> truth { { 1.0, 2.0, 52.0, 1 }, { 1.5, 1.9, 55.0, 2 },
                                     { 4.0, 4.5, 60.0, 3 } };
    const std::vector<Sent> sent {
        { 1.0, 1.3, 52 },   // right
        { 1.3, 1.34, 53 },  // a semitone from the note being played
        { 1.5, 1.7, 55 },   // right
        { 1.7, 1.8, 52 },   // the E3 that still rings under the G3
        { 1.8, 1.85, 67 },  // an octave over the G3
        { 3.0, 3.2, 45 },   // in a rest
        { 3.85, 3.9, 60 },  // the next note, early (100 ms before its window)
        { 4.0, 4.5, 60 },   // right
    };
    const auto r = judge (truth, sent);
    CHECK (r.verdict[0] == Verdict::right);
    CHECK (r.verdict[1] == Verdict::right);
    CHECK (r.verdict[2] == Verdict::right);
    CHECK (r.extra[1] == Extra::neighbour);
    CHECK (r.extra[3] == Extra::ringing);
    CHECK (r.extra[4] == Extra::octave);
    CHECK (r.extra[5] == Extra::silence);
    CHECK (r.extra[6] == Extra::ahead);
    CHECK (r.context[0] == Context::silence);
    CHECK (r.context[1] == Context::overRinging);
    CHECK (r.lineEnd[0] == 1.5); // the next onset ends a note in the line
}

TEST_CASE ("score: strokes on two strings at once are left out", "[midi][score]")
{
    // Two strings 10 ms apart: a double stop. A one-note converter is not judged on it, nor
    // on what it sends there.
    const std::vector<Truth> truth { { 1.0, 1.5, 52.0, 1 }, { 1.01, 1.5, 59.0, 3 },
                                     { 2.0, 2.5, 60.0, 3 } };
    const std::vector<Sent> sent { { 1.02, 1.1, 47 }, { 1.1, 1.5, 59 }, { 2.0, 2.5, 60 } };
    const auto r = judge (truth, sent);
    CHECK (r.verdict[0] == Verdict::chord);
    CHECK (r.verdict[1] == Verdict::chord);
    CHECK (r.verdict[2] == Verdict::right);
    CHECK (r.extra[0] == Extra::none);
    CHECK (r.extra[1] == Extra::none);
    // The same string twice within 30 ms is not a chord.
    const auto same = judge ({ { 1.0, 1.02, 52.0, 1 }, { 1.02, 1.5, 54.0, 1 } },
                             { { 1.03, 1.5, 54 } });
    CHECK (same.verdict[1] == Verdict::right);
    CHECK (same.verdict[0] != Verdict::chord);
}

TEST_CASE ("score: frames add up, and quantiles interpolate", "[midi][score]")
{
    const std::vector<Truth> truth { { 0.1, 0.3, 60.0, 0 } };
    const std::vector<Sent> sent { { 0.1, 0.2, 60 }, { 0.2, 0.25, 61 }, { 0.4, 0.5, 60 } };
    const auto r = judge (truth, sent);
    CHECK (r.frames[0] == 100); // the note being played
    CHECK (r.frames[2] == 50);  // another pitch
    CHECK (r.frames[4] == 50);  // nothing
    CHECK (r.frames[3] == 100); // a note in the reference's rest
    CHECK (quantile ({ 1.0, 2.0, 3.0, 4.0 }, 0.5) == 2.5);
    CHECK (quantile ({}, 0.5) == 0.0);
}
