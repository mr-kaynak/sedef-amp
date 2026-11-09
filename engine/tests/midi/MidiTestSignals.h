// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Synthetic guitar DI for the guitar-to-MIDI layer: bass lines played on a guitar, with the
// playing that makes a converter emit wrong or extra notes (pick noise, the previous note still
// ringing, palm mutes, re-picked notes, hammer-ons, slides, vibrato), the reference note list of
// each scenario, and the scoring its acceptance bars are written in.

#include "../pedals/OctaverDi.h"
#include "sedef/midi/GuitarToMidi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace sedef::test::midi
{

using octaver::bridgePluck;
using octaver::kA2;
using octaver::kA3;
using octaver::kD3;
using octaver::kE2;
using octaver::kE3;
using octaver::kE4;
using octaver::kE5;
using octaver::piezoPluck;
using octaver::ringingNeighbour;

/// A note the player meant: its MIDI pitch (as played on the guitar, before any transpose), when
/// it starts (the pick, or the fretting-hand move of a legato note) and when it stops sounding.
struct RefNote
{
    int pitch = 0;
    double start = 0.0, end = 0.0;
    bool picked = true;
    float strength = 0.0f; ///< the pluck's peak, for velocity checks
    double rings = -1.0;   ///< when the string really stops, if later than `end` (left ringing)
};

struct Scenario
{
    std::string name;
    double fs = 48000.0;
    std::vector<float> signal;
    std::vector<RefNote> notes;
};

inline double hzOf (int pitch)
{
    return 440.0 * std::exp2 ((pitch - 69) / 12.0);
}

inline int pitchOf (double hz)
{
    return static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (hz / 440.0)));
}

enum class Pickup
{
    bridge, ///< magnetic, near the bridge: the 2nd and 3rd partials over the fundamental
    neck,   ///< magnetic, warmer
    piezo   ///< a weak fundamental
};

inline Pluck pluckFor (Pickup pickup, double hz, float peak, std::uint32_t seed)
{
    Pluck p = pickup == Pickup::piezo ? piezoPluck (hz, seed) : bridgePluck (hz, seed);
    if (pickup == Pickup::neck)
    {
        p.pickupPosition = 0.25;
        p.pluckPosition = 0.15;
    }
    p.peak = peak;
    p.pickNoise = 0.2f * peak;
    return p;
}

/// Small deterministic generator for the scenarios (not the engine's).
struct Dice
{
    std::uint32_t state;
    explicit Dice (std::uint32_t seed) : state (seed * 2654435761u + 12345u) {}
    double unit()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double> (state) / 4294967296.0;
    }
    int below (int n) { return std::min (n - 1, static_cast<int> (unit() * n)); }
    double between (double a, double b) { return a + (b - a) * unit(); }
};

/// Open strings of a guitar in standard tuning, low to high (MIDI pitches).
inline constexpr int kOpenStrings[] = { 40, 45, 50, 55, 59, 64 };

