// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModsTestUtils.h"
#include "sedef/dsp/Anemone.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;
using dsp::Anemone;

namespace
{
constexpr double kFs = 48000.0;

void run (Anemone& a, AudioView v)
{
    a.process (v);
}

/// Holds the envelope at `level` with a steady signal on channel 0 and measures the filter on
/// channel 1 with an impulse no larger than the level (so it does not move the envelope).
std::vector<double> heldResponse (const Anemone::Settings& s, float level,
                                  const std::vector<double>& f, double fs, float* cutoff)
{
    Anemone a;
    a.prepare ({ fs, 512, 2 });
    a.setSettings (s);
    const int settle = static_cast<int> (0.5 * fs);
    const int length = 1 << 15;
    test::Buffer b (2, settle + length);
    std::fill (b[0].begin(), b[0].end(), level);
    b[1][static_cast<size_t> (settle)] = level;
    test::processInBlocks (b, 512, [&] (AudioView v) { a.process (v); });
    *cutoff = a.currentCutoffHz();
    std::vector<float> h (b[1].begin() + settle, b[1].end());
    for (auto& x : h)
        x /= level;
    return test::responseDb (h, f, fs);
}

/// Time (ms) for `values` to cross from 10 % to 90 % of the way from `from` to `to`, starting at
/// `start`.
double crossingMs (const std::vector<float>& values, size_t start, float from, float to, double fs)
{
    const auto at = [&] (float fraction) {
        const float target = from + fraction * (to - from);
        for (size_t i = start; i < values.size(); ++i)
            if ((to > from) ? values[i] >= target : values[i] <= target)
                return static_cast<double> (i);
        return static_cast<double> (values.size());
    };
    return 1000.0 * (at (0.9f) - at (0.1f)) / fs;
}
} // namespace

TEST_CASE ("Anemone output stays finite for random settings and full-scale noise",
           "[pedals][anemone]")
{
    test::checkFiniteWithRandomSettings<Anemone> (
        [] (Random& r) {
            return Anemone::Settings { r.unipolar(), r.unipolar(), r.unipolar(),
                                       static_cast<int> (r.nextU32() % 3u),
                                       static_cast<int> (r.nextU32() % 2u) };
        },
        run);
}

TEST_CASE ("Anemone passes silence as silence", "[pedals][anemone]")
{
    for (int mode : { 0, 1, 2 })
        for (int dir : { 0, 1 })
            CHECK (test::silenceOutputPeak<Anemone> (
                       Anemone::Settings { 1.0f, 1.0f, 1.0f, mode, dir }, run) == 0.0f);
}

TEST_CASE ("Anemone maps the envelope to the cutoff", "[pedals][anemone]")
{
    // The mapping: travel = 1 - exp(-env * g(sens)), cutoff = 200 Hz * 2^(octaves * travel)
    // (Down: 2^(octaves * (1 - travel))), octaves = 1 + 4 * range.
    CHECK (Anemone::cutoffHz (0.0f, 0.0f, Anemone::Up) == Approx (200.0f));
    CHECK (Anemone::cutoffHz (1.0f, 0.0f, Anemone::Up) == Approx (400.0f));
    CHECK (Anemone::cutoffHz (1.0f, 1.0f, Anemone::Up) == Approx (6400.0f));
    CHECK (Anemone::cutoffHz (0.0f, 1.0f, Anemone::Down) == Approx (6400.0f));
    CHECK (Anemone::cutoffHz (1.0f, 0.6f, Anemone::Down) == Approx (200.0f));
    CHECK (Anemone::travel (0.0f, 1.0f) == 0.0f);
    CHECK (Anemone::travel (0.1f, 0.5f) == Approx (1.0 - std::exp (-0.1 * std::pow (10.0, 0.75))));

    // The filter really sits where the mapping says: the band-pass peak of the filter, measured
    // while a steady level holds the envelope, lands on the mapped cutoff.
    const auto f = test::logFrequencies (100.0, 12000.0, 192);
    for (int dir : { Anemone::Up, Anemone::Down })
        for (float sens : { 0.25f, 0.6f })
            for (float level : { 0.03f, 0.1f, 0.4f })
            {
                const Anemone::Settings s { sens, 0.7f, 0.8f, Anemone::BandPass, dir };
                float cutoff = 0.0f;
                const auto r = heldResponse (s, level, f, kFs, &cutoff);
                const float expected = Anemone::cutoffHz (Anemone::travel (level, sens), 0.7f, dir);
                INFO ("dir " << dir << " sens " << sens << " level " << level);
                CHECK (cutoff == Approx (expected).epsilon (0.002));
                CHECK (test::findPeak (r, f).hz == Approx (expected).epsilon (0.01));
                CHECK (test::peakQ (r, f) == Approx (Anemone::resonanceToQ (0.8f)).epsilon (0.06));
            }
    // Harder playing: up sweeps up, down sweeps down.
    float softUp = 0, hardUp = 0, softDown = 0, hardDown = 0;
    heldResponse ({ 0.5f, 0.6f, 0.5f, 0, Anemone::Up }, 0.05f, { 1000.0 }, kFs, &softUp);
    heldResponse ({ 0.5f, 0.6f, 0.5f, 0, Anemone::Up }, 0.4f, { 1000.0 }, kFs, &hardUp);
    heldResponse ({ 0.5f, 0.6f, 0.5f, 0, Anemone::Down }, 0.05f, { 1000.0 }, kFs, &softDown);
    heldResponse ({ 0.5f, 0.6f, 0.5f, 0, Anemone::Down }, 0.4f, { 1000.0 }, kFs, &hardDown);
    CHECK (hardUp > 2.0f * softUp);
    CHECK (hardDown < 0.5f * softDown);
}

