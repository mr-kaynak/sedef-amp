// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../analysis/PitchTestSignals.h"
#include "OctaverDi.h"
#include "PedalTestUtils.h"
#include "sedef/dsp/Trench.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>

using namespace sedef;
using dsp::Trench;

namespace
{
constexpr double kFs = 48000.0;
constexpr size_t kPick = 4800; // the note starts 100 ms in

// E2-E5, the range the brief asks for.
constexpr double kNotes[] = { 82.4069, 164.814, 329.628, 659.255 };

void run (Trench& t, AudioView v)
{
    t.process (v);
}

const Trench::Settings kSub1Only { 0.0f, 1.0f, 0.0f, 1.0f };
const Trench::Settings kSub2Only { 0.0f, 0.0f, 1.0f, 1.0f };
const Trench::Settings kBothSubs { 0.0f, 1.0f, 1.0f, 1.0f };

std::vector<float> render (const std::vector<float>& in, const Trench::Settings& s, double fs = kFs,
                           int block = 64)
{
    Trench t;
    t.prepare ({ fs, block, 1 });
    t.setSettings (s);
    test::Buffer b (1, static_cast<int> (in.size()));
    b[0] = in;
    test::processInBlocks (
        b, block, [&] (AudioView v) { t.process (v); }, [&] (int) { t.setSettings (s); });
    return b[0];
}

std::vector<float> pluck (double f1, double fs = kFs, double seconds = 2.0, double position = 0.18,
                          float peak = 0.3f)
{
    std::vector<float> v (static_cast<size_t> (seconds * fs), 0.0f);
    test::Pluck p;
    p.f1 = f1;
    p.pluckPosition = position;
    p.peak = peak;
    p.pickNoise = 0.15f * peak;
    test::addPluck (v, fs, static_cast<size_t> (kPick * fs / kFs), p);
    return v;
}

/// Mean frequency over [from, to) from rising zero crossings.
double measuredHz (const std::vector<float>& v, double fromSeconds, double toSeconds, double fs)
{
    const auto from = static_cast<std::ptrdiff_t> (fromSeconds * fs);
    const auto to = static_cast<std::ptrdiff_t> (toSeconds * fs);
    const std::vector<float> seg (v.begin() + from, v.begin() + to);
    const auto z = test::risingZeroCrossings (seg);
    if (z.size() < 3)
        return 0.0;
    return fs * static_cast<double> (z.size() - 1) / (z.back() - z.front());
}

/// RMS over 5 ms windows, hop 1 ms.
std::vector<float> envelope (const std::vector<float>& v, double fs)
{
    const auto w = static_cast<size_t> (0.005 * fs);
    const auto hop = static_cast<size_t> (0.001 * fs);
    std::vector<float> e;
    for (size_t i = 0; i + w <= v.size(); i += hop)
        e.push_back (test::rms (v.data() + i, static_cast<int> (w)));
    return e;
}

/// Milliseconds from the pick until the sub's envelope first reaches half its level over
/// 300-500 ms after the pick.
double lockMs (const std::vector<float>& sub, double fs)
{
    const auto e = envelope (sub, fs);
    const auto pickMs = static_cast<size_t> (1000.0 * kPick / kFs);
    float sustained = 0.0f;
    for (size_t ms = pickMs + 300; ms < pickMs + 500; ++ms)
        sustained += e[ms] / 200.0f;
    for (size_t ms = pickMs; ms < e.size(); ++ms)
        if (e[ms] >= 0.5f * sustained)
            return static_cast<double> (ms - pickMs) + 2.5; // window centre
    return 1.0e9;
}

float peakIn (const std::vector<float>& v, double fromSeconds, double toSeconds, double fs)
{
    float p = 0.0f;
    for (auto i = static_cast<size_t> (fromSeconds * fs);
         i < std::min (v.size(), static_cast<size_t> (toSeconds * fs)); ++i)
        p = std::max (p, std::abs (v[i]));
    return p;
}

/// A note that does not decay (every partial rings at its starting level), so its sub has a
/// constant target after the attack.
std::vector<float> steadyNote (double f1, double fs = kFs)
{
    std::vector<float> v (static_cast<size_t> (1.6 * fs), 0.0f);
    test::Pluck p;
    p.f1 = f1;
    p.decaySeconds = 1.0e4;
    p.pickNoise = 0.045f;
    test::addPluck (v, fs, static_cast<size_t> (kPick * fs / kFs), p);
    return v;
}

/// An open E major strum, low to high, 12 ms between strings, at about the same peak as one note.
std::vector<float> strum (double fs = kFs)
{
    std::vector<float> v (static_cast<size_t> (2.0 * fs), 0.0f);
    const double notes[] = { 82.4069, 123.471, 164.814, 207.652, 246.942, 329.628 };
    for (size_t s = 0; s < 6; ++s)
    {
        test::Pluck p;
        p.f1 = notes[s];
        p.peak = 0.12f;
        p.pickNoise = 0.02f;
        p.seed = static_cast<std::uint32_t> (s + 3);
        test::addPluck (v, fs, kPick + static_cast<size_t> (0.012 * fs) * s, p);
    }
    return v;
}
} // namespace

