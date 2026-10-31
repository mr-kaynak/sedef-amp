// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PitchTestSignals.h"
#include "TestUtils.h"
#include "sedef/analysis/PitchTracker.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace sedef;
using analysis::PitchTracker;
using Catch::Approx;

namespace
{
struct StringNote
{
    const char* name;
    double hz;
    double inharmonicity;
};

// Open strings of a guitar in standard tuning and the high E's 12th fret (half the length, so four
// times the stiffness coefficient).
constexpr StringNote kStrings[] = {
    { "E2", 82.41, 1.0e-4 },  { "A2", 110.0, 1.0e-4 },  { "D3", 146.83, 1.0e-4 },
    { "G3", 196.0, 1.0e-4 },  { "B3", 246.94, 1.0e-4 }, { "E4", 329.63, 1.0e-4 },
    { "E5", 659.26, 4.0e-4 },
};

constexpr double kPickAt = 0.2;

std::vector<float> pluckSignal (double fs, const test::Pluck& p, double seconds = 2.0)
{
    std::vector<float> sig (static_cast<size_t> (seconds * fs), 0.0f);
    test::addPluck (sig, fs, static_cast<size_t> (kPickAt * fs), p);
    test::addNoise (sig, -75.0f);
    return sig;
}

test::PluckRun runPluck (double fs, const test::Pluck& p, double seconds = 2.0)
{
    PitchTracker tracker;
    tracker.prepare (fs);
    return test::runTracker (tracker, pluckSignal (fs, p, seconds), fs, kPickAt, p.f1);
}
} // namespace

TEST_CASE ("PitchTracker stable reading of a plucked string is within 0.15 cent",
           "[analysis][tracker]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    for (const auto& s : kStrings)
    {
        test::Pluck p;
        p.f1 = s.hz;
        p.inharmonicity = s.inharmonicity;
        p.glideCents = 9.0;
        p.decaySeconds = s.hz > 300.0 ? 1.2 : 2.0;
        p.seed = static_cast<std::uint32_t> (s.hz);
        const auto run = runPluck (fs, p);
        CAPTURE (fs, s.name, run.firstStable, run.firstStableCents, run.maxStableCents,
                 run.lastStableStd);
        REQUIRE (run.firstStable > 0.0);
        CHECK (run.firstStable < 1.0);
        CHECK (run.maxStableCents < 0.15);
        CHECK (run.onsetsAfterPick == 0);
    }
}

TEST_CASE ("PitchTracker gives a first reading quickly and without octave errors",
           "[analysis][tracker]")
{
    const double fs = 48000.0;
    for (const auto& s : kStrings)
    {
        for (std::uint32_t seed = 1; seed <= 3; ++seed)
        {
            test::Pluck p;
            p.f1 = s.hz;
            p.inharmonicity = s.inharmonicity;
            p.pluckPosition = 0.12 + 0.05 * seed; // changes the balance of the partials
            p.seed = seed;
            const auto run = runPluck (fs, p, 1.5);
            CAPTURE (s.name, seed, run.firstValid, run.validAfterSkip, run.rightNoteAfterSkip);
            REQUIRE (run.firstValid > 0.0);
            CHECK (run.firstValid < (s.hz < 100.0 ? 0.12 : 0.1));
            REQUIRE (run.validAfterSkip > 100);
            CHECK (run.rightNoteAfterSkip >= 0.99 * run.validAfterSkip);
        }
    }
}

