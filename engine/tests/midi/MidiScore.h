// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Scoring a converter's notes against a reference note list of real playing
// (docs/design/guitar-to-midi.md, "Real playing"). Deterministic: no clock, no randomness, and
// every tie is broken by time, then by index.
//
// The reference is a list of notes (onset, offset, pitch, string) as a per-string annotation of
// a recording gives them. Several strings may sound at once (a note left ringing under the
// next); the line being played is the latest onset.
//
// Definitions (Tolerances has the numbers and why):
// - A reference note's *window* is [onset - early, onset + late].
// - *Chord*: reference notes on different strings whose onsets are within `chord` of each other
//   are played together, which a one-note converter cannot give. They are left out, and so is
//   any note sent inside their windows.
// - *Right*: a sent note of the reference's pitch starts in the window. Each sent note serves
//   one reference note: reference notes are taken in onset order, each taking the earliest sent
//   note still free.
// - *Octave error*: not right, and a free sent note 12 or 24 semitones away starts in the window.
// - *Wrong pitch*: neither, and a free sent note of another pitch starts in the window (cut at
//   the next reference onset, so the next note's own note-on is not taken for this one).
// - *Missed*: nothing starts for it. Kinds: `held` (its pitch was already sounding and went on:
//   it is heard, not played again), `other` (another pitch sounded through it), `silent`.
// - *Extra*: a sent note that serves no reference note. Kinds, the first that applies:
//   `split` (the pitch of the note being played: it started a second time), `ringing` (the
//   pitch of an earlier reference note that still sounds on its string), `ahead` (the pitch of
//   the next reference note, within `ahead` before its window), `octave` (12, 19 or 24
//   semitones from the note being played, or 12 or 24 under it), `neighbour` (one or two
//   semitones from the note being played or the next one), `silence` (no reference note
//   sounds), `other`.
// - *Onset error*: sent onset minus reference onset, over the right notes.
// - *Note-off error*: sent note-off minus the reference note's end in the line (its offset, or
//   the next reference onset if that comes first), over the right notes.
// - *Frames*: every millisecond, what sounds against the reference: the note being played,
//   an earlier note still ringing, a pitch that is in the reference nowhere, a note where the
//   reference has none, or nothing where it has one.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sedef::test::midi::scoring
{

struct Tolerances
{
    /// A note may start this long before the annotated onset: the annotation dates a note
    /// where its string's pitch is first read, a few to 20 ms after the pick, and the fretting
    /// hand can sound the note before the pick.
    double early = 0.05;
    /// ...and this long after it: twice the slowest sure note (a soft pick over a ringing
    /// string, 60 ms). A note later than that is not the note played, to a listener.
    double late = 0.12;
    /// Onsets on two strings closer than this are one stroke (a double stop, a chord).
    double chord = 0.03;
    /// A sent pitch counts as the reference's when within this of its (fractional) pitch: a
    /// note annotated half-way between two semitones (a bend, a slide) is right as either.
    double pitch = 0.75;
    /// An extra note of the next note's pitch this long before its window is that note, early.
    double ahead = 0.25;
    /// "On time": the share of right notes whose onset error is at most this.
    double onTime = 0.05;
};

struct Truth
{
    double on = 0.0, off = 0.0, pitch = 0.0;
    int string = -1;
};

struct Sent
{
    double on = 0.0, off = 0.0;
    int pitch = 0;
};

enum class Verdict : std::uint8_t
{
    right,
    octave,
    wrong,
    missedHeld,
    missedOther,
    missedSilent,
    chord
};

enum class Extra : std::uint8_t
{
    none, ///< not an extra note (it serves a reference note, or lies in a chord's window)
    split,
    ringing,
    ahead,
    octave,
    neighbour,
    silence,
    other
};

inline constexpr int kExtraKinds = 8;
inline constexpr const char* kExtraNames[kExtraKinds] = {
    "-", "split", "ringing", "ahead", "octave", "neighbour", "silence", "other"
};
inline constexpr const char* kVerdictNames[7] = { "right",       "octave",       "wrong",
                                                  "missed-held", "missed-other", "missed-silent",
                                                  "chord" };

/// How a reference note comes about, for the breakdown by context.
enum class Context : std::uint8_t
{
    silence,    ///< nothing sounds in the reference just before it
    sameString, ///< the note before it is on its string and still sounds: a new fret, or picked
                ///< again
    overRinging ///< another string still sounds
};
inline constexpr const char* kContextNames[3] = { "from silence", "same string",
                                                  "over a ringing string" };

struct Result
{
    std::vector<Verdict> verdict; ///< per reference note
    std::vector<int> servedBy;    ///< per reference note: the sent note, -1 if none
    std::vector<double> lineEnd;  ///< per reference note: its end in the line
    std::vector<Context> context; ///< per reference note
    std::vector<Extra> extra;     ///< per sent note
    std::vector<int> serves;      ///< per sent note: the reference note, -1 if none
    std::vector<int> nearest; ///< per sent note: the reference note being played at its onset, -1
    /// Frames (ms): [0] the note being played, [1] an earlier note still ringing, [2] a pitch
    /// the reference does not hold, [3] a note where the reference has none, [4] nothing where
    /// the reference has a note, [5] both silent.
    std::array<long, 6> frames {};
};

inline bool samePitch (int sent, double reference, const Tolerances& tol)
{
    return std::abs (static_cast<double> (sent) - reference) < tol.pitch;
}

/// `truth` and `sent` sorted by onset. Notes still sounding at the end have off = the end.
inline Result judge (const std::vector<Truth>& truth, const std::vector<Sent>& sent,
                     const Tolerances& tol = {})
{
    const auto nt = truth.size(), ns = sent.size();
    Result r;
    r.verdict.assign (nt, Verdict::missedSilent);
    r.servedBy.assign (nt, -1);
    r.lineEnd.assign (nt, 0.0);
    r.context.assign (nt, Context::silence);
    r.extra.assign (ns, Extra::none);
    r.serves.assign (ns, -1);
    r.nearest.assign (ns, -1);

    // Chords, the line's ends and each note's context.
    std::vector<bool> chord (nt, false);
    for (std::size_t i = 0; i < nt; ++i)
        for (std::size_t j = i + 1; j < nt && truth[j].on - truth[i].on < tol.chord; ++j)
            if (truth[j].string != truth[i].string)
                chord[i] = chord[j] = true;
    for (std::size_t i = 0; i < nt; ++i)
    {
        r.lineEnd[i] = truth[i].off;
        if (i + 1 < nt)
            r.lineEnd[i] = std::min (truth[i].off, std::max (truth[i].on, truth[i + 1].on));
        const double before = truth[i].on - 0.01;
        for (std::size_t j = i; j-- > 0;)
        {
            if (truth[j].off <= before)
                continue;
            if (truth[j].string != truth[i].string)
            {
                r.context[i] = Context::overRinging;
                break;
            }
            r.context[i] = Context::sameString;
        }
        // A note on the same string cannot ring on, whatever the annotation's offset says.
        if (r.context[i] != Context::overRinging && i > 0 &&
            truth[i - 1].string == truth[i].string && truth[i].on - truth[i - 1].off < 0.03)
            r.context[i] = Context::sameString;
    }

    std::vector<bool> used (ns, false);
    const auto take = [&] (std::size_t i, auto&& fits, double to) {
        for (std::size_t k = 0; k < ns; ++k)
        {
            if (sent[k].on < truth[i].on - tol.early)
                continue;
            if (sent[k].on > to)
                break;
            if (!used[k] && fits (sent[k].pitch))
            {
                used[k] = true;
                r.servedBy[i] = static_cast<int> (k);
                r.serves[k] = static_cast<int> (i);
                return true;
            }
        }
        return false;
    };
    for (std::size_t i = 0; i < nt; ++i)
    {
        if (chord[i])
            r.verdict[i] = Verdict::chord;
        else if (take (
                     i, [&] (int p) { return samePitch (p, truth[i].pitch, tol); },
                     truth[i].on + tol.late))
            r.verdict[i] = Verdict::right;
    }
    for (std::size_t i = 0; i < nt; ++i)
        if (r.verdict[i] == Verdict::missedSilent &&
            take (
                i,
                [&] (int p) {
                    return samePitch (p - 12, truth[i].pitch, tol) ||
                           samePitch (p + 12, truth[i].pitch, tol) ||
                           samePitch (p - 24, truth[i].pitch, tol) ||
                           samePitch (p + 24, truth[i].pitch, tol);
                },
                truth[i].on + tol.late))
            r.verdict[i] = Verdict::octave;
    for (std::size_t i = 0; i < nt; ++i)
    {
        if (r.verdict[i] != Verdict::missedSilent)
            continue;
        double to = truth[i].on + tol.late;
        for (std::size_t j = i + 1; j < nt; ++j)
            if (truth[j].on > truth[i].on + tol.chord)
            {
                to = std::min (to, truth[j].on - tol.early);
                break;
            }
        // A note in a chord's window is the chord's, not this note's wrong pitch.
        const auto anyPitch = [] (int) {
            return true;
        };
        bool inChord = false;
        for (std::size_t k = 0; k < ns && !inChord; ++k)
            if (!used[k] && sent[k].on >= truth[i].on - tol.early && sent[k].on <= to)
            {
                for (std::size_t j = 0; j < nt; ++j)
                    if (chord[j] && sent[k].on >= truth[j].on - tol.early &&
                        sent[k].on <= truth[j].on + tol.late)
                        inChord = true;
                break;
            }
        if (!inChord && take (i, anyPitch, to))
        {
            r.verdict[i] = Verdict::wrong;
            continue;
        }
        // Missed: what sounded through its window instead.
        const double at = truth[i].on + tol.late;
        for (std::size_t k = 0; k < ns; ++k)
            if (sent[k].on < truth[i].on - tol.early && sent[k].off > at)
                r.verdict[i] = samePitch (sent[k].pitch, truth[i].pitch, tol)
                                   ? Verdict::missedHeld
                                   : Verdict::missedOther;
    }

    // The reference note being played at a time: the latest onset at or before it that still
    // sounds in the line (the next onset ends it), -1 in a rest.
    const auto playedAt = [&] (double t) {
        int best = -1;
        for (std::size_t i = 0; i < nt && truth[i].on <= t; ++i)
            if (truth[i].off > t)
                best = static_cast<int> (i);
        return best;
    };
    for (std::size_t k = 0; k < ns; ++k)
    {
        const double u = sent[k].on;
        const int q = sent[k].pitch;
        r.nearest[k] = playedAt (u);
        if (used[k])
            continue;
        bool inChord = false;
        for (std::size_t j = 0; j < nt; ++j)
            if (chord[j] && u >= truth[j].on - tol.early && u <= truth[j].on + tol.late)
                inChord = true;
        if (inChord)
            continue;
        const int cur = r.nearest[k];
        int next = -1;
        for (std::size_t j = 0; j < nt; ++j)
            if (truth[j].on > u)
            {
                next = static_cast<int> (j);
                break;
            }
        const auto interval = [&] (int j) {
            return static_cast<int> (
                std::lround (std::abs (q - truth[static_cast<std::size_t> (j)].pitch)));
        };
        bool sounding = false, ringing = false;
        for (std::size_t j = 0; j < nt && truth[j].on <= u; ++j)
            if (truth[j].off > u)
            {
                sounding = true;
                if (static_cast<int> (j) != cur && samePitch (q, truth[j].pitch, tol))
                    ringing = true;
            }
        Extra kind = Extra::other;
        if (cur >= 0 && samePitch (q, truth[static_cast<std::size_t> (cur)].pitch, tol))
            kind = Extra::split;
        else if (ringing)
            kind = Extra::ringing;
        else if (next >= 0 && samePitch (q, truth[static_cast<std::size_t> (next)].pitch, tol) &&
                 truth[static_cast<std::size_t> (next)].on - tol.early - u <= tol.ahead)
            kind = Extra::ahead;
        else if (cur >= 0 &&
                 (interval (cur) == 12 || interval (cur) == 24 ||
                  (interval (cur) == 19 && q > truth[static_cast<std::size_t> (cur)].pitch)))
            kind = Extra::octave;
        else if ((cur >= 0 && interval (cur) >= 1 && interval (cur) <= 2) ||
                 (next >= 0 && interval (next) >= 1 && interval (next) <= 2 &&
                  truth[static_cast<std::size_t> (next)].on - u <= tol.ahead))
            kind = Extra::neighbour;
        else if (!sounding)
            kind = Extra::silence;
        r.extra[k] = kind;
    }

    // Frames.
    double end = 0.0;
    for (const auto& t : truth)
        end = std::max (end, t.off);
    for (const auto& s : sent)
        end = std::max (end, s.off);
    std::size_t si = 0;
    for (long ms = 0; static_cast<double> (ms) * 0.001 < end; ++ms)
    {
        const double t = (static_cast<double> (ms) + 0.5) * 0.001;
        while (si < ns && sent[si].off <= t)
            ++si;
        int out = -1;
        for (std::size_t k = si; k < ns && sent[k].on <= t; ++k)
            if (sent[k].off > t)
                out = sent[k].pitch;
        int cur = -1;
        bool any = false, held = false, inChord = false;
        for (std::size_t j = 0; j < nt && truth[j].on <= t; ++j)
            if (truth[j].off > t)
            {
                any = true;
                cur = static_cast<int> (j);
                if (out >= 0 && samePitch (out, truth[j].pitch, tol))
                    held = true;
                inChord = inChord || chord[j];
            }
        if (inChord)
            continue;
        const bool current = cur >= 0 && out >= 0 &&
                             samePitch (out, truth[static_cast<std::size_t> (cur)].pitch, tol);
        const int slot = out < 0 ? (any ? 4 : 5) : (!any ? 3 : (current ? 0 : (held ? 1 : 2)));
        ++r.frames[static_cast<std::size_t> (slot)];
    }
    return r;
}

/// The value at quantile `p` (0..1) of `v`, by linear interpolation; 0 when empty.
inline double quantile (std::vector<double> v, double p)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const double x = p * static_cast<double> (v.size() - 1);
    const auto lo = static_cast<std::size_t> (x);
    const auto hi = std::min (v.size() - 1, lo + 1);
    return v[lo] + (v[hi] - v[lo]) * (x - static_cast<double> (lo));
}

} // namespace sedef::test::midi::scoring