TEST_CASE ("Anemone's modes are low-, band- and high-pass at the cutoff", "[pedals][anemone]")
{
    // Envelope held at a level that puts the cutoff at about 1 kHz; resonance 0 (Q 0.71).
    const auto f = test::logFrequencies (60.0, 16000.0, 48);
    const auto at = [&] (const std::vector<double>& r, double hz) {
        size_t i = 0;
        while (i + 1 < f.size() && f[i] < hz)
            ++i;
        return r[i];
    };
    float cutoff = 0.0f;
    const auto lp = heldResponse ({ 0.5f, 0.6f, 0.0f, Anemone::LowPass, 0 }, 0.2f, f, kFs, &cutoff);
    const auto bp =
        heldResponse ({ 0.5f, 0.6f, 0.0f, Anemone::BandPass, 0 }, 0.2f, f, kFs, &cutoff);
    const auto hp =
        heldResponse ({ 0.5f, 0.6f, 0.0f, Anemone::HighPass, 0 }, 0.2f, f, kFs, &cutoff);
    INFO ("cutoff " << cutoff);
    REQUIRE (cutoff > 700.0f);
    REQUIRE (cutoff < 1500.0f);
    const double lo = cutoff / 8.0, hi = cutoff * 8.0;
    CHECK (at (lp, lo) - at (lp, hi) > 33.0); // 12 dB/oct on the stop side, 3 octaves out
    CHECK (at (lp, lo) == Approx (0.0).margin (0.5));
    CHECK (at (hp, hi) - at (hp, lo) > 33.0);
    CHECK (at (bp, cutoff) - at (bp, lo) > 15.0);
    CHECK (at (bp, cutoff) - at (bp, hi) > 15.0);
}

TEST_CASE ("Anemone responds like a funk envelope: fast attack, slower release",
           "[pedals][anemone]")
{
    // A 0.3 FS note for 300 ms after silence. The envelope rises 10-90 % in a few milliseconds
    // (the quack) and falls back over a few hundred (the "ow").
    Anemone a;
    a.prepare ({ kFs, 512, 1 });
    a.setSettings ({ 0.5f, 0.6f, 0.5f, Anemone::LowPass, Anemone::Up });
    const int onset = static_cast<int> (0.1 * kFs);
    const int offset = static_cast<int> (0.4 * kFs);
    test::Buffer b (1, static_cast<int> (1.4 * kFs));
    for (int i = onset; i < offset; ++i)
        b[0][static_cast<size_t> (i)] =
            0.3f * static_cast<float> (std::sin (kTwoPiD * 220.0 * (i - onset) / kFs));
    std::vector<float> env, cutoff;
    for (int i = 0; i < b.frames(); ++i)
    {
        a.process (b.view().subView (i, 1));
        env.push_back (a.currentEnvelope());
        cutoff.push_back (a.currentCutoffHz());
    }
    const float held = env[static_cast<size_t> (offset - 1)];
    CHECK (held == Approx (0.3f).margin (0.02f));

    const double attack = crossingMs (env, static_cast<size_t> (onset), 0.0f, held, kFs);
    const double release = crossingMs (env, static_cast<size_t> (offset), held, 0.0f, kFs);
    INFO ("attack " << attack << " ms, release " << release << " ms");
    CHECK (attack < 10.0);
    CHECK (release > 200.0);
    CHECK (release < 320.0);

    // The cutoff (in octaves) gets most of the way up within 10 ms of the pick.
    const float base = std::log2 (cutoff[static_cast<size_t> (onset)]);
    const float top = std::log2 (cutoff[static_cast<size_t> (offset - 1)]);
    const float at10ms = std::log2 (cutoff[static_cast<size_t> (onset + 480)]);
    CHECK (top - base > 2.0f);
    CHECK ((at10ms - base) / (top - base) > 0.8f);
}

