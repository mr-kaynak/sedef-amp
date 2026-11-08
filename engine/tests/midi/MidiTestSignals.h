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
#include <cstdlib>
#include <cstdint>
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
        s.notes.push_back ({ p.pitch, p.start, std::min (meant, end) + 0.005, true, p.pluck.peak });
    }
    addNoise (s.signal, -80.0f, seed + 3);
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