TEST_CASE ("Trench output stays finite for random settings and full-scale noise",
           "[pedals][trench]")
{
    test::checkFiniteWithRandomSettings<Trench> (
        [] (Random& r) {
            return Trench::Settings { r.unipolar(), r.unipolar(), r.unipolar(), r.unipolar(),
                                      r.unipolar() < 0.5f ? Trench::Mono : Trench::Poly };
        },
        run);
}

TEST_CASE ("Trench passes silence as silence", "[pedals][trench]")
{
    CHECK (test::silenceOutputPeak<Trench> (Trench::Settings {}, run) == 0.0f);
    CHECK (test::silenceOutputPeak<Trench> (Trench::Settings { 1.0f, 1.0f, 1.0f, 1.0f }, run) ==
           0.0f);
    CHECK (test::silenceOutputPeak<Trench> (
               Trench::Settings { 1.0f, 1.0f, 1.0f, 1.0f, Trench::Poly }, run) == 0.0f);
}

TEST_CASE ("Trench with the subs down passes the input untouched", "[pedals][trench]")
{
    // Direct at noon is unity and the subs add exactly nothing: a bit-exact copy, no latency.
    const auto in = pluck (164.814);
    CHECK (render (in, { 0.5f, 0.0f, 0.0f, 0.5f }) == in);
    CHECK (render (in, { 0.5f, 0.0f, 0.0f, 0.5f, Trench::Poly }) == in);
    CHECK (Trench().latencySamples() == 0);
}

TEST_CASE ("Trench subs sit at f/2 and f/4 from E2 to E5", "[pedals][trench]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (double f1 : kNotes)
            for (double position : { 0.12, 0.25 })
            {
                INFO ("fs " << fs << ", note " << f1 << " Hz, pluck at " << position);
                const auto in = pluck (f1, fs, 1.6, position);
                const double sub1 = measuredHz (render (in, kSub1Only, fs), 0.4, 1.5, fs);
                const double sub2 = measuredHz (render (in, kSub2Only, fs), 0.4, 1.5, fs);
                CHECK (std::abs (test::centsError (sub1, f1 / 2.0)) < 1.0);
                CHECK (std::abs (test::centsError (sub2, f1 / 4.0)) < 1.0);
            }
}

TEST_CASE ("Trench locks onto a note within a few of its periods", "[pedals][trench]")
{
    // The sub needs a few edges that agree before it speaks: about four periods of the note plus
    // the gate's 4 ms attack. Bounds: E2 (12 ms period) 60 ms, E3 and up 35 ms.
    for (double f1 : kNotes)
    {
        INFO ("note " << f1 << " Hz");
        const double ms = lockMs (render (pluck (f1), kSub1Only), kFs);
        CHECK (ms < (f1 < 100.0 ? 60.0 : 35.0));
    }
}

TEST_CASE ("Trench subs are about as loud as the note at noon", "[pedals][trench]")
{
    for (double f1 : kNotes)
    {
        INFO ("note " << f1 << " Hz");
        const auto in = pluck (f1);
        const auto sub = render (in, { 0.0f, 0.5f, 0.0f, 0.5f });
        const auto from = static_cast<size_t> (0.4 * kFs), to = static_cast<size_t> (0.9 * kFs);
        const double ratio = test::db (test::rms (sub.data() + from, static_cast<int> (to - from)) /
                                       test::rms (in.data() + from, static_cast<int> (to - from)));
        CHECK (std::abs (ratio) < 3.0);
    }
}

