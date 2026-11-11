// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Guitar-to-MIDI developer tool (docs/design/guitar-to-midi.md). Not shipped.
//
//   sedef-midi-render wav <in.wav> <out.mid> [--gain dB] [--octave n] [--steady] [--bend]
//                         [--gate dB] [--block n] [--events]
//       Converts a guitar DI recording as the plug-in would, writes a Standard MIDI File and
//       prints the notes (with --events: every event sent, pitch bends too).
//   sedef-midi-render scenarios <dir>
//       Renders the synthetic test scenarios: <name>.wav, <name>.mid (track 1 what was played,
//       track 2 what was sent) and a README.md with the scores.
//   sedef-midi-render bench
//       CPU cost per sample at 48 kHz, for the tracker alone and for the whole MIDI layer.
//   sedef-midi-render why <in.wav> [--gain dB] [--from s] [--to s] [--steady 1] [--gate dB]
//                         [--octave n]
//       Lists every note with why it started and ended, and how its pick was seen.
//   sedef-midi-render sets [--low auto|e2|b1|f#1] [--learned b1|f#1] [--shift semitones]
//                          [--steady] [--bend] [--extras] [--misses]
//       The test suite's synthetic sets with one range setting, on strings tuned `shift`
//       semitones away (-5: a seven-string's low strings, -10: an eight-string's): scores, and
//       the latency of every note.
//   sedef-midi-render latency
//       Pick to note-on for every note from E1 to E4 in each range.
//   sedef-midi-render provisional [--low ...] [--shift n] [--wav in.wav --truth notes.txt]
//       What sending each note at the tracker's first reading after its pick would buy (how much
//       sooner) and cost (how often that reading is wrong).
//   sedef-midi-render bends [--range n]
//       Synthetic bends, slides and vibrato in both modes: the notes and bends sent, and how far
//       what sounds is from the string's pitch.
//   sedef-midi-render trace <in.wav> <from s> <to s> [--gain dB]
//       What the tracker reads, every millisecond, next to what is sent.

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

midi::LowNote lowNoteNamed (const std::string& name)
{
    return name == "e2"    ? midi::LowNote::e2
           : name == "b1"  ? midi::LowNote::b1
           : name == "f#1" ? midi::LowNote::fSharp1
                           : midi::LowNote::automatic;
}

