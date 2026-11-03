// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AmpTestUtils.h"
#include "sedef/core/Oversampler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <functional>
#include <limits>

using namespace sedef;
using dsp::AmpSection;

namespace
{
AmpSection::Settings defaults (int model)
{
    AmpSection::Settings s;
    s.model = model;
    return s;
}

AmpSection::Settings randomSettings (Random& r)
{
    AmpSection::Settings s;
    s.model = static_cast<int> (r.unipolar() * 3.0f);
    s.lagoon = { r.unipolar(), r.unipolar(), r.unipolar(), r.unipolar(), r.unipolar() };
    s.reef.voice = static_cast<int> (r.unipolar() * 3.0f);
    for (float* k : { &s.reef.gain, &s.reef.bass, &s.reef.mid, &s.reef.treble, &s.reef.presence,
                      &s.reef.master, &s.reef.output, &s.abyss.gain, &s.abyss.bass, &s.abyss.mid,
                      &s.abyss.treble, &s.abyss.presence, &s.abyss.master, &s.abyss.output })
        *k = r.unipolar();
    s.abyss.bright = r.unipolar() > 0.5f;
    s.abyss.shift = r.unipolar() > 0.5f;
    return s;
}

/// Largest sample-to-sample step in [from, to).
float maxJump (const std::vector<float>& v, size_t from, size_t to)
{
    float j = 0.0f;
    for (size_t i = std::max<size_t> (from, 1); i < to; ++i)
        j = std::max (j, std::abs (v[i] - v[i - 1]));
    return j;
}

std::vector<float> sine (double hz, double fs, double seconds, double dbfsRms = -18.0)
{
    std::vector<float> v (static_cast<size_t> (seconds * fs));
    test::fillSine (v, hz, fs,
                    static_cast<float> (std::sqrt (2.0) * std::pow (10.0, dbfsRms / 20.0)));
    return v;
}

double sineThd (const AmpSection::Settings& s, double hz = 220.0, double fs = 48000.0)
{
    AmpSection amp;
    test::prepareAmp (amp, s, fs);
    auto v = sine (hz, fs, 1.5);
    test::runAmp (amp, v);
    return test::thd (v, hz, fs);
}

/// Renders `input` through an amp, calling `automate (blockIndex, settings)` before each block.
std::vector<float> render (const std::vector<float>& input, AmpSection::Settings s, int block,
                           const std::function<void (int, AmpSection::Settings&)>& automate,
                           double fs = 48000.0)
{
    AmpSection amp;
    test::prepareAmp (amp, s, fs, block);
    auto out = input;
    int index = 0;
    for (size_t start = 0; start < out.size(); start += static_cast<size_t> (block), ++index)
    {
        automate (index, s);
        amp.setSettings (s);
        const int n = static_cast<int> (std::min (static_cast<size_t> (block), out.size() - start));
        float* p = out.data() + start;
        amp.process (AudioView (&p, 1, n));
    }
    return out;
}
} // namespace

TEST_CASE ("Amp output is finite for random settings and full-scale noise", "[amp]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const int block = GENERATE (1, 17, 64, 512);
    Random r (static_cast<std::uint32_t> (fs) + static_cast<std::uint32_t> (block));

    AmpSection amp;
    amp.prepare ({ fs, block, 2 });
    test::Buffer buf (2, block);
    const int blocks = static_cast<int> (0.4 * fs) / block;
    bool finite = true;
    float loudest = 0.0f;
    for (int b = 0; b < blocks; ++b)
    {
        if (b % std::max (1, 2048 / block) == 0)
            amp.setSettings (randomSettings (r));
        test::fillNoise (buf[0], 1.0f, r.nextU32());
        test::fillNoise (buf[1], 1.0f, r.nextU32());
        amp.process (buf.view());
        finite = finite && test::allFinite (buf[0]) && test::allFinite (buf[1]);
        loudest = std::max ({ loudest, test::peak (buf[0]), test::peak (buf[1]) });
    }
    CHECK (finite);
    CHECK (loudest < 16.0f); // bounded even at full drive and +12 dB output
}

TEST_CASE ("Amp survives non-finite and out-of-range settings", "[amp]")
{
    AmpSection::Settings s;
    s.model = 7;
    s.lagoon.volume = std::numeric_limits<float>::quiet_NaN();
    s.reef.voice = -3;
    s.reef.gain = 1.0e9f;
    s.abyss.master = -std::numeric_limits<float>::infinity();
    AmpSection amp;
    test::prepareAmp (amp, s);
    auto v = test::guitarStrum (48000.0, 0.5);
    test::runAmp (amp, v);
    CHECK (test::allFinite (v));
}