TEST_CASE ("Trench does not burst on onsets, chords or releases", "[pedals][trench]")
{
    // Reference: the sub's peak on the same note once it has settled. The notes here do not decay,
    // so anything louder during the attack would be the tracker's doing. A strum and a muted note
    // are bounded the same way.
    const double pickS = static_cast<double> (kPick) / kFs;
    for (double f1 : kNotes)
    {
        INFO ("note " << f1 << " Hz");
        const auto sub = render (steadyNote (f1), kBothSubs);
        const float steady = peakIn (sub, pickS + 0.5, pickS + 1.0, kFs);
        CHECK (steady > 0.1f);
        // 10 %: the sampled peak of a band-limited square varies by a few per cent with where its
        // edges fall between samples.
        CHECK (peakIn (sub, pickS, pickS + 0.5, kFs) <= 1.1f * steady);
    }

    const auto reference = render (steadyNote (164.814), kBothSubs);
    const auto chordIn = strum();
    const auto chord = render (chordIn, kBothSubs);
    INFO ("strum peak " << test::peak (chordIn));
    CHECK (test::peak (chordIn) >= 0.15f);
    CHECK (test::peak (chord) <= peakIn (reference, pickS + 0.5, pickS + 1.0, kFs));

    // A muted note: the sub never rises after the mute and is 60 dB down within 150 ms.
    std::vector<float> in (static_cast<size_t> (1.5 * kFs), 0.0f);
    test::Pluck p;
    p.f1 = 164.814;
    p.muteAt = 0.5;
    test::addPluck (in, kFs, kPick, p);
    const auto muted = render (in, kBothSubs);
    const double muteS = pickS + 0.5;
    const float before = peakIn (muted, muteS - 0.1, muteS, kFs);
    CHECK (peakIn (muted, muteS, muteS + 0.3, kFs) <= before);
    CHECK (peakIn (muted, muteS + 0.15, muteS + 0.5, kFs) < 0.001f * before);
}

TEST_CASE ("Trench re-locks when the note changes under the finger", "[pedals][trench]")
{
    // Legato: a fifth up, an octave down, an octave up. The sub follows within 30 ms, lands in tune
    // and never exceeds its sustained level.
    for (double cents : { 700.0, -1200.0, 1200.0 })
    {
        INFO ("step " << cents << " cents");
        std::vector<float> in (static_cast<size_t> (2.0 * kFs), 0.0f);
        test::Pluck p;
        p.f1 = 164.814;
        p.stepAt = 0.5;
        p.stepCents = cents;
        test::addPluck (in, kFs, kPick, p);
        const double f2 = p.f1 * std::exp2 (cents / 1200.0);
        const double stepS = static_cast<double> (kPick) / kFs + 0.5;

        Trench t;
        t.prepare ({ kFs, 64, 1 });
        t.setSettings (kSub1Only);
        test::Buffer b (1, static_cast<int> (in.size()));
        b[0] = in;
        double relockMs = 1.0e9;
        test::processInBlocks (b, 64, [&] (AudioView v) {
            t.process (v);
            const double now = static_cast<double> (v.channel (0) - b[0].data() + 64) / kFs;
            if (now > stepS && relockMs > 1.0e8 &&
                std::abs (test::centsError (t.trackState().hz, f2)) < 20.0 &&
                t.trackState().subGain > 0.5f)
                relockMs = 1000.0 * (now - stepS);
        });
        CHECK (relockMs < 30.0);
        CHECK (std::abs (test::centsError (measuredHz (b[0], stepS + 0.2, stepS + 1.2, kFs),
                                           f2 / 2.0)) < 1.0);
        CHECK (peakIn (b[0], stepS, stepS + 0.2, kFs) <=
               1.1f * peakIn (b[0], stepS - 0.3, stepS, kFs));
    }
}

