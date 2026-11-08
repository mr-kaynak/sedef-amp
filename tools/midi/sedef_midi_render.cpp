// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Guitar-to-MIDI developer tool (docs/design/guitar-to-midi.md). Not shipped.
//
//   sedef-midi-render wav <in.wav> <out.mid> [--gain dB] [--octave n] [--steady] [--bend]
//                         [--gate dB] [--block n]
//       Converts a guitar DI recording as the plug-in would, writes a Standard MIDI File and
//       prints the notes.
//   sedef-midi-render scenarios <dir>
//       Renders the synthetic test scenarios: <name>.wav, <name>.mid (track 1 what was played,
//       track 2 what was sent) and a README.md with the scores.
//   sedef-midi-render bench
//       CPU cost per sample at 48 kHz, for the tracker alone and for the whole MIDI layer.

#include "../WavIo.h"
#include "midi/MidiTestSignals.h"
#include "sedef/analysis/MonoTracker.h"
#include "sedef/midi/GuitarToMidi.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace sedef;
using namespace sedef::test::midi;

namespace
{

// ---------------------------------------------------------------------------------------------
// Standard MIDI File, format 1, 1000 ticks per quarter at 120 bpm: a tick is half a millisecond.

constexpr int kTicksPerQuarter = 1000;
constexpr double kTicksPerSecond = 2000.0;

struct TimedEvent
{
    double seconds = 0.0;
    std::vector<std::uint8_t> bytes;
};

void putVarLen (std::vector<std::uint8_t>& out, std::uint32_t v)
{
    std::uint8_t buf[5];
    int n = 0;
    buf[n++] = static_cast<std::uint8_t> (v & 0x7F);
    while ((v >>= 7) != 0)
        buf[n++] = static_cast<std::uint8_t> (0x80 | (v & 0x7F));
    while (n > 0)
        out.push_back (buf[--n]);
}

std::vector<std::uint8_t> trackChunk (const std::string& name, std::vector<TimedEvent> events,
                                      bool tempo)
{
    std::stable_sort (events.begin(), events.end(), [] (const TimedEvent& a, const TimedEvent& b) {
        return a.seconds < b.seconds;
    });
    std::vector<std::uint8_t> body;
    putVarLen (body, 0);
    body.insert (body.end(), { 0xFF, 0x03, static_cast<std::uint8_t> (name.size()) });
    body.insert (body.end(), name.begin(), name.end());
    if (tempo)
    {
        putVarLen (body, 0);
        body.insert (body.end(), { 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20 }); // 500000 us
    }
    std::uint32_t last = 0;
    for (const auto& e : events)
    {
        const auto tick =
            static_cast<std::uint32_t> (std::lround (std::max (0.0, e.seconds) * kTicksPerSecond));
        putVarLen (body, tick - std::min (tick, last));
        last = std::max (tick, last);
        body.insert (body.end(), e.bytes.begin(), e.bytes.end());
    }
    putVarLen (body, 0);
    body.insert (body.end(), { 0xFF, 0x2F, 0x00 });
    std::vector<std::uint8_t> chunk { 'M', 'T', 'r', 'k' };
    const auto size = static_cast<std::uint32_t> (body.size());
    for (int shift = 24; shift >= 0; shift -= 8)
        chunk.push_back (static_cast<std::uint8_t> ((size >> shift) & 0xFF));
    chunk.insert (chunk.end(), body.begin(), body.end());
    return chunk;
}

void writeMidiFile (const std::string& path, const std::vector<std::vector<std::uint8_t>>& tracks)
{
    std::vector<std::uint8_t> out { 'M',
                                    'T',
                                    'h',
                                    'd',
                                    0,
                                    0,
                                    0,
                                    6,
                                    0,
                                    1,
                                    0,
                                    static_cast<std::uint8_t> (tracks.size()),
                                    static_cast<std::uint8_t> (kTicksPerQuarter >> 8),
                                    static_cast<std::uint8_t> (kTicksPerQuarter & 0xFF) };
    for (const auto& t : tracks)
        out.insert (out.end(), t.begin(), t.end());
    std::ofstream f (path, std::ios::binary);
    f.write (reinterpret_cast<const char*> (out.data()), static_cast<std::streamsize> (out.size()));
}

std::vector<TimedEvent> sentEvents (const Run& run)
{
    std::vector<TimedEvent> events;
    for (std::size_t i = 0; i < run.events.size(); ++i)
    {
        const auto& e = run.events[i];
        events.push_back ({ run.eventTimes[i], { e.bytes, e.bytes + e.size } });
    }
    return events;
}

std::vector<TimedEvent> playedEvents (const Scenario& sc)
{
    // On channel 2, so a piano roll can colour it apart.
    std::vector<TimedEvent> events;
    for (const auto& n : sc.notes)
    {
        const int velocity =
            std::clamp (static_cast<int> (std::lround (
                            127.0 * std::sqrt (static_cast<double> (n.strength) / 0.4))),
                        1, 127);
        events.push_back ({ n.start,
                            { 0x91, static_cast<std::uint8_t> (n.pitch),
                              static_cast<std::uint8_t> (velocity) } });
        events.push_back ({ n.end, { 0x81, static_cast<std::uint8_t> (n.pitch), 0 } });
    }
    return events;
}

void writeWavMono (const std::string& path, const std::vector<float>& x, double fs)
{
    tools::writeWav (path, x, x, fs);
}

const char* noteName (int note)
{
    static const char* names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return names[((note % 12) + 12) % 12];
}

// ---------------------------------------------------------------------------------------------

int convertWav (int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf (stderr, "usage: sedef-midi-render wav <in.wav> <out.mid> [options]\n");
        return 2;
    }
    const auto audio = tools::readWav (argv[2]);
    if (!audio)
    {
        std::fprintf (stderr, "cannot read %s (PCM/float WAV only)\n", argv[2]);
        return 1;
    }
    midi::Settings s;
    double gainDb = 0.0;
    int block = 256;
    for (int i = 4; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&] {
            return i + 1 < argc ? std::atof (argv[++i]) : 0.0;
        };
        if (a == "--gain")
            gainDb = next();
        else if (a == "--octave")
            s.octave = static_cast<int> (next());
        else if (a == "--steady")
            s.timing = midi::Timing::steady;
        else if (a == "--bend")
            s.mode = midi::Mode::notesAndBend;
        else if (a == "--gate")
            s.gateDb = static_cast<float> (next());
        else if (a == "--block")
            block = std::max (1, static_cast<int> (next()));
    }
    auto x = audio->mono;
    const auto g = static_cast<float> (std::pow (10.0, gainDb / 20.0));
    for (auto& v : x)
        v *= g;
    const auto run = convert (x, audio->sampleRate, s, block);
    writeMidiFile (argv[3], { trackChunk ("Sedef MIDI", sentEvents (run), true) });
    for (const auto& n : run.notes)
        std::printf ("%9.3f  %9.3f  %-2s%d (%3d)  vel %3d\n", n.on, n.off, noteName (n.pitch),
                     n.pitch / 12 - 1, n.pitch, n.velocity);
    std::printf ("%zu notes, at most %d sounding at once\n", run.notes.size(), run.maxSounding);
    return 0;
}

