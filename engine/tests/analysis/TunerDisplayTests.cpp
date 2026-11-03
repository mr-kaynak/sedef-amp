// SPDX-License-Identifier: AGPL-3.0-or-later
// The tuner display (docs/dsp/tuner.md, "Display") on realistic tuning scenarios, read at the
// editor's 60 Hz. Hidden "[.report]" prints the before/after table; "[.dump]" records the event
// streams the UI tests replay.
#include "TunerScenarios.h"
#include "sedef/analysis/PitchTracker.h"
#include "sedef/analysis/TunerDisplay.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace sedef;
using analysis::PitchTracker;

namespace
{
/// What a display showed at one poll.
struct Shown
{
    double t;
    bool visible; ///< a note and a needle position are shown
    int note;     ///< midi, when visible
    double cents; ///< needle position relative to the scenario's nominal note
    int state;    ///< 0 idle, 1 neutral, 2 in tune, 3 sharp, 4 flat
};

/// The 0.2.0 gauge: an exponential approach (16 per second) at 60 frames per second to the raw
/// short-window cents, back to the centre while invalid; in tune within +-1 cent, no hysteresis.
std::vector<Shown> legacyGauge (const std::vector<test::TunerPoll>& polls,
                                const test::TunerScenario& s)
{
    std::vector<Shown> out;
    double angle = 0.0;
    const double k = 1.0 - std::exp (-16.0 / 60.0);
    const double nominalMidi = 69.0 + 12.0 * std::log2 (s.nominalHz / 440.0);
    for (const auto& p : polls)
    {
        const auto& r = p.r;
        const double target =
            r.valid ? std::clamp (static_cast<double> (r.cents), -50.0, 50.0) : 0.0;
        angle += (target - angle) * k;
        Shown x { p.t, r.valid, r.valid ? r.midiNote : -1, 0.0, 0 };
        // The needle reads relative to the note shown; express it relative to the nominal note.
        x.cents = angle + (r.valid ? 100.0 * (r.midiNote - nominalMidi) : 0.0);
        if (r.valid)
            x.state = std::abs (r.cents) <= 1.0f ? 2 : (r.cents > 0.0f ? 3 : 4);
        out.push_back (x);
    }
    return out;
}

/// The engine's display fields as a needle (no UI animation), in tune within +-1 cent.
std::vector<Shown> displayGauge (const std::vector<test::TunerPoll>& polls,
                                 const test::TunerScenario& s)
{
    std::vector<Shown> out;
    const double nominalMidi = 69.0 + 12.0 * std::log2 (s.nominalHz / 440.0);
    for (const auto& p : polls)
    {
        const auto& r = p.r;
        Shown x { p.t, r.displayValid, r.displayNote, 0.0, 0 };
        x.cents = r.displayCents + 100.0 * (r.displayNote - nominalMidi);
        if (r.displayValid && !r.displayHeld)
            x.state = std::abs (r.displayCents) <= 1.0f ? 2 : (r.displayCents > 0.0f ? 3 : 4);
        out.push_back (x);
    }
    return out;
}

struct Metrics
{
    double first = -1.0;     ///< first poll with a needle shown, s after the pick
    double rms = 0.0;        ///< needle minus true pitch over the sustain window
    double peak = 0.0;       ///<
    double jitter = 0.0;     ///< RMS of the needle around its own 0.3 s moving average
    double jitterPeak = 0.0; ///<
    double trueJitter = 0.0; ///< same for the true pitch (a wobble, a vibrato)
    double settle = -1.0;    ///< after a move ends, until within 1 cent for good (s)
    double overshoot = 0.0;
    double lag = 0.0;  ///< largest |needle - true| during a move
    int noteFlips = 0; ///< changes between two shown notes
    int dropouts = 0;  ///< needle hidden and shown again while the note rings
    int tuneEntries = 0;
    int tuneExits = 0;
    double lastValid = -1.0;
    double holdMove = 0.0; ///< how far the needle moves in the second after the note ends
};

Metrics measure (const std::vector<Shown>& shown, const std::vector<test::TunerPoll>& polls,
                 const test::TunerScenario& s)
{
    Metrics m;
    for (const auto& p : polls)
        if (p.t >= 0.0 && p.r.valid)
            m.lastValid = p.t;
    const double sustainTo = std::min (s.sustainTo, m.lastValid - 0.1);

    double sum2 = 0.0, lo = 1e9, hi = -1e9, tlo = 1e9, thi = -1e9;
    int n = 0, prevNote = -1, prevState = 0;
    bool wasVisible = false, hidden = false;
    for (const auto& x : shown)
    {
        if (x.t < 0.0)
            continue;
        const double truth = s.trueCents (x.t);
        if (x.visible && m.first < 0.0)
            m.first = x.t;
        if (x.t >= s.sustainFrom && x.t <= sustainTo && x.visible)
        {
            const double e = x.cents - truth;
            sum2 += e * e;
            ++n;
            m.peak = std::max (m.peak, std::abs (e));
            lo = std::min (lo, x.cents);
            hi = std::max (hi, x.cents);
            tlo = std::min (tlo, truth);
            thi = std::max (thi, truth);
        }
        if (s.moveAt >= 0.0 && x.t >= s.moveAt && x.t <= s.moveEnd && x.visible)
            m.lag = std::max (m.lag, std::abs (x.cents - truth));
        if (s.moveAt >= 0.0 && x.t >= s.moveAt && x.t <= sustainTo)
        {
            const double final = s.trueCents (s.moveEnd + 0.5);
            const double dir = final > s.trueCents (s.moveAt - 0.01) ? 1.0 : -1.0;
            if (x.visible)
                m.overshoot = std::max (m.overshoot, dir * (x.cents - final));
            if (x.t >= s.moveEnd && (!x.visible || std::abs (x.cents - truth) > 1.0))
                m.settle = -1.0;
            else if (x.t >= s.moveEnd && m.settle < 0.0)
                m.settle = x.t - s.moveEnd;
        }
        if (x.visible)
        {
            if (prevNote >= 0 && x.note != prevNote)
                ++m.noteFlips;
            prevNote = x.note;
            if (hidden && x.t < m.lastValid)
                ++m.dropouts;
            hidden = false;
            wasVisible = true;
        }
        else if (wasVisible && x.t < m.lastValid)
            hidden = true;
        if (x.state == 2 && prevState != 2)
            ++m.tuneEntries;
        if (prevState == 2 && (x.state == 3 || x.state == 4))
            ++m.tuneExits;
        prevState = x.state;
    }
    m.rms = n > 0 ? std::sqrt (sum2 / n) : 0.0;
    (void)lo, (void)hi, (void)tlo, (void)thi;

    // Jitter: movement faster than a 0.3 s moving average (slow drift and sag are not jitter).
    std::vector<double> needle, truth;
    for (const auto& x : shown)
        if (x.t >= s.sustainFrom - 0.15 && x.t <= sustainTo + 0.15 && x.visible)
        {
            needle.push_back (x.cents);
            truth.push_back (s.trueCents (x.t));
        }
    const int half = static_cast<int> (std::lround (0.15 / test::kTunerPoll));
    const auto jitterOf = [&] (const std::vector<double>& v, double& rms, double& peak) {
        double acc = 0.0;
        int count = 0;
        for (int i = half; i + half < static_cast<int> (v.size()); ++i)
        {
            double mean = 0.0;
            for (int k = -half; k <= half; ++k)
                mean += v[static_cast<size_t> (i + k)];
            mean /= 2 * half + 1;
            const double d = v[static_cast<size_t> (i)] - mean;
            acc += d * d;
            peak = std::max (peak, std::abs (d));
            ++count;
        }
        rms = count > 0 ? std::sqrt (acc / count) : 0.0;
    };
    double unused = 0.0;
    jitterOf (needle, m.jitter, m.jitterPeak);
    jitterOf (truth, m.trueJitter, unused);

    // The needle in the second after the last valid reading.
    double at = 0.0;
    bool have = false;
    for (const auto& x : shown)
    {
        if (x.t < m.lastValid)
            continue;
        if (!have)
        {
            at = x.cents;
            have = true;
        }
        if (x.t <= m.lastValid + 1.0)
            m.holdMove = std::max (m.holdMove, std::abs (x.cents - at));
    }
    return m;
}

void printMetrics (const char* label, const Metrics& m)
{
    std::printf ("  %-7s first %3.0f ms  err %5.2f  peak %5.2f  jitter %5.3f pk %5.2f (true %4.2f)",
                 label, 1000.0 * m.first, m.rms, m.peak, m.jitter, m.jitterPeak, m.trueJitter);
    if (m.lag > 0.0 || m.settle >= 0.0)
        std::printf ("  lag %5.2f  settle %4.0f ms  over %4.2f", m.lag, 1000.0 * m.settle,
                     m.overshoot);
    std::printf ("  notes %d  drop %d  in-tune %d/%d  hold %5.2f\n", m.noteFlips, m.dropouts,
                 m.tuneEntries, m.tuneExits, m.holdMove);
}
} // namespace

