// SPDX-License-Identifier: AGPL-3.0-or-later
// The intonation workbench end to end on realistic plucks: the PitchTracker, read at the plugin
// editor's 60 Hz, and the workbench's capture (ui/src/features/intonation/capture.ts) measuring
// an open string and its 12th-fret octave. Hidden "[.report]" prints the comparison with the
// previous capture rule (docs/intonation.md, "Capturing a reading").
#include "PitchTestSignals.h"
#include "TestUtils.h"
#include "sedef/analysis/PitchTracker.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <optional>

using namespace sedef;
using analysis::PitchTracker;

namespace
{
constexpr double kFs = 48000.0;
constexpr double kPickAt = 0.2;
constexpr double kPollSeconds = 1.0 / 60.0; // the editor sends one reading per timer tick

struct GuitarString
{
    const char* name;
    double hz;
    double inharmonicity;
    double decaySeconds; ///< open string; the fretted octave rings shorter
    double octaveErrorCents;
};

// Standard tuning with plausible stiffness and sustain, and a different 12th-fret error each.
constexpr GuitarString kStrings[] = {
    { "E2", 82.41, 1.0e-4, 2.5, 3.6 },  { "A2", 110.0, 1.0e-4, 2.2, -2.4 },
    { "D3", 146.83, 1.0e-4, 2.0, 1.1 }, { "G3", 196.0, 1.5e-4, 1.8, 0.6 },
    { "B3", 246.94, 1.5e-4, 1.5, 4.4 }, { "E4", 329.63, 1.5e-4, 1.2, -1.8 },
};

/// A realistic pluck of an open string or of its 12th-fret octave:
/// - an attack glide of 8 cents (open) or 12 cents (fretted: the same pick on half the length)
///   decaying with a 50-60 ms time constant, so it is mostly gone 150 ms after the pick;
/// - a random wobble within +-1 cent (fretting pressure, the neck and body moving), different on
///   every pluck, at rates from `wobbleMinHz` to 8 Hz;
/// - a slow flat drift of 1.5 cents a second as the note decays. It is the same for both notes,
///   so their octave is the same at any moment after the pick; reading both at the same moment
///   keeps it out of the result;
/// - exponential decay (sooner for the fretted note), four times the stiffness at the 12th fret,
///   white noise and mains hum.
std::vector<float> realisticPluck (const GuitarString& s, bool fretted, std::uint32_t seed,
                                   double wobbleMinHz, double seconds = 2.2)
{
    test::Pluck p;
    p.f1 = fretted ? 2.0 * s.hz * std::exp2 (s.octaveErrorCents / 1200.0) : s.hz;
    p.inharmonicity = s.inharmonicity * (fretted ? 4.0 : 1.0);
    p.decaySeconds = s.decaySeconds * (fretted ? 0.6 : 1.0);
    p.glideCents = fretted ? 12.0 : 8.0;
    p.glideTau = fretted ? 0.06 : 0.05;
    p.driftCentsPerSecond = -1.5;
    p.wobble = test::Wobble (
        1.0, seed * 31u + (fretted ? 17u : 0u) + static_cast<std::uint32_t> (s.hz), wobbleMinHz);
    p.pluckPosition = 0.12 + 0.01 * (seed % 8);
    p.peak = fretted ? 0.2f : 0.3f;
    p.seed = seed + (fretted ? 100u : 0u);

    std::vector<float> sig (static_cast<size_t> ((kPickAt + seconds) * kFs), 0.0f);
    test::addPluck (sig, kFs, static_cast<size_t> (kPickAt * kFs), p);
    test::addNoise (sig, -70.0f, seed + 3u);
    for (size_t i = 0; i < sig.size(); ++i)
    {
        const double ph = kTwoPiD * 60.0 * static_cast<double> (i) / kFs;
        sig[i] +=
            dbToGain (-62.0f) * static_cast<float> (std::sin (ph) + 0.5 * std::sin (3.0 * ph));
    }
    return sig;
}

struct Poll
{
    double t; ///< seconds after the pick
    PitchTracker::Reading r;
};

/// The readings the UI receives: the tracker fed in 128-sample blocks, read every editor tick.
std::vector<Poll> poll (const std::vector<float>& sig)
{
    PitchTracker tracker;
    tracker.prepare (kFs);
    std::vector<Poll> out;
    size_t pos = 0;
    const double length = static_cast<double> (sig.size()) / kFs;
    for (double t = kPollSeconds; t < length; t += kPollSeconds)
    {
        const auto end = std::min (sig.size(), static_cast<size_t> (t * kFs));
        while (pos < end)
        {
            const int n = static_cast<int> (std::min<size_t> (128, end - pos));
            tracker.push (sig.data() + pos, n);
            pos += static_cast<size_t> (n);
        }
        out.push_back ({ t - kPickAt, tracker.latest() });
    }
    return out;
}

struct Captured
{
    double at; ///< seconds after the pick
    double hz;
};

/// The workbench's capture (capture.ts): the mean stableHz over one second from 0.6 s after the
/// pluck (starting by 1 s at the latest), while the reading stays steady: spread within a cent, no
/// more than four cents of movement, no gap over 0.25 s. The tracker must call the note stable at
/// least once in that second. A pluck whose reading stops being steady is not measured. Readings
/// count within half a semitone of `targetHz`: the open string's note, or for the 12th fret twice
/// the open string as measured.
std::optional<Captured> workbenchCapture (const std::vector<Poll>& polls, double targetHz)
{
    constexpr double kFrom = 0.6, kLatestStart = 1.0, kSeconds = 1.0;
    constexpr double kMaxSpread = 1.0, kMaxMove = 4.0, kGap = 0.25;
    std::uint32_t onset = 0;
    int count = 0, n = 0;
    double onsetAt = 0.0, lastValid = 0.0, spanStart = 0.0, sum = 0.0, lo = 0.0, hi = 0.0;
    bool sawStable = false, missed = false;
    for (const auto& p : polls)
    {
        const auto& r = p.r;
        // A new pluck, or a segment the tracker restarted (stableCount falls back): a new note.
        if (r.onsetId != onset || (r.valid && r.stableCount < count))
        {
            onset = r.onsetId;
            onsetAt = p.t;
            count = n = 0;
            missed = false;
        }
        if (missed)
            continue;
        if (!r.valid)
        {
            missed = n > 0 && p.t - lastValid > kGap;
            continue;
        }
        lastValid = p.t;
        count = r.stableCount;
        if (p.t - onsetAt < kFrom)
            continue;
        const bool steady =
            r.stableCount > 0 && r.stableStdCents <= kMaxSpread &&
            std::abs (test::centsError (r.stableHz, targetHz)) < 50.0 &&
            (n == 0 || test::centsError (std::max<double> (hi, r.stableHz),
                                         std::min<double> (lo, r.stableHz)) <= kMaxMove);
        if (!steady || (n == 0 && p.t - onsetAt > kLatestStart))
        {
            missed = n > 0 || p.t - onsetAt > kLatestStart;
            continue;
        }
        if (n == 0)
        {
            spanStart = p.t;
            sum = 0.0;
            lo = hi = r.stableHz;
            sawStable = false;
        }
        lo = std::min<double> (lo, r.stableHz);
        hi = std::max<double> (hi, r.stableHz);
        sum += r.stableHz;
        ++n;
        sawStable = sawStable || r.stable;
        if (p.t - spanStart >= kSeconds && sawStable)
            return Captured { p.t, sum / n };
    }
    return std::nullopt;
}

/// The previous rule: `stable` without a break for 0.5 s; any break starts it again.
std::optional<Captured> previousCapture (const std::vector<Poll>& polls, double targetHz)
{
    std::uint32_t onset = 0;
    int count = 0, n = 0;
    double since = 0.0, sum = 0.0;
    for (const auto& p : polls)
    {
        const auto& r = p.r;
        if (!(r.valid && r.stable && std::abs (test::centsError (r.stableHz, targetHz)) < 50.0))
        {
            n = 0;
            continue;
        }
        if (n == 0 || r.onsetId != onset || r.stableCount < count)
        {
            since = p.t;
            onset = r.onsetId;
            sum = 0.0;
            n = 0;
        }
        count = r.stableCount;
        sum += r.stableHz;
        ++n;
        if (p.t - since >= 0.5)
            return Captured { p.t, sum / n };
    }
    return std::nullopt;
}

/// The previous open-string rule: the instantaneous reading within +-1 cent of the target for a
/// second (gaps under 0.25 s bridged; a valid reading outside the window starts it again).
bool previousTuneHold (const std::vector<Poll>& polls, double targetHz)
{
    double since = -1.0, lastIn = 0.0;
    for (const auto& p : polls)
    {
        const bool in = p.r.valid && std::abs (test::centsError (p.r.frequencyHz, targetHz)) <= 1.0;
        if (in)
        {
            if (since < 0.0)
                since = p.t;
            lastIn = p.t;
        }
        else if (since >= 0.0 && (p.r.valid || p.t - lastIn > 0.25))
            since = -1.0;
        if (since >= 0.0 && lastIn - since >= 1.0)
            return true;
    }
    return false;
}

double firstStable (const std::vector<Poll>& polls)
{
    for (const auto& p : polls)
        if (p.t > 0.0 && p.r.stable)
            return p.t;
    return -1.0;
}
} // namespace

