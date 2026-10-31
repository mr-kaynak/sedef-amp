// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimeFxTestUtils.h"
#include "sedef/dsp/Drift.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <vector>

using namespace sedef;
using namespace sedef::test;
using Catch::Approx;
using sedef::dsp::Drift;

namespace
{
/// A clean, centred, wet-only echo: easy to read repeats from.
Drift::Settings cleanEcho (float timeMs)
{
    Drift::Settings s;
    s.enabled = true;
    s.sync = Drift::Free;
    s.timeMs = timeMs;
    s.feedback = 0.0f;
    s.tapeMod = 0.0f;
    s.tapeDrive = 0.0f;
    s.mix = 1.0f;
    s.width = 1.0f;
    return s;
}

Drift::Settings randomSettings (Random& rng, bool alwaysEnabled)
{
    Drift::Settings s;
    s.enabled = alwaysEnabled || rng.unipolar() < 0.7f;
    s.heads = static_cast<int> (rng.unipolar() * 4.0f);
    s.timeMs = mapExp (rng.unipolar(), 16.0f, 2500.0f);
    s.sync = static_cast<int> (rng.unipolar() * 3.0f);
    s.note = static_cast<int> (rng.unipolar() * 21.0f);
    s.tapBpm = mapLin (rng.unipolar(), 40.0f, 240.0f);
    s.feedback = rng.unipolar();
    s.tapeMod = rng.unipolar();
    s.tapeDrive = rng.unipolar();
    s.bass = rng.unipolar();
    s.treble = rng.unipolar();
    s.width = rng.unipolar();
    s.mix = rng.unipolar();
    return s;
}

void run (Drift& d, Buffer& b, int blockSize, const Transport& t = {})
{
    forEachBlock (b.view(), blockSize, [&] (AudioView v, int) { d.process (v, t); });
}

constexpr double kFs = 48000.0;
constexpr size_t kImpulseAt = 100;
} // namespace

TEST_CASE ("Drift noteBeats covers the 21 note choices", "[timefx][drift]")
{
    CHECK (Drift::noteBeats (0) == Approx (1.0 / 24.0)); // 1/64T
    CHECK (Drift::noteBeats (3) == Approx (3.0 / 32.0)); // 1/64D
    CHECK (Drift::noteBeats (7) == Approx (0.25));       // 1/16
    CHECK (Drift::noteBeats (10) == Approx (0.5));       // 1/8
    CHECK (Drift::noteBeats (11) == Approx (2.0 / 3.0)); // 1/4T
    CHECK (Drift::noteBeats (12) == Approx (0.75));      // 1/8D
    CHECK (Drift::noteBeats (13) == Approx (1.0));       // 1/4
    CHECK (Drift::noteBeats (15) == Approx (1.5));       // 1/4D
    CHECK (Drift::noteBeats (17) == Approx (8.0 / 3.0)); // 1/1T
    CHECK (Drift::noteBeats (19) == Approx (4.0));       // 1/1
    CHECK (Drift::noteBeats (20) == Approx (6.0));       // 1/1D
    // The list is sorted by length. Triplets are 2/3 and dotted notes 3/2 of their straight value.
    struct Family
    {
        int straight, triplet, dotted;
    };
    for (const Family f :
         { Family { 1, 0, 3 }, Family { 4, 2, 6 }, Family { 7, 5, 9 }, Family { 10, 8, 12 },
           Family { 13, 11, 15 }, Family { 16, 14, 18 }, Family { 19, 17, 20 } })
    {
        CHECK (Drift::noteBeats (f.triplet) == Approx (Drift::noteBeats (f.straight) * 2.0 / 3.0));
        CHECK (Drift::noteBeats (f.dotted) == Approx (Drift::noteBeats (f.straight) * 1.5));
    }
    for (int i = 1; i < 21; ++i)
        CHECK (Drift::noteBeats (i) > Drift::noteBeats (i - 1));
    CHECK (Drift::noteBeats (-5) == Drift::noteBeats (0));
    CHECK (Drift::noteBeats (99) == Drift::noteBeats (20));
}