/// The value after `flag` in the arguments, or `fallback`.
std::string option (int argc, char** argv, const char* flag, const char* fallback = "")
{
    for (int i = 2; i + 1 < argc; ++i)
        if (std::string (argv[i]) == flag)
            return argv[i + 1];
    return fallback;
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
    bool listEvents = false;
    for (int i = 4; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&] {
            return i + 1 < argc ? std::atof (argv[++i]) : 0.0;
        };
        if (a == "--events")
            listEvents = true;
        else if (a == "--gain")
            gainDb = next();
        else if (a == "--octave")
            s.octave = static_cast<int> (next());
        else if (a == "--steady")
            s.timing = midi::Timing::steady;
        else if (a == "--bend")
            s.mode = midi::Mode::notesAndBend;
        else if (a == "--gate")
        {
            s.gateMode = midi::GateMode::manual;
            s.gateDb = static_cast<float> (next());
        }
        else if (a == "--block")
            block = std::max (1, static_cast<int> (next()));
    }
    auto x = audio->mono;
    const auto g = static_cast<float> (std::pow (10.0, gainDb / 20.0));
    for (auto& v : x)
        v *= g;
    const auto run = convert (x, audio->sampleRate, s, block);
    writeMidiFile (argv[3], { trackChunk ("Sedef MIDI", sentEvents (run), true) });
    if (listEvents)
    {
        for (std::size_t k = 0; k < run.events.size(); ++k)
        {
            const auto& e = run.events[k];
            if (e.isNoteOn())
                std::printf ("%10.4f on   %3d %3d\n", run.eventTimes[k], e.note(), e.velocity());
            else if (e.isNoteOff())
                std::printf ("%10.4f off  %3d\n", run.eventTimes[k], e.note());
            else if (e.isPitchBend())
                std::printf ("%10.4f bend %5d\n", run.eventTimes[k],
                             e.bendValue() - midi::kBendCentre);
            else
                std::printf ("%10.4f cc   %3d %3d\n", run.eventTimes[k], e.bytes[1], e.bytes[2]);
        }
        return 0;
    }
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
    const double gainDb = std::atof (option (argc, argv, "--gain", "0").c_str());
    const auto g = static_cast<float> (std::pow (10.0, gainDb / 20.0));
    const double fs = audio->sampleRate;
    midi::GuitarToMidi conv;
    midi::Settings s;
    s.octave = 0;
    s.lowNote = lowNoteNamed (option (argc, argv, "--low"));
    if (option (argc, argv, "--bend") == "1")
        s.mode = midi::Mode::notesAndBend;
    conv.setSettings (s);
    conv.prepare (fs);
    conv.setLearnedLowNote (lowNoteNamed (option (argc, argv, "--learned", "e2")));
    const auto& m = conv.tracker();
    midi::EventList out;
    std::uint32_t picks = 0;
    const int ms = std::max (1, static_cast<int> (fs / 1000.0));
    for (std::size_t i = 0; i < audio->mono.size(); ++i)
    {
        const float x = g * audio->mono[i];
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
            std::printf ("t %.4f %s%s hz %7.2f (%5.2f) est %7.2f clar %.2f lvl %6.1f dB gate %6.1f "
                         "note %3d",
                         t, pick ? "PICK " : "",
                         ev == analysis::MonoTracker::Event::noteOn
                             ? "ON "
                             : (ev == analysis::MonoTracker::Event::noteChange
                                    ? "CHG "
                                    : (ev == analysis::MonoTracker::Event::noteOff ? "OFF " : "")),
                         static_cast<double> (m.hz()),
                         m.hz() > 0 ? 69 + 12 * std::log2 (m.hz() / 440.0) : 0.0,
                         static_cast<double> (m.estimateHz()), static_cast<double> (m.clarity()),
                         static_cast<double> (conv.levelDb()),
                         static_cast<double> (conv.gateLevelDb()), conv.note());
            for (const auto& e : out)
                std::printf ("  [%s %d v%d]", e.isNoteOn() ? "on" : (e.isNoteOff() ? "off" : "cc"),
                             e.note(), e.velocity());
            std::printf ("\n");
        }
    }
    return 0;
}

const char* whyName (midi::Why w)
{
    switch (w)
    {
    case midi::Why::none:
        return "-";
    case midi::Why::pick:
        return "pick";
    case midi::Why::correction:
        return "correction";
    case midi::Why::legato:
        return "legato";
    case midi::Why::repick:
        return "repick";
    case midi::Why::replaced:
        return "replaced";
    case midi::Why::unvoiced:
        return "unvoiced";
    case midi::Why::gate:
        return "gate";
    case midi::Why::mute:
        return "mute";
    case midi::Why::damped:
        return "damped";
    case midi::Why::decayed:
        return "decayed";
    case midi::Why::other:
        return "other";
    }
    return "?";
}

const char* pickName (midi::PickKind k)
{
    switch (k)
    {
    case midi::PickKind::none:
        return "-";
    case midi::PickKind::detector:
        return "detector";
    case midi::PickKind::weak:
        return "weak";
    case midi::PickKind::lock:
        return "lock";
    }
    return "?";
}