/// A bass line played on a guitar: notes from E2 up to about A3 (fret 0-7 on the lower strings),
/// eighths, sixteenths and quarters at `bpm`, picked with strengths over 12 dB.
/// - A note on the same string as the one before stops it as it is picked.
/// - A note on another string: usually the fretting hand lets the old one go a few milliseconds
///   before or after the pick (it overlaps the new note by up to 15 ms); an open string is often
///   left to ring on under the new notes, at least 6 dB under each of them.
/// - Now and then a staccato note with a rest after it, and a note picked again at once.
inline Scenario bassLine (std::uint32_t seed, Pickup pickup, double bpm, double fs, int count = 48,
                          bool openStringsRing = true)
{
    Dice dice (seed);
    Scenario s;
    s.name = "bass line";
    s.fs = fs;
    const double sixteenth = 60.0 / bpm / 4.0;
    struct Played
    {
        int string, pitch;
        double start;
        Pluck pluck;
        double mute = -1.0;
    };
    std::vector<Played> played;
    double t = 0.15;
    int pitch = kOpenStrings[0] + dice.below (8);
    int string = 0;
    for (int k = 0; k < count; ++k)
    {
        // The next pitch: a step, a leap within a fifth, an octave now and then, or the same note.
        const double r = dice.unit();
        int next = pitch;
        if (r < 0.2)
            next = pitch;
        else if (r < 0.65)
            next = pitch + (dice.unit() < 0.5 ? -1 : 1) * (1 + dice.below (2));
        else if (r < 0.92)
            next = pitch + (dice.unit() < 0.5 ? -1 : 1) * (3 + dice.below (5));
        else
            next = pitch + (pitch < 50 ? 12 : -12);
        next = std::clamp (next, 40, 57);
        // The string it is played on: the one in use if the fret fits, else the nearest that does.
        int str = string;
        if (next - kOpenStrings[str] < 0 || next - kOpenStrings[str] > 7)
        {
            str = 0;
            for (int c = 0; c < 6; ++c)
                if (next - kOpenStrings[c] >= 0 && next - kOpenStrings[c] <= 7)
                {
                    str = c;
                    if (dice.unit() < 0.5)
                        break;
                }
        }
        const int lengths[] = { 1, 2, 2, 2, 4 };
        const double length = sixteenth * lengths[dice.below (5)];
        const float peak =
            static_cast<float> (0.22 * std::pow (10.0, dice.between (-6.0, 6.0) / 20.0));
        Played p { str, next, t,
                   pluckFor (pickup, hzOf (next), peak,
                             100u + seed * 97u + static_cast<std::uint32_t> (k)) };
        p.pluck.decaySeconds = dice.between (1.0, 3.0);
        played.push_back (p);
        t += length;
        pitch = next;
        string = str;
    }
    const double total = t + 0.5;
    s.signal.assign (static_cast<std::size_t> (total * fs), 0.0f);

    // How each note stops.
    for (std::size_t k = 0; k < played.size(); ++k)
    {
        auto& p = played[k];
        const double nextStart = k + 1 < played.size() ? played[k + 1].start : t;
        const bool last = k + 1 == played.size();
        const double r = dice.unit();
        if (last)
            p.mute = nextStart;
        else if (played[k + 1].string == p.string)
            p.mute = nextStart - 0.001; // the pick stops the old vibration
        else if (openStringsRing && r < 0.4 && p.pitch == kOpenStrings[p.string])
            p.mute = -1.0; // an open string left alone rings on (a fretted one stops when the
                           // finger lifts)
        else if (r < 0.24)
            p.mute = p.start + 0.6 * (nextStart - p.start); // staccato, a rest after it
        else
            p.mute = nextStart + dice.between (-0.01, 0.015);
    }
    for (std::size_t k = 0; k < played.size(); ++k)
    {
        auto p = played[k];
        // A string that rings on fades out when the player next uses that string, or after 1 s.
        double end = p.mute;
        if (end < 0.0)
        {
            end = p.start + 1.0;
            for (std::size_t j = k + 1; j < played.size(); ++j)
                if (played[j].string == p.string)
                {
                    end = std::min (end, played[j].start - 0.001);
                    break;
                }
        }
        p.pluck.muteAt = end - p.start;
        if (played[k].mute < 0.0)
            for (std::size_t j = k + 1; j < played.size() && played[j].start < end; ++j)
                p.pluck.peak = std::min (p.pluck.peak, 0.5f * played[j].pluck.peak);
        addPluck (s.signal, fs, static_cast<std::size_t> (p.start * fs), p.pluck);
        const double meant = played[k].mute < 0.0
                                 ? (k + 1 < played.size() ? played[k + 1].start : end)
                                 : played[k].mute;
        s.notes.push_back (
            { p.pitch, p.start, std::min (meant, end) + 0.005, true, p.pluck.peak, end });
    }
    addNoise (s.signal, -80.0f, seed + 3);
    return s;
}

