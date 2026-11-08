// SPDX-License-Identifier: AGPL-3.0-or-later
// Accuracy and latency of the guitar-to-MIDI layer over every synthetic scenario, printed as
// tables (docs/design/guitar-to-midi.md, "Measurements"). Hidden: run with "[.midireport]".
#include "MidiTestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>

using namespace sedef;
using namespace sedef::test::midi;

namespace
{
void line (const char* name, const Score& s)
{
    std::printf ("%-34s %4d  %6.2f%%  wrong %2d  oct %2d  missed %2d  extra %2d  lateOff %2d  "
                 "earlyOff %2d  lat p50 %5.1f p99 %5.1f max %5.1f  legato p50 %5.1f\n",
                 name, s.expected, 100.0 * s.correctShare(), s.wrongPitch, s.octaveErrors, s.missed,
                 s.extra, s.lateOff, s.earlyOff, percentile (s.latencyMs, 0.5),
                 percentile (s.latencyMs, 0.99), percentile (s.latencyMs, 1.0),
                 percentile (s.legatoLatencyMs, 0.5));
}

const Scenario* current = nullptr;

void add (Score& total, const Score& s)
{
    if (environment ("SEDEF_MIDI_MISSES") != nullptr)
        for (const auto& [r, n] : s.wrongs)
            std::printf ("    wrong: played %3d at %.3f, sent %3d at %.3f for %.1f ms\n", r.pitch,
                         r.start, n.pitch, n.on, 1000.0 * (n.off - n.on));
    if (environment ("SEDEF_MIDI_MISSES") != nullptr && current != nullptr)
        for (const auto& m : s.misses)
        {
            int before = -1;
            double gap = 0.0;
            for (const auto& r : current->notes)
                if (r.start < m.start - 0.001)
                {
                    before = r.pitch;
                    gap = m.start - r.start;
                }
            std::printf ("    missed %3d at %.3f (%s, %.2f) after %3d %.0f ms before\n", m.pitch,
                         m.start, m.picked ? "pick" : "legato", static_cast<double> (m.strength),
                         before, 1000.0 * gap);
        }
    if (environment ("SEDEF_MIDI_EXTRAS") != nullptr && current != nullptr)
        for (const auto& e : s.extras)
        {
            // The played note sounding when it came, and the one before.
            int now = -1, before = -1;
            double since = 0.0;
            for (const auto& r : current->notes)
                if (r.start <= e.on + 0.002)
                {
                    before = now;
                    now = r.pitch;
                    since = e.on - r.start;
                }
            std::printf ("    extra %3d at %.3f for %5.1f ms: played %3d (%+3d) %5.1f ms ago, "
                         "before it %3d (%+3d)\n",
                         e.pitch, e.on, 1000.0 * (e.off - e.on), now, e.pitch - now, 1000.0 * since,
                         before, e.pitch - before);
        }
    total.expected += s.expected;
    total.correct += s.correct;
    total.wrongPitch += s.wrongPitch;
    total.octaveErrors += s.octaveErrors;
    total.missed += s.missed;
    total.extra += s.extra;
    total.lateOff += s.lateOff;
    total.earlyOff += s.earlyOff;
    total.latencyMs.insert (total.latencyMs.end(), s.latencyMs.begin(), s.latencyMs.end());
    total.legatoLatencyMs.insert (total.legatoLatencyMs.end(), s.legatoLatencyMs.begin(),
                                  s.legatoLatencyMs.end());
}
} // namespace