TEST_CASE ("Drift output is finite for random settings and full-scale noise", "[timefx][drift]")
{
    const double fs = GENERATE (from_range (kTestRates));
    const int blockSize = GENERATE (from_range (kTestBlockSizes));
    Drift d;
    d.prepare ({ fs, 512, 2 });
    Random rng (31);
    Buffer b (2, static_cast<int> (fs * 1.0));
    fillNoise (b[0], 1.0f, 1);
    fillNoise (b[1], 1.0f, 2);
    Transport t { true, 133.0, true, 0.0 };
    int sinceChange = 1 << 20;
    forEachBlock (b.view(), blockSize, [&] (AudioView v, int) {
        if ((sinceChange += v.numFrames()) >= 3000)
        {
            d.setSettings (randomSettings (rng, false));
            t.hasHostTempo = rng.unipolar() < 0.5f;
            t.bpm = mapLin (rng.unipolar(), 20.0f, 300.0f);
            sinceChange = 0;
        }
        d.process (v, t);
    });
    CHECK (allFinite (b[0]));
    CHECK (allFinite (b[1]));
    CHECK (peak (b[0]) < 8.0f);
}

TEST_CASE ("Drift repeats land on the head positions of each pattern", "[timefx][drift]")
{
    const float timeMs = 120.0f;
    const double t = timeMs * 1.0e-3 * kFs;
    const std::array<std::vector<double>, 4> patterns { {
        { 1.0 },
        { 0.5, 1.0 },
        { 1.0 / 3.0, 2.0 / 3.0, 1.0 },
        { 1.0 / 3.0, 0.5, 5.0 / 6.0, 1.0 },
    } };
    for (int heads = 0; heads < 4; ++heads)
    {
        INFO ("heads " << heads + 1);
        Drift d;
        d.prepare ({ kFs, 512, 2 });
        auto s = cleanEcho (timeMs);
        s.heads = heads;
        d.setSettings (s);
        Buffer b (2, static_cast<int> (0.2 * kFs));
        b[0][kImpulseAt] = 1.0f;
        b[1][kImpulseAt] = 1.0f;
        run (d, b, 64);

        std::vector<bool> expected (b[0].size(), false);
        float weakest = 1.0f;
        for (double f : patterns[static_cast<size_t> (heads)])
        {
            const size_t at = kImpulseAt + static_cast<size_t> (std::lround (f * t));
            const size_t found = argMaxAbs (b[0], at - 20, at + 20);
            CHECK (std::abs (static_cast<long> (found) - static_cast<long> (at)) <= 3);
            weakest = std::min (weakest, std::abs (b[0][found]));
            for (size_t i = at - 20; i < at + 20; ++i)
                expected[i] = true;
        }
        CHECK (weakest > 0.05f);
        // Nothing else of note: no stray heads, no feedback (feedback is 0).
        float stray = 0.0f;
        for (size_t i = 0; i < b[0].size(); ++i)
            if (!expected[i])
                stray = std::max (stray, std::abs (b[0][i]));
        CHECK (stray < 0.1f * weakest);
        // Ping-pong: the first generation is entirely on the left.
        CHECK (peak (b[1]) < 1.0e-6f);
    }
}

TEST_CASE ("Drift width 1 alternates repeats between the sides", "[timefx][drift]")
{
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    auto s = cleanEcho (100.0f);
    s.feedback = 0.6f;
    d.setSettings (s);
    Buffer b (2, static_cast<int> (0.45 * kFs));
    b[0][kImpulseAt] = 1.0f;
    b[1][kImpulseAt] = 1.0f;
    run (d, b, 17);

    const size_t t = 4800;
    for (int k = 1; k <= 4; ++k)
    {
        const size_t at = kImpulseAt + static_cast<size_t> (k) * t;
        const auto& hot = b[k % 2 == 1 ? 0 : 1];
        const auto& cold = b[k % 2 == 1 ? 1 : 0];
        INFO ("repeat " << k);
        const size_t found = argMaxAbs (hot, at - 30, at + 30);
        CHECK (std::abs (static_cast<long> (found) - static_cast<long> (at)) <= 2 * k + 1);
        CHECK (peakIn (hot, at - 30, at + 30) > 0.02f);
        CHECK (peakIn (cold, at - 30, at + 30) < 1.0e-3f * peakIn (hot, at - 30, at + 30));
    }
    // Feedback below unity: each repeat is quieter than the last.
    CHECK (peakIn (b[1], kImpulseAt + 2 * t - 30, kImpulseAt + 2 * t + 30) <
           peakIn (b[0], kImpulseAt + t - 30, kImpulseAt + t + 30));
}