/// A guitarist playing a bass line without a bassist's muting: the same pitch walk as bassLine,
/// but sloppy.
/// - The previous note keeps ringing under the next (when the next is on another string), 6 to
///   20 dB under it, until its string is used again or the hand lets go 50-400 ms later.
/// - Open strings ring in sympathy with a played note whose partials they share (a unison on
///   another string, an octave or two above the open string, a twelfth): 20-30 dB under it.
/// - Loud pick noise (up to 80 % of the note's peak), and now and then a pick scraping a damped
///   string between notes: a noise burst with no note.
/// - Uneven muting: notes let go from 30 ms before to 60 ms after the next pick, staccato now and
///   then.
inline Scenario sloppyLine (std::uint32_t seed, Pickup pickup, double bpm, double fs,
                            int count = 48)
{
    Dice dice (seed * 7919u + 17u);
    Scenario s;
    s.name = "sloppy line";
    s.fs = fs;
    const double sixteenth = 60.0 / bpm / 4.0;
    struct Played
    {
        int string, pitch;
        double start;
        Pluck pluck;
        double end = -1.0;  ///< when the string stops sounding
        double meant = 0.0; ///< when the player meant the note to stop
    };
    std::vector<Played> played;
    double t = 0.15;
    int pitch = kOpenStrings[0] + dice.below (8);
    int string = 0;
    for (int k = 0; k < count; ++k)
    {
        const double r = dice.unit();
        int next = pitch;
        if (r < 0.15)
            next = pitch;
        else if (r < 0.65)
            next = pitch + (dice.unit() < 0.5 ? -1 : 1) * (1 + dice.below (2));
        else if (r < 0.92)
            next = pitch + (dice.unit() < 0.5 ? -1 : 1) * (3 + dice.below (5));
        else
            next = pitch + (pitch < 50 ? 12 : -12);
        next = std::clamp (next, 40, 57);
        int str = string;
        if (next - kOpenStrings[str] < 0 || next - kOpenStrings[str] > 7)
        {
            str = 0;
            for (int c = 0; c < 6; ++c)
                if (next - kOpenStrings[c] >= 0 && next - kOpenStrings[c] <= 7)
                {
                    str = c;
                    if (dice.unit() < 0.5)
                        break;
                }
        }
        const int lengths[] = { 2, 2, 2, 4, 4 };
        const double length = sixteenth * lengths[dice.below (5)];
        const float peak =
            static_cast<float> (0.22 * std::pow (10.0, dice.between (-6.0, 6.0) / 20.0));
        Played p { str, next, t,
                   pluckFor (pickup, hzOf (next), peak,
                             300u + seed * 131u + static_cast<std::uint32_t> (k)) };
        p.pluck.decaySeconds = dice.between (1.0, 3.0);
        p.pluck.pickNoise = static_cast<float> (dice.between (0.2, 0.8)) * peak;
        played.push_back (p);
        t += length;
        pitch = next;
        string = str;
    }
    const double total = t + 0.6;
    s.signal.assign (static_cast<std::size_t> (total * fs), 0.0f);

    // How each note stops, and how loud a note left ringing is under the next (from the last
    // note back, so the next note's level is final when this one is set under it).
    for (std::size_t back = played.size(); back-- > 0;)
    {
        const std::size_t k = back;
        auto& p = played[k];
        const bool last = k + 1 == played.size();
        const double next = last ? t : played[k + 1].start;
        p.meant = next;
        if (last)
            p.end = next;
        else if (played[k + 1].string == p.string)
            p.end = next - 0.001; // the pick stops the old vibration
        else
        {
            const double r = dice.unit();
            if (r < 0.55)
            {
                // Left ringing: until its string is used again, or the hand lets go.
                p.end = next + dice.between (0.05, 0.4);
                for (std::size_t j = k + 1; j < played.size(); ++j)
                    if (played[j].string == p.string)
                    {
                        p.end = std::min (p.end, played[j].start - 0.001);
                        break;
                    }
                const double under = dice.between (6.0, 20.0);
                const double decay = std::exp (-(next - p.start) / p.pluck.decaySeconds);
                p.pluck.peak = std::min (
                    p.pluck.peak, static_cast<float> (played[k + 1].pluck.peak *
                                                      std::pow (10.0, -under / 20.0) / decay));
            }
            else if (r < 0.7)
            {
                p.end = p.start + 0.6 * (next - p.start); // staccato
                p.meant = p.end;
            }
            else
                p.end = next + dice.between (-0.03, 0.06);
        }
    }
    for (std::size_t k = 0; k < played.size(); ++k)
    {
        auto p = played[k];
        p.pluck.muteAt = p.end - p.start;
        addPluck (s.signal, fs, static_cast<std::size_t> (p.start * fs), p.pluck);
        s.notes.push_back (
            { p.pitch, p.start, std::min (p.meant, p.end) + 0.005, true, p.pluck.peak, p.end });

        // Open strings that share a partial with the note ring in sympathy, well under it.
        for (int c = 0; c < 6; ++c)
        {
            const int d = p.pitch - kOpenStrings[c];
            if (c == p.string || !(d == 0 || d == 12 || d == 19 || d == 24) || dice.unit() < 0.4)
                continue;
            Pluck q = pluckFor (pickup, hzOf (kOpenStrings[c]), 0.0f,
                                900u + seed * 31u + static_cast<std::uint32_t> (k) * 6u +
                                    static_cast<std::uint32_t> (c));
            q.peak = static_cast<float> (p.pluck.peak *
                                         std::pow (10.0, -dice.between (20.0, 30.0) / 20.0));
            q.pickNoise = 0.0f;
            q.glideCents = 0.0;
            q.decaySeconds = 2.0;
            double stop = p.start + dice.between (0.3, 1.0);
            for (std::size_t j = k + 1; j < played.size(); ++j)
                if (played[j].string == c)
                {
                    stop = std::min (stop, played[j].start - 0.001);
                    break;
                }
            const double from = p.start + dice.between (0.003, 0.01);
            if (stop <= from + 0.02)
                continue;
            q.muteAt = stop - from;
            addPluck (s.signal, fs, static_cast<std::size_t> (from * fs), q);
        }

        // Now and then the pick scrapes a damped string on the way to the next note.
        if (k + 1 < played.size() && dice.unit() < 0.12)
        {
            const double at = p.start + dice.between (0.5, 0.85) * (played[k + 1].start - p.start);
            const float amp = static_cast<float> (p.pluck.peak * dice.between (0.1, 0.3));
            std::uint32_t r = 77u + seed * 13u + static_cast<std::uint32_t> (k);
            const auto from = static_cast<std::size_t> (at * fs);
            for (std::size_t i = from;
                 i < std::min (s.signal.size(), from + static_cast<std::size_t> (0.015 * fs)); ++i)
            {
                r ^= r << 13;
                r ^= r >> 17;
                r ^= r << 5;
                const double u = static_cast<double> (r) / 4294967296.0 * 2.0 - 1.0;
                const double age = static_cast<double> (i - from) / fs;
                s.signal[i] += amp * static_cast<float> (u * std::exp (-age / 0.003));
            }
        }
    }
    addNoise (s.signal, -80.0f, seed + 5);
    return s;
}

