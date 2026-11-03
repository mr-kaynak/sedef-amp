// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModsTestUtils.h"
#include "sedef/dsp/Jetty.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;
using dsp::Jetty;

namespace
{
constexpr double kFs = 48000.0;

void run (Jetty& j, AudioView v)
{
    j.process (v);
}

std::vector<double> staticResponse (float delayMs, float feedback, double fs,
                                    const std::vector<double>& f)
{
    return test::responseDb (
        test::impulseResponse<Jetty> ({ 1.0f, 0.0f, delayMs, feedback }, fs, 1 << 15), f, fs);
}

/// Delay trajectory (ms) over `seconds`.
std::vector<float> delayTrajectory (const Jetty::Settings& s, double fs, double seconds)
{
    Jetty j;
    j.prepare ({ fs, 512, 1 });
    j.setSettings (s);
    std::vector<float> d;
    test::Buffer b (1, static_cast<int> (seconds * fs));
    for (int i = 0; i < b.frames(); ++i)
    {
        j.process (b.view().subView (i, 1));
        d.push_back (j.currentDelayMs());
    }
    return d;
}

} // namespace

TEST_CASE ("Jetty output stays finite for random settings and full-scale noise", "[pedals][jetty]")
{
    test::checkFiniteWithRandomSettings<Jetty> (
        [] (Random& r) {
            return Jetty::Settings { 0.05f + 4.95f * r.unipolar(), r.unipolar(),
                                     0.25f + 9.75f * r.unipolar(), r.bipolar() };
        },
        run);
}

TEST_CASE ("Jetty passes silence as silence", "[pedals][jetty]")
{
    CHECK (test::silenceOutputPeak<Jetty> (Jetty::Settings { 5.0f, 1.0f, 10.0f, 1.0f }, run) ==
           0.0f);
    CHECK (test::silenceOutputPeak<Jetty> (Jetty::Settings { 5.0f, 1.0f, 0.25f, -1.0f }, run) ==
           0.0f);
}

TEST_CASE ("Jetty's comb notches sit at odd multiples of half the delay's frequency",
           "[pedals][jetty]")
{
    // Depth 0 and no feedback: dry + delayed, so |H| = |cos(pi f d)|: notches at (k + 1/2) / d.
    // Integer delays at 48 kHz, fractional ones at 44.1 kHz (the interpolated read).
    for (double fs : { 48000.0, 44100.0, 96000.0 })
        for (float ms : { 1.0f, 2.0f, 5.0f })
        {
            INFO ("fs " << fs << " delay " << ms << " ms");
            const double first = 500.0 / ms; // Hz
            const auto f = test::logFrequencies (0.5 * first, 5.5 * first, 384);
            const auto r = staticResponse (ms, 0.0f, fs, f);
            const auto notches = test::findNotches (r, f, 20.0);
            REQUIRE (notches.size() >= 3);
            for (size_t k = 0; k < 3; ++k)
            {
                CHECK (notches[k].hz == Approx ((2.0 * k + 1.0) * first).epsilon (0.01));
                CHECK (notches[k].db < -30.0);
            }
        }
}

TEST_CASE ("Jetty's feedback polarity picks which comb teeth ring", "[pedals][jetty]")
{
    // 1 ms. Positive feedback rings at multiples of 1 kHz (between the notches); negative feedback
    // rings at the odd multiples of 500 Hz, where the notches were, and the dry signal no longer
    // cancels there. Peaks found within 3 % of the teeth, standing well above the troughs.
    const auto f = test::logFrequencies (350.0, 3300.0, 384);
    const auto peaks = [&] (float feedback) {
        auto r = staticResponse (1.0f, feedback, kFs, f);
        for (auto& x : r)
            x = -x;
        auto p = test::findNotches (r, f, 6.0); // maxima of the response
        for (auto& e : p)
            e.db = -e.db;
        return p;
    };
    const auto pos = peaks (0.8f);
    REQUIRE (pos.size() == 3);
    for (size_t k = 0; k < 3; ++k)
        CHECK (pos[k].hz == Approx (1000.0 * (k + 1)).epsilon (0.03));
    const auto neg = peaks (-1.0f);
    REQUIRE (neg.size() >= 3);
    // (The feedback path's coupling capacitor leads the phase a little at 500 Hz, which pulls the
    // first negative-feedback tooth about 3 % up.)
    for (size_t k = 0; k < 3; ++k)
        CHECK (neg[k].hz == Approx (500.0 * (2 * k + 1)).epsilon (0.04));

    const auto contrast = [&] (float feedback) {
        const auto r = staticResponse (1.0f, feedback, kFs, f);
        const auto [lo, hi] = std::minmax_element (r.begin(), r.end());
        return *hi - *lo;
    };
    CHECK (contrast (0.8f) > 20.0);
    CHECK (contrast (-1.0f) > 15.0);

    // Feedback sharpens the teeth: the 1 kHz tooth's Q goes from the plain comb's 2 to well above.
    const auto tooth = test::logFrequencies (600.0, 1600.0, 768);
    CHECK (test::peakQ (staticResponse (1.0f, 0.0f, kFs, tooth), tooth) ==
           Approx (2.0).epsilon (0.05));
    CHECK (test::peakQ (staticResponse (1.0f, 0.8f, kFs, tooth), tooth) > 6.0);
}

