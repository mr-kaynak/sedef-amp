// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Chords from one pickup channel: the feasibility bench (docs/design/guitar-to-midi-chords.md).
// Not shipped.
//
//   sedef-poly-bench synth [options]
//       Synthetic chords from the repo's string models, three ways of playing (sustained,
//       strummed changes, arpeggios) on three pickups: precision, recall, F, ghost notes per 20
//       chords, octave errors, onset to note-on.
//   sedef-poly-bench sweep [options]
//       The same against the latency parameter.
//   sedef-poly-bench wav <in.wav> [--truth notes.txt] [--from s] [--to s] [options]
//       A recording: the notes sent, grouped by strum; with --truth (lines "onset offset
//       pitch", seconds and MIDI numbers) the same scores.
//   sedef-poly-bench set <list.txt> [options]
//       A list of "<wav> <truth> <kind>" lines, scored per kind.
//   sedef-poly-bench cpu [options]
//       Cost per second of audio at 48 kHz.
//   sedef-poly-bench dump <dir>
//       The synthetic sets as WAV files with their note lists.
//
// Options: --window s, --decide s, --on share, --rise x, --sparsity x, --bands n, --compress g,
// --sweeps n, --match s (how long after its onset a note may start and still count), --trace 1.

#include "../WavIo.h"
#include "PolyEstimator.h"
#include "midi/MidiTestSignals.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace sedef;
using sedef::test::addNoise;
using sedef::test::addPluck;
using sedef::test::Pluck;
using sedef::test::midi::Dice;
using sedef::test::midi::Pickup;