TEST_CASE ("Trench Mono keeps a steady sub on real playing", "[pedals][trench]")
{
    // The playing that made the first tracker chop: a bridge pickup or a piezo (2nd and 3rd
    // partials above the fundamental), a loud pick, the previous note still ringing, vibrato and
    // a whole-step bend. After the lock, on every note: no gate flip, the sub at 90 % of its
    // settled level (against the input it follows) for 95 % of the note, no modulation above
    // 5 Hz deeper than 3 dB, both subs within 5 cents and every sub period in the right octave.
    // Lock: 30 ms below A3, 15 ms from A3 up (about two periods and the gate's attack).
    using namespace test::octaver;
    std::vector<Scenario> all;
    for (double f : { kE2, kA2, kE3, kA3, kE4, kE5 })
    {
        all.push_back (single ("bridge", bridgePluck (f), kFs));
        all.push_back (single ("piezo", piezoPluck (f), kFs));
        auto loud = bridgePluck (f);
        loud.pickNoise = 0.3f;
        all.push_back (single ("pick noise", loud, kFs));
    }
    for (double f : { kE2, kA2, kE3, kA3 })
        for (double cents : { -500.0, 700.0 })
            all.push_back (ringingNeighbour (f, cents, kFs));
    for (double f : { kE2, kA3 })
    {
        all.push_back (vibrato (f, kFs));
        all.push_back (bend (f, kFs));
    }

    for (const auto& s : all)
    {
        const auto band = envelopeBand (s.signal, kFs);
        const auto r1 = runTrench (s.signal, kFs, kSub1Only);
        const auto r2 = runTrench (s.signal, kFs, kSub2Only);
        const auto& n = s.notes[0];
        const double f = n.pluck.f1;
        INFO (s.name << ", " << f << " Hz");
        const auto st = judge (r1, band, kFs, n, 2.0);
        const auto st2 = judge (r2, band, kFs, n, 4.0);
        CHECK (st.flips == 0);
        CHECK (st.held >= 0.95);
        CHECK (st.amDb <= 3.0);
        CHECK (std::abs (st.cents) < 5.0);
        CHECK (std::abs (st2.cents) < 5.0);
        CHECK (st.rightOctave > 0.99);
        CHECK (st2.rightOctave > 0.99);
        CHECK (st.lockMs <= (f < kA3 ? 30.0 : 15.0));
    }
}

TEST_CASE ("Trench Mono follows palm-muted sixteenths", "[pedals][trench]")
{
    // 140 bpm on E2 and A2. The first note locks within the bars above; after that the gate stays
    // open across the picks, every chug gets its sub at a steady level, and the sub's periods are
    // in the right octave (each pick restarts the string, so the sub's phase may jump there).
    using namespace test::octaver;
    for (double f : { kE2, kA2 })
    {
        INFO (f << " Hz");
        const auto s = palmMuted (f, kFs);
        const auto band = envelopeBand (s.signal, kFs);
        const auto r = runTrench (s.signal, kFs, kSub1Only);
        const auto first = judge (r, band, kFs, s.notes[0], 2.0);
        CHECK (first.lockMs <= 30.0);
        const auto chugs =
            judgeChugs (r, band, kFs, s.notes, s.notes[0].start + first.lockMs / 1000.0);
        CHECK (chugs.flips == 0);
        CHECK (chugs.spreadDb < 3.0);
        auto all = s.notes[0];
        all.end = s.notes.back().end;
        CHECK (judge (r, band, kFs, all, 2.0).rightOctave > 0.95);
    }
}

TEST_CASE ("Trench Mono re-locks through a legato run", "[pedals][trench]")
{
    // Hammer-ons a whole step apart every 150 ms: each step's sub is right within 15 ms, the gate
    // never drops, and no step's sub peaks above the previous step's.
    using namespace test::octaver;
    for (double f : { kA2, kE3 })
    {
        const auto s = legatoRun (f, kFs);
        const auto band = envelopeBand (s.signal, kFs);
        const auto r = runTrench (s.signal, kFs, kSub1Only);
        for (size_t k = 1; k < s.notes.size(); ++k)
        {
            INFO (f << " Hz, step " << k);
            const auto st = judge (r, band, kFs, s.notes[k], 2.0, 0.0);
            CHECK (st.lockMs <= 15.0);
            CHECK (st.flips == 0);
            CHECK (st.rightOctave > 0.99);
            CHECK (peakIn (r.out, s.notes[k].start, s.notes[k].start + 0.05, kFs) <=
                   1.1f * peakIn (r.out, s.notes[k - 1].start + 0.05, s.notes[k].start, kFs));
        }
    }
}