TEST_CASE ("Jetty's sweep follows Speed and Depth", "[pedals][jetty]")
{
    // Depth d sweeps the delay 1.5 d octaves either side of Delay, at the Speed.
    for (double fs : { 44100.0, 192000.0 })
        for (float depth : { 0.4f, 1.0f })
            for (float speed : { 0.05f, 0.5f, 5.0f })
            {
                INFO ("fs " << fs << " depth " << depth << " speed " << speed);
                const double seconds = std::max (3.0, 2.5 / speed);
                const auto d = delayTrajectory ({ speed, depth, 3.0f, 0.0f }, fs, seconds);
                const auto [lo, hi] = std::minmax_element (d.begin(), d.end());
                CHECK (*hi == Approx (3.0f * std::exp2 (1.5f * depth)).epsilon (0.005));
                CHECK (*lo == Approx (3.0f * std::exp2 (-1.5f * depth)).epsilon (0.005));
                // Period between the longest delays.
                std::vector<size_t> peaks;
                for (size_t i = 1; i + 1 < d.size(); ++i)
                    if (d[i] > d[i - 1] && d[i] >= d[i + 1])
                        peaks.push_back (i);
                REQUIRE (peaks.size() >= 2);
                const double period = static_cast<double> (peaks.back() - peaks.front()) /
                                      static_cast<double> (peaks.size() - 1);
                CHECK (fs / period == Approx (speed).epsilon (0.01));
            }
    // The delay stays inside the line's range at the extremes.
    CHECK (Jetty::sweepDelayMs (10.0f, 1.0f, 1.0f) == Approx (16.0f));
    CHECK (Jetty::sweepDelayMs (0.25f, 1.0f, -1.0f) == Approx (0.1f));

    // In the audio: a 1 kHz tone through the plain comb is cut every time a notch passes it,
    // once per millisecond of delay travel. Over one LFO cycle (up and back) the delay covers
    // twice its sweep, so a 1 kHz tone dips about 2 * 1000 * (dmax - dmin) times.
    for (float depth : { 0.3f, 1.0f })
    {
        Jetty j;
        j.prepare ({ kFs, 512, 1 });
        j.setSettings ({ 0.5f, depth, 4.0f, 0.0f });
        test::Buffer b (1, static_cast<int> (3.2 * kFs));
        test::fillSine (b[0], 1000.0, kFs, 0.25f);
        test::processInBlocks (b, 512, [&] (AudioView v) { j.process (v); });
        // One whole cycle, 1.0 s to 3.0 s, in 2.5 ms windows.
        const auto env =
            test::rmsEnvelope (std::vector<float> (b[0].begin() + static_cast<long> (kFs),
                                                   b[0].begin() + static_cast<long> (3.0 * kFs)),
                               120);
        const float top = test::peak (env);
        int dips = 0;
        for (size_t i = 1; i + 1 < env.size(); ++i)
            if (env[i] < env[i - 1] && env[i] <= env[i + 1] && env[i] < 0.3f * top)
                ++dips;
        const double sweepMs =
            4.0 * (std::exp2 (1.5 * depth) - std::exp2 (-1.5 * depth)); // dmax - dmin
        INFO ("depth " << depth << " dips " << dips);
        CHECK (dips == Approx (2.0 * sweepMs).margin (2.0));
    }
}