/// Chromatic runs up or down one string, fingerstyle, the way a guitarist plays them slowly: the
/// fretting hand moves to the next fret 25-60 ms before the next pick, while the string still
/// rings (the ringing string glides to the new pitch over about 10 ms, with no energy added, and
/// half the time the finger taps the fret audibly), sometimes lifting off first so the open
/// string sounds for 20-60 ms; then the finger picks the note on the same string, with no click.
/// The strings fade fast, about 14 dB in half a second, as on the musician's own DI. Tunings:
/// standard, or a whole step down.
inline Scenario fretThenPick (std::uint32_t seed, Pickup pickup, double fs, int runs = 2,
                              int notesPerRun = 13)
{
    Dice dice (seed * 104729u + 7u);
    Scenario s;
    s.name = "fret then pick";
    s.fs = fs;
    struct Played
    {
        int pitch;
        double start;
        Pluck pluck;
    };
    std::vector<Played> played;
    std::vector<std::pair<double, float>> taps; // when, how loud
    double t = 0.2;
    for (int r = 0; r < runs; ++r)
    {
        const int open = kOpenStrings[dice.below (6)] - (dice.unit() < 0.5 ? 2 : 0);
        const bool up = dice.unit() < 0.6;
        const auto first = static_cast<int> (played.size());
        for (int k = 0; k < notesPerRun; ++k)
        {
            const int fret = up ? k : notesPerRun - 1 - k;
            const float peak =
                static_cast<float> (0.2 * std::pow (10.0, dice.between (-6.0, 0.0) / 20.0));
            Pluck p = pluckFor (pickup, hzOf (open + fret), peak,
                                1000u + seed * 97u + static_cast<std::uint32_t> (played.size()));
            p.pickNoise = 0.0f;
            p.decaySeconds = dice.between (0.25, 0.35);
            played.push_back ({ open + fret, t, p });
            t += dice.between (0.48, 0.65);
        }
        // The fretting hand reaches each next fret on the ringing string before the next pick.
        for (int k = first; k + 1 < static_cast<int> (played.size()); ++k)
        {
            auto& p = played[static_cast<std::size_t> (k)];
            const auto& next = played[static_cast<std::size_t> (k + 1)];
            const double life = next.start - p.start;
            const double move = life - dice.between (0.025, 0.06);
            const double cents = 100.0 * (next.pitch - p.pitch);
            const int fret = p.pitch - open;
            if (fret > 0 && dice.unit() < 0.3)
            {
                // Off the fret first: the open string sounds, then the finger lands on the next.
                p.pluck.stepAt = move - dice.between (0.02, 0.06);
                p.pluck.stepCents = -100.0 * fret;
                p.pluck.step2At = move;
                p.pluck.step2Cents = 100.0 * (next.pitch - open);
            }
            else
            {
                p.pluck.stepAt = move;
                p.pluck.stepCount = 4;
                p.pluck.stepCents = cents / 4.0;
                p.pluck.stepEvery = 0.003;
            }
            p.pluck.muteAt = life; // the pick on the same string takes over its vibration
            if (dice.unit() < 0.5)
                taps.push_back ({ p.start + move,
                                  static_cast<float> (0.3 * p.pluck.peak *
                                                      std::exp (-move / p.pluck.decaySeconds)) });
        }
        played.back().pluck.muteAt = dice.between (0.4, 0.6);
        t += 0.8;
    }
    s.signal.assign (static_cast<std::size_t> ((t + 0.5) * fs), 0.0f);
    for (std::size_t k = 0; k < played.size(); ++k)
    {
        const auto& p = played[k];
        addPluck (s.signal, fs, static_cast<std::size_t> (p.start * fs), p.pluck);
        const double end = p.start + p.pluck.muteAt;
        s.notes.push_back ({ p.pitch, p.start, end, true, p.pluck.peak });
    }
    std::uint32_t r = 31u + seed;
    for (const auto& [at, amp] : taps)
    {
        const auto from = static_cast<std::size_t> (at * fs);
        for (std::size_t i = from;
             i < std::min (s.signal.size(), from + static_cast<std::size_t> (0.004 * fs)); ++i)
        {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            const double u = static_cast<double> (r) / 4294967296.0 * 2.0 - 1.0;
            s.signal[i] += amp * static_cast<float> (
                                     u * std::exp (-static_cast<double> (i - from) / fs / 0.001));
        }
    }
    addNoise (s.signal, -80.0f, seed + 11);
    return s;
}

/// Palm-muted notes on one pitch: `perBeat` notes per beat at `bpm`, each damped just before the
/// next is picked.
inline Scenario palmMutedRun (double hz, double bpm, int perBeat, double fs, int count = 16,
                              Pickup pickup = Pickup::bridge)
{
    const double step = 60.0 / bpm / perBeat;
    Scenario s;
    s.name = "palm-muted";
    s.fs = fs;
    s.signal.assign (static_cast<std::size_t> ((0.2 + step * count + 0.3) * fs), 0.0f);
    for (int k = 0; k < count; ++k)
    {
        Pluck p = pluckFor (pickup, hz, 0.25f, static_cast<std::uint32_t> (11 + k));
        p.pluckPosition = 0.15;
        p.pickupPosition = pickup == Pickup::piezo ? 0.0 : 0.12;
        p.decaySeconds = 0.08;
        p.maxPartialHz = 2500.0;
        p.muteAt = step - 0.004;
        const double at = 0.1 + step * k;
        addPluck (s.signal, fs, static_cast<std::size_t> (at * fs), p);
        s.notes.push_back ({ pitchOf (hz), at, at + step - 0.004 + 0.005, true, p.peak });
    }
    addNoise (s.signal, -80.0f);
    return s;
}