TEST_CASE ("PitchTracker reads steady tones at host rates to a fraction of a cent",
           "[analysis][tracker]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    for (double hz : { 30.87, 41.2, 82.41, 110.0, 146.83, 196.0, 246.94, 329.63, 659.26, 1318.5 })
    {
        CAPTURE (fs, hz);
        auto tone = test::harmonicTone (hz, fs, static_cast<size_t> (1.5 * fs));
        test::addNoise (tone, -75.0f);
        PitchTracker tracker;
        tracker.prepare (fs);
        const int block = static_cast<int> (fs * 0.005);
        double sum = 0.0, worst = 0.0;
        int count = 0;
        for (size_t start = 0; start + static_cast<size_t> (block) <= tone.size();
             start += static_cast<size_t> (block))
        {
            tracker.push (tone.data() + start, block);
            if (static_cast<double> (start) < 0.6 * fs)
                continue;
            const auto r = tracker.latest();
            REQUIRE (r.valid);
            const double err = test::centsError (r.frequencyHz, hz);
            sum += err;
            worst = std::max (worst, std::abs (err));
            ++count;
        }
        const auto r = tracker.latest();
        CAPTURE (worst, sum / count, r.stableStdCents);
        CHECK (worst < 0.5);
        CHECK (std::abs (sum / count) < 0.1);
        CHECK (r.stable);
        CHECK (std::abs (test::centsError (r.stableHz, hz)) < 0.1);
    }
}

TEST_CASE ("PitchTracker counts a re-pluck as a new onset and restarts the settled segment",
           "[analysis][tracker]")
{
    const double fs = 48000.0;
    for (double hz : { 82.41, 196.0, 329.63 })
    {
        CAPTURE (hz);
        test::Pluck p;
        p.f1 = hz;
        p.decaySeconds = 1.5;
        std::vector<float> sig (static_cast<size_t> (2.4 * fs), 0.0f);
        test::addPluck (sig, fs, static_cast<size_t> (0.2 * fs), p);
        // The pick stops the ringing string, then plucks it again 1 s later.
        const auto again = static_cast<size_t> (1.2 * fs);
        for (size_t i = again; i < sig.size(); ++i)
            sig[i] *=
                static_cast<float> (std::exp (-static_cast<double> (i - again) / (0.003 * fs)));
        p.seed = 9;
        test::addPluck (sig, fs, again, p);
        test::addNoise (sig, -75.0f);

        PitchTracker tracker;
        tracker.prepare (fs);
        const int block = 48;
        std::uint32_t idBefore = 0;
        bool stableBefore = false;
        for (size_t start = 0; start < sig.size(); start += static_cast<size_t> (block))
        {
            tracker.push (sig.data() + start, block);
            const double t = static_cast<double> (start + static_cast<size_t> (block)) / fs;
            const auto r = tracker.latest();
            if (t < 0.25)
                idBefore = r.onsetId;
            else if (t < 1.2)
            {
                CHECK (r.onsetId == idBefore); // no spurious onsets while the note rings
                stableBefore = stableBefore || r.stable;
            }
            else if (t > 1.21 && t < 1.3)
            {
                CHECK (r.onsetId == idBefore + 1);
                CHECK_FALSE (r.stable);
                CHECK (r.stableCount == 0);
            }
            else if (t > 2.3)
            {
                CHECK (r.onsetId == idBefore + 1);
                CHECK (r.stable);
                CHECK (std::abs (test::centsError (r.stableHz, hz)) < 0.15);
            }
        }
        CHECK (stableBefore);
    }
}