int trace (int argc, char** argv)
{
    // What MonoTracker reads, every millisecond between two times, next to what is sent.
    if (argc < 5)
    {
        std::fprintf (stderr,
                      "usage: sedef-midi-render trace <in.wav> <from s> <to s> [--gain dB]\n");
        return 2;
    }
    const auto audio = tools::readWav (argv[2]);
    if (!audio)
        return 1;
    const double from = std::atof (argv[3]), to = std::atof (argv[4]);
    const double gainDb = argc > 6 ? std::atof (argv[6]) : 0.0;
    const auto g = static_cast<float> (std::pow (10.0, gainDb / 20.0));
    const double fs = audio->sampleRate;
    analysis::MonoTracker m;
    m.prepare (fs);
    midi::GuitarToMidi conv;
    conv.prepare (fs);
    midi::Settings s;
    s.octave = 0;
    conv.setSettings (s);
    midi::EventList out;
    std::uint32_t picks = 0;
    const int ms = std::max (1, static_cast<int> (fs / 1000.0));
    for (std::size_t i = 0; i < audio->mono.size(); ++i)
    {
        const float x = g * audio->mono[i];
        m.push (x);
        out.clear();
        conv.process (&x, 1, out);
        const double t = static_cast<double> (i) / fs;
        const bool pick = m.pickCount() != picks;
        picks = m.pickCount();
        if (t < from || t > to)
            continue;
        const auto ev = m.event();
        if (pick || ev != analysis::MonoTracker::Event::none || !out.empty() ||
            i % static_cast<std::size_t> (ms) == 0)
        {
            std::printf ("t %.4f %s%s hz %7.2f (%5.2f) est %7.2f clar %.2f lvl %6.1f dB", t,
                         pick ? "PICK " : "",
                         ev == analysis::MonoTracker::Event::noteOn
                             ? "ON "
                             : (ev == analysis::MonoTracker::Event::noteChange
                                    ? "CHG "
                                    : (ev == analysis::MonoTracker::Event::noteOff ? "OFF " : "")),
                         static_cast<double> (m.hz()),
                         m.hz() > 0 ? 69 + 12 * std::log2 (m.hz() / 440.0) : 0.0,
                         static_cast<double> (m.estimateHz()), static_cast<double> (m.clarity()),
                         static_cast<double> (conv.levelDb()));
            for (const auto& e : out)
                std::printf ("  [%s %d v%d]", e.isNoteOn() ? "on" : (e.isNoteOff() ? "off" : "cc"),
                             e.note(), e.velocity());
            std::printf ("\n");
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------

struct Named
{
    std::string name;
    Scenario sc;
    midi::Settings settings;
};

midi::Settings plain()
{
    midi::Settings s;
    s.octave = 0;
    return s;
}

int scenarios (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: sedef-midi-render scenarios <dir>\n");
        return 2;
    }
    const std::string dir = argv[2];
    const double fs = 48000.0;
    std::vector<Named> list;
    const char* pickups[] = { "bridge", "neck", "piezo" };
    for (std::uint32_t seed = 1; seed <= 3; ++seed)
        for (int p = 0; p < 3; ++p)
            list.push_back ({ "bass-line-" + std::to_string (seed) + "-" + pickups[p],
                              bassLine (seed, static_cast<Pickup> (p), 120.0 + 4.0 * seed, fs),
                              plain() });
    {
        auto steady = plain();
        steady.timing = midi::Timing::steady;
        list.push_back (
            { "bass-line-1-bridge-steady", bassLine (1, Pickup::bridge, 124.0, fs), steady });
        auto bend = plain();
        bend.mode = midi::Mode::notesAndBend;
        list.push_back ({ "vibrato-a2-notes-and-bend", held (110.0, fs, 30.0, 0.0), bend });
    }
    list.push_back (
        { "palm-mutes-e2-sixteenths-140", palmMutedRun (82.407, 140.0, 4, fs), plain() });
    list.push_back ({ "palm-mutes-a2-eighths-120", palmMutedRun (110.0, 120.0, 2, fs), plain() });
    list.push_back ({ "hammer-ons-a2", legato (110.0, 200.0, 4, 0.15, fs), plain() });
    list.push_back ({ "pull-offs-d3", legato (146.83, -200.0, 4, 0.15, fs), plain() });
    list.push_back ({ "slide-up-a2", slide (110.0, 5, 0.05, fs), plain() });
    list.push_back ({ "over-ringing-e3", overRinging (164.81, -500.0, fs), plain() });

    std::string readme =
        "# Guitar-to-MIDI test renders\n\n"
        "Synthetic guitar DI scenarios from the engine's MIDI tests, rendered at 48 kHz in "
        "64-sample blocks with the octave at 0 (so what was played and what was sent sit on the "
        "same notes). Each `.wav` is the input; each `.mid` has two tracks: what was played "
        "(channel 2) and what Sedef MIDI sent (channel 1). Latency is from the pick to the "
        "note-on.\n\n"
        "| Scenario | Notes | Right | Wrong (octave) | Missed | Extra | Latency p50 / max (ms) |\n"
        "|---|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& n : list)
    {
        const auto run = convert (n.sc.signal, fs, n.settings);
        const auto st = score (run, n.sc, 0);
        writeWavMono (dir + "/" + n.name + ".wav", n.sc.signal, fs);
        writeMidiFile (dir + "/" + n.name + ".mid",
                       { trackChunk ("Played", playedEvents (n.sc), true),
                         trackChunk ("Sedef MIDI", sentEvents (run), false) });
        char line[256];
        std::snprintf (
            line, sizeof line, "| %s | %d | %.1f %% | %d (%d) | %d | %d | %.1f / %.1f |\n",
            n.name.c_str(), st.expected, 100.0 * st.correctShare(), st.wrongPitch, st.octaveErrors,
            st.missed, st.extra, percentile (st.latencyMs, 0.5),
            st.latencyMs.empty() ? 0.0
                                 : *std::max_element (st.latencyMs.begin(), st.latencyMs.end()));
        readme += line;
        std::printf ("%s", line);
    }
    std::ofstream (dir + "/README.md") << readme;
    return 0;
}

// ---------------------------------------------------------------------------------------------

int bench()
{
    const double fs = 48000.0;
    std::vector<float> x;
    for (std::uint32_t seed = 1; seed <= 4; ++seed)
    {
        const auto sc = bassLine (seed, seed % 2 == 0 ? Pickup::piezo : Pickup::bridge, 132.0, fs);
        x.insert (x.end(), sc.signal.begin(), sc.signal.end());
    }
    const auto samples = static_cast<double> (x.size());
    const auto timeIt = [&] (auto&& body) {
        double best = 1.0e30;
        for (int round = 0; round < 5; ++round)
        {
            const auto t0 = std::chrono::steady_clock::now();
            body();
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min (best, std::chrono::duration<double, std::nano> (t1 - t0).count());
        }
        return best / samples;
    };
    analysis::MonoTracker tracker;
    tracker.prepare (fs);
    volatile float sink = 0.0f;
    const double trackerNs = timeIt ([&] {
        tracker.reset();
        for (float v : x)
            tracker.push (v);
        sink = sink + tracker.hz();
    });
    std::printf ("%-34s %7.1f ns/sample  (%.2f %% of one core at 48 kHz)\n", "MonoTracker alone",
                 trackerNs, trackerNs * fs * 1.0e-7);
    for (auto timing : { midi::Timing::fastest, midi::Timing::steady })
        for (auto mode : { midi::Mode::notes, midi::Mode::notesAndBend })
        {
            midi::GuitarToMidi g;
            g.prepare (fs);
            midi::Settings s;
            s.timing = timing;
            s.mode = mode;
            g.setSettings (s);
            midi::EventList out;
            const double ns = timeIt ([&] {
                g.reset();
                for (std::size_t i = 0; i + 256 <= x.size(); i += 256)
                {
                    out.clear();
                    g.process (x.data() + i, 256, out);
                }
                sink = sink + static_cast<float> (g.notesSent());
            });
            char name[64];
            std::snprintf (name, sizeof name, "GuitarToMidi %s, %s",
                           timing == midi::Timing::fastest ? "Fastest" : "Steady",
                           mode == midi::Mode::notes ? "Notes" : "Notes + Bend");
            std::printf ("%-34s %7.1f ns/sample  (%.2f %% of one core at 48 kHz)\n", name, ns,
                         ns * fs * 1.0e-7);
        }
    return 0;
}

} // namespace

int main (int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "wav")
        return convertWav (argc, argv);
    if (mode == "scenarios")
        return scenarios (argc, argv);
    if (mode == "bench")
        return bench();
    if (mode == "trace")
        return trace (argc, argv);
    std::fprintf (stderr, "usage: sedef-midi-render wav|scenarios|bench ...\n");
    return 2;
}