TEST_CASE ("Workbench capture measures a realistic 12th-fret octave within 0.3 cent",
           "[analysis][intonation]")
{
    for (const auto& s : kStrings)
    {
        for (std::uint32_t seed = 1; seed <= 3; ++seed)
        {
            const auto open = poll (realisticPluck (s, false, seed, 1.5));
            const auto fretted = poll (realisticPluck (s, true, seed, 1.5));
            const auto a = workbenchCapture (open, s.hz);
            // The 12th fret counts near twice the open string as measured.
            const auto b = a ? workbenchCapture (fretted, 2.0 * a->hz) : std::nullopt;
            CAPTURE (s.name, seed, firstStable (open), firstStable (fretted));
            // The tracker calls both notes stable at its earliest (0.6 s; 0.65 s for E2) or soon
            // after, despite the wobble.
            CHECK (firstStable (open) > 0.0);
            CHECK (firstStable (open) < 0.9);
            CHECK (firstStable (fretted) > 0.0);
            CHECK (firstStable (fretted) < 0.9);
            REQUIRE (a.has_value());
            REQUIRE (b.has_value());
            CAPTURE (a->at, b->at);
            // One second from 0.6 s after the pick, read at 60 Hz.
            CHECK (a->at < 1.7);
            CHECK (b->at < 1.7);
            const double cents = 1200.0 * std::log2 (b->hz / (2.0 * a->hz));
            CAPTURE (cents, s.octaveErrorCents);
            CHECK (std::abs (cents - s.octaveErrorCents) < 0.3);
        }
    }
}