TEST_CASE ("PitchTracker stays invalid on noise and silence", "[analysis][tracker]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    PitchTracker tracker;
    tracker.prepare (fs);
    const int block = 128;

    std::vector<float> silence (static_cast<size_t> (fs), 0.0f);
    for (size_t s = 0; s + static_cast<size_t> (block) <= silence.size();
         s += static_cast<size_t> (block))
    {
        tracker.push (silence.data() + s, block);
        CHECK_FALSE (tracker.latest().valid);
    }
    CHECK (tracker.latest().levelDb < -100.0f);

    for (std::uint32_t seed = 1; seed <= 3; ++seed)
    {
        std::vector<float> noise (static_cast<size_t> (2.0 * fs), 0.0f);
        test::addNoise (noise, -30.0f, seed);
        int valid = 0;
        for (size_t s = 0; s + static_cast<size_t> (block) <= noise.size();
             s += static_cast<size_t> (block))
        {
            tracker.push (noise.data() + s, block);
            valid += tracker.latest().valid ? 1 : 0;
            CHECK_FALSE (tracker.latest().stable);
        }
        CHECK (valid == 0);
        CHECK (tracker.latest().levelDb == Approx (-30.0).margin (0.5));
    }

    // A note far below the gate is not reported either.
    auto quiet = test::harmonicTone (110.0, fs, static_cast<size_t> (fs), 4, dbToGain (-70.0f));
    tracker.reset();
    tracker.push (quiet.data(), static_cast<int> (quiet.size()));
    CHECK_FALSE (tracker.latest().valid);
}

TEST_CASE ("PitchTracker follows the reference pitch", "[analysis][tracker]")
{
    const double fs = 48000.0;
    PitchTracker tracker;
    tracker.prepare (fs);
    tracker.setReferenceHz (432.0f);
    auto tone = test::harmonicTone (432.0, fs, static_cast<size_t> (fs));
    tracker.push (tone.data(), static_cast<int> (tone.size()));
    const auto r = tracker.latest();
    REQUIRE (r.valid);
    CHECK (r.midiNote == 69);
    CHECK (r.cents == Approx (0.0).margin (0.05));
    CHECK (r.frequencyHz == Approx (432.0).epsilon (1.0e-4));

    // A 440 Hz tone is 31.77 cents sharp of A at 432.
    tracker.reset();
    auto a440 = test::harmonicTone (440.0, fs, static_cast<size_t> (fs));
    tracker.push (a440.data(), static_cast<int> (a440.size()));
    CHECK (tracker.latest().midiNote == 69);
    CHECK (tracker.latest().cents == Approx (1200.0 * std::log2 (440.0 / 432.0)).margin (0.05));
}

TEST_CASE ("PitchTracker::toNote maps frequencies to the nearest note and cents",
           "[analysis][tracker]")
{
    int midi = -1;
    float cents = 99.0f;
    const auto at = [] (double a4, double centsFromA4)
    {
        return static_cast<float> (a4 * std::exp2 (centsFromA4 / 1200.0));
    };

    PitchTracker::toNote (440.0f, 440.0f, midi, cents);
    CHECK (midi == 69);
    CHECK (cents == Approx (0.0).margin (1e-4));

    PitchTracker::toNote (at (440.0, 49.9), 440.0f, midi, cents);
    CHECK (midi == 69);
    CHECK (cents == Approx (49.9).margin (1e-3));
    PitchTracker::toNote (at (440.0, 50.1), 440.0f, midi, cents);
    CHECK (midi == 70);
    CHECK (cents == Approx (-49.9).margin (1e-3));
    PitchTracker::toNote (at (440.0, -49.9), 440.0f, midi, cents);
    CHECK (midi == 69);
    CHECK (cents == Approx (-49.9).margin (1e-3));
    PitchTracker::toNote (at (440.0, -50.1), 440.0f, midi, cents);
    CHECK (midi == 68);
    CHECK (cents == Approx (49.9).margin (1e-3));

    // Exactly half way belongs to the note above, so cents stay in [-50, +50).
    PitchTracker::toNote (at (440.0, 50.0), 440.0f, midi, cents);
    CHECK (((midi == 70 && cents == Approx (-50.0).margin (1e-3)) ||
            (midi == 69 && cents == Approx (50.0).margin (1e-3))));

    PitchTracker::toNote (82.4069f, 440.0f, midi, cents); // E2
    CHECK (midi == 40);
    CHECK (cents == Approx (0.0).margin (1e-2));
    PitchTracker::toNote (30.8677f, 440.0f, midi, cents); // B0
    CHECK (midi == 23);
    CHECK (cents == Approx (0.0).margin (1e-2));
    PitchTracker::toNote (1318.51f, 440.0f, midi, cents); // E6
    CHECK (midi == 88);
    CHECK (cents == Approx (0.0).margin (1e-2));

    PitchTracker::toNote (432.0f, 432.0f, midi, cents);
    CHECK (midi == 69);
    CHECK (cents == Approx (0.0).margin (1e-4));

    PitchTracker::toNote (0.0f, 440.0f, midi, cents);
    CHECK (midi == 0);
    CHECK (cents == 0.0f);
}