/// One pick, then `steps` hammer-ons (or pull-offs, negative `cents`) every `every` seconds.
inline Scenario legato (double hz, double cents, int steps, double every, double fs,
                        Pickup pickup = Pickup::bridge)
{
    Pluck p = pluckFor (pickup, hz, 0.25f, 7);
    p.decaySeconds = 3.0;
    p.stepAt = 0.3;
    p.stepCents = cents;
    p.stepCount = steps;
    p.stepEvery = every;
    const double end = 0.1 + p.stepAt + steps * every + 0.3;
    p.muteAt = end - 0.1;
    Scenario s;
    s.name = cents > 0.0 ? "hammer-ons" : "pull-offs";
    s.fs = fs;
    s.signal.assign (static_cast<std::size_t> ((end + 0.2) * fs), 0.0f);
    addPluck (s.signal, fs, static_cast<std::size_t> (0.1 * fs), p);
    const int base = pitchOf (hz);
    const int step = static_cast<int> (std::lround (cents / 100.0));
    s.notes.push_back ({ base, 0.1, 0.1 + p.stepAt, true, p.peak });
    for (int k = 0; k < steps; ++k)
    {
        const double from = 0.1 + p.stepAt + k * every;
        const double to = k + 1 < steps ? from + every : end + 0.005;
        s.notes.push_back ({ base + step * (k + 1), from, to, false, p.peak });
    }
    addNoise (s.signal, -80.0f);
    return s;
}

/// A slide over `semitones` frets, one fret every `every` seconds: every fret it passes is a note.
inline Scenario slide (double hz, int semitones, double every, double fs)
{
    return legato (hz, semitones > 0 ? 100.0 : -100.0, std::abs (semitones), every, fs);
}

/// One long note with vibrato (+-`cents` at 5.5 Hz), or a quarter-tone bend and back.
inline Scenario held (double hz, double fs, double vibratoCents, double bendCents,
                      Pickup pickup = Pickup::bridge)
{
    Pluck p = pluckFor (pickup, hz, 0.25f, 5);
    p.decaySeconds = 3.0;
    p.vibratoAt = vibratoCents > 0.0 ? 0.3 : -1.0;
    p.vibratoCents = vibratoCents;
    if (bendCents != 0.0)
    {
        p.rampAt = 0.4;
        p.rampSeconds = 0.15;
        p.rampCents = bendCents;
    }
    p.muteAt = 1.4;
    Scenario s;
    s.name = vibratoCents > 0.0 ? "vibrato" : "slight bend";
    s.fs = fs;
    s.signal.assign (static_cast<std::size_t> (1.8 * fs), 0.0f);
    addPluck (s.signal, fs, static_cast<std::size_t> (0.1 * fs), p);
    s.notes.push_back ({ pitchOf (hz), 0.1, 1.5 + 0.005, true, p.peak });
    addNoise (s.signal, -80.0f);
    return s;
}

/// A note picked while the previous one (a fourth below or a fifth above) still rings 6 dB down.
inline Scenario overRinging (double hz, double cents, double fs)
{
    const auto r = ringingNeighbour (hz, cents, fs);
    Scenario s;
    s.name = "over a ringing string";
    s.fs = fs;
    s.signal = r.signal;
    // The old note sounds from 0.1 s; the new one is picked at 0.5 s and stops at the end.
    s.notes.push_back ({ pitchOf (hz * std::exp2 (cents / 1200.0)), 0.1, 0.5 + 0.03, true, 0.17f });
    s.notes.push_back ({ pitchOf (hz), 0.5, 1.6, true, 0.3f });
    return s;
}

/// A fingerstyle note as the musician's own DI has it: plucked near the bridge with no click,
/// and on a bridge pickup the low strings' fundamental barely heard (-30 to -40 dB of the energy
/// at the attack under 130 Hz, as on that DI), their 2 ms power swinging with every period.
inline Pluck fingerPluck (Pickup pickup, double hz, float peak, std::uint32_t seed)
{
    Dice dice (seed * 7907u + 3u);
    Pluck p = pluckFor (pickup, hz, peak, seed);
    p.pickNoise = 0.0f;
    if (pickup != Pickup::piezo)
    {
        p.pluckPosition = dice.between (0.07, 0.11);
        p.pickupPosition = pickup == Pickup::bridge ? 0.06 : 0.22;
    }
    const double weak = dice.between (0.12, 0.25);
    if (pickup == Pickup::bridge && hz < 200.0)
        p.fundamentalGain = hz < 130.0 ? weak : weak + (1.0 - weak) * (hz - 130.0) / 70.0;
    p.decaySeconds = dice.between (0.4, 0.8);
    return p;
}

