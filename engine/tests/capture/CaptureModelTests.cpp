// SPDX-License-Identifier: AGPL-3.0-or-later
// Captures play what their weights say: each architecture against an independent reference.

#include "CaptureTestUtils.h"
#include "amp/AmpTestUtils.h"
#include "sedef/dsp/capture/ReferenceStrum.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <get_dsp.h>

using namespace sedef;
namespace nt = sedef::test::nam;

namespace
{
/// Runs a model built for `fs` with no extra latency over `in` (host blocks of 64).
std::vector<float> play (const std::shared_ptr<const dsp::CaptureFile>& f,
                         const std::vector<float>& in, double fs = 48000.0, int latency = 0,
                         int block = 64)
{
    auto m = f->instantiate ({ fs, block, latency });
    REQUIRE (m != nullptr);
    std::vector<float> out (in.size());
    for (std::size_t s = 0; s < in.size(); s += static_cast<std::size_t> (block))
    {
        const int n = static_cast<int> (
            std::min<std::size_t> (static_cast<std::size_t> (block), in.size() - s));
        m->process (in.data() + s, out.data() + s, n);
    }
    // Undo the normalisation: compare the network itself.
    const float g = m->outputGain();
    for (auto& v : out)
        v /= g;
    return out;
}

float maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    float d = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i)
        d = std::max (d, std::abs (a[i] - b[i]));
    return d;
}

double sigmoid (double x)
{
    return 1.0 / (1.0 + std::exp (-x));
}
} // namespace

TEST_CASE ("A Linear capture is its impulse response", "[capture]")
{
    // y[n] = 0.5 x[n] + 0.25 x[n-1] - 0.125 x[n-2]
    const auto f = nt::parseOrFail (nt::linear ({ 0.5f, 0.25f, -0.125f }));
    CHECK (f->info().architecture == "Linear");
    CHECK (f->info().numWeights == 3);
    std::vector<float> in (2048);
    test::fillNoise (in, 0.5f, 9);
    const auto out = play (f, in);
    std::vector<float> expected (in.size());
    for (std::size_t n = 0; n < in.size(); ++n)
        expected[n] = 0.5f * in[n] + (n >= 1 ? 0.25f * in[n - 1] : 0.0f) -
                      (n >= 2 ? 0.125f * in[n - 2] : 0.0f);
    CHECK (maxAbsDiff (out, expected) < 1.0e-6f);
}