namespace
{
test::TunerScenario scenario (const char* name)
{
    for (auto& s : test::tunerScenarios())
        if (s.name == name)
            return s;
    FAIL ("no scenario " << name);
    return {};
}

/// The display relative to the scenario's nominal note.
double displayed (const test::TunerScenario& s, const PitchTracker::Reading& r)
{
    return r.displayCents +
           100.0 * (r.displayNote - (69.0 + 12.0 * std::log2 (s.nominalHz / 440.0)));
}

// MSVC deprecates std::getenv (C4996). The hidden dump cases read one variable from the test
// thread, which is safe; the pragma wraps the whole function because MSVC applies warnings in
// the 4700-4999 range per function.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
/// The environment variable `name`, or nullptr when it is not set.
const char* environment (const char* name)
{
    return std::getenv (name);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
} // namespace

TEST_CASE ("TunerDisplay is steady on a sustained note, with hum or another string ringing",
           "[analysis][display]")
{
    for (const char* name :
         { "e2-clean", "e2-hum", "a2-hum", "e2-bridge", "e1-bass", "a2-after-e2" })
    {
        const auto s = scenario (name);
        const auto polls = test::pollTracker (s);
        const auto shown = displayGauge (polls, s);
        const auto m = measure (shown, polls, s);
        CAPTURE (name, m.jitter, m.jitterPeak, m.rms, m.peak);
        CHECK (m.jitter < 0.05);
        CHECK (m.jitterPeak < 0.1);
        // Another string ringing pulls the short window by ~10 cents; the long window settles
        // within half a cent in the sustain window, which starts 0.6 s after the pick.
        CHECK (m.rms < (s.ringing ? 0.6 : 0.1));
        CHECK (m.noteFlips == (s.ringing ? 1 : 0)); // the previous string, then this one
        CHECK (m.dropouts == 0);
        bool settled = false;
        for (const auto& p : polls)
            if (p.t > 0.0 && p.t < (s.ringing ? 1.2 : 0.8))
                settled = settled || p.r.displaySettled;
        CHECK (settled);
    }
}

TEST_CASE ("TunerDisplay follows a peg turn and a step promptly, without overshoot",
           "[analysis][display]")
{
    {
        const auto s = scenario ("a2-peg");
        const auto polls = test::pollTracker (s);
        const auto m = measure (displayGauge (polls, s), polls, s);
        CAPTURE (m.lag, m.settle, m.overshoot);
        CHECK (m.lag < 3.0); // 0.2.0: 3.1 cents behind at 30 cents per second
        CHECK (m.settle >= 0.0);
        CHECK (m.settle < 0.15);
        CHECK (m.overshoot < 0.1);
    }
    {
        const auto s = scenario ("g3-step");
        const auto polls = test::pollTracker (s);
        const auto m = measure (displayGauge (polls, s), polls, s);
        CAPTURE (m.settle, m.overshoot);
        CHECK (m.settle >= 0.0);
        CHECK (m.settle < 0.18); // 0.2.0: 217 ms
        CHECK (m.overshoot < 0.1);
    }
}

TEST_CASE ("TunerDisplay keeps the note still through a re-pluck and holds a dying note",
           "[analysis][display]")
{
    {
        // The same string picked again after a retune: the needle does not swing sharp with the
        // attack; it moves once, to the new pitch.
        const auto s = scenario ("e4-repluck");
        const auto polls = test::pollTracker (s);
        double held = 0.0, worst = -1.0e9;
        double reached = -1.0;
        for (const auto& p : polls)
        {
            if (p.t < 0.0)
            {
                held = displayed (s, p.r);
                continue;
            }
            const double c = displayed (s, p.r);
            if (p.t < 0.15)
                worst = std::max (worst, c);
            if (reached < 0.0 && std::abs (c - s.trueCents (p.t)) < 1.0)
                reached = p.t;
            CHECK (p.r.displayNote == 64);
        }
        CAPTURE (held, worst, reached);
        CHECK (worst <= held + 0.1);
        CHECK (reached > 0.0);
        CHECK (reached < 0.3);
    }
    {
        const auto s = scenario ("d3-dies");
        const auto polls = test::pollTracker (s);
        const auto m = measure (displayGauge (polls, s), polls, s);
        CHECK (m.holdMove < 0.05);
        const auto last = polls.back().r;
        CHECK_FALSE (last.valid);
        CHECK (last.displayValid);
        CHECK (last.displayHeld);
        CHECK (last.displayNote == 50);
        CHECK (std::abs (last.displayCents + 7.0f) < 0.2f);
    }
}

TEST_CASE ("TunerDisplay changes note on a hammer-on, once", "[analysis][display]")
{
    const auto s = scenario ("g3-hammer");
    const auto polls = test::pollTracker (s);
    int changes = 0, last = -1;
    double switched = -1.0;
    for (const auto& p : polls)
    {
        if (!p.r.displayValid)
            continue;
        if (last >= 0 && p.r.displayNote != last)
        {
            ++changes;
            switched = p.t - 1.0;
        }
        last = p.r.displayNote;
    }
    CAPTURE (switched);
    CHECK (changes == 1);
    CHECK (last == 57);
    CHECK (switched > 0.0);
    CHECK (switched < 0.25);
}

TEST_CASE ("TunerDisplay does not take another string's pull on the reading for a move",
           "[analysis][display]")
{
    // The reading sits 8 cents off the long window for good (another string ringing pulls a short
    // window): the long window wins once it has proved steady, and the tracker is never asked to
    // restart it.
    analysis::TunerDisplay d;
    d.prepare (0.005);
    const double hz = 110.0;
    const double pulled = hz * std::exp2 (8.0 / 1200.0);
    int moves = 0;
    for (int hop = 0; hop < 400; ++hop)
    {
        analysis::TunerDisplay::Input in;
        in.onset = hop == 0;
        in.readingValid = true;
        in.readingHz = pulled * std::exp2 ((hop % 2 ? 0.8 : -0.8) / 1200.0);
        in.preciseValid = hop >= 40;
        in.preciseHz = hz;
        in.preciseSeconds = std::min (0.32, 0.005 * (hop - 40) + 0.05);
        moves += d.update (in) ? 1 : 0;
    }
    CHECK (moves == 0);
    CHECK (std::abs (1200.0 * std::log2 (d.output().hz / hz)) < 0.2);
    CHECK (d.output().settled);
}

TEST_CASE ("TunerDisplay reads the same at any sample rate and host block size",
           "[analysis][display]")
{
    // The display runs per analysis hop, so a sample-by-sample host and one handing over 4096
    // samples at a time see the same needle; only the age of the reading the editor gets differs
    // (up to one block).
    const double fs = GENERATE (44100.0, 96000.0, 192000.0);
    const int block = GENERATE (1, 4096);
    CAPTURE (fs, block);
    {
        const auto s = scenario ("e2-hum");
        const auto polls = test::pollHostBlocks (s, fs, block);
        const auto m = measure (displayGauge (polls, s), polls, s);
        CAPTURE (m.jitter, m.jitterPeak, m.rms);
        CHECK (m.jitter < 0.05);
        CHECK (m.jitterPeak < 0.1);
        CHECK (m.rms < 0.1);
        CHECK (m.noteFlips == 0);
        CHECK (m.dropouts == 0);
    }
    {
        const auto s = scenario ("g3-step");
        const auto polls = test::pollHostBlocks (s, fs, block);
        const auto m = measure (displayGauge (polls, s), polls, s);
        CAPTURE (m.settle, m.overshoot);
        CHECK (m.settle >= 0.0);
        CHECK (m.settle < 0.18 + block / fs);
        CHECK (m.overshoot < 0.1);
    }
    {
        const auto s = scenario ("d3-dies");
        const auto polls = test::pollHostBlocks (s, fs, block);
        const auto m = measure (displayGauge (polls, s), polls, s);
        CHECK (m.holdMove < 0.05);
        CHECK (polls.back().r.displayHeld);
        CHECK (polls.back().r.displayNote == 50);
    }
}

TEST_CASE ("Tuner display report", "[.report][tuner-report]")
{
    for (const auto& s : test::tunerScenarios())
    {
        const auto polls = test::pollTracker (s);
        std::printf ("\n%s: %s\n", s.name.c_str(), s.what.c_str());
        printMetrics ("before", measure (legacyGauge (polls, s), polls, s));
        printMetrics ("display", measure (displayGauge (polls, s), polls, s));
    }
}

TEST_CASE ("Tuner display streams", "[.dump][tuner-dump]")
{
    // SEDEF_TUNER_DUMP=<dir>: one JSON file per scenario with every 60 Hz reading, for the UI's
    // replay tests (ui/src/features/tuner/fixtures) and the plots.
    const char* dir = environment ("SEDEF_TUNER_DUMP");
    REQUIRE (dir != nullptr);
    for (const auto& s : test::tunerScenarios())
    {
        const auto polls = test::pollTracker (s);
        {
            // The signal itself, 16-bit mono, to hear what the tuner was given.
            const auto sig = s.render (test::kTunerFs);
            std::ofstream w (std::string (dir) + "/" + s.name + ".wav", std::ios::binary);
            const auto put = [&] (std::uint32_t v, int bytes) {
                for (int b = 0; b < bytes; ++b)
                    w.put (static_cast<char> ((v >> (8 * b)) & 0xffu));
            };
            const auto bytes = static_cast<std::uint32_t> (sig.size() * 2);
            w.write ("RIFF", 4);
            put (36 + bytes, 4);
            w.write ("WAVEfmt ", 8);
            put (16, 4);
            put (1, 2);
            put (1, 2);
            put (static_cast<std::uint32_t> (test::kTunerFs), 4);
            put (static_cast<std::uint32_t> (test::kTunerFs) * 2, 4);
            put (2, 2);
            put (16, 2);
            w.write ("data", 4);
            put (bytes, 4);
            for (float x : sig)
                put (static_cast<std::uint32_t> (static_cast<std::int16_t> (
                         std::lround (std::clamp (x, -1.0f, 1.0f) * 32767.0f))) &
                         0xffffu,
                     2);
        }
        std::ofstream f (std::string (dir) + "/" + s.name + ".json");
        REQUIRE (f.good());
        const double nominalMidi = 69.0 + 12.0 * std::log2 (s.nominalHz / 440.0);
        char buf[512];
        f << "{\"name\":\"" << s.name << "\",\"what\":\"" << s.what
          << "\",\"nominalMidi\":" << nominalMidi << ",\"sustainFrom\":" << s.sustainFrom
          << ",\"sustainTo\":" << s.sustainTo << ",\"moveAt\":" << s.moveAt
          << ",\"moveEnd\":" << s.moveEnd
          << ",\n\"fields\":[\"t\",\"valid\",\"frequencyHz\",\"midiNote\",\"cents\",\"clarity\","
             "\"levelDb\",\"stable\",\"stableHz\",\"stableStdCents\",\"stableCount\",\"onsetId\","
             "\"displayValid\",\"displayHeld\",\"displaySettled\",\"displayHz\",\"displayNote\","
             "\"displayCents\",\"trueCents\"],\n\"rows\":[\n";
        for (size_t i = 0; i < polls.size(); ++i)
        {
            const auto& p = polls[i];
            const auto& r = p.r;
            std::snprintf (
                buf, sizeof (buf),
                "[%.4f,%d,%.4f,%d,%.3f,%.3f,%.1f,%d,%.4f,%.3f,%d,%u,%d,%d,%d,%.4f,%d,%.3f,"
                "%.3f]%s\n",
                p.t, r.valid ? 1 : 0, r.frequencyHz, r.midiNote, r.cents, r.clarity, r.levelDb,
                r.stable ? 1 : 0, r.stableHz, r.stableStdCents, r.stableCount,
                static_cast<unsigned> (r.onsetId), r.displayValid ? 1 : 0, r.displayHeld ? 1 : 0,
                r.displaySettled ? 1 : 0, r.displayHz, r.displayNote, r.displayCents,
                s.trueCents (p.t), i + 1 < polls.size() ? "," : "");
            f << buf;
        }
        f << "]}\n";
    }
}

TEST_CASE ("Tuner display fixtures for the UI", "[.dump][tuner-fixtures]")
{
    // SEDEF_TUNER_FIXTURES=ui/src/features/tuner/fixtures/streams.json: the display fields of a
    // few scenarios at 60 Hz, which ui/src/features/tuner/replay.test.ts plays through the gauge's
    // readout and needle. Rows: t, displayValid, displayHeld, displaySettled, displayNote,
    // displayCents, true cents from the nominal note (A4 = 440 Hz).
    const char* path = environment ("SEDEF_TUNER_FIXTURES");
    REQUIRE (path != nullptr);
    const char* names[] = { "e2-hum", "g3-real", "b3-edge", "a2-after-e2",
                            "a2-peg", "g3-step", "d3-dies", "e4-repluck" };
    std::ofstream f (path);
    REQUIRE (f.good());
    f << "{\n\"about\":\"PitchTracker display fields at 60 Hz for tuner scenarios; regenerate with "
         "SEDEF_TUNER_FIXTURES=<this file> sedef_engine_tests [tuner-fixtures]\",\n"
         "\"fields\":[\"t\",\"valid\",\"held\",\"settled\",\"note\",\"cents\",\"trueCents\"],\n"
         "\"scenarios\":[\n";
    bool firstScenario = true;
    for (const auto& s : test::tunerScenarios())
    {
        if (std::find_if (std::begin (names), std::end (names),
                          [&] (const char* n) { return s.name == n; }) == std::end (names))
            continue;
        const auto polls = test::pollTracker (s);
        const double nominalMidi = 69.0 + 12.0 * std::log2 (s.nominalHz / 440.0);
        f << (firstScenario ? "" : ",\n") << "{\"name\":\"" << s.name << "\",\"what\":\"" << s.what
          << "\",\"nominalMidi\":" << std::lround (nominalMidi)
          << ",\"sustainFrom\":" << s.sustainFrom << ",\"sustainTo\":" << s.sustainTo
          << ",\"moveAt\":" << s.moveAt << ",\"moveEnd\":" << s.moveEnd << ",\"rows\":[";
        firstScenario = false;
        char buf[160];
        for (size_t i = 0; i < polls.size(); ++i)
        {
            const auto& p = polls[i];
            const auto& r = p.r;
            std::snprintf (buf, sizeof (buf), "%s[%.3f,%d,%d,%d,%d,%.3f,%.2f]", i ? "," : "", p.t,
                           r.displayValid ? 1 : 0, r.displayHeld ? 1 : 0, r.displaySettled ? 1 : 0,
                           r.displayNote, r.displayCents, s.trueCents (p.t));
            f << buf;
        }
        f << "]}";
    }
    f << "\n]}\n";
}