/// What a finger does just before it plucks a silent string, `lead` seconds ahead: it lands on the
/// string (a contact noise), pushes it a little (the string's own note) and, while it rests there,
/// the string sounds the harmonic whose node it touches (the 9th to 13th). The pluck's level
/// stands 15-30 dB over all of it (20-22 dB on the musician's DI), and takes over the string.
inline void addTouch (std::vector<float>& out, double fs, double pluckAt, double lead, double hz,
                      float peak, std::uint32_t seed)
{
    Dice dice (seed * 92821u + 5u);
    const double at = pluckAt - lead;
    // The push: the string's note, faint, until the pluck.
    Pluck push = fingerPluck (
        Pickup::bridge, hz,
        peak * static_cast<float> (std::pow (10.0, -dice.between (26.0, 36.0) / 20.0)), seed + 1u);
    push.muteAt = lead;
    addPluck (out, fs, static_cast<std::size_t> (at * fs), push);
    // The harmonic under the resting finger, from a third of the way in until the pluck.
    const double harmonicHz = hz * (9 + dice.below (5));
    const double from = at + lead / 3.0;
    const double level = peak * std::pow (10.0, -dice.between (27.0, 35.0) / 20.0);
    for (auto i = static_cast<std::size_t> (from * fs); i < static_cast<std::size_t> (pluckAt * fs);
         ++i)
    {
        if (i >= out.size())
            break;
        const double t = static_cast<double> (i) / fs - from;
        const double left = pluckAt - static_cast<double> (i) / fs;
        const double fade = std::min ({ 1.0, t / 0.003, left / 0.002 });
        out[i] += static_cast<float> (level * fade * std::sin (kTwoPiD * harmonicHz * t));
    }
    // The contact: 2 ms of noise.
    std::uint32_t r = 77u + seed;
    for (auto i = static_cast<std::size_t> (at * fs);
         i < std::min (out.size(), static_cast<std::size_t> ((at + 0.002) * fs)); ++i)
    {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        const double u = static_cast<double> (r) / 4294967296.0 * 2.0 - 1.0;
        const double t = static_cast<double> (i) / fs - at;
        out[i] += static_cast<float> (0.03 * peak * u * std::exp (-t / 0.0005));
    }
}

/// Tunings for the from-silence notes: standard, a whole step down, drop D.
inline constexpr int kTunings[3][6] = { { 40, 45, 50, 55, 59, 64 },
                                        { 38, 43, 48, 53, 57, 62 },
                                        { 38, 45, 50, 55, 59, 64 } };

/// Fingerstyle notes from silence: every string of `tuning`, open and at frets 2 and 5, each
/// after a rest of 0.5 to 3 s (the first after 0.5 s of nothing but the input's noise), each with
/// the finger touching the string 10-50 ms before it plucks it (when `touch`). Each note sounds
/// 0.4-0.8 s and is then damped.
inline Scenario fromSilence (std::uint32_t seed, int tuning, Pickup pickup, double fs,
                             bool touch = true)
{
    Dice dice (seed * 15485863u + static_cast<std::uint32_t> (tuning) * 31u + 1u);
    Scenario s;
    s.name = "from silence";
    s.fs = fs;
    struct Played
    {
        int pitch;
        double at, lead;
        Pluck pluck;
    };
    std::vector<Played> played;
    constexpr double kRests[] = { 0.5, 1.0, 2.0, 3.0 };
    double t = 0.5;
    int k = 0;
    for (int string = 0; string < 6; ++string)
        for (int fret : { 0, 2, 5 })
        {
            const int pitch = kTunings[tuning][string] + fret;
            const auto peak =
                static_cast<float> (0.2 * std::pow (10.0, dice.between (-6.0, 0.0) / 20.0));
            Pluck p = fingerPluck (pickup, hzOf (pitch), peak,
                                   300u + seed * 131u + static_cast<std::uint32_t> (k));
            p.muteAt = dice.between (0.4, 0.8);
            played.push_back ({ pitch, t, dice.between (0.01, 0.05), p });
            t += p.muteAt + kRests[(k + static_cast<int> (seed)) % 4];
            ++k;
        }
    s.signal.assign (static_cast<std::size_t> ((t + 0.3) * fs), 0.0f);
    for (std::size_t n = 0; n < played.size(); ++n)
    {
        const auto& p = played[n];
        if (touch)
            addTouch (s.signal, fs, p.at, p.lead, hzOf (p.pitch), p.pluck.peak,
                      seed * 1000u + static_cast<std::uint32_t> (n));
        addPluck (s.signal, fs, static_cast<std::size_t> (p.at * fs), p.pluck);
        s.notes.push_back ({ p.pitch, p.at, p.at + p.pluck.muteAt + 0.005, true, p.pluck.peak });
    }
    addNoise (s.signal, -80.0f, seed + 3);
    return s;
}