TEST_CASE ("Trench keeps the subs silent at low input", "[pedals][trench]")
{
    // A note picked 50 dB below a normal one, and hiss at -60 dBFS RMS: the gate stays shut, so the
    // subs add exactly nothing. A light note 20 dB down still gets its sub.
    CHECK (test::peak (render (pluck (164.814, kFs, 2.0, 0.18, 0.001f), kBothSubs)) == 0.0f);
    std::vector<float> hiss (static_cast<size_t> (kFs), 0.0f);
    test::addNoise (hiss, -60.0f);
    CHECK (test::peak (render (hiss, kBothSubs)) == 0.0f);
    const auto light = render (pluck (164.814, kFs, 2.0, 0.18, 0.03f), kSub1Only);
    CHECK (std::abs (test::centsError (measuredHz (light, 0.4, 1.5, kFs), 82.407)) < 1.0);

    // Poly shares the level gate.
    const Trench::Settings polySubs { 0.0f, 1.0f, 1.0f, 1.0f, Trench::Poly };
    CHECK (test::peak (render (pluck (164.814, kFs, 2.0, 0.18, 0.001f), polySubs)) == 0.0f);
    CHECK (test::peak (render (hiss, polySubs)) == 0.0f);
    const auto lightIn = pluck (164.814, kFs, 2.0, 0.18, 0.03f);
    const auto lightPoly = render (lightIn, { 0.0f, 0.5f, 0.0f, 1.0f, Trench::Poly });
    const auto from = static_cast<size_t> (0.4 * kFs), to = static_cast<size_t> (1.4 * kFs);
    CHECK (std::abs (test::db (test::rms (lightPoly.data() + from, static_cast<int> (to - from)) /
                               test::rms (lightIn.data() + from, static_cast<int> (to - from)))) <
           3.0);
}

TEST_CASE ("Trench automation is click-free", "[pedals][trench]")
{
    const auto check = [] (auto settingsAt) {
        const auto r = test::measureSweep<Trench> (settingsAt, run);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return Trench::Settings { t, 0.5f, 0.0f, 0.5f }; });
    check ([] (float t) { return Trench::Settings { 0.5f, t, 0.0f, 0.5f }; });
    check ([] (float t) { return Trench::Settings { 0.5f, 0.5f, t, 0.5f }; });
    check ([] (float t) { return Trench::Settings { 0.5f, 0.5f, 0.5f, t }; });
    check ([] (float t) { return Trench::Settings { 1.0f - t, t, 1.0f - t, t }; });
    // Mode changes crossfade.
    check ([] (float t) {
        return Trench::Settings { 0.5f, 0.5f, 0.5f, 0.5f, t < 0.5f ? Trench::Mono : Trench::Poly };
    });
    check ([] (float t) {
        return Trench::Settings { 0.5f, 0.5f, 0.5f, 0.5f, t < 0.5f ? Trench::Poly : Trench::Mono };
    });
    check ([] (float t) { return Trench::Settings { t, 0.5f, 0.0f, 0.5f, Trench::Poly }; });
    check ([] (float t) { return Trench::Settings { 0.5f, t, 0.0f, 0.5f, Trench::Poly }; });
    check ([] (float t) { return Trench::Settings { 0.5f, 0.5f, t, 0.5f, Trench::Poly }; });
    check ([] (float t) { return Trench::Settings { 0.5f, 0.5f, 0.0f, t, Trench::Poly }; });
    check ([] (float t) { return Trench::Settings { 0.5f, t, t, t, Trench::Poly }; });
}

TEST_CASE ("Trench Poly divides notes and chords at about their level", "[pedals][trench]")
{
    // At noon, Poly's sub is about as loud as what it divides (within 3 dB, over the second after
    // the pick), on single notes and on an open E strum. (Mono follows the strum's root, whose
    // harmonics the other strings are.) How well each string of a chord is divided is judged in
    // PolySubTests.cpp.
    const Trench::Settings polyNoon { 0.0f, 0.5f, 0.0f, 1.0f, Trench::Poly };
    const auto from = static_cast<size_t> (0.4 * kFs), to = static_cast<size_t> (1.4 * kFs);
    const auto levelDb = [&] (const std::vector<float>& out, const std::vector<float>& in) {
        return test::db (test::rms (out.data() + from, static_cast<int> (to - from)) /
                         test::rms (in.data() + from, static_cast<int> (to - from)));
    };
    for (double f1 : { 82.4069, 164.814, 329.628 })
    {
        INFO ("note " << f1 << " Hz");
        const auto in = pluck (f1);
        CHECK (std::abs (levelDb (render (in, polyNoon), in)) < 3.0);
    }
    const auto chord = strum();
    CHECK (std::abs (levelDb (render (chord, polyNoon), chord)) < 3.0);
}