TEST_CASE ("Silence in gives silence out", "[amp]")
{
    const int model = GENERATE (0, 1, 2);
    const double fs = 48000.0;
    auto s = defaults (model);
    s.lagoon = { 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
    s.reef.gain = s.reef.master = s.reef.output = 1.0f;
    s.abyss.gain = s.abyss.master = s.abyss.output = 1.0f;

    AmpSection amp;
    test::prepareAmp (amp, s, fs);
    // Loud signal first so every sag, bias-shift and filter state is excited, then silence.
    auto v = test::guitarStrum (fs, 1.0);
    v.resize (static_cast<size_t> (4.0 * fs), 0.0f);
    test::runAmp (amp, v);
    const std::vector<float> tail (v.end() - static_cast<long> (fs * 0.5), v.end());
    CHECK (test::dbfs (test::peak (tail)) < -100.0);

    // And from a cold start, digital silence stays exactly silent.
    AmpSection cold;
    test::prepareAmp (cold, s, fs);
    std::vector<float> zeros (4096, 0.0f);
    test::runAmp (cold, zeros);
    CHECK (test::peak (zeros) == 0.0f);
}

TEST_CASE ("Models are level-matched at default settings", "[amp]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto input = test::guitarStrum (fs, 6.0);
    std::array<double, 3> level {};
    for (int m = 0; m < 3; ++m)
    {
        AmpSection amp;
        test::prepareAmp (amp, defaults (m), fs);
        auto v = input;
        test::runAmp (amp, v);
        level[static_cast<size_t> (m)] = test::dbfs (test::rms (v, static_cast<size_t> (fs * 0.5)));
    }
    CAPTURE (fs, level[0], level[1], level[2]);
    for (double l : level)
    {
        CHECK (l > -18.0);
        CHECK (l < -12.0);
    }
    const auto [lo, hi] = std::minmax_element (level.begin(), level.end());
    CHECK (*hi - *lo < 3.0);
}

TEST_CASE ("Distortion rises monotonically with gain on Reef and Abyss", "[amp]")
{
    // A 220 Hz sine at -18 dBFS RMS: compression (small-signal gain minus gain at this level) rises
    // with every 0.1 of gain, and THD spans a wide range. On Reef THD itself rises at every step.
    // On Abyss it need not: its cold clipper starts asymmetric (second harmonic, a warm crunch) and
    // turns symmetric as it saturates (odd harmonics), and the second harmonic fades on the way, so
    // THD dips around a third of the travel while the distortion keeps growing.
    const int model = GENERATE (1, 2);
    const auto gainAt = [] (const AmpSection::Settings& s, double dbfsRms) {
        AmpSection amp;
        test::prepareAmp (amp, s, 48000.0);
        auto v = sine (220.0, 48000.0, 0.5, dbfsRms);
        test::runAmp (amp, v);
        return test::dbfs (test::rms (v, v.size() - 16384)) - dbfsRms;
    };
    std::vector<double> curve, squash;
    for (int g = 0; g <= 10; ++g)
    {
        auto s = defaults (model);
        s.reef.gain = s.abyss.gain = static_cast<float> (g) / 10.0f;
        curve.push_back (sineThd (s));
        squash.push_back (gainAt (s, -100.0) - gainAt (s, -18.0));
    }
    for (size_t i = 1; i < curve.size(); ++i)
    {
        CAPTURE (model, i, curve[i - 1], curve[i], squash[i - 1], squash[i]);
        CHECK (squash[i] > squash[i - 1]);
        if (model == AmpSection::Reef)
            CHECK (curve[i] > curve[i - 1]);
    }
    // The knob spans a wide range. (At the top, output THD flattens out: the stages are already
    // square and the post-clip tone stack and low-passes shape the harmonics; what keeps rising
    // is compression and sustain.)
    CHECK (curve.back() > 3.0 * curve.front());
}

TEST_CASE ("Lagoon stays clean with volume and master low", "[amp]")
{
    auto s = defaults (AmpSection::Lagoon);
    s.lagoon.volume = 0.25f;
    s.lagoon.master = 0.25f;
    const double low = sineThd (s);
    CAPTURE (low);
    CHECK (low < 0.01);

    // ...and breaks up when both are cranked.
    s.lagoon.volume = s.lagoon.master = 1.0f;
    CHECK (sineThd (s) > 0.1);
}

TEST_CASE ("Switching models does not click", "[amp]")
{
    const double fs = 48000.0;
    const auto input = sine (110.0, fs, 1.2);
    const size_t switchAt = static_cast<size_t> (0.6 * fs);
    for (int from = 0; from < 3; ++from)
        for (int to = 0; to < 3; ++to)
        {
            if (from == to)
                continue;
            const auto steadyFrom = render (input, defaults (from), 64, [] (int, auto&) {});
            const auto steadyTo = render (input, defaults (to), 64, [] (int, auto&) {});
            const auto switched =
                render (input, defaults (from), 64, [&] (int block, AmpSection::Settings& s) {
                    s.model = static_cast<size_t> (block * 64) >= switchAt ? to : from;
                });
            const size_t settled = static_cast<size_t> (0.3 * fs);
            const float steady = std::max (maxJump (steadyFrom, settled, input.size()),
                                           maxJump (steadyTo, settled, input.size()));
            const float during =
                maxJump (switched, switchAt - 64, switchAt + static_cast<size_t> (0.05 * fs));
            CAPTURE (from, to, steady, during);
            CHECK (during <= 1.1f * steady);
            CHECK (test::allFinite (switched));
        }
}

TEST_CASE ("Rapid model changes (reversal, queued third model) stay click-free", "[amp]")
{
    const double fs = 48000.0;
    const auto input = sine (110.0, fs, 1.0);
    float steady = 0.0f;
    for (int m = 0; m < 3; ++m)
        steady = std::max (steady, maxJump (render (input, defaults (m), 64, [] (int, auto&) {}),
                                            static_cast<size_t> (0.3 * fs), input.size()));
    // Model changes every 10 ms: faster than the 30 ms fade, so fades reverse and queue.
    const auto out = render (input, defaults (0), 32, [] (int block, AmpSection::Settings& s) {
        s.model = block < 400 ? 0 : (block / 15) % 3;
    });
    CAPTURE (steady, maxJump (out, static_cast<size_t> (0.3 * fs), out.size()));
    CHECK (maxJump (out, static_cast<size_t> (0.3 * fs), out.size()) <= 1.1f * steady);
}

TEST_CASE ("Automating every knob per block is click-free", "[amp]")
{
    const double fs = 48000.0;
    const auto input = sine (110.0, fs, 1.4);
    const int block = 64;
    const size_t settled = static_cast<size_t> (0.3 * fs);

    using Setter = std::function<void (AmpSection::Settings&, float)>;
    struct Knob
    {
        int model;
        const char* name;
        Setter set;
    };
    const std::vector<Knob> knobs {
        { 0, "lagoon.volume", [] (auto& s, float v) { s.lagoon.volume = v; } },
        { 0, "lagoon.bass", [] (auto& s, float v) { s.lagoon.bass = v; } },
        { 0, "lagoon.cut", [] (auto& s, float v) { s.lagoon.cut = v; } },
        { 0, "lagoon.master", [] (auto& s, float v) { s.lagoon.master = v; } },
        { 0, "lagoon.output", [] (auto& s, float v) { s.lagoon.output = v; } },
        { 1, "reef.voice",
          [] (auto& s, float v) { s.reef.voice = static_cast<int> (std::lround (v * 2.0f)); } },
        { 1, "reef.gain", [] (auto& s, float v) { s.reef.gain = v; } },
        { 1, "reef.bass", [] (auto& s, float v) { s.reef.bass = v; } },
        { 1, "reef.mid", [] (auto& s, float v) { s.reef.mid = v; } },
        { 1, "reef.treble", [] (auto& s, float v) { s.reef.treble = v; } },
        { 1, "reef.presence", [] (auto& s, float v) { s.reef.presence = v; } },
        { 1, "reef.master", [] (auto& s, float v) { s.reef.master = v; } },
        { 1, "reef.output", [] (auto& s, float v) { s.reef.output = v; } },
        { 2, "abyss.bright", [] (auto& s, float v) { s.abyss.bright = v > 0.5f; } },
        { 2, "abyss.gain", [] (auto& s, float v) { s.abyss.gain = v; } },
        { 2, "abyss.bass", [] (auto& s, float v) { s.abyss.bass = v; } },
        { 2, "abyss.mid", [] (auto& s, float v) { s.abyss.mid = v; } },
        { 2, "abyss.treble", [] (auto& s, float v) { s.abyss.treble = v; } },
        { 2, "abyss.shift", [] (auto& s, float v) { s.abyss.shift = v > 0.5f; } },
        { 2, "abyss.presence", [] (auto& s, float v) { s.abyss.presence = v; } },
        { 2, "abyss.master", [] (auto& s, float v) { s.abyss.master = v; } },
        { 2, "abyss.output", [] (auto& s, float v) { s.abyss.output = v; } },
    };

    for (const auto& k : knobs)
    {
        // Reference: the largest step any static setting along the sweep produces. The steepest
        // edge of a saturated waveform peaks at particular settings (full-range Abyss gain: near
        // 0.44), so the reference grid is fine.
        float steady = 0.0f;
        for (int i = 0; i <= 16; ++i)
        {
            auto s = defaults (k.model);
            k.set (s, static_cast<float> (i) / 16.0f);
            steady = std::max (steady, maxJump (render (input, s, block, [] (int, auto&) {}),
                                                settled, input.size()));
        }

        // Sweep 0 -> 1 -> 0 over one second, a new value every block (discrete controls flip
        // several times on the way).
        const int sweepBlocks = static_cast<int> (fs) / block;
        const int startBlock = static_cast<int> (settled) / block;
        const auto out =
            render (input, defaults (k.model), block, [&] (int b, AmpSection::Settings& s) {
                const float p = std::clamp (static_cast<float> (b - startBlock) /
                                                static_cast<float> (sweepBlocks),
                                            0.0f, 1.0f);
                k.set (s, 1.0f - std::abs (2.0f * p - 1.0f));
            });
        const float during = maxJump (out, settled, out.size());
        CAPTURE (k.name, steady, during);
        CHECK (test::allFinite (out));
        CHECK (during <= 1.1f * steady);
    }
}

TEST_CASE ("Latency equals the oversampler's and an impulse arrives there", "[amp]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    AmpSection amp;
    auto s = defaults (AmpSection::Lagoon);
    s.lagoon = { 0.2f, 0.5f, 0.0f, 0.2f, 0.5f }; // clean, cut fully open
    test::prepareAmp (amp, s, fs);

    Oversampler os;
    os.prepare (64, 1, Oversampler::factorLog2For (fs));
    CHECK (amp.latencySamples() == os.latencySamples());

    std::vector<float> v (4096, 0.0f);
    const size_t at = 300;
    v[at] = 0.25f;
    test::runAmp (amp, v);
    size_t peakAt = 0;
    for (size_t i = 0; i < v.size(); ++i)
        if (std::abs (v[i]) > std::abs (v[peakAt]))
            peakAt = i;
    // The linear-phase oversampler is a pure delay of exactly latencySamples(). On top of it the
    // minimum-phase voicing low-passes (Miller, plate, transformer, cut) add about 40 us of group
    // delay, which moves the peak a few samples later at high rates. Never earlier.
    const auto latency = static_cast<size_t> (amp.latencySamples());
    const auto smear = static_cast<size_t> (std::ceil (60.0e-6 * fs));
    CAPTURE (fs, latency, peakAt - at);
    CHECK (peakAt >= at + latency);
    CHECK (peakAt <= at + latency + smear);
}

TEST_CASE ("Stereo channels are processed independently", "[amp]")
{
    const double fs = 48000.0;
    const int model = GENERATE (0, 1, 2);
    const auto left = test::guitarStrum (fs, 0.5, 3);
    const auto right = test::guitarStrum (fs, 0.5, 11);

    AmpSection mono;
    test::prepareAmp (mono, defaults (model), fs);
    auto expected = left;
    test::runAmp (mono, expected);

    AmpSection stereo;
    auto s = defaults (model);
    stereo.setSettings (s);
    stereo.prepare ({ fs, 64, 2 });
    test::Buffer buf (2, static_cast<int> (left.size()));
    buf[0] = left;
    buf[1] = right;
    for (int start = 0; start < buf.frames(); start += 64)
        stereo.process (buf.view().subView (start, std::min (64, buf.frames() - start)));
    float diff = 0.0f;
    for (size_t i = 0; i < left.size(); ++i)
        diff = std::max (diff, std::abs (buf[0][i] - expected[i]));
    CHECK (diff == 0.0f);
}

TEST_CASE ("Block size does not change the output", "[amp]")
{
    const double fs = 48000.0;
    const int model = GENERATE (0, 1, 2);
    const auto input = test::guitarStrum (fs, 0.5);
    AmpSection a, b;
    test::prepareAmp (a, defaults (model), fs);
    test::prepareAmp (b, defaults (model), fs);
    auto x = input, y = input;
    test::runAmp (a, x, 512);
    test::runAmp (b, y, 7);
    float diff = 0.0f;
    for (size_t i = 0; i < x.size(); ++i)
        diff = std::max (diff, std::abs (x[i] - y[i]));
    CHECK (diff < 1.0e-6f);
}