TEST_CASE ("PitchTracker stable reading follows a slow flat drift", "[analysis][tracker]")
{
    // A decaying string sags flat; the stability window must describe the recent sustain, not
    // the average since the pick.
    const double fs = 48000.0;
    test::Pluck p;
    p.f1 = 146.83;
    p.driftCentsPerSecond = -1.5;
    p.decaySeconds = 3.0;
    const auto sig = pluckSignal (fs, p, 2.5);
    PitchTracker tracker;
    tracker.prepare (fs);
    const int block = 64;
    int checked = 0;
    double firstHz = 0.0, firstT = 0.0, lastHz = 0.0, lastT = 0.0;
    for (size_t start = 0; start + static_cast<size_t> (block) <= sig.size();
         start += static_cast<size_t> (block))
    {
        tracker.push (sig.data() + start, block);
        const double t = static_cast<double> (start + static_cast<size_t> (block)) / fs - kPickAt;
        const auto r = tracker.latest();
        if (t > 1.1)
        {
            REQUIRE (r.stable);
            // Measurement lag: half the 0.4 s stability span plus half the 320 ms settled window.
            const double expected = p.hzAt (t - 0.2 - 0.16);
            CAPTURE (t, test::centsError (r.stableHz, expected));
            CHECK (std::abs (test::centsError (r.stableHz, expected)) < 0.1);
            if (firstHz == 0.0)
            {
                firstHz = r.stableHz;
                firstT = t;
            }
            lastHz = r.stableHz;
            lastT = t;
            ++checked;
        }
    }
    CHECK (checked > 100);
    CHECK (test::centsError (lastHz, firstHz) ==
           Approx (p.driftCentsPerSecond * (lastT - firstT)).margin (0.1));
}

TEST_CASE ("PitchTracker handles odd block sizes, reset and repeated prepare",
           "[analysis][tracker]")
{
    const double fs = 44100.0;
    auto tone = test::harmonicTone (196.0, fs, static_cast<size_t> (fs));
    PitchTracker tracker;
    tracker.prepare (fs);
    Random rng (5);
    size_t pos = 0;
    while (pos < tone.size())
    {
        const auto n = std::min<size_t> (1 + rng.nextU32() % 700, tone.size() - pos);
        tracker.push (tone.data() + pos, static_cast<int> (n));
        pos += n;
    }
    tracker.push (tone.data(), 0);
    REQUIRE (tracker.latest().valid);
    CHECK (std::abs (test::centsError (tracker.latest().frequencyHz, 196.0)) < 0.5);

    const auto id = tracker.latest().onsetId;
    tracker.reset();
    CHECK_FALSE (tracker.latest().valid);
    CHECK (tracker.latest().onsetId == id); // never goes backwards

    tracker.prepare (96000.0);
    auto tone96 = test::harmonicTone (196.0, 96000.0, 96000);
    tracker.push (tone96.data(), static_cast<int> (tone96.size()));
    REQUIRE (tracker.latest().valid);
    CHECK (std::abs (test::centsError (tracker.latest().frequencyHz, 196.0)) < 0.5);
}