TEST_CASE ("Trench measurements", "[.report]")
{
    std::printf ("Trench, 48 kHz, pluck at 0.18 of the string\n");
    std::printf (
        "note      sub1 (cents)  sub2 (cents)  lock (ms)  onset/steady peak  noon level\n");
    const char* names[] = { "E2", "E3", "E4", "E5" };
    for (size_t k = 0; k < 4; ++k)
    {
        const double f1 = kNotes[k];
        const auto in = pluck (f1);
        const auto s1 = render (in, kSub1Only);
        const auto s2 = render (in, kSub2Only);
        const auto both = render (in, kBothSubs);
        const auto noon = render (in, { 0.0f, 0.5f, 0.0f, 0.5f });
        const double pickS = static_cast<double> (kPick) / kFs;
        const auto from = static_cast<size_t> (0.4 * kFs), to = static_cast<size_t> (0.9 * kFs);
        const auto steady = render (steadyNote (f1), kBothSubs);
        const float onset = peakIn (steady, pickS, pickS + 0.5, kFs) /
                            peakIn (steady, pickS + 0.5, pickS + 1.0, kFs);
        std::printf ("%s %6.1f  %+10.3f  %+12.3f  %9.1f  %17.2f  %+8.2f dB\n", names[k], f1,
                     test::centsError (measuredHz (s1, 0.4, 1.5, kFs), f1 / 2.0),
                     test::centsError (measuredHz (s2, 0.4, 1.5, kFs), f1 / 4.0), lockMs (s1, kFs),
                     onset,
                     test::db (test::rms (noon.data() + from, static_cast<int> (to - from)) /
                               test::rms (in.data() + from, static_cast<int> (to - from))));
    }
    const auto chord = render (strum(), kBothSubs);
    const auto ref = render (steadyNote (164.814), kBothSubs);
    std::printf ("strum: sub peak %.3f vs a steady E3's sub peak %.3f\n", test::peak (chord),
                 peakIn (ref, 0.6, 1.1, kFs));

    // Mono on real playing: the scenarios of "Trench Mono keeps a steady sub on real playing".
    using namespace test::octaver;
    std::printf ("\nMono on real playing, sub -1 (sub -2 cents in brackets)\n");
    std::printf ("scenario                      note     lock  flips  held   AM     cents        "
                 "right octave\n");
    std::vector<Scenario> all;
    for (double f : { kE2, kA2, kE3, kA3, kE4, kE5 })
    {
        all.push_back (single ("bridge", bridgePluck (f), kFs));
        all.push_back (single ("piezo", piezoPluck (f), kFs));
        auto loud = bridgePluck (f);
        loud.pickNoise = 0.3f;
        all.push_back (single ("pick noise", loud, kFs));
    }
    for (double f : { kE2, kA2, kE3, kA3 })
        for (double cents : { -500.0, 700.0 })
            all.push_back (ringingNeighbour (f, cents, kFs));
    for (double f : { kE2, kA3 })
    {
        all.push_back (vibrato (f, kFs));
        all.push_back (bend (f, kFs));
    }
    for (const auto& sc : all)
    {
        const auto band = envelopeBand (sc.signal, kFs);
        const auto st = judge (runTrench (sc.signal, kFs, kSub1Only), band, kFs, sc.notes[0], 2.0);
        const auto st2 = judge (runTrench (sc.signal, kFs, kSub2Only), band, kFs, sc.notes[0], 4.0);
        std::printf ("%-28s %6.1f  %5.1f ms  %d  %5.1f %%  %4.1f dB  %+5.2f (%+5.2f)  %5.1f %%\n",
                     sc.name.c_str(), sc.notes[0].pluck.f1, st.lockMs, st.flips, 100.0 * st.held,
                     st.amDb, st.cents, st2.cents, 100.0 * st.rightOctave);
    }
    for (double f : { kE2, kA2 })
    {
        const auto sc = palmMuted (f, kFs);
        const auto band = envelopeBand (sc.signal, kFs);
        const auto r = runTrench (sc.signal, kFs, kSub1Only);
        const auto first = judge (r, band, kFs, sc.notes[0], 2.0);
        const auto chugs =
            judgeChugs (r, band, kFs, sc.notes, sc.notes[0].start + first.lockMs / 1000.0);
        std::printf ("palm-muted sixteenths, %5.1f Hz: first lock %.1f ms, %d flips after, chug "
                     "levels within %.2f dB\n",
                     f, first.lockMs, chugs.flips, chugs.spreadDb);
    }
    for (double f : { kA2, kE3 })
    {
        const auto sc = legatoRun (f, kFs);
        const auto band = envelopeBand (sc.signal, kFs);
        const auto r = runTrench (sc.signal, kFs, kSub1Only);
        std::printf ("legato run from %5.1f Hz: re-lock", f);
        for (size_t k = 1; k < sc.notes.size(); ++k)
            std::printf (" %.1f", judge (r, band, kFs, sc.notes[k], 2.0, 0.0).lockMs);
        std::printf (" ms\n");
    }
}