TEST_CASE ("A WaveNet capture matches an independent forward pass", "[capture]")
{
    // One layer array, two channels, kernel 2, dilations 1 and 2, Tanh, then the head. Written
    // out here from the weight layout (rechannel, per layer: dilated conv + bias, input mixin,
    // layer1x1 + bias; head rechannel + bias; head scale), not with the core's code.
    const int C = 2;
    const std::vector<int> dil { 1, 2 };
    const auto w =
        nt::randomWeights (static_cast<std::size_t> (nt::tinyWaveNetWeights (C, 2)), 5, 0.6f);
    const auto f = nt::parseOrFail (nt::file ("WaveNet", nt::tinyWaveNetConfig (C, dil), w));

    std::vector<float> in (1500);
    test::fillNoise (in, 0.7f, 21);
    const auto out = play (f, in);

    std::size_t p = 0;
    const auto take = [&] {
        return static_cast<double> (w[p++]);
    };
    std::vector<double> rechannel (C);
    for (auto& r : rechannel)
        r = take();
    struct Layer
    {
        double conv[2][2][2]; // [out][in][tap]
        double bias[2], mixin[2], l1x1[2][2], l1x1b[2];
    };
    std::vector<Layer> layers (dil.size());
    for (auto& L : layers)
    {
        for (int o = 0; o < C; ++o)
            for (int i = 0; i < C; ++i)
                for (int k = 0; k < 2; ++k)
                    L.conv[o][i][k] = take();
        for (int o = 0; o < C; ++o)
            L.bias[o] = take();
        for (int o = 0; o < C; ++o)
            L.mixin[o] = take();
        for (int o = 0; o < C; ++o)
            for (int i = 0; i < C; ++i)
                L.l1x1[o][i] = take();
        for (int o = 0; o < C; ++o)
            L.l1x1b[o] = take();
    }
    double head[2];
    for (auto& h : head)
        h = take();
    const double headBias = take();
    const double headScale = take();
    REQUIRE (p == w.size());

    // Signals per layer input, with zero history. The core prewarms on silence, which leaves a
    // different (biased) history, so the comparison starts after the receptive field.
    const std::size_t N = in.size();
    std::vector<std::vector<std::array<double, 2>>> x (dil.size() + 1,
                                                       std::vector<std::array<double, 2>> (N));
    std::vector<std::array<double, 2>> headAcc (N, { 0.0, 0.0 });
    for (std::size_t t = 0; t < N; ++t)
        for (int c = 0; c < C; ++c)
            x[0][t][static_cast<std::size_t> (c)] = rechannel[static_cast<std::size_t> (c)] * in[t];
    for (std::size_t l = 0; l < dil.size(); ++l)
    {
        const auto& L = layers[l];
        for (std::size_t t = 0; t < N; ++t)
        {
            double z[2];
            for (int o = 0; o < C; ++o)
            {
                double acc = L.bias[o] + L.mixin[o] * in[t];
                for (int i = 0; i < C; ++i)
                {
                    // tap 0 is the older sample (offset -dilation), tap 1 the current one
                    const double older = t >= static_cast<std::size_t> (dil[l])
                                             ? x[l][t - static_cast<std::size_t> (dil[l])]
                                                [static_cast<std::size_t> (i)]
                                             : 0.0;
                    acc += L.conv[o][i][0] * older +
                           L.conv[o][i][1] * x[l][t][static_cast<std::size_t> (i)];
                }
                z[o] = std::tanh (acc);
                headAcc[t][static_cast<std::size_t> (o)] += z[o];
            }
            for (int o = 0; o < C; ++o)
            {
                double r = L.l1x1b[o];
                for (int i = 0; i < C; ++i)
                    r += L.l1x1[o][i] * z[i];
                x[l + 1][t][static_cast<std::size_t> (o)] =
                    x[l][t][static_cast<std::size_t> (o)] + r;
            }
        }
    }
    double worst = 0.0;
    for (std::size_t t = 64; t < N; ++t)
    {
        const double y = headScale * (headBias + head[0] * headAcc[t][0] + head[1] * headAcc[t][1]);
        worst = std::max (worst, std::abs (y - out[t]));
    }
    CAPTURE (worst);
    CHECK (worst < 2.0e-5);
}