TEST_CASE ("Workbench capture finishes on time when the pitch wanders slowly",
           "[analysis][intonation]")
{
    // Wobble down to 0.4 Hz breaks the tracker's strict stable flag now and then; the capture
    // carries on through that and finishes at the usual time. A wander this slow cannot average
    // out in one second, so the result is good to about half a cent.
    for (const auto& s : kStrings)
    {
        for (std::uint32_t seed = 1; seed <= 2; ++seed)
        {
            const auto a = workbenchCapture (poll (realisticPluck (s, false, seed, 0.4)), s.hz);
            const auto b =
                a ? workbenchCapture (poll (realisticPluck (s, true, seed, 0.4)), 2.0 * a->hz)
                  : std::nullopt;
            CAPTURE (s.name, seed);
            REQUIRE (a.has_value());
            REQUIRE (b.has_value());
            CAPTURE (a->at, b->at);
            CHECK (a->at < 1.7);
            CHECK (b->at < 1.7);
            const double cents = 1200.0 * std::log2 (b->hz / (2.0 * a->hz));
            CAPTURE (cents, s.octaveErrorCents);
            CHECK (std::abs (cents - s.octaveErrorCents) < 0.6);
        }
    }
}

TEST_CASE ("Workbench capture report", "[.report][intonation-report]")
{
    const int seeds = 12;
    for (double minHz : { 1.5, 0.4 })
    {
        std::printf ("\n+-1 cent wobble from %.1f Hz, -1.5 cents/s drift, %d plucks per note\n",
                     minHz, seeds);
        std::printf ("Open strings in tune. Previous tune step: +-1 cent for 1 s on the instant "
                     "reading.\nCaptures: previous = stable for 0.5 s without a break; workbench = "
                     "capture.ts.\n");
        std::printf ("string  tune step  capture    measured  latest   rms err  worst err  "
                     "(cents)  first stable by\n");
        for (const auto& s : kStrings)
        {
            struct Acc
            {
                int done = 0;
                double latest = 0.0, sum2 = 0.0, worst = 0.0;
            } acc[2];
            int held = 0, tuned = 0;
            double stableBy = 0.0;
            for (std::uint32_t seed = 1; seed <= static_cast<std::uint32_t> (seeds); ++seed)
            {
                const auto open = poll (realisticPluck (s, false, seed, minHz, 2.5));
                const auto fretted = poll (realisticPluck (s, true, seed, minHz, 2.5));
                held += previousTuneHold (open, s.hz) ? 1 : 0;
                tuned += workbenchCapture (open, s.hz) ? 1 : 0;
                stableBy = std::max ({ stableBy, firstStable (open), firstStable (fretted) });
                for (int k = 0; k < 2; ++k)
                {
                    const auto capture = k == 0 ? previousCapture : workbenchCapture;
                    const auto a = capture (open, s.hz);
                    const auto b = capture (fretted, 2.0 * s.hz);
                    if (!a || !b)
                        continue;
                    const double err =
                        1200.0 * std::log2 (b->hz / (2.0 * a->hz)) - s.octaveErrorCents;
                    auto& x = acc[k];
                    ++x.done;
                    x.latest = std::max ({ x.latest, a->at, b->at });
                    x.sum2 += err * err;
                    x.worst = std::max (x.worst, std::abs (err));
                }
            }
            for (int k = 0; k < 2; ++k)
                std::printf ("%-6s  %4d/%-2d    %-9s  %5d/%-2d  %5.2f s  %7.3f  %9.3f  %16.2f s\n",
                             s.name, k == 0 ? held : tuned, seeds,
                             k == 0 ? "previous" : "workbench", acc[k].done, seeds, acc[k].latest,
                             acc[k].done ? std::sqrt (acc[k].sum2 / acc[k].done) : 0.0,
                             acc[k].worst, stableBy);
        }
    }
}