TEST_CASE ("Anemone is stable at full resonance", "[pedals][anemone]")
{
    for (double fs : test::kAllSampleRates)
        for (int mode : { 0, 1, 2 })
            for (int dir : { 0, 1 })
            {
                INFO ("fs " << fs << " mode " << mode << " dir " << dir);
                Anemone a;
                a.prepare ({ fs, 512, 1 });
                a.setSettings ({ 1.0f, 1.0f, 1.0f, mode, dir });
                // Full-scale noise bursts, then silence: bounded while it plays, rings out after.
                test::Buffer b (1, static_cast<int> (1.5 * fs));
                std::vector<float> noise (static_cast<size_t> (0.5 * fs));
                test::fillNoise (noise, 1.0f, 5);
                std::copy (noise.begin(), noise.end(), b[0].begin());
                test::processInBlocks (b, 512, [&] (AudioView v) { a.process (v); });
                REQUIRE (test::allFinite (b[0]));
                CHECK (test::peak (b[0]) < 12.0f);
                const auto tail = static_cast<int> (1.3 * fs);
                CHECK (test::rms (b[0].data() + tail, b.frames() - tail) < 1.0e-6f);
            }
    // The resonance is what the knob says: Q 16 at the top.
    CHECK (Anemone::resonanceToQ (1.0f) == Approx (16.0f));
    CHECK (Anemone::resonanceToQ (0.0f) == Approx (0.7071f));
}

TEST_CASE ("Anemone keeps plucked notes' level across Resonance", "[pedals][anemone]")
{
    // Plucked, decaying notes (the cutoff sweeps across the harmonics as each one dies away):
    // low- and band-pass near unity, high-pass a little thinner, at every Resonance.
    for (float res : { 0.0f, 0.5f, 1.0f })
    {
        INFO ("resonance " << res);
        CHECK (test::pluckGainDb<Anemone> ({ 0.5f, 0.6f, res, Anemone::LowPass, 0 }) ==
               Approx (0.0).margin (1.5));
        CHECK (test::pluckGainDb<Anemone> ({ 0.5f, 0.6f, res, Anemone::BandPass, 0 }) ==
               Approx (0.0).margin (1.5));
        CHECK (test::pluckGainDb<Anemone> ({ 0.5f, 0.6f, res, Anemone::HighPass, 0 }) ==
               Approx (-2.0).margin (1.5));
    }
}

TEST_CASE ("Anemone automation and switches are click-free", "[pedals][anemone]")
{
    int sweep = 0;
    const auto check = [&sweep] (auto settingsAt) {
        const auto r = test::measureSweep<Anemone> (settingsAt, run, kFs, 220.0);
        INFO ("sweep " << sweep++ << ": " << r.sweep << " vs " << r.reference);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    for (int mode : { 0, 1, 2 })
    {
        check ([mode] (float t) { return Anemone::Settings { t, 0.6f, 0.5f, mode, 0 }; });
        check ([mode] (float t) { return Anemone::Settings { 0.5f, t, 0.5f, mode, 0 }; });
        check ([mode] (float t) { return Anemone::Settings { 0.5f, 0.6f, t, mode, 1 }; });
    }
    check ([] (float t) {
        return Anemone::Settings { 0.5f, 0.6f, 0.6f, static_cast<int> (t * 3.99f) % 3, 0 };
    });
    check ([] (float t) {
        return Anemone::Settings { 0.5f, 0.6f, 0.6f, 1, static_cast<int> (t * 3.99f) % 2 };
    });
}

TEST_CASE ("Anemone is linear for a steady note and block-size independent", "[pedals][anemone]")
{
    // A held note: the envelope settles, and the filter adds no harmonics of its own (the
    // envelope's release ripple at 2f is below -60 dB).
    for (int mode : { 0, 1, 2 })
    {
        const int frames = 48000;
        test::Buffer b (1, frames);
        test::fillSine (b[0], 330.0, kFs, 0.3f);
        Anemone a;
        a.prepare ({ kFs, 512, 1 });
        a.setSettings ({ 0.5f, 0.6f, 0.3f, mode, 0 });
        test::processInBlocks (b, 512, [&] (AudioView v) { a.process (v); });
        const float* tail = b[0].data() + frames / 2;
        CHECK (test::thd (tail, frames / 2, 330.0, kFs) < 1.0e-3);
    }
    for (int mode : { 0, 1, 2 })
        CHECK (test::blockSizeSpread<Anemone> ({ 0.6f, 0.7f, 0.8f, mode, mode % 2 }, kFs) <
               1.0e-6f);
}