TEST_CASE ("Jetty is stable at full feedback", "[pedals][jetty]")
{
    for (double fs : { 44100.0, 96000.0, 192000.0 })
        for (float feedback : { -1.0f, 1.0f })
            for (float delay : { 0.25f, 10.0f })
            {
                INFO ("fs " << fs << " feedback " << feedback << " delay " << delay);
                Jetty j;
                j.prepare ({ fs, 512, 1 });
                j.setSettings ({ 5.0f, 1.0f, delay, feedback });
                test::Buffer b (1, static_cast<int> (4.0 * fs));
                std::vector<float> noise (static_cast<size_t> (0.5 * fs));
                test::fillNoise (noise, 1.0f, 13);
                std::copy (noise.begin(), noise.end(), b[0].begin());
                test::processInBlocks (b, 512, [&] (AudioView v) { j.process (v); });
                REQUIRE (test::allFinite (b[0]));
                CHECK (test::peak (b[0]) < 3.0f); // the line's soft limit holds the loop
                const auto tail = static_cast<int> (3.5 * fs);
                CHECK (test::rms (b[0].data() + tail, b.frames() - tail) < 1.0e-5f);
            }
}

TEST_CASE ("Jetty's interpolated delay sweeps without zipper noise", "[pedals][jetty]")
{
    const auto check = [] (auto settingsAt) {
        const auto r = test::measureSweep<Jetty> (settingsAt, run, kFs, 220.0);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return Jetty::Settings { 0.3f, 0.0f, 0.25f + 9.75f * t, 0.5f }; });
    check ([] (float t) { return Jetty::Settings { 0.3f, t, 2.0f, 0.5f }; });
    check ([] (float t) { return Jetty::Settings { 0.05f + 4.95f * t, 0.6f, 2.0f, 0.5f }; });
    check ([] (float t) { return Jetty::Settings { 0.3f, 0.6f, 2.0f, 2.0f * t - 1.0f }; });

    // A fast, deep sweep of a 220 Hz tone puts nothing above 6 kHz: no zipper from the
    // fractional read (a truncating read would spray the whole band).
    Jetty j;
    j.prepare ({ kFs, 512, 1 });
    j.setSettings ({ 5.0f, 1.0f, 2.0f, 0.0f });
    test::Buffer b (1, static_cast<int> (2.0 * kFs));
    test::fillSine (b[0], 220.0, kFs, 0.25f);
    test::processInBlocks (b, 512, [&] (AudioView v) { j.process (v); });
    const auto start = static_cast<size_t> (0.25 * kFs);
    CHECK (test::settledRmsAbove (b[0], 6000.0, kFs, start) < 1.0e-3f * test::rms (b[0], start));
}

TEST_CASE ("Jetty keeps a guitar near unity across Feedback", "[pedals][jetty]")
{
    for (float feedback : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
    {
        INFO ("feedback " << feedback);
        CHECK (test::guitarGainDb<Jetty> ({ 0.3f, 0.6f, 2.0f, feedback }, kFs, 4.0) ==
               Approx (0.0).margin (1.5));
    }
}

TEST_CASE ("Jetty is linear at guitar levels and block-size independent", "[pedals][jetty]")
{
    for (float feedback : { -0.7f, 0.0f, 0.7f })
    {
        const int frames = 24000;
        test::Buffer b (1, frames);
        test::fillSine (b[0], 1318.5, kFs, 0.25f);
        Jetty j;
        j.prepare ({ kFs, 512, 1 });
        j.setSettings ({ 1.0f, 0.0f, 3.0f, feedback }); // static comb, near a resonant tooth
        test::processInBlocks (b, 512, [&] (AudioView v) { j.process (v); });
        const float* tail = b[0].data() + frames / 2;
        INFO ("feedback " << feedback);
        CHECK (test::thd (tail, frames / 2, 1318.5, kFs) < 1.0e-4);
        CHECK (test::worstInharmonicDb (tail, frames / 2, 1318.5, kFs) < -75.0);
    }
    // Driven into the line's limit (a full-scale note at full feedback), the limiter's harmonics
    // stay musical and its aliases stay well down.
    {
        const int frames = 24000;
        test::Buffer b (1, frames);
        test::fillSine (b[0], 1318.5, kFs, 1.0f);
        Jetty j;
        j.prepare ({ kFs, 512, 1 });
        j.setSettings ({ 1.0f, 0.0f, 3.0f, 1.0f });
        test::processInBlocks (b, 512, [&] (AudioView v) { j.process (v); });
        const float* tail = b[0].data() + frames / 2;
        CHECK (test::worstInharmonicDb (tail, frames / 2, 1318.5, kFs) < -50.0);
    }
    CHECK (test::blockSizeSpread<Jetty> ({ 2.0f, 0.8f, 3.0f, 0.7f }, kFs) < 1.0e-6f);
    CHECK (test::blockSizeSpread<Jetty> ({ 0.2f, 0.5f, 6.0f, -0.7f }, kFs) < 1.0e-6f);
}