TEST_CASE ("An LSTM capture matches an independent forward pass", "[capture]")
{
    const int H = 3;
    const auto w = nt::randomWeights (static_cast<std::size_t> (nt::lstmWeights (1, H)), 8, 0.5f);
    const auto f = nt::parseOrFail (nt::file ("LSTM", nt::lstmConfig (1, H), w));

    // Weight order: W (4H x (1 + H)) row-major over [x, h], b (4H), initial h (H), initial c (H),
    // head W (H), head b. Gates i, f, g, o.
    std::size_t p = 0;
    std::vector<std::vector<double>> W (4 * H, std::vector<double> (1 + H));
    for (auto& row : W)
        for (auto& v : row)
            v = w[p++];
    std::vector<double> b (4 * H), h (H), c (H), head (H);
    for (auto& v : b)
        v = w[p++];
    for (auto& v : h)
        v = w[p++];
    for (auto& v : c)
        v = w[p++];
    for (auto& v : head)
        v = w[p++];
    const double headBias = w[p++];
    REQUIRE (p == w.size());

    // The core prewarms on half a second of silence before the first sample.
    const auto step = [&] (double xin) {
        std::vector<double> g (4 * H);
        for (int r = 0; r < 4 * H; ++r)
        {
            double acc = b[static_cast<std::size_t> (r)] + W[static_cast<std::size_t> (r)][0] * xin;
            for (int k = 0; k < H; ++k)
                acc += W[static_cast<std::size_t> (r)][static_cast<std::size_t> (k + 1)] *
                       h[static_cast<std::size_t> (k)];
            g[static_cast<std::size_t> (r)] = acc;
        }
        for (int k = 0; k < H; ++k)
        {
            const auto i = static_cast<std::size_t> (k);
            c[i] = sigmoid (g[i + H]) * c[i] + sigmoid (g[i]) * std::tanh (g[i + 2 * H]);
        }
        for (int k = 0; k < H; ++k)
        {
            const auto i = static_cast<std::size_t> (k);
            h[i] = sigmoid (g[i + 3 * H]) * std::tanh (c[i]);
        }
        double y = headBias;
        for (int k = 0; k < H; ++k)
            y += head[static_cast<std::size_t> (k)] * h[static_cast<std::size_t> (k)];
        return y;
    };
    // DSP::prewarm runs whole 512-sample blocks: 24000 samples round up to 24064.
    for (int i = 0; i < (24000 + 511) / 512 * 512; ++i)
        step (0.0);

    std::vector<float> in (2000);
    test::fillNoise (in, 0.8f, 2);
    const auto out = play (f, in);
    double worst = 0.0;
    for (std::size_t t = 0; t < in.size(); ++t)
        worst = std::max (worst, std::abs (step (in[t]) - out[t]));
    CAPTURE (worst);
    CHECK (worst < 2.0e-5);
}

TEST_CASE ("Sedef's ConvNet matches the core's ConvNet sample for sample", "[capture]")
{
    const bool batchnorm = GENERATE (false, true);
    const int C = 4;
    const std::vector<int> dil { 1, 2, 4, 8 };
    const auto w = nt::convNetWeightsFor (C, 4, batchnorm, 13);
    REQUIRE (static_cast<int> (w.size()) == nt::convNetWeights (C, 4, batchnorm));
    const auto config = nt::convNetConfig (C, dil, batchnorm);
    const auto f = nt::parseOrFail (nt::file ("ConvNet", config, w));

    // The core's own ConvNet (it allocates per block, which is why Sedef doesn't play it).
    nam::dspData data;
    data.version = "0.5.4";
    data.architecture = "ConvNet";
    data.config = config;
    data.weights = w;
    data.expected_sample_rate = 48000.0;
    auto reference = nam::get_dsp (data);
    reference->Reset (48000.0, 512);

    std::vector<float> in (3000);
    test::fillNoise (in, 0.6f, 17);
    std::vector<float> expected (in.size());
    for (std::size_t s = 0; s < in.size(); s += 64)
    {
        float* ip = in.data() + s;
        float* op = expected.data() + s;
        reference->process (&ip, &op, static_cast<int> (std::min<std::size_t> (64, in.size() - s)));
    }
    const auto out = play (f, in);
    CAPTURE (batchnorm, maxAbsDiff (out, expected));
    CHECK (maxAbsDiff (out, expected) < 1.0e-5f);
}