TEST_CASE ("Drift width 0 centres every repeat", "[timefx][drift]")
{
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    auto s = cleanEcho (80.0f);
    s.feedback = 0.6f;
    s.width = 0.0f;
    s.tapeMod = 0.5f;
    d.setSettings (s);
    Buffer b (2, static_cast<int> (0.5 * kFs));
    b[0][kImpulseAt] = 1.0f;
    b[1][kImpulseAt] = 1.0f;
    run (d, b, 64);
    float diff = 0.0f;
    for (size_t i = 0; i < b[0].size(); ++i)
        diff = std::max (diff, std::abs (b[0][i] - b[1][i]));
    CHECK (peak (b[0]) > 0.05f);
    CHECK (diff < 1.0e-6f);
}

TEST_CASE ("Drift host sync follows the transport tempo", "[timefx][drift]")
{
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    auto s = cleanEcho (300.0f); // free time is ignored under host sync
    s.sync = Drift::Host;
    s.note = 13; // 1/4
    s.tapBpm = 90.0f;
    d.setSettings (s);

    Buffer b (2, static_cast<int> (0.6 * kFs));
    b[0][kImpulseAt] = 1.0f;
    b[1][kImpulseAt] = 1.0f;
    const Transport host { true, 120.0, true, 0.0 };
    run (d, b, 64, host);
    CHECK (d.effectiveTimeMs() == Approx (500.0f));
    const size_t at = kImpulseAt + 24000;
    CHECK (std::abs (static_cast<long> (argMaxAbs (b[0], at - 50, at + 50)) -
                     static_cast<long> (at)) <= 3);

    Buffer one (2, 64);
    s.note = 12; // 1/8D
    d.setSettings (s);
    d.process (one.view(), host);
    CHECK (d.effectiveTimeMs() == Approx (375.0f));

    // Without a host tempo, host sync falls back to the tap tempo.
    d.process (one.view(), Transport {});
    CHECK (d.effectiveTimeMs() == Approx (500.0f)); // 90 bpm dotted eighth = 0.75 * 666.7 ms
}

TEST_CASE ("Drift tap sync uses the tap tempo and ignores the host", "[timefx][drift]")
{
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    auto s = cleanEcho (300.0f);
    s.sync = Drift::Tap;
    s.note = 13;
    s.tapBpm = 90.0f;
    d.setSettings (s);
    Buffer b (2, static_cast<int> (0.8 * kFs));
    b[0][kImpulseAt] = 1.0f;
    b[1][kImpulseAt] = 1.0f;
    run (d, b, 64, Transport { true, 120.0, true, 0.0 });
    CHECK (d.effectiveTimeMs() == Approx (666.6667f));
    const size_t at = kImpulseAt + 32000;
    CHECK (std::abs (static_cast<long> (argMaxAbs (b[0], at - 50, at + 50)) -
                     static_cast<long> (at)) <= 3);
}

TEST_CASE ("Drift clamps the delay time to 16-2500 ms", "[timefx][drift]")
{
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    Buffer one (2, 32);
    auto s = cleanEcho (5000.0f);
    d.setSettings (s);
    d.process (one.view(), {});
    CHECK (d.effectiveTimeMs() == 2500.0f);
    s.timeMs = 1.0f;
    d.setSettings (s);
    d.process (one.view(), {});
    CHECK (d.effectiveTimeMs() == 16.0f);
    s.sync = Drift::Host;
    s.note = 0; // 1/64T at 300 bpm = 8.3 ms
    d.setSettings (s);
    d.process (one.view(), Transport { true, 300.0, true, 0.0 });
    CHECK (d.effectiveTimeMs() == 16.0f);
    s.note = 20; // 1/1D at 30 bpm = 12 s
    d.setSettings (s);
    d.process (one.view(), Transport { true, 30.0, true, 0.0 });
    CHECK (d.effectiveTimeMs() == 2500.0f);
}