TEST_CASE ("PitchTracker stays accurate with hum, a weak fundamental and a quiet note",
           "[analysis][tracker]")
{
    const double fs = 48000.0;
    const auto hum = [&] (std::vector<float>& s, double mains, float db)
    {
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double ph = kTwoPiD * mains * static_cast<double> (i) / fs;
            s[i] += dbToGain (db) *
                    static_cast<float> (std::sin (ph) + 0.5 * std::sin (3.0 * ph + 0.4));
        }
    };
    struct Case
    {
        const char* what;
        double hz;
        double fundamentalGain;
        float peak, noiseDb, humDb;
        double mains;
    };
    // Hum ~28 dB under the note sits 1.5 bins from E2 in a short window; the settled window's long
    // sub-windows reject it. A fundamental 20-30 dB down is inferred from partials 2 and 3.
    const Case cases[] = {
        { "E2 + 60 Hz hum", 82.41, 1.0, 0.3f, -75.0f, -45.0f, 60.0 },
        { "G3 + 60 Hz hum", 196.0, 1.0, 0.3f, -75.0f, -45.0f, 60.0 },
        { "A2 + 50 Hz hum", 110.0, 1.0, 0.3f, -75.0f, -45.0f, 50.0 },
        { "B0, fundamental -30 dB", 30.87, 0.03, 0.3f, -75.0f, -200.0f, 50.0 },
        { "E2, fundamental -20 dB", 82.41, 0.1, 0.3f, -75.0f, -200.0f, 50.0 },
        { "G3, fundamental -30 dB", 196.0, 0.03, 0.3f, -75.0f, -200.0f, 50.0 },
        { "D3, quiet over noise", 146.83, 1.0, 0.02f, -66.0f, -200.0f, 50.0 },
    };
    for (const auto& c : cases)
    {
        test::Pluck p;
        p.f1 = c.hz;
        p.fundamentalGain = c.fundamentalGain;
        p.peak = c.peak;
        std::vector<float> sig (static_cast<size_t> (2.2 * fs), 0.0f);
        test::addPluck (sig, fs, static_cast<size_t> (kPickAt * fs), p);
        test::addNoise (sig, c.noiseDb);
        if (c.humDb > -100.0f)
            hum (sig, c.mains, c.humDb);
        PitchTracker tracker;
        tracker.prepare (fs);
        const auto run = test::runTracker (tracker, sig, fs, kPickAt, c.hz);
        CAPTURE (c.what, run.firstStable, run.maxStableCents, run.lastStableStd, run.validAfterSkip,
                 run.rightNoteAfterSkip);
        REQUIRE (run.firstStable > 0.0);
        CHECK (run.firstStable < 1.0);
        CHECK (run.maxStableCents < 0.15);
        CHECK (run.rightNoteAfterSkip >= 0.99 * run.validAfterSkip);
    }
}

TEST_CASE ("PitchTracker does not read mains hum on an idle input as a note", "[analysis][tracker]")
{
    const double fs = 48000.0;
    for (double mains : { 50.0, 60.0 })
    {
        CAPTURE (mains);
        std::vector<float> sig (static_cast<size_t> (2.5 * fs), 0.0f);
        for (size_t i = 0; i < sig.size(); ++i)
        {
            const double ph = kTwoPiD * mains * static_cast<double> (i) / fs;
            sig[i] =
                dbToGain (-50.0f) * static_cast<float> (std::sin (ph) + 0.4 * std::sin (3.0 * ph));
        }
        test::Pluck p;
        p.f1 = 110.0;
        test::addPluck (sig, fs, static_cast<size_t> (1.0 * fs), p);

        PitchTracker tracker;
        tracker.prepare (fs);
        const int block = 240;
        for (size_t s = 0; s + static_cast<size_t> (block) <= sig.size();
             s += static_cast<size_t> (block))
        {
            tracker.push (sig.data() + s, block);
            if (static_cast<double> (s) < 0.99 * fs)
                CHECK_FALSE (tracker.latest().valid);
        }
        const auto r = tracker.latest();
        CHECK (r.stable);
        CHECK (std::abs (test::centsError (r.stableHz, 110.0)) < 0.15);
    }
}