TEST_CASE ("The standard WaveNet loads with its 13,802 weights", "[capture]")
{
    nt::json meta { { "name", "Test Amp" },     { "modeled_by", "Sedef tests" },
                    { "gear_make", "Nobody" },  { "gear_model", "Box" },
                    { "gear_type", "amp_cab" }, { "tone_type", "crunch" },
                    { "loudness", -12.5 },      { "input_level_dbu", 19.0 },
                    { "output_level_dbu", 7.5 } };
    const auto f = nt::parseOrFail (nt::standardWaveNet (3, 48000.0, meta));
    const auto& info = f->info();
    CHECK (info.architecture == "WaveNet");
    CHECK (info.numWeights == nt::kStandardWaveNetWeights);
    CHECK (info.receptiveField == 4093);
    CHECK (info.macsPerSample == 13801);
    CHECK (info.name == "Test Amp");
    CHECK (info.modeledBy == "Sedef tests");
    CHECK (info.gearMake == "Nobody");
    CHECK (info.gearModel == "Box");
    CHECK (info.kind == dsp::CaptureKind::FullRig);
    CHECK (info.toneType == "crunch");
    CHECK (info.loudnessDb == Catch::Approx (-12.5));
    CHECK (info.inputLevelDbu == Catch::Approx (19.0));
    CHECK (info.outputLevelDbu == Catch::Approx (7.5));
    CHECK (info.sampleRate == 48000.0);
    CHECK (info.sampleRateKnown);
}

TEST_CASE ("Captures are normalised to the built-in amps' level", "[capture]")
{
    // The reference strum the amps are matched with comes out at -15 dBFS RMS, whatever the
    // capture's own level, for every architecture.
    std::vector<std::string> files {
        nt::smallWaveNet (4),
        nt::file ("LSTM", nt::lstmConfig (2, 8),
                  nt::randomWeights (static_cast<std::size_t> (nt::lstmWeights (2, 8)), 6, 0.4f)),
        nt::file ("ConvNet", nt::convNetConfig (4, { 1, 2, 4, 8, 16 }, true),
                  nt::convNetWeightsFor (4, 5, true, 7)),
        nt::linear ({ 2.0f, -1.0f, 0.5f }),
    };
    for (const auto& text : files)
    {
        const auto f = nt::parseOrFail (text);
        auto in = dsp::capture::referenceStrum (48000.0, dsp::capture::kReferenceSeconds);
        auto m = f->instantiate ({ 48000.0, 64, 0 });
        for (std::size_t s = 0; s < in.size(); s += 64)
            m->process (in.data() + s, in.data() + s, 64);
        const double level = test::dbfs (test::rms (in, static_cast<std::size_t> (0.5 * 48000.0)));
        CAPTURE (f->info().architecture, f->measuredDbfs(), f->normalisationDb(), level);
        CHECK (level == Catch::Approx (-15.0).margin (0.05));
    }
}