TEST_CASE ("Drift self-oscillates at full feedback but stays bounded", "[timefx][drift]")
{
    const float drive = GENERATE (0.0f, 1.0f);
    const float tone = GENERATE (0.5f, 1.0f);
    const bool continuousInput = GENERATE (false, true);
    INFO ("drive " << drive << " tone " << tone << " continuous " << continuousInput);
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    Drift::Settings s;
    s.enabled = true;
    s.sync = Drift::Free;
    s.timeMs = 300.0f;
    s.heads = 3;
    s.feedback = 1.05f; // beyond the knob: clamped to full, a loop gain of 1.05
    s.tapeDrive = drive;
    s.tapeMod = 1.0f;
    s.bass = tone;
    s.treble = tone;
    s.mix = 1.0f;
    d.setSettings (s);

    Buffer b (2, 48000);
    Random rng (77);
    float worst = 0.0f;
    bool finite = true;
    float lastRms = 0.0f;
    for (int second = 0; second < 30; ++second)
    {
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < b[c].size(); ++i)
                b[c][i] = (continuousInput || (second == 0 && i < 9600)) ? rng.bipolar() : 0.0f;
        run (d, b, 512);
        finite = finite && allFinite (b[0]) && allFinite (b[1]);
        worst = std::max ({ worst, peak (b[0]), peak (b[1]) });
        lastRms = rms (b[0]);
    }
    CHECK (finite);
    CHECK (worst < 4.0f);
    if (!continuousInput)
        CHECK (lastRms > 0.02f); // still singing 30 s after a 0.2 s burst
}

TEST_CASE ("Drift spillover: disabling stops new input but the tail rings out", "[timefx][drift]")
{
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    auto s = cleanEcho (100.0f);
    s.feedback = 0.5f;
    s.width = 0.0f;
    s.mix = 0.5f; // dry and wet both at full level
    d.setSettings (s);

    const size_t t = 4800;
    Buffer b (2, static_cast<int> (0.6 * kFs));
    b[0][kImpulseAt] = 1.0f;
    b[1][kImpulseAt] = 1.0f;
    const size_t secondAt = kImpulseAt + 2 * t + 1440; // 230 ms, after the input has faded out
    b[0][secondAt] = 1.0f;
    b[1][secondAt] = 1.0f;
    forEachBlock (b.view(), 64, [&] (AudioView v, int start) {
        if (start >= static_cast<int> (kImpulseAt + t + 2400)) // disable at 150 ms
        {
            s.enabled = false;
            d.setSettings (s);
        }
        d.process (v, {});
    });

    const float first = peakIn (b[0], kImpulseAt + t - 20, kImpulseAt + t + 20);
    const float third = peakIn (b[0], kImpulseAt + 3 * t - 20, kImpulseAt + 3 * t + 20);
    CHECK (first > 0.05f);
    CHECK (third > 0.1f * first); // the old tail keeps going after the switch-off
    CHECK (b[0][secondAt] == Approx (1.0f).margin (0.01f)); // dry passes at unity when off
    // ... but the new note is not recorded: no echo of it 100 ms later.
    CHECK (peakIn (b[0], secondAt + t - 20, secondAt + t + 20) < 1.0e-3f * first);

    // Once the tail is gone the effect drops out completely: bit-exact pass-through.
    Buffer quiet (2, static_cast<int> (6.0 * kFs));
    run (d, quiet, 512);
    Buffer noise (2, 4096);
    fillNoise (noise[0], 1.0f, 3);
    fillNoise (noise[1], 1.0f, 4);
    const auto dry = noise.data;
    run (d, noise, 512);
    CHECK (noise.data == dry);

    // Re-enabling records again.
    s.enabled = true;
    d.setSettings (s);
    Buffer again (2, static_cast<int> (0.2 * kFs));
    for (size_t i = 0; i < 2000; ++i)
        again[0][i] = again[1][i] = 0.5f * std::sin (0.05f * static_cast<float> (i));
    run (d, again, 64);
    CHECK (peakIn (again[0], t + 100, t + 2000) > 0.05f);
}