int why (int argc, char** argv)
{
    // Every note sent (Fastest, octave 0), with how it started and ended and what the tracker
    // read when it did: the diagnosis of extra and split notes.
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: sedef-midi-render why <in.wav> [--gain dB] [--from s] "
                              "[--to s] [--steady 1] [--gate dB] [--octave n]\n");
        return 2;
    }
    const auto audio = tools::readWav (argv[2]);
    if (!audio)
        return 1;
    double gainDb = 0.0, from = 0.0, to = 1.0e9;
    midi::Settings s;
    s.octave = 0;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        const std::string a = argv[i];
        if (a == "--gain")
            gainDb = std::atof (argv[i + 1]);
        else if (a == "--from")
            from = std::atof (argv[i + 1]);
        else if (a == "--to")
            to = std::atof (argv[i + 1]);
        else if (a == "--steady" && std::atoi (argv[i + 1]) != 0)
            s.timing = midi::Timing::steady;
        else if (a == "--gate")
        {
            s.gateMode = midi::GateMode::manual;
            s.gateDb = static_cast<float> (std::atof (argv[i + 1]));
        }
        else if (a == "--octave")
            s.octave = std::atoi (argv[i + 1]);
        else if (a == "--low")
            s.lowNote = lowNoteNamed (argv[i + 1]);
    }
    const auto g = static_cast<float> (std::pow (10.0, gainDb / 20.0));
    const double fs = audio->sampleRate;
    midi::GuitarToMidi conv;
    conv.setSettings (s);
    conv.prepare (fs);
    conv.setLearnedLowNote (lowNoteNamed (option (argc, argv, "--learned", "e2")));
    struct Open
    {
        double on = -1.0;
        int velocity = 0;
        std::string start;
    };
    std::vector<Open> open (128);
    midi::EventList out;
    std::printf ("    on       off     ms  note        vel  started                 ended\n");
    for (std::size_t i = 0; i < audio->mono.size(); ++i)
    {
        const float x = g * audio->mono[i];
        out.clear();
        conv.process (&x, 1, out);
        const double t = static_cast<double> (i) / fs;
        for (const auto& e : out)
        {
            if (e.isNoteOn())
            {
                auto& o = open[static_cast<std::size_t> (e.note())];
                o.on = t;
                o.velocity = e.velocity();
                o.start = std::string (whyName (conv.startWhy()));
                if (conv.startWhy() == midi::Why::pick || conv.startWhy() == midi::Why::repick ||
                    conv.startWhy() == midi::Why::correction)
                    o.start += std::string ("/") + pickName (conv.pickKind());
            }
            else if (e.isNoteOff())
            {
                auto& o = open[static_cast<std::size_t> (e.note())];
                if (o.on >= 0.0 && o.on >= from && o.on <= to)
                    std::printf ("%8.3f  %8.3f  %4.0f  %-2s%d (%3d)  %3d  %-22s  %s\n", o.on, t,
                                 1000.0 * (t - o.on), noteName (e.note()), e.note() / 12 - 1,
                                 e.note(), o.velocity, o.start.c_str(), whyName (conv.endWhy()));
                o.on = -1.0;
            }
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
    for (std::uint32_t seed = 1; seed <= 6; ++seed)
        for (int p = 0; p < 3; ++p)
            list.push_back ({ "sloppy-line-" + std::to_string (seed) + "-" + pickups[p],
                              sloppyLine (seed, static_cast<Pickup> (p), 110.0 + 5.0 * seed, fs),
                              plain() });
    for (std::uint32_t seed = 1; seed <= 4; ++seed)
        for (int p = 0; p < 3; ++p)
            list.push_back ({ "fret-then-pick-" + std::to_string (seed) + "-" + pickups[p],
                              fretThenPick (seed, static_cast<Pickup> (p), fs), plain() });

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
        {
            std::ofstream ref (dir + "/" + n.name + ".ref.txt");
            for (const auto& r : n.sc.notes)
                ref << r.start << " " << r.end << " " << r.pitch << " " << (r.picked ? 1 : 0)
                    << "\n";
        }
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
// The synthetic sets of the test suite, for one range setting and one tuning.

int sets (int argc, char** argv)
{
    const double fs = 48000.0;
    midi::Settings s;
    s.octave = 0;
    auto learned = midi::LowNote::e2;
    int shift = 0;
    bool extras = false, misses = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        const std::string next = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--low")
            s.lowNote = lowNoteNamed (next);
        else if (a == "--learned")
            learned = lowNoteNamed (next);
        else if (a == "--shift")
            shift = std::atoi (next.c_str());
        else if (a == "--steady")
            s.timing = midi::Timing::steady;
        else if (a == "--bend")
            s.mode = midi::Mode::notesAndBend;
        else if (a == "--extras")
            extras = true;
        else if (a == "--misses")
            misses = true;
    }
    const bool steady = s.timing == midi::Timing::steady;
    const double window = steady ? 0.12 : 0.08;
    struct Total
    {
        Score score;
        int shorts = 0, under = 0, sent = 0, widened[4] = { 0, 0, 0, 0 };
        std::vector<double> perPitch[128];
    };
    const int lowest = 40 + shift;
    const std::string wavDir = option (argc, argv, "--wav");
    int wavIndex = 0;
    const auto run = [&] (Total& t, const Scenario& sc, bool judgeEnds) {
        if (!wavDir.empty())
            writeWavMono (wavDir + "/" + std::to_string (wavIndex++) + ".wav", sc.signal, fs);
        const auto r = convert (sc.signal, fs, s, 64, learned);
        const auto st = score (r, sc, 0, window, judgeEnds);
        ++t.widened[static_cast<int> (r.range)];
        t.score.expected += st.expected;
        t.score.correct += st.correct;
        t.score.wrongPitch += st.wrongPitch;
        t.score.octaveErrors += st.octaveErrors;
        t.score.missed += st.missed;
        t.score.extra += st.extra;
        t.score.earlyOff += st.earlyOff;
        t.score.lateOff += st.lateOff;
        t.score.latencyMs.insert (t.score.latencyMs.end(), st.latencyMs.begin(),
                                  st.latencyMs.end());
        for (const auto& n : r.notes)
        {
            ++t.sent;
            t.shorts += n.off >= 0.0 && 1000.0 * (n.off - n.on) < 40.0 ? 1 : 0;
            t.under += n.pitch < lowest ? 1 : 0;
        }
        for (const auto& ref : sc.notes)
            for (const auto& n : r.notes)
                if (n.pitch == ref.pitch && n.on >= ref.start - 0.002 && n.on <= ref.start + window)
                {
                    t.perPitch[ref.pitch].push_back (1000.0 * (n.on - ref.start));
                    break;
                }
        if (extras)
            for (const auto& e : st.extras)
                std::printf ("    extra %s%d at %.3f for %.1f ms\n", noteName (e.pitch),
                             e.pitch / 12 - 1, e.on, 1000.0 * (e.off - e.on));
        if (misses)
        {
            for (const auto& m : st.misses)
                std::printf ("    missed %s%d at %.3f\n", noteName (m.pitch), m.pitch / 12 - 1,
                             m.start);
            for (const auto& [ref, n] : st.wrongs)
                std::printf ("    wrong: played %s%d at %.3f, sent %s%d at %.3f for %.1f ms\n",
                             noteName (ref.pitch), ref.pitch / 12 - 1, ref.start,
                             noteName (n.pitch), n.pitch / 12 - 1, n.on, 1000.0 * (n.off - n.on));
        }
    };
    const auto print = [&] (const char* name, const Total& t) {
        std::printf ("%-16s %4d notes  %6.2f%% right  %2d wrong (%d octave)  %2d missed  %2d extra "
                     "(%.2f/100)  %2d short (%.2f/100)  %d ended early, %d late  %d under the "
                     "lowest string  latency p50 "
                     "%.1f p95 %.1f max %.1f ms  range at the end: E2 %d, B1 %d, F#1 %d\n",
                     name, t.score.expected, 100.0 * t.score.correctShare(), t.score.wrongPitch,
                     t.score.octaveErrors, t.score.missed, t.score.extra, t.score.extraPer100(),
                     t.shorts, 100.0 * t.shorts / std::max (1, t.score.expected), t.score.earlyOff,
                     t.score.lateOff, t.under, percentile (t.score.latencyMs, 0.5),
                     percentile (t.score.latencyMs, 0.95), percentile (t.score.latencyMs, 1.0),
                     t.widened[1], t.widened[2], t.widened[3]);
    };
    const Pickup pickups[] = { Pickup::bridge, Pickup::neck, Pickup::piezo };
    Total bass, sloppy, fretted, silent, all;
    for (auto pickup : pickups)
        for (std::uint32_t seed = 1; seed <= 6; ++seed)
        {
            if (extras || misses)
                std::printf ("  bass %u %d (wav %d)\n", seed, static_cast<int> (pickup), wavIndex);
            run (bass,
                 lowered (
                     shift,
                     [&] { return bassLine (seed, pickup, 120.0 + 4.0 * seed, fs, 48, false); }),
                 true);
            if (extras || misses)
                std::printf ("  sloppy %u %d (wav %d)\n", seed, static_cast<int> (pickup),
                             wavIndex);
            run (sloppy,
                 lowered (shift,
                          [&] { return sloppyLine (seed, pickup, 110.0 + 5.0 * seed, fs, 48); }),
                 false);
        }
    for (auto pickup : pickups)
        for (std::uint32_t seed = 1; seed <= 4; ++seed)
        {
            if (extras || misses)
                std::printf ("  fretted %u %d\n", seed, static_cast<int> (pickup));
            run (fretted, lowered (shift, [&] { return fretThenPick (seed, pickup, fs); }), false);
        }
    for (int tuning = 0; tuning < 3; ++tuning)
        for (auto pickup : { Pickup::bridge, Pickup::neck })
            for (std::uint32_t seed = 1; seed <= 3; ++seed)
            {
                if (extras || misses)
                    std::printf ("  silence %d %u %d\n", tuning, seed, static_cast<int> (pickup));
                run (silent,
                     lowered (shift, [&] { return fromSilence (seed, tuning, pickup, fs); }), true);
            }
    print ("bass lines", bass);
    print ("sloppy lines", sloppy);
    print ("fretted first", fretted);
    print ("from silence", silent);
    int chordNotes = 0, chordUnder = 0, chordRange[4] = { 0, 0, 0, 0 };
    for (auto pickup : pickups)
        for (std::uint32_t seed = 1; seed <= 4; ++seed)
        {
            const auto sc = lowered (shift, [&] { return strummedChords (seed, pickup, fs); });
            const auto r = convert (sc.signal, fs, s, 64, learned);
            chordNotes += static_cast<int> (r.notes.size());
            ++chordRange[static_cast<int> (r.range)];
            for (const auto& n : r.notes)
                chordUnder += n.pitch < lowest ? 1 : 0;
        }
    std::printf ("%-16s  144 chords, %d notes, %d under the lowest string  range at the end: E2 "
                 "%d, B1 %d, F#1 %d\n",
                 "strummed chords", chordNotes, chordUnder, chordRange[1], chordRange[2],
                 chordRange[3]);
    // Latency by note, over every set but the first note of each from-silence run.
    std::printf ("latency by note (median / max ms, notes):");
    for (const Total* t : { &bass, &sloppy, &fretted })
        for (int p = 0; p < 128; ++p)
            all.perPitch[p].insert (all.perPitch[p].end(), t->perPitch[p].begin(),
                                    t->perPitch[p].end());
    for (int p = 0; p < 128; ++p)
        if (all.perPitch[p].size() >= 5)
            std::printf ("  %s%d %.1f/%.1f (%zu)", noteName (p), p / 12 - 1,
                         percentile (all.perPitch[p], 0.5), percentile (all.perPitch[p], 1.0),
                         all.perPitch[p].size());
    std::printf ("\n");
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Pick to note-on for every note from F#1 to E4, in each range: a note picked a second time (the
// converter knows the player and the instrument by then), on a bridge and a neck pickup, with a
// pick and with the fingers.

int latency (int argc, char** argv)
{
    const double fs = 48000.0;
    if (const auto wav = option (argc, argv, "--wav"); !wav.empty())
    {
        // One of the table's cases as a recording: --pitch n [--fingers 1] [--neck 1] [--seed n]
        const auto sc = pickedTwice (
            std::atoi (option (argc, argv, "--pitch", "40").c_str()),
            option (argc, argv, "--neck") == "1" ? Pickup::neck : Pickup::bridge,
            static_cast<std::uint32_t> (std::atoi (option (argc, argv, "--seed", "1").c_str())), fs,
            option (argc, argv, "--fingers") == "1");
        writeWavMono (wav, sc.signal, fs);
        return 0;
    }
    const midi::LowNote ranges[] = { midi::LowNote::e2, midi::LowNote::b1, midi::LowNote::fSharp1,
                                     midi::LowNote::automatic };
    std::printf (
        "note    Hz    two periods   E2 range      B1 range      F#1 range     Automatic   "
        "(median / max ms; -: not played)\n");
    for (int pitch = 28; pitch <= 64; ++pitch)
    {
        std::printf ("%-3s%d %6.1f  %8.1f   ", noteName (pitch), pitch / 12 - 1, hzOf (pitch),
                     2000.0 / hzOf (pitch));
        for (const auto range : ranges)
        {
            midi::Settings s;
            s.octave = 0;
            s.lowNote = range;
            std::vector<double> ms;
            int missing = 0;
            for (auto pickup : { Pickup::bridge, Pickup::neck })
                for (int fingers = 0; fingers < 2; ++fingers)
                    for (std::uint32_t seed = 1; seed <= 3; ++seed)
                    {
                        const auto sc = pickedTwice (pitch, pickup, seed, fs, fingers != 0);
                        const auto run = convert (sc.signal, fs, s);
                        double on = -1.0;
                        for (const auto& n : run.notes)
                            if (n.pitch == pitch && n.on >= 1.098 && n.on <= 1.2 && on < 0.0)
                                on = n.on;
                        if (on < 0.0)
                            ++missing;
                        else
                            ms.push_back (1000.0 * (on - 1.1));
                    }
            if (ms.empty())
                std::printf ("    -         ");
            else
                std::printf ("%5.1f /%5.1f%s ", percentile (ms, 0.5), percentile (ms, 1.0),
                             missing > 0 ? "*" : " ");
        }
        std::printf ("\n");
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// What a provisional note-on would buy and cost: the note sent at the tracker's first reading
// after each pick (before the MIDI layer has ruled out a lower note or a wrong reading), against
// the note the MIDI layer sends.

struct Provisional
{
    int notes = 0, firstRight = 0, firstWrong = 0, none = 0, sentRight = 0;
    std::vector<double> firstMs, sentMs, gainMs;
};

void provisionalOver (Provisional& p, const std::vector<float>& x, double fs,
                      const std::vector<std::pair<double, int>>& played, const midi::Settings& s,
                      midi::LowNote learned)
{
    midi::GuitarToMidi conv;
    conv.setSettings (s);
    conv.prepare (fs);
    conv.setLearnedLowNote (learned);
    const auto& m = conv.tracker();
    struct Reading
    {
        double t;
        int pitch;
    };
    std::vector<Reading> readings, sent;
    midi::EventList out;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        out.clear();
        conv.process (&x[i], 1, out);
        const double t = static_cast<double> (i) / fs;
        const auto ev = m.event();
        if (ev == analysis::MonoTracker::Event::noteOn ||
            ev == analysis::MonoTracker::Event::noteChange)
            readings.push_back (
                { t, static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (m.hz() / 440.0))) });
        for (const auto& e : out)
            if (e.isNoteOn())
                sent.push_back ({ t, e.note() });
    }
    for (std::size_t k = 0; k < played.size(); ++k)
    {
        const double start = played[k].first;
        const double end =
            std::min (start + 0.08, k + 1 < played.size() ? played[k + 1].first : start + 0.08);
        ++p.notes;
        double first = -1.0, right = -1.0;
        int firstPitch = -1;
        for (const auto& r : readings)
            if (r.t >= start + 0.004 && r.t <= end && first < 0.0)
            {
                first = r.t;
                firstPitch = r.pitch;
            }
        for (const auto& r : sent)
            if (r.t >= start - 0.002 && r.t <= start + 0.08 && r.pitch == played[k].second &&
                right < 0.0)
                right = r.t;
        if (right >= 0.0)
        {
            ++p.sentRight;
            p.sentMs.push_back (1000.0 * (right - start));
        }
        if (first < 0.0)
        {
            ++p.none; // the tracker kept its reading (the same note again): nothing to send early
            continue;
        }
        p.firstMs.push_back (1000.0 * (first - start));
        if (firstPitch == played[k].second)
        {
            ++p.firstRight;
            if (right >= 0.0)
                p.gainMs.push_back (1000.0 * (right - first));
        }
        else
            ++p.firstWrong;
    }
}

int provisional (int argc, char** argv)
{
    const double fs = 48000.0;
    midi::Settings s;
    s.octave = 0;
    s.lowNote = lowNoteNamed (option (argc, argv, "--low"));
    const auto learned = lowNoteNamed (option (argc, argv, "--learned", "e2"));
    const int shift = std::atoi (option (argc, argv, "--shift", "0").c_str());
    const auto print = [] (const char* name, const Provisional& p) {
        std::printf ("%-16s %4d picks: first reading right %4d (%.1f %%), wrong %3d (%.1f %%), "
                     "none (the reading stayed) %3d; first reading at p50 %.1f ms, the note sent "
                     "at p50 %.1f ms; sooner by p50 %.1f, p90 %.1f ms when right\n",
                     name, p.notes, p.firstRight, 100.0 * p.firstRight / std::max (1, p.notes),
                     p.firstWrong, 100.0 * p.firstWrong / std::max (1, p.notes), p.none,
                     percentile (p.firstMs, 0.5), percentile (p.sentMs, 0.5),
                     percentile (p.gainMs, 0.5), percentile (p.gainMs, 0.9));
    };
    const std::string wav = option (argc, argv, "--wav");
    if (!wav.empty())
    {
        // A recording and its notes: "<onset seconds> <midi note>" per line ('#' comments).
        const auto audio = tools::readWav (wav);
        std::ifstream truth (option (argc, argv, "--truth"));
        if (!audio || !truth)
            return 1;
        std::vector<std::pair<double, int>> played;
        std::string line;
        while (std::getline (truth, line))
        {
            double t = 0.0;
            int note = 0;
            if (!line.empty() && line[0] != '#' &&
                std::sscanf (line.c_str(), "%lf %d", &t, &note) == 2)
                played.push_back ({ t, note });
        }
        Provisional p;
        provisionalOver (p, audio->mono, audio->sampleRate, played, s, learned);
        print ("recording", p);
        return 0;
    }
    const auto over = [&] (Provisional& p, const Scenario& sc) {
        std::vector<std::pair<double, int>> played;
        for (const auto& n : sc.notes)
            if (n.picked)
                played.push_back ({ n.start, n.pitch });
        provisionalOver (p, sc.signal, fs, played, s, learned);
    };
    Provisional bass, sloppy, fretted;
    for (auto pickup : { Pickup::bridge, Pickup::neck, Pickup::piezo })
    {
        for (std::uint32_t seed = 1; seed <= 6; ++seed)
        {
            over (bass, lowered (shift, [&] {
                      return bassLine (seed, pickup, 120.0 + 4.0 * seed, fs, 48, false);
                  }));
            over (sloppy, lowered (shift, [&] {
                      return sloppyLine (seed, pickup, 110.0 + 5.0 * seed, fs, 48);
                  }));
        }
        for (std::uint32_t seed = 1; seed <= 4; ++seed)
            over (fretted, lowered (shift, [&] { return fretThenPick (seed, pickup, fs); }));
    }
    print ("bass lines", bass);
    print ("sloppy lines", sloppy);
    print ("fretted first", fretted);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Bends, slides and vibrato on synthetic strings: what each mode sends, next to where the string's
// pitch was.

int bends (int argc, char** argv)
{
    const double fs = 48000.0;
    int range = 2;
    for (int i = 2; i + 1 < argc; ++i)
        if (std::string (argv[i]) == "--range")
            range = std::atoi (argv[i + 1]);
    const char* list = environment ("SEDEF_BENDS_LIST"); // rows whose events to list
    const auto row = [&] (const std::string& what, const Moving& m, double from, double to) {
        for (int mode = 0; mode < 2; ++mode)
        {
            midi::Settings s;
            s.octave = 0;
            s.mode = mode == 0 ? midi::Mode::notes : midi::Mode::notesAndBend;
            s.bendRange = range;
            const auto run = convert (m.sc.signal, fs, s);
            const auto f = following (run, m, range, from, to);
            std::string notes;
            for (const auto& n : run.notes)
                notes += std::string (noteName (n.pitch)) + std::to_string (n.pitch / 12 - 1) +
                         "@" + std::to_string (static_cast<int> (std::lround (1000.0 * n.on))) +
                         " ";
            if (list != nullptr && what.find (list) != std::string::npos && mode == 0)
                if (const char* wav = environment ("SEDEF_BENDS_WAV"))
                    writeWavMono (wav, m.sc.signal, fs);
            if (list != nullptr && what.find (list) != std::string::npos)
                for (std::size_t k = 0; k < run.events.size(); ++k)
                {
                    const auto& e = run.events[k];
                    std::printf ("    %8.4f %s %d   string at %+.1f c\n", run.eventTimes[k],
                                 e.isNoteOn()    ? "on  "
                                 : e.isNoteOff() ? "off "
                                                 : "bend",
                                 e.isPitchBend()
                                     ? (e.bendValue() - midi::kBendCentre) * 100 * range / 8192
                                     : e.note(),
                                 m.centsAt (run.eventTimes[k]));
                }
            std::printf ("%-34s %-12s notes %d bends %4d  worst %6.1f rms %5.1f  step %5.1f  "
                         "last %7.1f  silent %3.0f ms  %s\n",
                         what.c_str(), mode == 0 ? "Notes" : "Notes+Bend", f.notes, f.bends,
                         f.worst, f.rms, f.largestStep, f.last, 1000.0 * f.silent, notes.c_str());
        }
    };
    char label[96];
    for (double hz : { 82.41, 110.0, 196.0 })
    {
        for (double drift : { 0.0, -5.0 })
        {
            const auto m = heldString (hz, 3.0, drift, fs, Pickup::bridge, -12.0);
            std::snprintf (label, sizeof label, "held %.0f Hz, drift %.0f c/s", hz, drift);
            row (label, m, m.pickAt + 0.1, m.muteAt - 0.01);
        }
        for (double cents : { 50.0, 100.0, 200.0, 300.0 })
            for (double seconds : { 0.08, 0.15, 0.3, 0.6 })
                for (int release = 0; release < 2; ++release)
                {
                    const auto m = bentString (hz, cents, seconds, release != 0, fs);
                    std::snprintf (label, sizeof label, "bend %.0f Hz +%.0f c in %.0f ms%s", hz,
                                   cents, 1000.0 * seconds, release != 0 ? ", back" : "");
                    row (label, m, m.pickAt + 0.1, m.muteAt - 0.01);
                }
        for (double cents : { 10.0, 15.0, 30.0, 50.0 })
            for (double rate : { 4.0, 6.5 })
            {
                const auto m = vibratoString (hz, cents, rate, fs);
                std::snprintf (label, sizeof label, "vibrato %.0f Hz +-%.0f c at %.1f Hz", hz,
                               cents, rate);
                row (label, m, m.pickAt + 0.1, m.muteAt - 0.01);
            }
        for (int semis : { 2, -2, 5, -5, 7, 12 })
            for (double every : { 0.015, 0.03, 0.06, 0.12 })
            {
                const auto m = slidString (hz * (semis < 0 ? 2.0 : 1.0), semis, every, fs);
                std::snprintf (label, sizeof label, "slide %.0f Hz %+d frets, %.0f ms each",
                               hz * (semis < 0 ? 2.0 : 1.0), semis, 1000.0 * every);
                row (label, m, m.pickAt + 0.1, m.muteAt - 0.01);
            }
    }
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
    if (mode == "bends")
        return bends (argc, argv);
    if (mode == "sets")
        return sets (argc, argv);
    if (mode == "provisional")
        return provisional (argc, argv);
    if (mode == "latency")
        return latency (argc, argv);
    if (mode == "why")
        return why (argc, argv);
    std::fprintf (stderr, "usage: sedef-midi-render wav|scenarios|bench ...\n");
    return 2;
}