/// The same notes as fromSilence(), each picked while the one before it on the same string still
/// rings (the pick takes over the string's vibration): the latency a string has from a ringing
/// string, to compare the notes from silence with.
inline Scenario fromRinging (std::uint32_t seed, int tuning, Pickup pickup, double fs)
{
    Dice dice (seed * 15485863u + static_cast<std::uint32_t> (tuning) * 31u + 1u);
    Scenario s;
    s.name = "from a ringing string";
    s.fs = fs;
    std::vector<std::pair<double, Pluck>> plucks;
    double t = 0.3;
    int k = 0;
    for (int string = 0; string < 6; ++string)
    {
        // The open string rings first, then the three notes, each over the one before.
        Pluck lead = fingerPluck (pickup, hzOf (kTunings[tuning][string] + 7), 0.15f,
                                  900u + static_cast<std::uint32_t> (string));
        lead.muteAt = 0.4;
        plucks.push_back ({ t, lead });
        s.notes.push_back ({ kTunings[tuning][string] + 7, t, t + 0.405, true, lead.peak });
        t += 0.4;
        for (int fret : { 0, 2, 5 })
        {
            const int pitch = kTunings[tuning][string] + fret;
            const auto peak =
                static_cast<float> (0.2 * std::pow (10.0, dice.between (-6.0, 0.0) / 20.0));
            Pluck p = fingerPluck (pickup, hzOf (pitch), peak,
                                   300u + seed * 131u + static_cast<std::uint32_t> (k));
            p.muteAt = fret == 5 ? 0.5 : dice.between (0.4, 0.6);
            plucks.push_back ({ t, p });
            s.notes.push_back ({ pitch, t, t + p.muteAt + 0.005, true, p.peak });
            t += p.muteAt;
            ++k;
        }
        t += 0.6;
    }
    s.signal.assign (static_cast<std::size_t> ((t + 0.3) * fs), 0.0f);
    for (const auto& [at, p] : plucks)
        addPluck (s.signal, fs, static_cast<std::size_t> (at * fs), p);
    addNoise (s.signal, -80.0f, seed + 3);
    return s;
}

/// Strummed chords, which a monophonic converter cannot play: open shapes and power chords, 3 to
/// 6 strings struck 8-30 ms apart, down or up, picked or with the fingers, ringing 0.6-1.2 s
/// together and then damped, 0.2-0.8 s apart. Each RefNote is one chord: its lowest string's
/// pitch, from the first string struck until the damping.
inline Scenario strummedChords (std::uint32_t seed, Pickup pickup, double fs, int count = 12)
{
    static constexpr int kShapes[][6] = {
        { 40, 47, 52, 56, 59, 64 }, // E
        { 0, 45, 52, 57, 60, 64 },  // A minor
        { 0, 0, 50, 57, 62, 66 },   // D
        { 43, 47, 50, 55, 59, 67 }, // G
        { 0, 48, 52, 55, 60, 64 },  // C
        { 40, 47, 52, 0, 0, 0 },    // E5
        { 0, 45, 52, 57, 0, 0 },    // A5
    };
    Dice dice (seed * 49979687u + 17u);
    Scenario s;
    s.name = "strummed chords";
    s.fs = fs;
    std::vector<std::pair<double, Pluck>> plucks;
    double t = 0.3;
    for (int c = 0; c < count; ++c)
    {
        const auto& shape = kShapes[dice.below (7)];
        const bool down = dice.unit() < 0.6;
        const bool fingers = dice.unit() < 0.5;
        const double spread = dice.between (0.008, 0.03);
        const double ring = dice.between (0.6, 1.2);
        const auto peak =
            static_cast<float> (0.15 * std::pow (10.0, dice.between (-6.0, 0.0) / 20.0));
        std::vector<int> strings;
        for (int k = 0; k < 6; ++k)
            if (shape[k] > 0)
                strings.push_back (k);
        if (!down)
            std::reverse (strings.begin(), strings.end());
        double at = t;
        int lowest = 127;
        for (int k : strings)
        {
            const int pitch = shape[k];
            lowest = std::min (lowest, pitch);
            Pluck p = fingers ? fingerPluck (pickup, hzOf (pitch), peak,
                                             seed * 71u + static_cast<std::uint32_t> (c * 6 + k))
                              : pluckFor (pickup, hzOf (pitch), peak,
                                          seed * 71u + static_cast<std::uint32_t> (c * 6 + k));
            p.decaySeconds = 1.5;
            p.muteAt = t + ring - at;
            plucks.push_back ({ at, p });
            at += spread * dice.between (0.6, 1.4);
        }
        s.notes.push_back ({ lowest, t, t + ring + 0.005, true, peak });
        t += ring + dice.between (0.2, 0.8);
    }
    s.signal.assign (static_cast<std::size_t> ((t + 0.5) * fs), 0.0f);
    for (const auto& [at, p] : plucks)
        addPluck (s.signal, fs, static_cast<std::size_t> (at * fs), p);
    addNoise (s.signal, -80.0f, seed + 5);
    return s;
}

// ---------------------------------------------------------------------------------------------
// Running the converter and judging its notes.

struct SentNote
{
    int pitch = 0, velocity = 0, channel = 1;
    double on = 0.0, off = -1.0; ///< seconds; off < 0 while still sounding at the end
};

struct Run
{
    std::vector<SentNote> notes;
    std::vector<sedef::midi::Event> events;
    std::vector<double> eventTimes;
    int maxSounding = 0;
};