TEST_CASE ("Drift mix law: 0 is dry only, 1 is wet only", "[timefx][drift]")
{
    Random rng (8);
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    auto s = randomSettings (rng, true);
    s.mix = 0.0f;
    d.setSettings (s);
    Buffer b (2, 20000);
    fillNoise (b[0], 0.7f, 5);
    fillNoise (b[1], 0.7f, 6);
    const auto dry = b.data;
    run (d, b, 128);
    CHECK (b.data == dry);

    Drift w;
    w.prepare ({ kFs, 512, 2 });
    auto e = cleanEcho (50.0f);
    w.setSettings (e);
    Buffer i (2, 4000);
    i[0][kImpulseAt] = 1.0f;
    i[1][kImpulseAt] = 1.0f;
    run (w, i, 64);
    CHECK (peakIn (i[0], 0, kImpulseAt + 2000) < 1.0e-6f); // no dry impulse at mix 1
    CHECK (peakIn (i[0], kImpulseAt + 2380, kImpulseAt + 2420) > 0.05f);
}

TEST_CASE ("Drift silence in is silence out", "[timefx][drift]")
{
    Random rng (12);
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    Buffer b (2, static_cast<int> (kFs));
    int sinceChange = 1 << 20;
    forEachBlock (b.view(), 17, [&] (AudioView v, int) {
        if ((sinceChange += v.numFrames()) >= 2000)
        {
            auto s = randomSettings (rng, true);
            s.feedback = 1.0f;
            d.setSettings (s);
            sinceChange = 0;
        }
        d.process (v, {});
    });
    CHECK (peak (b[0]) == 0.0f);
    CHECK (peak (b[1]) == 0.0f);
}

TEST_CASE ("Drift output does not depend on the host block size", "[timefx][drift]")
{
    Random rng (44);
    const auto s = randomSettings (rng, true);
    std::vector<std::vector<float>> reference;
    for (int blockSize : { 512, 1, 17, 64 })
    {
        Drift d;
        d.prepare ({ kFs, 512, 2 });
        d.setSettings (s);
        Buffer b (2, 30000);
        fillNoise (b[0], 0.5f, 1);
        fillNoise (b[1], 0.5f, 2);
        run (d, b, blockSize);
        if (reference.empty())
            reference = b.data;
        else
            CHECK (b.data == reference);
    }
}

TEST_CASE ("Drift time, sync and head changes are click-free", "[timefx][drift]")
{
    // Without feedback the wet signal is the input tone replayed at a varying speed (0.25x-2x), so
    // any discontinuity stands out against its tiny second difference. With feedback, fast time
    // changes legitimately cascade the pitch upwards (each pass replays the last one sped up), and
    // a second-difference bound could no longer tell that from a click.
    const int blockSize = GENERATE (1, 64, 512);
    const unsigned seed = GENERATE (91u, 5u, 6u, 7u);
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    Random rng (seed);
    const auto randomTime = [&rng] {
        auto s = cleanEcho (mapExp (rng.unipolar(), 16.0f, 2500.0f));
        s.heads = static_cast<int> (rng.unipolar() * 4.0f);
        s.sync = static_cast<int> (rng.unipolar() * 3.0f);
        s.note = static_cast<int> (rng.unipolar() * 21.0f);
        s.tapBpm = mapLin (rng.unipolar(), 40.0f, 240.0f);
        s.width = rng.unipolar();
        s.mix = 0.5f + 0.5f * rng.unipolar();
        return s;
    };
    d.setSettings (randomTime());
    // The first 3 s fill the whole tape with the tone at a fixed setting, so no later read can
    // reach back to the tone's own onset (a kink that would read as a click).
    const int preroll = static_cast<int> (3.0 * kFs);
    Buffer b (2, preroll + static_cast<int> (kFs * 6.0));
    fillSine (b[0], 110.0, kFs, 0.3f);
    b[1] = b[0];
    int sinceChange = 0;
    Transport t { true, 120.0, true, 0.0 };
    forEachBlock (b.view(), blockSize, [&] (AudioView v, int start) {
        if (start >= preroll && (sinceChange += v.numFrames()) >= 9600)
        {
            d.setSettings (randomTime());
            t.bpm = mapLin (rng.unipolar(), 40.0f, 240.0f);
            sinceChange = 0;
        }
        d.process (v, t);
    });
    // Dry plus up to four heads replaying the 110 Hz tone at up to 2x speed: second difference
    // below ~1e-3. A step of 0.002 (-54 dBFS) would show.
    CHECK (maxSecondDifference (b[0], static_cast<size_t> (preroll)) < 1.5e-3f);
    CHECK (maxSecondDifference (b[1], static_cast<size_t> (preroll)) < 1.5e-3f);
}