TEST_CASE ("The engine's reference strum is the amp tests' strum", "[capture]")
{
    // Generated by phasor rotation instead of exp and sin per sample: the same signal.
    const auto a = test::guitarStrum (44100.0, 6.0);
    const auto b = dsp::capture::referenceStrum (44100.0, 6.0);
    REQUIRE (a.size() == b.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max (worst, std::abs (a[i] - b[i]));
    CHECK (worst < 1.0e-5f);
}

TEST_CASE ("Calibrated captures take the interface level into account", "[capture]")
{
    // A capture made with the interface at +19 dBu: at Sedef's reference +12 dBu the strum
    // reaches it 7 dB lower, which the normalisation already includes.
    const auto plain = nt::parseOrFail (nt::linear ({ 1.0f }));
    const auto calibrated =
        nt::parseOrFail (nt::linear ({ 1.0f }, 48000.0, { { "input_level_dbu", 19.0 } }));
    CHECK (plain->measuredDbfs() == Catch::Approx (-18.0).margin (0.2)); // the strum itself
    CHECK (calibrated->measuredDbfs() == Catch::Approx (plain->measuredDbfs() - 7.0).margin (0.01));
    CHECK (calibrated->normalisationDb() ==
           Catch::Approx (plain->normalisationDb() + 7.0).margin (0.01));
    auto m = calibrated->instantiate ({ 48000.0, 64, 0 });
    CHECK (m->calibrated());
    CHECK (m->inputScale() == Catch::Approx (std::pow (10.0, -19.0 / 20.0)));
    CHECK_FALSE (plain->instantiate ({ 48000.0, 64, 0 })->calibrated());
}

TEST_CASE ("A known level skips the measurement and plays the same", "[capture]")
{
    // A session restore passes the level measured when the capture was first loaded.
    const auto text = nt::standardWaveNet (5);
    const auto measured = nt::parseOrFail (text);
    const auto restored = dsp::CaptureFile::parse (text, measured->normalisationDb());
    REQUIRE (restored.file != nullptr);
    CHECK (restored.file->normalisationDb() == measured->normalisationDb());
    CHECK (restored.file->measuredDbfs() == Catch::Approx (measured->measuredDbfs()).margin (1e-4));

    const dsp::CaptureSpec spec { 48000.0, 128, 0 };
    auto a = measured->instantiate (spec);
    auto b = restored.file->instantiate (spec);
    auto x = test::guitarStrum (48000.0, 0.5);
    auto y = x;
    for (std::size_t s = 0; s + 128 <= x.size(); s += 128)
    {
        a->process (x.data() + s, x.data() + s, 128);
        b->process (y.data() + s, y.data() + s, 128);
    }
    CHECK (x == y);

    // Still validated, and the level is kept within range.
    CHECK (dsp::CaptureFile::parse ("{}", 0.0f).file == nullptr);
    CHECK (dsp::CaptureFile::parse (text, 500.0f).file->normalisationDb() ==
           dsp::CaptureFile::kMaxNormalisationDb);
}

TEST_CASE ("A WaveNet conditioned by its own model plays at its measured level", "[capture]")
{
    // The format's condition_dsp: a small WaveNet turns the input into a 3-channel condition for
    // the main one. Each keeps its own weights array (137 and 147 here).
    const auto array = [] (int in, int condition, int head, int channels, std::vector<int> d,
                           bool headBias) {
        return nt::json { { "input_size", in },     { "condition_size", condition },
                          { "head_size", head },    { "channels", channels },
                          { "kernel_size", 3 },     { "dilations", d },
                          { "activation", "Tanh" }, { "gated", false },
                          { "head_bias", headBias } };
    };
    nt::json inner;
    inner["version"] = "0.6.0";
    inner["architecture"] = "WaveNet";
    inner["config"] = {
        { "layers", { array (1, 1, 2, 3, { 1, 2 }, false), array (3, 1, 3, 2, { 8 }, true) } },
        { "head", nullptr },
        { "head_scale", 0.02 }
    };
    auto innerWeights = nt::randomWeights (137, 21, 0.4f);
    innerWeights.back() = 1.0f;
    inner["weights"] = innerWeights;
    inner["sample_rate"] = 48000;
    nt::json config { { "layers",
                        { array (1, 3, 2, 3, { 1, 2 }, false), array (3, 3, 1, 2, { 8 }, true) } },
                      { "head", nullptr },
                      { "head_scale", 0.02 },
                      { "condition_dsp", inner } };
    auto weights = nt::randomWeights (147, 22, 0.4f);
    weights.back() = 1.0f;
    const auto f = nt::parseOrFail (nt::file ("WaveNet", config, weights, 48000.0, {}, "0.6.0"));
    CHECK (f->info().numWeights == 284);

    // Wrong counts are still caught, in either array.
    weights.push_back (0.1f);
    CHECK (dsp::CaptureFile::parse (nt::file ("WaveNet", config, weights, 48000.0, {}, "0.6.0"))
               .file == nullptr);

    auto m = f->instantiate ({ 48000.0, 128, 0 });
    REQUIRE (m != nullptr);
    auto x = test::guitarStrum (48000.0, 6.0);
    for (std::size_t s = 0; s + 128 <= x.size(); s += 128)
        m->process (x.data() + s, x.data() + s, 128);
    x.resize (x.size() / 128 * 128);
    const double level = test::dbfs (test::rms (x, static_cast<std::size_t> (0.5 * 48000.0)));
    CAPTURE (f->measuredDbfs(), f->normalisationDb());
    CHECK (level == Catch::Approx (dsp::CaptureFile::kTargetDbfs).margin (0.1));
}