TEST_CASE ("Guitar-to-MIDI report", "[.midireport]")
{
    const double fs = 48000.0;
    midi::Settings s;
    s.octave = 0;
    Score total;
    char name[96];
    const bool ring = environment ("SEDEF_MIDI_NO_RING") == nullptr;
    for (auto pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
        for (std::uint32_t seed = 1; seed <= 6; ++seed)
        {
            const double bpm = 120.0 + 4.0 * seed;
            const auto sc = bassLine (seed, pickup, bpm, fs, 48, ring);
            current = &sc;
            const auto st = score (convert (sc.signal, fs, s), sc, 0);
            std::snprintf (name, sizeof name, "bass line %d %s %.0f bpm", static_cast<int> (seed),
                           pickup == Pickup::bridge ? "bridge"
                                                    : (pickup == Pickup::neck ? "neck" : "piezo"),
                           bpm);
            line (name, st);
            add (total, st);
        }
    for (double hz : { kE2, kA2 })
        for (int perBeat : { 2, 4 })
            for (double bpm : { 120.0, 140.0 })
                for (auto pickup : { Pickup::bridge, Pickup::piezo })
                {
                    const auto sc = palmMutedRun (hz, bpm, perBeat, fs, 16, pickup);
                    current = &sc;
                    const auto st = score (convert (sc.signal, fs, s), sc, 0);
                    std::snprintf (name, sizeof name, "palm %s %d/beat %.0f %s",
                                   hz < 100 ? "E2" : "A2", perBeat, bpm,
                                   pickup == Pickup::bridge ? "bridge" : "piezo");
                    line (name, st);
                    add (total, st);
                }
    for (double hz : { kA2, kD3, kE3 })
        for (double cents : { 200.0, -200.0, 100.0 })
        {
            const auto sc = legato (hz, cents, 4, 0.15, fs);
            current = &sc;
            const auto st = score (convert (sc.signal, fs, s), sc, 0);
            std::snprintf (name, sizeof name, "legato %.0f Hz %+.0f", hz, cents);
            line (name, st);
            add (total, st);
        }
    for (double hz : { kA2, kE3 })
        for (int semis : { 5, -5 })
        {
            const auto sc = slide (hz, semis, 0.05, fs);
            current = &sc;
            const auto st = score (convert (sc.signal, fs, s), sc, 0);
            std::snprintf (name, sizeof name, "slide %.0f Hz %+d", hz, semis);
            line (name, st);
            add (total, st);
        }
    for (double hz : { kE2, kA2, kE3, kA3 })
        for (int kind = 0; kind < 2; ++kind)
        {
            const auto sc = held (hz, fs, kind == 0 ? 30.0 : 0.0, kind == 1 ? 45.0 : 0.0);
            current = &sc;
            const auto st = score (convert (sc.signal, fs, s), sc, 0);
            std::snprintf (name, sizeof name, "%s %.0f Hz", sc.name.c_str(), hz);
            line (name, st);
            add (total, st);
        }
    for (double hz : { kE2, kE3, kA3 })
        for (double cents : { -500.0, 700.0 })
        {
            const auto sc = overRinging (hz, cents, fs);
            current = &sc;
            const auto st = score (convert (sc.signal, fs, s), sc, 0);
            std::snprintf (name, sizeof name, "ringing %.0f Hz %+.0f", hz, cents);
            line (name, st);
            add (total, st);
        }
    line ("TOTAL", total);
    for (double lag : { 30.0, 32.0, 34.0, 36.0, 38.0, 40.0 })
    {
        int within = 0;
        for (double v : total.latencyMs)
            within += v <= lag ? 1 : 0;
        std::printf ("  picked notes on within %.0f ms: %.2f%%\n", lag,
                     100.0 * within / static_cast<double> (total.latencyMs.size()));
    }
}

namespace
{
/// Pick to note-on (ms) for single notes; the tracker's own lock for the same note.
struct Latency
{
    std::vector<double> midi, tracker;
};

Latency latencyFor (double hz, Pickup pickup, double fs, const midi::Settings& s, int block,
                    int seeds = 8)
{
    Latency l;
    for (int seed = 1; seed <= seeds; ++seed)
    {
        Scenario sc;
        sc.fs = fs;
        sc.signal.assign (static_cast<std::size_t> (0.6 * fs), 0.0f);
        auto p = pluckFor (pickup, hz, static_cast<float> (0.1 + 0.03 * seed),
                           static_cast<std::uint32_t> (seed * 31));
        test::addPluck (sc.signal, fs, static_cast<std::size_t> (0.1 * fs), p);
        test::addNoise (sc.signal, -80.0f, static_cast<std::uint32_t> (seed));
        const auto run = convert (sc.signal, fs, s, block);
        for (const auto& n : run.notes)
            if (n.pitch == pitchOf (hz) + 12 * s.octave)
            {
                l.midi.push_back (1000.0 * (n.on - 0.1));
                break;
            }
        analysis::MonoTracker m;
        m.prepare (fs);
        for (std::size_t i = 0; i < sc.signal.size(); ++i)
        {
            m.push (sc.signal[i]);
            if (m.event() == analysis::MonoTracker::Event::noteOn)
            {
                l.tracker.push_back (1000.0 * (static_cast<double> (i + 1) / fs - 0.1));
                break;
            }
        }
    }
    return l;
}
} // namespace

TEST_CASE ("Guitar-to-MIDI latency report", "[.midireport]")
{
    const double fs = 48000.0;
    const struct
    {
        const char* name;
        double hz;
    } strings[] = { { "E2", kE2 },     { "A2", kA2 }, { "D3", kD3 }, { "G3", 195.998 },
                    { "B3", 246.942 }, { "E4", kE4 }, { "E5", kE5 } };
    std::printf ("\nstring  pickup  timing   block  tracker lock p50/max   note-on p50/max (ms)\n");
    for (auto timing : { midi::Timing::fastest, midi::Timing::steady })
        for (int block : { 64, 256 })
            for (const auto& str : strings)
                for (auto pickup : { Pickup::bridge, Pickup::piezo })
                {
                    midi::Settings s;
                    s.octave = 0;
                    s.timing = timing;
                    const auto l = latencyFor (str.hz, pickup, fs, s, block);
                    std::printf (
                        "%-6s  %-6s  %-7s  %5d  %6.1f / %6.1f        %6.1f / %6.1f   (%zu/8)\n",
                        str.name, pickup == Pickup::bridge ? "bridge" : "piezo",
                        timing == midi::Timing::fastest ? "Fastest" : "Steady", block,
                        percentile (l.tracker, 0.5), percentile (l.tracker, 1.0),
                        percentile (l.midi, 0.5), percentile (l.midi, 1.0), l.midi.size());
                }
}