TEST_CASE ("Drift tone, feedback, drive, mod, width and mix automation is click-free",
           "[timefx][drift]")
{
    const int blockSize = GENERATE (1, 64, 512);
    const unsigned seed = GENERATE (91u, 5u, 6u, 7u);
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    Random rng (seed);
    const auto randomVoice = [&rng] {
        auto s = randomSettings (rng, true);
        s.sync = Drift::Free;
        s.timeMs = 230.0f;
        s.feedback = std::min (s.feedback, 0.9f);
        return s;
    };
    d.setSettings (randomVoice());
    const int preroll = static_cast<int> (0.5 * kFs);
    Buffer b (2, preroll + static_cast<int> (kFs * 6.0));
    fillSine (b[0], 110.0, kFs, 0.3f);
    b[1] = b[0];
    int sinceChange = 0;
    forEachBlock (b.view(), blockSize, [&] (AudioView v, int start) {
        if (start >= preroll && (sinceChange += v.numFrames()) >= 9600)
        {
            d.setSettings (randomVoice());
            sinceChange = 0;
        }
        d.process (v, {});
    });
    CHECK (allFinite (b[0]));
    // Drive and treble add harmonics of the 110 Hz tone, so the bound sits well above a pure
    // sine's; a hard step of 0.02 (-34 dBFS) would still exceed it.
    CHECK (maxSecondDifference (b[0], static_cast<size_t> (preroll)) < 0.02f);
    CHECK (maxSecondDifference (b[1], static_cast<size_t> (preroll)) < 0.02f);
}

TEST_CASE ("Drift time changes glide instead of jumping", "[timefx][drift]")
{
    Drift d;
    d.prepare ({ kFs, 512, 2 });
    auto s = cleanEcho (300.0f);
    s.mix = 0.5f;
    s.feedback = 0.3f;
    d.setSettings (s);
    Buffer b (2, static_cast<int> (kFs * 3.0));
    fillSine (b[0], 220.0, kFs, 0.5f);
    b[1] = b[0];
    forEachBlock (b.view(), 64, [&] (AudioView v, int start) {
        if (start == 64 * 750) // 1 s: 300 ms -> 450 ms
        {
            s.timeMs = 450.0f;
            d.setSettings (s);
        }
        else if (start == 64 * 1500) // 2 s: 450 ms -> 20 ms
        {
            s.timeMs = 20.0f;
            d.setSettings (s);
        }
        d.process (v, {});
    });
    CHECK (d.effectiveTimeMs() == 20.0f);
    CHECK (maxSecondDifference (b[0]) < 0.05f);
}

TEST_CASE ("Drift benchmark", "[.bench][timefx-bench]")
{
    Drift d;
    d.prepare ({ 48000.0, 64, 2 });
    Drift::Settings s;
    s.enabled = true;
    s.heads = 3;
    s.sync = Drift::Free;
    s.timeMs = 420.0f;
    s.feedback = 0.6f;
    s.tapeMod = 0.5f;
    s.tapeDrive = 0.5f;
    s.mix = 0.4f;
    s.width = 0.7f;
    d.setSettings (s);
    benchmarkNsPerFrame ("Drift (4 heads, all on)", 10.0, [&] (AudioView v) { d.process (v, {}); });
}