namespace
{

struct Ref
{
    int pitch = 0;
    double start = 0.0, end = 0.0;
    int chord = 0;
};

struct Set
{
    std::string name;
    double fs = 48000.0;
    std::vector<float> signal;
    std::vector<Ref> notes;
    int chords = 0;
};

struct Sent
{
    int pitch = 0;
    double on = 0.0, off = -1.0;
    float strength = 0.0f;
};

// ---------------------------------------------------------------------------------------------
// Synthetic chords.

constexpr int kOpen[6] = { 40, 45, 50, 55, 59, 64 };

struct Shape
{
    const char* name;
    int fret[6]; ///< per string, low to high; -1: not played
};

constexpr Shape kShapes[] = {
    { "E", { 0, 2, 2, 1, 0, 0 } },     { "Em", { 0, 2, 2, 0, 0, 0 } },
    { "A", { -1, 0, 2, 2, 2, 0 } },    { "Am", { -1, 0, 2, 2, 1, 0 } },
    { "D", { -1, -1, 0, 2, 3, 2 } },   { "Dm", { -1, -1, 0, 2, 3, 1 } },
    { "G", { 3, 2, 0, 0, 0, 3 } },     { "C", { -1, 3, 2, 0, 1, 0 } },
    { "F", { 1, 3, 3, 2, 1, 1 } },     { "Bm", { -1, 2, 4, 4, 3, 2 } },
    { "B7", { -1, 2, 1, 2, 0, 2 } },   { "E5", { 0, 2, 2, -1, -1, -1 } },
    { "A5", { -1, 0, 2, 2, -1, -1 } }, { "G5", { 3, 5, 5, -1, -1, -1 } },
    { "E7", { 0, 2, 0, 1, 0, 0 } },    { "Am7", { -1, 0, 2, 0, 1, 0 } },
};
constexpr int kShapeCount = static_cast<int> (sizeof (kShapes) / sizeof (kShapes[0]));

enum class Kind
{
    sustained,
    strummed,
    arpeggio
};

const char* kindName (Kind k)
{
    return k == Kind::sustained ? "sustained" : k == Kind::strummed ? "strummed" : "arpeggio";
}

const char* pickupName (Pickup p)
{
    return p == Pickup::bridge ? "bridge" : p == Pickup::neck ? "neck" : "piezo";
}

/// One guitar: each string's stiffness (open) and how far it is off pitch.
struct Guitar
{
    double stiffness[6], cents[6];
    explicit Guitar (std::uint32_t seed)
    {
        Dice dice (seed * 2246822519u + 7u);
        for (int s = 0; s < 6; ++s)
        {
            stiffness[s] = 3.0e-5 * std::pow (5.0, dice.unit()); // 3e-5 to 1.5e-4
            cents[s] = dice.between (-4.0, 4.0);
        }
    }
};

Set synthetic (Kind kind, Pickup pickup, std::uint32_t seed, int count, double fs)
{
    Dice dice (seed * 668265263u + static_cast<std::uint32_t> (kind) * 97u + 3u);
    const Guitar guitar (seed);
    Set set;
    set.name = std::string (kindName (kind)) + " " + pickupName (pickup);
    set.fs = fs;
    set.chords = count;

    struct Struck
    {
        double at;
        Pluck pluck;
        int string, pitch, chord;
    };
    std::vector<Struck> plucks;
    int ringing[6] = { -1, -1, -1, -1, -1, -1 }; // index into plucks

    auto strike = [&] (int string, int fret, double at, float peak, bool fingers, int chord) {
        const int pitch = kOpen[string] + fret;
        const double hz =
            440.0 * std::exp2 ((pitch - 69) / 12.0) * std::exp2 (guitar.cents[string] / 1200.0);
        const auto s = seed * 131u + static_cast<std::uint32_t> (plucks.size()) * 7u + 1u;
        Pluck p = fingers ? test::midi::fingerPluck (pickup, hz, peak, s)
                          : test::midi::pluckFor (pickup, hz, peak, s);
        p.inharmonicity = guitar.stiffness[string] * std::exp2 (fret / 6.0);
        p.decaySeconds = dice.between (1.2, 2.5);
        p.seed = s;
        if (ringing[string] >= 0)
        {
            auto& old = plucks[static_cast<std::size_t> (ringing[string])];
            if (old.pluck.muteAt < 0.0 || old.at + old.pluck.muteAt > at)
                old.pluck.muteAt = std::max (0.0, at - 0.004 - old.at);
        }
        ringing[string] = static_cast<int> (plucks.size());
        plucks.push_back ({ at, p, string, pitch, chord });
    };
    auto damp = [&] (double at) {
        for (int s = 0; s < 6; ++s)
            if (ringing[s] >= 0)
            {
                auto& old = plucks[static_cast<std::size_t> (ringing[s])];
                if (old.pluck.muteAt < 0.0 || old.at + old.pluck.muteAt > at)
                    old.pluck.muteAt = std::max (0.0, at - old.at);
                ringing[s] = -1;
            }
    };

    double t = 0.3;
    int lastShape = -1;
    for (int c = 0; c < count; ++c)
    {
        int index = dice.below (kShapeCount);
        if (index == lastShape)
            index = (index + 1) % kShapeCount;
        lastShape = index;
        const Shape& shape = kShapes[index];
        const bool fingers = dice.unit() < 0.4;
        const auto level =
            static_cast<float> (0.12 * std::pow (10.0, dice.between (-6.0, 0.0) / 20.0));
        std::vector<int> strings;
        for (int s = 0; s < 6; ++s)
            if (shape.fret[s] >= 0)
                strings.push_back (s);
        auto strum = [&] (double at, bool down, std::size_t howMany) {
            std::vector<int> order = strings;
            if (!down)
                std::reverse (order.begin(), order.end());
            order.resize (std::min (order.size(), howMany));
            const double spread = dice.between (0.008, 0.03);
            for (int s : order)
            {
                const auto peak =
                    level * static_cast<float> (std::pow (10.0, dice.between (-3.0, 3.0) / 20.0));
                strike (s, shape.fret[s], at, peak, fingers, c);
                at += spread * dice.between (0.6, 1.4);
            }
            return at;
        };

        if (kind == Kind::sustained)
        {
            strum (t, dice.unit() < 0.7, 6);
            const double ring = dice.between (1.2, 1.8);
            damp (t + ring);
            t += ring + dice.between (0.3, 0.6);
        }
        else if (kind == Kind::strummed)
        {
            const int strums = 2 + dice.below (3);
            const double beat = dice.between (0.3, 0.5);
            for (int k = 0; k < strums; ++k)
            {
                const bool down = k % 2 == 0;
                strum (t, down, down ? 6 : static_cast<std::size_t> (3 + dice.below (2)));
                t += beat;
            }
            damp (t - 0.03); // the hand lets go just before the next chord
        }
        else
        {
            const double step = dice.between (0.12, 0.25);
            std::vector<int> order = strings;
            for (std::size_t k = strings.size() - 1; k-- > 1;)
                order.push_back (strings[k]); // up and back down
            for (int s : order)
            {
                strike (s, shape.fret[s], t, level, true, c);
                t += step;
            }
            t += 0.3;
            damp (t - 0.03);
            t += dice.between (0.0, 0.2);
        }
    }

    set.signal.assign (static_cast<std::size_t> ((t + 0.5) * fs), 0.0f);
    for (const auto& s : plucks)
    {
        addPluck (set.signal, fs, static_cast<std::size_t> (s.at * fs), s.pluck);
        const double end = s.pluck.muteAt >= 0.0 ? s.at + s.pluck.muteAt : t + 0.5;
        set.notes.push_back ({ s.pitch, s.at, end, s.chord });
    }
    addNoise (set.signal, -80.0f, seed + 5);
    std::sort (set.notes.begin(), set.notes.end(),
               [] (const Ref& a, const Ref& b) { return a.start < b.start; });
    return set;
}

// ---------------------------------------------------------------------------------------------
// Running and scoring.

struct RunResult
{
    std::vector<Sent> notes;
    double seconds = 0.0; ///< processing time
};

RunResult run (const std::vector<float>& signal, double fs, const poly::Config& cfg,
               int block = 256)
{
    RunResult r;
    const auto t0 = std::chrono::steady_clock::now();
    poly::Estimator est (fs, cfg);
    std::vector<poly::NoteEvent> events;
    for (std::size_t at = 0; at < signal.size(); at += static_cast<std::size_t> (block))
        est.push (signal.data() + at,
                  static_cast<int> (
                      std::min<std::size_t> (static_cast<std::size_t> (block), signal.size() - at)),
                  events);
    est.flush (events);
    r.seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::map<int, std::size_t> open;
    for (const auto& e : events)
    {
        if (e.on)
        {
            open[e.pitch] = r.notes.size();
            r.notes.push_back ({ e.pitch, e.time, -1.0, e.strength });
        }
        else if (auto it = open.find (e.pitch); it != open.end())
        {
            r.notes[it->second].off = e.time;
            open.erase (it);
        }
    }
    return r;
}

struct Score
{
    int expected = 0, sent = 0, correct = 0, missed = 0, ghosts = 0, octaveGhosts = 0, chords = 0;
    int again = 0; ///< ghosts at the pitch of a played note that sounds: a second start
    int heldExpected = 0, heldCorrect = 0;
    // Every 10 ms, what sounds against what is played `lag` earlier: notes, and pitch classes.
    long frameTp = 0, frameFp = 0, frameFn = 0, classTp = 0, classFp = 0, classFn = 0;
    std::vector<double> latencyMs;

