// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../analysis/PitchTestSignals.h"
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
            return Trench::Settings { r.unipolar(), r.unipolar(), r.unipolar(), r.unipolar() };
        },
        run);
}

TEST_CASE ("Trench passes silence as silence", "[pedals][trench]")
{
    CHECK (test::silenceOutputPeak<Trench> (Trench::Settings {}, run) == 0.0f);
    CHECK (test::silenceOutputPeak<Trench> (Trench::Settings { 1.0f, 1.0f, 1.0f, 1.0f }, run) ==
           0.0f);
}

TEST_CASE ("Trench with the subs down passes the input untouched", "[pedals][trench]")
{
    // Direct at noon is unity and the subs add exactly nothing: a bit-exact copy, no latency.
    const auto in = pluck (164.814);
    CHECK (render (in, { 0.5f, 0.0f, 0.0f, 0.5f }) == in);
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
}