/// Runs `signal` through a GuitarToMidi in blocks of `block`, collecting what it sends.
inline Run convert (const std::vector<float>& signal, double fs, const sedef::midi::Settings& s,
                    int block = 64)
{
    sedef::midi::GuitarToMidi g;
    g.setSettings (s);
    g.prepare (fs);
    Run run;
    sedef::midi::EventList out;
    std::vector<int> open (128, -1);
    for (std::size_t start = 0; start < signal.size(); start += static_cast<std::size_t> (block))
    {
        const int n = static_cast<int> (
            std::min<std::size_t> (static_cast<std::size_t> (block), signal.size() - start));
        out.clear();
        g.process (signal.data() + start, n, out);
        for (const auto& e : out)
        {
            const double t = static_cast<double> (start + static_cast<std::size_t> (e.offset)) / fs;
            run.events.push_back (e);
            run.eventTimes.push_back (t);
            if (e.isNoteOn())
            {
                open[static_cast<std::size_t> (e.note())] = static_cast<int> (run.notes.size());
                run.notes.push_back ({ e.note(), e.velocity(), e.channel(), t, -1.0 });
            }
            else if (e.isNoteOff())
            {
                auto& o = open[static_cast<std::size_t> (e.note())];
                if (o >= 0)
                    run.notes[static_cast<std::size_t> (o)].off = t;
                o = -1;
            }
        }
        run.maxSounding = std::max (run.maxSounding, g.soundingCount());
    }
    return run;
}

struct Score
{
    int expected = 0;
    int correct = 0;    ///< the right pitch, once, within the window after the note starts
    int wrongPitch = 0; ///< a note in the window, at another pitch
    int octaveErrors = 0;
    int missed = 0;
    int extra = 0;    ///< notes sent that answer no played note
    int lateOff = 0;  ///< a note-off more than 60 ms after its note stopped
    int earlyOff = 0; ///< a note-off while its note still sounds (more than 30 ms early)
    std::vector<double> latencyMs; ///< start to note-on, for each correct picked note
    std::vector<double> legatoLatencyMs;
    std::vector<SentNote> extras;
    std::vector<RefNote> misses;
    std::vector<std::pair<RefNote, SentNote>> wrongs;

    double correctShare() const
    {
        return expected > 0 ? static_cast<double> (correct) / expected : 1.0;
    }
    double extraPer100() const { return expected > 0 ? 100.0 * extra / expected : 0.0; }
};

/// Matches the sent notes to the played ones. A played note is answered by the first sent note
/// whose note-on falls within [start - 2 ms, start + window]; `transpose` semitones are added to
/// the played pitches. Notes ended by a mute must stop within 60 ms; notes that keep sounding must
/// not stop more than 30 ms before they do (decayed notes are not judged on their ends).
inline Score score (const Run& run, const Scenario& sc, int transpose, double window = 0.08,
                    bool judgeEnds = true)
{
    Score st;
    std::vector<bool> used (run.notes.size(), false);
    for (const auto& ref : sc.notes)
    {
        ++st.expected;
        const int want = ref.pitch + transpose;
        int match = -1, other = -1;
        for (std::size_t k = 0; k < run.notes.size(); ++k)
        {
            if (used[k])
                continue;
            const auto& n = run.notes[k];
            if (n.on < ref.start - 0.002 || n.on > ref.start + window)
                continue;
            if (n.pitch == want)
            {
                match = static_cast<int> (k);
                break;
            }
            if (other < 0)
                other = static_cast<int> (k);
        }
        if (match >= 0)
        {
            used[static_cast<std::size_t> (match)] = true;
            const auto& n = run.notes[static_cast<std::size_t> (match)];
            ++st.correct;
            (ref.picked ? st.latencyMs : st.legatoLatencyMs)
                .push_back (1000.0 * (n.on - ref.start));
            if (judgeEnds)
            {
                const double off = n.off < 0.0 ? 1.0e9 : n.off;
                if (off > ref.end + 0.06)
                    ++st.lateOff;
                if (off < ref.end - 0.03)
                    ++st.earlyOff;
            }
        }
        else if (other >= 0)
        {
            used[static_cast<std::size_t> (other)] = true;
            ++st.wrongPitch;
            st.wrongs.push_back ({ ref, run.notes[static_cast<std::size_t> (other)] });
            const int d = std::abs (run.notes[static_cast<std::size_t> (other)].pitch - want);
            if (d % 12 == 0)
                ++st.octaveErrors;
        }
        else
        {
            ++st.missed;
            st.misses.push_back (ref);
        }
    }
    for (std::size_t k = 0; k < used.size(); ++k)
        if (!used[k])
        {
            ++st.extra;
            st.extras.push_back (run.notes[k]);
        }
    return st;
}

inline double percentile (std::vector<double> v, double p)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const auto k = static_cast<std::size_t> (
        std::clamp (p, 0.0, 1.0) * static_cast<double> (v.size() - 1) + 0.5);
    return v[k];
}

// MSVC deprecates std::getenv (C4996). The hidden report and debug cases read a variable from
// the test thread, which is safe; MSVC applies warnings in the 4700-4999 range per function.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
/// The environment variable `name`, or nullptr when it is not set.
inline const char* environment (const char* name)
{
    return std::getenv (name);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace sedef::test::midi