    void add (const Score& o)
    {
        expected += o.expected;
        sent += o.sent;
        correct += o.correct;
        missed += o.missed;
        ghosts += o.ghosts;
        octaveGhosts += o.octaveGhosts;
        chords += o.chords;
        again += o.again;
        frameTp += o.frameTp;
        frameFp += o.frameFp;
        frameFn += o.frameFn;
        classTp += o.classTp;
        classFp += o.classFp;
        classFn += o.classFn;
        heldExpected += o.heldExpected;
        heldCorrect += o.heldCorrect;
        latencyMs.insert (latencyMs.end(), o.latencyMs.begin(), o.latencyMs.end());
    }
    double precision() const { return sent > 0 ? static_cast<double> (correct) / sent : 1.0; }
    double recall() const { return expected > 0 ? static_cast<double> (correct) / expected : 1.0; }
    double f() const
    {
        const double p = precision(), r = recall();
        return p + r > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
    }
    double ghostsPer20() const { return chords > 0 ? 20.0 * ghosts / chords : 0.0; }
    static double fOf (long tp, long fp, long fn)
    {
        return 2 * tp + fp + fn > 0 ? 2.0 * tp / (2.0 * tp + fp + fn) : 1.0;
    }
    static double pOf (long tp, long fp)
    {
        return tp + fp > 0 ? static_cast<double> (tp) / (tp + fp) : 1.0;
    }
    double median() const { return test::midi::percentile (latencyMs, 0.5); }
    double p90() const { return test::midi::percentile (latencyMs, 0.9); }
};

/// Two strings at one pitch are one MIDI note: references of one pitch whose starts lie within
/// 50 ms are merged.
std::vector<Ref> merged (std::vector<Ref> refs)
{
    std::sort (refs.begin(), refs.end(),
               [] (const Ref& a, const Ref& b) { return a.start < b.start; });
    std::vector<Ref> out;
    for (const auto& r : refs)
    {
        bool same = false;
        for (auto& o : out)
            if (o.pitch == r.pitch && std::abs (o.start - r.start) < 0.05)
            {
                o.end = std::max (o.end, r.end);
                same = true;
                break;
            }
        if (!same)
            out.push_back (r);
    }
    return out;
}

/// A played note is answered by the first unused sent note of its pitch whose note-on lies in
/// [start - 5 ms, start + match]. Sent notes that answer nothing are ghosts; a ghost is an
/// octave error when a played note 12, 24 or 36 semitones away sounds as it starts. "Held" asks
/// less: 150 ms after a played note starts, does a note of its pitch sound (however it began)?
Score score (const std::vector<Sent>& sent, const std::vector<Ref>& played, int chords,
             double match, bool list = false, double lag = 0.1)
{
    const auto refs = merged (played);
    Score s;
    s.chords = chords;
    s.expected = static_cast<int> (refs.size());
    s.sent = static_cast<int> (sent.size());
    std::vector<bool> used (sent.size(), false);
    for (const auto& r : refs)
    {
        int found = -1;
        for (std::size_t k = 0; k < sent.size(); ++k)
            if (!used[k] && sent[k].pitch == r.pitch && sent[k].on >= r.start - 0.005 &&
                sent[k].on <= r.start + match)
            {
                found = static_cast<int> (k);
                break;
            }
        if (found >= 0)
        {
            used[static_cast<std::size_t> (found)] = true;
            ++s.correct;
            s.latencyMs.push_back (1000.0 * (sent[static_cast<std::size_t> (found)].on - r.start));
        }
        else
        {
            ++s.missed;
            if (list)
                std::printf ("    missed %d at %.3f\n", r.pitch, r.start);
        }
        if (r.end - r.start > 0.2)
        {
            ++s.heldExpected;
            const double at = r.start + 0.15;
            for (const auto& n : sent)
                if (n.pitch == r.pitch && n.on <= at && (n.off < 0.0 || n.off > at))
                {
                    ++s.heldCorrect;
                    break;
                }
        }
    }
    for (std::size_t k = 0; k < sent.size(); ++k)
        if (!used[k])
        {
            ++s.ghosts;
            bool octave = false, same = false;
            for (const auto& r : refs)
                if (sent[k].on >= r.start - 0.005 && sent[k].on <= r.end + 0.05)
                {
                    if (r.pitch == sent[k].pitch)
                        same = true;
                    else if ((r.pitch - sent[k].pitch) % 12 == 0)
                        octave = true;
                }
            if (same)
                ++s.again;
            else if (octave)
                ++s.octaveGhosts;
            if (list)
                std::printf ("    ghost %d at %.3f%s\n", sent[k].pitch, sent[k].on,
                             same     ? " (again)"
                             : octave ? " (octave)"
                                      : "");
        }
    double end = 0.0;
    for (const auto& r : refs)
        end = std::max (end, r.end);
    for (double t = 0.0; t < end; t += 0.01)
    {
        bool is[128] = {}, said[128] = {}, isClass[12] = {}, saidClass[12] = {};
        for (const auto& r : refs)
            if (r.start <= t && t < r.end && r.pitch > 0 && r.pitch < 128)
                is[r.pitch] = isClass[r.pitch % 12] = true;
        for (const auto& n : sent)
            if (n.on <= t + lag && (n.off < 0.0 || t + lag < n.off))
                said[n.pitch] = saidClass[n.pitch % 12] = true;
        for (int k = 0; k < 128; ++k)
        {
            s.frameTp += is[k] && said[k];
            s.frameFp += !is[k] && said[k];
            s.frameFn += is[k] && !said[k];
        }
        for (int k = 0; k < 12; ++k)
        {
            s.classTp += isClass[k] && saidClass[k];
            s.classFp += !isClass[k] && saidClass[k];
            s.classFn += isClass[k] && !saidClass[k];
        }
    }
    return s;
}

void header()
{
    std::printf ("%-22s %6s %6s %6s %6s %6s %6s %7s %6s %6s %7s %7s | %6s %6s %6s | %6s %6s %6s\n",
                 "set", "played", "sent", "P", "R", "F", "ghosts", "per 20", "again", "octave",
                 "median", "p90", "sndP", "sndR", "sndF", "clsP", "clsR", "clsF");
}

void row (const std::string& name, const Score& s)
{
    std::printf ("%-22s %6d %6d %6.3f %6.3f %6.3f %6d %7.1f %6d %6d %4.0f ms %4.0f ms | %6.3f "
                 "%6.3f %6.3f | %6.3f %6.3f %6.3f\n",
                 name.c_str(), s.expected, s.sent, s.precision(), s.recall(), s.f(), s.ghosts,
                 s.ghostsPer20(), s.again, s.octaveGhosts, s.median(), s.p90(),
                 Score::pOf (s.frameTp, s.frameFp), Score::pOf (s.frameTp, s.frameFn),
                 Score::fOf (s.frameTp, s.frameFp, s.frameFn), Score::pOf (s.classTp, s.classFp),
                 Score::pOf (s.classTp, s.classFn), Score::fOf (s.classTp, s.classFp, s.classFn));
}

// ---------------------------------------------------------------------------------------------

struct Options
{
    poly::Config cfg;
    double match = 0.15, from = 0.0, to = 1.0e9, settle = -1.0, third = 0.06;
    std::string truth;
    int seeds = 3, count = 20, every = 1;
    bool list = false, files = false;
    std::vector<std::string> positional;
};

Options parse (int argc, char** argv, int first)
{
    Options o;
    for (int i = first; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto value = [&] {
            return i + 1 < argc ? std::atof (argv[++i]) : 0.0;
        };
        if (a == "--window")
            o.cfg.window = value();
        else if (a == "--decide")
            o.cfg.decide = value();
        else if (a == "--on")
            o.cfg.onShare = value();
        else if (a == "--rise")
            o.cfg.rise = value();
        else if (a == "--off")
            o.cfg.offShare = value();
        else if (a == "--sparsity")
            o.cfg.sparsity = value();
        else if (a == "--bands")
            o.cfg.bands = static_cast<int> (value());
        else if (a == "--compress")
            o.cfg.compress = value();
        else if (a == "--sweeps")
            o.cfg.sweeps = static_cast<int> (value());
        else if (a == "--tilt")
            o.cfg.tilt = value();
        else if (a == "--resolve")
            o.cfg.resolve = value();
        else if (a == "--floor")
            o.cfg.lineFloor = value();
        else if (a == "--partials")
            o.cfg.maxPartials = static_cast<int> (value());
        else if (a == "--stiffness")
            o.cfg.stiffness = value();
        else if (a == "--tune")
            o.cfg.tuneCents = value();
        else if (a == "--top")
            o.cfg.topHz = value();
        else if (a == "--onset")
            o.cfg.onsetRise = value();
        else if (a == "--decide-for")
            o.cfg.decideFor = value();
        else if (a == "--keep")
            o.cfg.keepShare = value();
        else if (a == "--odd")
            o.cfg.oddShare = value();
        else if (a == "--spread")
            o.cfg.spread = value();
        else if (a == "--gate")
            o.cfg.gateDb = value();
        else if (a == "--match")
            o.match = value();
        else if (a == "--third")
            o.third = value();
        else if (a == "--settle")
            o.settle = value();
        else if (a == "--from")
            o.from = value();
        else if (a == "--to")
            o.to = value();
        else if (a == "--seeds")
            o.seeds = static_cast<int> (value());
        else if (a == "--count")
            o.count = static_cast<int> (value());
        else if (a == "--trace")
            o.cfg.trace = value() != 0.0;
        else if (a == "--every")
            o.every = static_cast<int> (value());
        else if (a == "--files")
            o.files = value() != 0.0;
        else if (a == "--list")
            o.list = value() != 0.0;
        else if (a == "--truth" && i + 1 < argc)
            o.truth = argv[++i];
        else
            o.positional.push_back (a);
    }
    return o;
}

std::vector<Ref> readTruth (const std::string& path)
{
    std::vector<Ref> refs;
    std::ifstream f (path);
    std::string line;
    while (std::getline (f, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in (line);
        double on = 0.0, off = 0.0, pitch = 0.0;
        if (in >> on >> off >> pitch)
            refs.push_back ({ static_cast<int> (std::lround (pitch)), on, off, 0 });
    }
    return refs;
}

/// Chords in a note list: groups of notes whose starts lie within 80 ms of the group's first.
int chordsIn (const std::vector<Ref>& refs)
{
    int chords = 0;
    double groupStart = -1.0;
    int size = 0;
    auto close = [&] {
        if (size >= 2)
            ++chords;
    };
    auto sorted = refs;
    std::sort (sorted.begin(), sorted.end(),
               [] (const Ref& a, const Ref& b) { return a.start < b.start; });
    for (const auto& r : sorted)
    {
        if (groupStart < 0.0 || r.start - groupStart > 0.08)
        {
            close();
            groupStart = r.start;
            size = 0;
        }
        ++size;
    }
    close();
    return chords;
}

const char* noteName (int pitch)
{
    static char buf[8][8];
    static int at = 0;
    static const char* names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    at = (at + 1) % 8;
    std::snprintf (buf[at], sizeof (buf[at]), "%s%d", names[pitch % 12], pitch / 12 - 1);
    return buf[at];
}

Score synthAll (const Options& o, bool print)
{
    Score total, goNoGo;
    if (print)
        header();
    for (Kind kind : { Kind::sustained, Kind::strummed, Kind::arpeggio })
    {
        Score perKind;
        for (Pickup pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
        {
            Score s;
            for (int seed = 1; seed <= o.seeds; ++seed)
            {
                const Set set =
                    synthetic (kind, pickup, static_cast<std::uint32_t> (seed), o.count, 48000.0);
                const auto r = run (set.signal, set.fs, o.cfg);
                if (o.list)
                    std::printf ("  %s seed %d\n", set.name.c_str(), seed);
                s.add (
                    score (r.notes, set.notes, set.chords, o.match, o.list, o.cfg.window + 0.03));
            }
            if (print)
                row (std::string (kindName (kind)) + " " + pickupName (pickup), s);
            perKind.add (s);
        }
        if (print)
            row (std::string (kindName (kind)) + " all", perKind);
        total.add (perKind);
        if (kind != Kind::arpeggio)
            goNoGo.add (perKind);
    }
    if (print)
        row ("sustained + strummed", goNoGo);
    return goNoGo;
}

} // namespace

int main (int argc, char** argv)
{
    const std::string cmd = argc > 1 ? argv[1] : "";
    Options o = parse (argc, argv, 2);

    if (cmd == "synth")
    {
        synthAll (o, true);
        return 0;
    }
    if (cmd == "sweep")
    {
        header();
        for (double w : { 0.025, 0.035, 0.045, 0.06, 0.08, 0.10, 0.13, 0.17 })
        {
            Options at = o;
            at.cfg.window = w;
            at.match = std::max (o.match, w + 0.1);
            char name[64];
            std::snprintf (name, sizeof (name), "window %.0f ms", 1000.0 * w);
            row (name, synthAll (at, false));
        }
        return 0;
    }
    if (cmd == "wav" && !o.positional.empty())
    {
        auto audio = tools::readWav (o.positional[0]);
        if (!audio)
        {
            std::fprintf (stderr, "cannot read %s\n", o.positional[0].c_str());
            return 1;
        }
        const auto r = run (audio->mono, audio->sampleRate, o.cfg);
        std::printf ("%s: %.1f s at %.0f Hz, %zu notes, %.1f %% of real time\n",
                     o.positional[0].c_str(), audio->mono.size() / audio->sampleRate,
                     audio->sampleRate, r.notes.size(),
                     100.0 * r.seconds / (audio->mono.size() / audio->sampleRate));
        double group = -1.0;
        for (const auto& n : r.notes)
        {
            if (n.on < o.from || n.on > o.to)
                continue;
            if (group < 0.0 || n.on - group > 0.12)
            {
                std::printf ("\n%7.3f ", n.on);
                group = n.on;
            }
            std::printf (" %s(%.2f s)", noteName (n.pitch), n.off < 0.0 ? -1.0 : n.off - n.on);
        }
        std::printf ("\n");
        if (!o.truth.empty())
        {
            const auto refs = readTruth (o.truth);
            header();
            row ("recording",
                 score (r.notes, refs, chordsIn (refs), o.match, o.list, o.cfg.window + 0.03));
        }
        return 0;
    }
    if (cmd == "set" && !o.positional.empty())
    {
        // A manifest of "<wav> <notes>" lines, relative to the manifest; the kind is the style
        // in the recording's name (the letters after the first underscore), or "all".
        const std::string manifest = o.positional[0];
        const std::string dir = manifest.substr (0, manifest.find_last_of ('/') + 1);
        std::ifstream f (manifest);
        std::string line;
        std::map<std::string, Score> perKind;
        Score all;
        double audioSeconds = 0.0, cpuSeconds = 0.0;
        int index = 0;
        while (std::getline (f, line))
        {
            std::istringstream in (line);
            std::string wav, truth;
            if (!(in >> wav >> truth))
                continue;
            if (o.every > 1 && index++ % o.every != 0)
                continue;
            auto audio = tools::readWav (dir + wav);
            if (!audio)
            {
                std::fprintf (stderr, "cannot read %s\n", wav.c_str());
                continue;
            }
            const auto r = run (audio->mono, audio->sampleRate, o.cfg);
            audioSeconds += audio->mono.size() / audio->sampleRate;
            cpuSeconds += r.seconds;
            const auto refs = readTruth (dir + truth);
            std::string kind = wav.substr (wav.find_last_of ('/') + 1);
            kind = kind.substr (kind.find ('_') + 1);
            std::size_t letters = 0;
            while (letters < kind.size() &&
                   std::isalpha (static_cast<unsigned char> (kind[letters])))
                ++letters;
            kind = kind.substr (0, letters);
            if (o.list)
                std::printf ("  %s\n", wav.c_str());
            const Score s =
                score (r.notes, refs, chordsIn (refs), o.match, o.list, o.cfg.window + 0.03);
            if (o.files)
                row (wav.substr (wav.find_last_of ('/') + 1, 20), s);
            perKind[kind].add (s);
            all.add (s);
        }
        header();
        for (const auto& [kind, s] : perKind)
            row (kind, s);
        row ("all", all);
        std::printf ("%.0f s of audio, %.2f %% of real time\n", audioSeconds,
                     100.0 * cpuSeconds / std::max (1.0, audioSeconds));
        return 0;
    }
    if (cmd == "stable" && !o.positional.empty())
    {
        // The estimator alone on recordings: every 50 ms where at least two annotated notes
        // sound and none has started or stopped for `--settle` seconds (default the window plus
        // 30 ms), which notes does the frame say sound? One row per threshold.
        const std::vector<double> shares = { 0.12, 0.2, 0.3, 0.4, 0.5, 0.65 };
        std::vector<long> tp (shares.size(), 0), fp (shares.size(), 0), fn (shares.size(), 0),
            ctp (shares.size(), 0), cfp (shares.size(), 0), cfn (shares.size(), 0);
        const std::string manifest = o.positional[0];
        const std::string dir = manifest.substr (0, manifest.find_last_of ('/') + 1);
        std::ifstream f (manifest);
        std::string line;
        int index = 0;
        long frames = 0, chordFrames = 0, chordRight = 0, rootRight = 0;
        long confusion[3][3] = {};
        const double settle = o.settle > 0.0 ? o.settle : o.cfg.window + 0.03;
        while (std::getline (f, line))
        {
            std::istringstream in (line);
            std::string wav, truthPath;
            if (!(in >> wav >> truthPath))
                continue;
            if (o.every > 1 && index++ % o.every != 0)
                continue;
            auto audio = tools::readWav (dir + wav);
            if (!audio)
                continue;
            const auto refs = readTruth (dir + truthPath);
            poly::Estimator est (audio->sampleRate, o.cfg);
            std::vector<poly::NoteEvent> events;
            std::size_t at = 0;
            for (double t = 0.2; t < audio->mono.size() / audio->sampleRate; t += 0.05)
            {
                bool is[128] = {}, isClass[12] = {};
                int count = 0;
                bool moving = false;
                for (const auto& r : refs)
                {
                    if (r.pitch <= 0 || r.pitch > 127)
                        continue;
                    if ((r.start > t - settle && r.start < t + 0.02) ||
                        (r.end > t - settle && r.end < t + 0.02))
                        moving = true;
                    if (r.start <= t && t < r.end)
                    {
                        count += is[r.pitch] ? 0 : 1;
                        is[r.pitch] = isClass[r.pitch % 12] = true;
                    }
                }
                if (moving || count < 2)
                    continue;
                const auto to =
                    std::min (audio->mono.size(), static_cast<std::size_t> (t * audio->sampleRate));
                est.push (audio->mono.data() + at, static_cast<int> (to - at), events);
                at = to;
                ++frames;
                const auto& st = est.strengths();
                float top = 0.0f;
                for (float v : st)
                    top = std::max (top, v);
                {
                    // Root and quality: when the annotated pitch classes are exactly a major or
                    // minor triad or a bare fifth, is that the best of the 36 such chords for
                    // the strengths folded to pitch classes (inside the chord minus outside)?
                    int wantRoot = -1, wantKind = -1, classes = 0;
                    for (int k = 0; k < 12; ++k)
                        classes += isClass[k] ? 1 : 0;
                    static constexpr int kTones[3][3] = { { 0, 4, 7 }, { 0, 3, 7 }, { 0, 7, 7 } };
                    for (int root = 0; root < 12 && wantRoot < 0; ++root)
                        for (int kind = 0; kind < 3; ++kind)
                        {
                            const int size = kind == 2 ? 2 : 3;
                            if (classes == size && isClass[(root + kTones[kind][0]) % 12] &&
                                isClass[(root + kTones[kind][1]) % 12] &&
                                isClass[(root + kTones[kind][2]) % 12])
                            {
                                wantRoot = root;
                                wantKind = kind;
                                break;
                            }
                        }
                    if (wantRoot >= 0)
                    {
                        double fold[12] = {};
                        double total = 0.0;
                        for (int pitch = o.cfg.lowNote; pitch <= o.cfg.highNote; ++pitch)
                        {
                            fold[pitch % 12] += static_cast<double> (
                                st[static_cast<std::size_t> (pitch - o.cfg.lowNote)]);
                            total += static_cast<double> (
                                st[static_cast<std::size_t> (pitch - o.cfg.lowNote)]);
                        }
                        // The best triad; it is a bare fifth when its third stands under
                        // `--third` (default a quarter) of the mean of its root and fifth.
                        int bestRoot = -1, bestKind = -1;
                        double best = -1.0e9;
                        for (int root = 0; root < 12; ++root)
                            for (int kind = 0; kind < 2; ++kind)
                            {
                                const double inside = fold[root] + fold[(root + 7) % 12] +
                                                      fold[(root + kTones[kind][1]) % 12];
                                const double value = inside - (total - inside);
                                if (value > best)
                                {
                                    best = value;
                                    bestRoot = root;
                                    bestKind = kind;
                                }
                            }
                        if (fold[(bestRoot + kTones[bestKind][1]) % 12] <
                            o.third * 0.5 * (fold[bestRoot] + fold[(bestRoot + 7) % 12]))
                            bestKind = 2;
                        ++chordFrames;
                        chordRight += bestRoot == wantRoot && bestKind == wantKind;
                        rootRight += bestRoot == wantRoot;
                        if (bestRoot == wantRoot)
                            ++confusion[wantKind][bestKind];
                    }
                }
                for (std::size_t i = 0; i < shares.size(); ++i)
                {
                    bool saidClass[12] = {};
                    for (int pitch = o.cfg.lowNote; pitch <= o.cfg.highNote; ++pitch)
                    {
                        const bool said =
                            top > 0.0f && st[static_cast<std::size_t> (pitch - o.cfg.lowNote)] >=
                                              static_cast<float> (shares[i]) * top;
                        if (said)
                            saidClass[pitch % 12] = true;
                        tp[i] += said && is[pitch];
                        fp[i] += said && !is[pitch];
                        fn[i] += !said && is[pitch];
                    }
                    for (int k = 0; k < 12; ++k)
                    {
                        ctp[i] += saidClass[k] && isClass[k];
                        cfp[i] += saidClass[k] && !isClass[k];
                        cfn[i] += !saidClass[k] && isClass[k];
                    }
                }
            }
        }
        std::printf ("%ld settled frames\n", frames);
        std::printf ("root and quality (major, minor, fifth) on the %ld frames that are one: %.3f "
                     "right, root alone %.3f\n",
                     chordFrames,
                     chordFrames > 0 ? static_cast<double> (chordRight) / chordFrames : 0.0,
                     chordFrames > 0 ? static_cast<double> (rootRight) / chordFrames : 0.0);
        static const char* kinds[3] = { "major", "minor", "fifth" };
        for (int a = 0; a < 3; ++a)
            std::printf ("  played %s, root right: read major %ld, minor %ld, fifth %ld\n",
                         kinds[a], confusion[a][0], confusion[a][1], confusion[a][2]);
        for (std::size_t i = 0; i < shares.size(); ++i)
            std::printf (
                "share %.2f: notes P %.3f R %.3f F %.3f | pitch classes P %.3f R %.3f F %.3f\n",
                shares[i], Score::pOf (tp[i], fp[i]), Score::pOf (tp[i], fn[i]),
                Score::fOf (tp[i], fp[i], fn[i]), Score::pOf (ctp[i], cfp[i]),
                Score::pOf (ctp[i], cfn[i]), Score::fOf (ctp[i], cfp[i], cfn[i]));
        return 0;
    }
    if (cmd == "oracle")
    {
        // The estimator alone, without the note logic: at each strum's last onset plus the
        // window, which notes does the frame say sound, against those that do? One row per
        // threshold (share of the strongest note).
        const std::vector<double> shares = { 0.08, 0.12, 0.16, 0.2, 0.25, 0.3, 0.4, 0.5 };
        const double wait = o.cfg.decide < 0.0 ? o.cfg.window : o.cfg.decide;
        std::vector<int> allTp (shares.size(), 0), allFp (shares.size(), 0),
            allFn (shares.size(), 0), allOct (shares.size(), 0);
        const bool brief = !o.positional.empty() && o.positional[0] == "brief";
        for (Kind kind : { Kind::sustained, Kind::strummed, Kind::arpeggio })
            for (Pickup pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
            {
                std::vector<int> tp (shares.size(), 0), fp (shares.size(), 0),
                    fn (shares.size(), 0), oct (shares.size(), 0);
                for (int seed = 1; seed <= o.seeds; ++seed)
                {
                    const Set set = synthetic (kind, pickup, static_cast<std::uint32_t> (seed),
                                               o.count, 48000.0);
                    poly::Estimator est (set.fs, o.cfg);
                    std::vector<poly::NoteEvent> events;
                    std::size_t at = 0;
                    for (std::size_t k = 0; k < set.notes.size(); ++k)
                    {
                        // The last note of a strum: the next starts more than 80 ms later.
                        if (k + 1 < set.notes.size() &&
                            set.notes[k + 1].start - set.notes[k].start < 0.08)
                            continue;
                        const double t = set.notes[k].start + wait + 0.004;
                        const auto to =
                            std::min (set.signal.size(), static_cast<std::size_t> (t * set.fs));
                        if (to <= at)
                            continue;
                        est.push (set.signal.data() + at, static_cast<int> (to - at), events);
                        at = to;
                        std::vector<char> truth (128, 0);
                        for (const auto& r : set.notes)
                            if (r.start <= t && r.end > t)
                                truth[static_cast<std::size_t> (r.pitch)] = 1;
                        const auto& st = est.strengths();
                        float top = 0.0f;
                        for (float v : st)
                            top = std::max (top, v);
                        if (o.list)
                        {
                            std::printf ("  %.3f:", t);
                            for (int pitch = 0; pitch < 128; ++pitch)
                            {
                                const int idx = pitch - o.cfg.lowNote;
                                const float v = idx >= 0 && idx < static_cast<int> (st.size())
                                                    ? st[static_cast<std::size_t> (idx)] / top
                                                    : 0.0f;
                                if (truth[static_cast<std::size_t> (pitch)] != 0)
                                    std::printf (" [%s %.0f]", noteName (pitch),
                                                 100.0 * static_cast<double> (v));
                                else if (v > 0.1f)
                                    std::printf (" %s %.0f", noteName (pitch),
                                                 100.0 * static_cast<double> (v));
                            }
                            std::printf ("\n");
                        }
                        for (std::size_t i = 0; i < shares.size(); ++i)
                            for (int pitch = 0; pitch < 128; ++pitch)
                            {
                                const int idx = pitch - o.cfg.lowNote;
                                const bool said = idx >= 0 && idx < static_cast<int> (st.size()) &&
                                                  top > 0.0f &&
                                                  st[static_cast<std::size_t> (idx)] >=
                                                      static_cast<float> (shares[i]) * top;
                                const bool is = truth[static_cast<std::size_t> (pitch)] != 0;
                                tp[i] += said && is;
                                fn[i] += !said && is;
                                if (said && !is)
                                {
                                    ++fp[i];
                                    for (int d = 12; d <= 36; d += 12)
                                        if ((pitch - d >= 0 &&
                                             truth[static_cast<std::size_t> (pitch - d)]) ||
                                            (pitch + d < 128 &&
                                             truth[static_cast<std::size_t> (pitch + d)]))
                                        {
                                            ++oct[i];
                                            break;
                                        }
                                }
                            }
                    }
                }
                for (std::size_t i = 0; i < shares.size(); ++i)
                    if (kind != Kind::arpeggio)
                    {
                        allTp[i] += tp[i];
                        allFp[i] += fp[i];
                        allFn[i] += fn[i];
                        allOct[i] += oct[i];
                    }
                if (brief)
                    continue;
                std::printf ("%-10s %-7s", kindName (kind), pickupName (pickup));
                for (std::size_t i = 0; i < shares.size(); ++i)
                {
                    const double pr =
                        tp[i] + fp[i] > 0 ? static_cast<double> (tp[i]) / (tp[i] + fp[i]) : 1.0;
                    const double rc =
                        tp[i] + fn[i] > 0 ? static_cast<double> (tp[i]) / (tp[i] + fn[i]) : 1.0;
                    std::printf ("  %.2f: P%.2f R%.2f F%.2f", shares[i], pr, rc,
                                 pr + rc > 0.0 ? 2.0 * pr * rc / (pr + rc) : 0.0);
                }
                std::printf ("\n");
            }
        std::printf ("sust+strum all   ");
        for (std::size_t i = 0; i < shares.size(); ++i)
        {
            const double pr = allTp[i] + allFp[i] > 0
                                  ? static_cast<double> (allTp[i]) / (allTp[i] + allFp[i])
                                  : 1.0;
            const double rc = allTp[i] + allFn[i] > 0
                                  ? static_cast<double> (allTp[i]) / (allTp[i] + allFn[i])
                                  : 1.0;
            std::printf ("  %.2f: P%.2f R%.2f F%.3f o%d", shares[i], pr, rc,
                         pr + rc > 0.0 ? 2.0 * pr * rc / (pr + rc) : 0.0, allOct[i]);
        }
        std::printf ("\n");
        return 0;
    }
    if (cmd == "probe")
    {
        // probe <pickup 0..2> <fingers 0|1> <pitch> [<pitch> ...]: the notes struck together at
        // 0.1 s, the strengths the estimator reads at a few times after.
        const auto pickup = static_cast<Pickup> (std::atoi (o.positional[0].c_str()));
        const bool fingers = std::atoi (o.positional[1].c_str()) != 0;
        std::vector<float> signal (48000, 0.0f);
        for (std::size_t k = 2; k < o.positional.size(); ++k)
        {
            const int pitch = std::atoi (o.positional[k].c_str());
            const double hz = 440.0 * std::exp2 ((pitch - 69) / 12.0);
            Pluck p =
                fingers ? test::midi::fingerPluck (pickup, hz, 0.1f, static_cast<std::uint32_t> (k))
                        : test::midi::pluckFor (pickup, hz, 0.1f, static_cast<std::uint32_t> (k));
            p.seed = static_cast<std::uint32_t> (k) * 17u;
            addPluck (signal, 48000.0, 4800 + (k - 2) * 480, p);
        }
        addNoise (signal, -80.0f, 5);
        poly::Estimator est (48000.0, o.cfg);
        std::vector<poly::NoteEvent> events;
        std::size_t at = 0;
        for (double t : { 0.2, 0.3, 0.5, 0.8 })
        {
            const auto to = static_cast<std::size_t> (t * 48000.0);
            est.push (signal.data() + at, static_cast<int> (to - at), events);
            at = to;
            const auto& st = est.strengths();
            float top = 0.0f;
            for (float v : st)
                top = std::max (top, v);
            std::printf ("%.2f s:", t);
            for (std::size_t k = 0; k < st.size(); ++k)
                if (st[k] > 0.05f * top)
                    std::printf (" %s=%.0f%%", noteName (o.cfg.lowNote + static_cast<int> (k)),
                                 100.0 * static_cast<double> (st[k] / top));
            std::printf ("\n");
        }
        return 0;
    }
    if (cmd == "cpu")
    {
        const Set set = synthetic (Kind::strummed, Pickup::bridge, 1, 40, 48000.0);
        for (int block : { 64, 512 })
        {
            double best = 1.0e9;
            for (int k = 0; k < 5; ++k)
                best = std::min (best, run (set.signal, set.fs, o.cfg, block).seconds);
            poly::Estimator est (48000.0, o.cfg);
            std::printf ("window %.0f ms, block %d: %.2f %% of one core (%d atoms, %d bins, "
                         "analysis at %.0f Hz)\n",
                         1000.0 * o.cfg.window, block, 100.0 * best / (set.signal.size() / set.fs),
                         est.atoms(), est.bins(), est.analysisRate());
        }
        return 0;
    }
    if (cmd == "dump" && !o.positional.empty())
    {
        for (Kind kind : { Kind::sustained, Kind::strummed, Kind::arpeggio })
            for (Pickup pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
            {
                const Set set = synthetic (kind, pickup, 1, o.count, 48000.0);
                std::string base =
                    o.positional[0] + "/" + kindName (kind) + "-" + pickupName (pickup);
                tools::writeWav (base + ".wav", set.signal, set.signal, set.fs);
                std::ofstream notes (base + ".txt");
                for (const auto& n : set.notes)
                    notes << n.start << " " << n.end << " " << n.pitch << "\n";
            }
        return 0;
    }
    std::fprintf (stderr,
                  "usage: sedef-poly-bench synth|sweep|wav|set|cpu|dump (see the source)\n");
    return 1;
}