TEST_CASE ("PitchTracker reads the new string while the previous one rings", "[analysis][tracker]")
{
    // Two strings a fourth apart are jointly periodic at a common subharmonic (E2 + A2: 27.5 Hz).
    // That must never be reported, and nothing is captured as stable while both ring.
    const double fs = 48000.0;
    const std::pair<double, double> changes[] = { { 82.41, 110.0 },
                                                  { 110.0, 146.83 },
                                                  { 196.0, 246.94 },
                                                  { 146.83, 110.0 },
                                                  { 329.63, 246.94 } };
    for (const auto& [ringing, plucked] : changes)
    {
        CAPTURE (ringing, plucked);
        std::vector<float> sig (static_cast<size_t> (3.0 * fs), 0.0f);
        test::Pluck a;
        a.f1 = ringing;
        a.decaySeconds = 2.5;
        test::addPluck (sig, fs, static_cast<size_t> (0.2 * fs), a);
        test::Pluck b;
        b.f1 = plucked;
        b.seed = 2;
        test::addPluck (sig, fs, static_cast<size_t> (1.2 * fs), b);
        test::addNoise (sig, -75.0f);

        PitchTracker tracker;
        tracker.prepare (fs);
        const int block = 240;
        int valid = 0, hops = 0;
        for (size_t s = 0; s + static_cast<size_t> (block) <= sig.size();
             s += static_cast<size_t> (block))
        {
            tracker.push (sig.data() + s, block);
            const double t = static_cast<double> (s + static_cast<size_t> (block)) / fs;
            const auto r = tracker.latest();
            if (t < 1.35)
                continue;
            ++hops;
            if (r.valid)
            {
                ++valid;
                CHECK (std::abs (test::centsError (r.frequencyHz, plucked)) < 50.0);
            }
            if (r.stable)
                CHECK (std::abs (test::centsError (r.stableHz, plucked)) < 0.5);
        }
        CHECK (valid > hops / 3);
    }
}

TEST_CASE ("PitchTracker starts a new segment on a legato note change", "[analysis][tracker]")
{
    // A hammer-on: the pitch steps without a new attack.
    const double fs = 48000.0;
    std::vector<float> sig (static_cast<size_t> (2.4 * fs), 0.0f);
    double ph = 0.0;
    for (size_t i = static_cast<size_t> (0.2 * fs); i < sig.size(); ++i)
    {
        const double t = static_cast<double> (i) / fs;
        ph += kTwoPiD * (t < 1.0 ? 196.0 : 220.0) / fs;
        const double env = 0.3 * std::exp (-(t - 0.2) / 2.0);
        sig[i] = static_cast<float> (
            env * (std::sin (ph) + 0.5 * std::sin (2.0 * ph) + 0.3 * std::sin (3.0 * ph)));
    }
    test::addNoise (sig, -75.0f);

    PitchTracker tracker;
    tracker.prepare (fs);
    const int block = 240;
    std::uint32_t idBefore = 0;
    double switched = -1.0;
    for (size_t s = 0; s + static_cast<size_t> (block) <= sig.size();
         s += static_cast<size_t> (block))
    {
        tracker.push (sig.data() + s, block);
        const double t = static_cast<double> (s + static_cast<size_t> (block)) / fs;
        const auto r = tracker.latest();
        if (t < 1.0)
        {
            idBefore = r.onsetId;
            continue;
        }
        if (switched < 0.0 && r.valid && std::abs (test::centsError (r.frequencyHz, 220.0)) < 1.0)
            switched = t - 1.0;
    }
    const auto r = tracker.latest();
    CHECK (r.onsetId == idBefore + 1);
    CHECK (switched > 0.0);
    CHECK (switched < 0.1);
    CHECK (r.stable);
    CHECK (std::abs (test::centsError (r.stableHz, 220.0)) < 0.05);
}
