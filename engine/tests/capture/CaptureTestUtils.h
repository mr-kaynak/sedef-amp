// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Tiny synthetic .nam captures, written here so no third-party capture is ever needed.

#include "TestUtils.h"
#include "sedef/dsp/capture/CaptureFile.h"
#include "sedef/dsp/capture/CaptureStage.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <json.hpp> // nlohmann::json, from NeuralAmpModelerCore's dependencies
#include <memory>
#include <string>
#include <vector>

namespace sedef::test::nam
{

using json = nlohmann::json;

inline std::string file (const std::string& architecture, const json& config,
                         const std::vector<float>& weights, double sampleRate = 48000.0,
                         const json& metadata = json(), const std::string& version = "0.5.4")
{
    json j;
    j["version"] = version;
    j["architecture"] = architecture;
    j["config"] = config;
    j["weights"] = weights;
    j["sample_rate"] = sampleRate;
    if (!metadata.is_null())
        j["metadata"] = metadata;
    return j.dump();
}

inline std::vector<float> randomWeights (std::size_t n, std::uint32_t seed, float scale = 0.3f)
{
    Random r (seed);
    std::vector<float> w (n);
    for (auto& x : w)
        x = scale * r.bipolar();
    return w;
}

/// Linear: an impulse response, taps in file order (newest sample first, as y = sum h[k] x[n-k]).
inline std::string linear (const std::vector<float>& taps, double sampleRate = 48000.0,
                           const json& metadata = json())
{
    json config { { "receptive_field", static_cast<int> (taps.size()) }, { "bias", false } };
    return file ("Linear", config, taps, sampleRate, metadata);
}

/// The trainer's WaveNet layout: two layer arrays of ten layers (kernel 3, dilations 1..512),
/// Tanh, no gating, the first with `channels1` channels and a head of `channels2`, the second
/// with `channels2` channels. The "standard" size is 16 and 8: 13,802 weights.
inline json waveNetConfig (int channels1, int channels2)
{
    const json dilations = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };
    json a1 { { "input_size", 1 },       { "condition_size", 1 }, { "head_size", channels2 },
              { "channels", channels1 }, { "kernel_size", 3 },    { "dilations", dilations },
              { "activation", "Tanh" },  { "gated", false },      { "head_bias", false } };
    json a2 { { "input_size", channels1 }, { "condition_size", 1 }, { "head_size", 1 },
              { "channels", channels2 },   { "kernel_size", 3 },    { "dilations", dilations },
              { "activation", "Tanh" },    { "gated", false },      { "head_bias", true } };
    return json { { "layers", json::array ({ a1, a2 }) }, { "head_scale", 0.02 } };
}

inline int waveNetWeights (int channels1, int channels2)
{
    const auto array = [] (int in, int channels, int head, bool headBias) {
        const int layer =
            channels * channels * 3 + channels + channels + channels * channels + channels;
        return in * channels + 10 * layer + channels * head + (headBias ? head : 0);
    };
    return array (1, channels1, channels2, false) + array (channels1, channels2, 1, true) + 1;
}

inline json standardWaveNetConfig()
{
    return waveNetConfig (16, 8);
}

inline constexpr int kStandardWaveNetWeights = 13802;

/// Random weights; the last one is the head scale, kept at a trained model's order of magnitude.
inline std::string waveNet (int channels1, int channels2, std::uint32_t seed, float scale,
                            double sampleRate = 48000.0, const json& metadata = json())
{
    auto w = randomWeights (static_cast<std::size_t> (waveNetWeights (channels1, channels2)), seed,
                            scale);
    w.back() = 0.5f;
    return file ("WaveNet", waveNetConfig (channels1, channels2), w, sampleRate, metadata);
}

inline std::string standardWaveNet (std::uint32_t seed = 1, double sampleRate = 48000.0,
                                    const json& metadata = json())
{
    return waveNet (16, 8, seed, 0.25f, sampleRate, metadata);
}

/// The same layout at 4 and 2 channels (1,004 weights): the same receptive field and code paths
/// at a fraction of the cost, for tests that run a capture for a long time (sanitizer builds run
/// many times slower).
inline std::string smallWaveNet (std::uint32_t seed = 1, double sampleRate = 48000.0,
                                 const json& metadata = json())
{
    return waveNet (4, 2, seed, 0.6f, sampleRate, metadata);
}

/// A small WaveNet for reference checks: one layer array, `channels` channels, kernel 2.
inline json tinyWaveNetConfig (int channels, const std::vector<int>& dilations,
                               const std::string& activation = "Tanh", bool gated = false)
{
    json a { { "input_size", 1 },          { "condition_size", 1 }, { "head_size", 1 },
             { "channels", channels },     { "kernel_size", 2 },    { "dilations", dilations },
             { "activation", activation }, { "gated", gated },      { "head_bias", true } };
    return json { { "layers", json::array ({ a }) }, { "head_scale", 0.5 } };
}

inline int tinyWaveNetWeights (int channels, int layers, bool gated = false)
{
    const int z = gated ? 2 * channels : channels;
    const int perLayer = 2 * z * channels + z + z + channels * channels + channels;
    return channels + layers * perLayer + channels + 1 + 1;
}

inline json lstmConfig (int layers, int hidden)
{
    return json { { "num_layers", layers }, { "input_size", 1 }, { "hidden_size", hidden } };
}

inline int lstmWeights (int layers, int hidden)
{
    int n = 0;
    for (int l = 0; l < layers; ++l)
    {
        const int in = l == 0 ? 1 : hidden;
        n += 4 * hidden * (in + hidden) + 4 * hidden + 2 * hidden;
    }
    return n + hidden + 1;
}

inline json convNetConfig (int channels, const std::vector<int>& dilations, bool batchnorm)
{
    return json { { "channels", channels },
                  { "dilations", dilations },
                  { "batchnorm", batchnorm },
                  { "activation", "Tanh" } };
}

inline int convNetWeights (int channels, int blocks, bool batchnorm)
{
    int n = 0;
    for (int b = 0; b < blocks; ++b)
    {
        const int in = b == 0 ? 1 : channels;
        n += 2 * channels * in + (batchnorm ? 4 * channels + 1 : channels);
    }
    return n + channels + 1;
}

/// Batch-norm weights must keep the variance positive: (mean, var, weight, bias, eps) per block.
inline std::vector<float> convNetWeightsFor (int channels, int blocks, bool batchnorm,
                                             std::uint32_t seed)
{
    Random r (seed);
    std::vector<float> w;
    for (int b = 0; b < blocks; ++b)
    {
        const int in = b == 0 ? 1 : channels;
        for (int i = 0; i < 2 * channels * in; ++i)
            w.push_back (0.4f * r.bipolar());
        if (batchnorm)
        {
            for (int i = 0; i < channels; ++i)
                w.push_back (0.1f * r.bipolar()); // mean
            for (int i = 0; i < channels; ++i)
                w.push_back (0.5f + 0.5f * r.unipolar()); // var
            for (int i = 0; i < channels; ++i)
                w.push_back (0.5f + r.unipolar()); // weight
            for (int i = 0; i < channels; ++i)
                w.push_back (0.1f * r.bipolar()); // bias
            w.push_back (1.0e-5f);                // eps
        }
        else
            for (int i = 0; i < channels; ++i)
                w.push_back (0.1f * r.bipolar());
    }
    for (int i = 0; i < channels + 1; ++i)
        w.push_back (0.3f * r.bipolar());
    return w;
}

inline std::shared_ptr<const dsp::CaptureFile> parseOrFail (const std::string& text)
{
    auto result = dsp::CaptureFile::parse (text);
    INFO (result.error);
    REQUIRE (result.file != nullptr);
    return result.file;
}

/// A stage prepared for mono at `fs` with the built-in amp's latency at that rate.
struct StageRig
{
    StageRig (double fs, int block, OversamplingMode mode = OversamplingMode::lowLatency)
    {
        amp.prepare ({ fs, block, 1, mode });
        cab.prepare ({ fs, block, 2 });
        stage.prepare ({ fs, block, 1, mode }, amp.latencySamples());
        maxBlock = block;
    }

    void load (const std::shared_ptr<const dsp::CaptureFile>& f)
    {
        std::string error;
        auto m = f->instantiate (stage.modelSpec(), &error);
        INFO (error);
        REQUIRE (m != nullptr);
        stage.setModel (std::move (m));
    }

    /// Mono amp stage only, in place, in blocks of `block`.
    void run (std::vector<float>& audio, int block = 0)
    {
        block = block > 0 ? block : maxBlock;
        for (std::size_t s = 0; s < audio.size(); s += static_cast<std::size_t> (block))
        {
            const int n = static_cast<int> (
                std::min<std::size_t> (static_cast<std::size_t> (block), audio.size() - s));
            stage.setSettings (settings);
            stage.process (amp, ampSettings, AudioView (audio.data() + s, n));
        }
    }

    dsp::AmpSection amp;
    dsp::CabSim cab;
    dsp::CaptureStage stage;
    dsp::CaptureStage::Settings settings;
    dsp::AmpSection::Settings ampSettings;
    int maxBlock = 512;
};

} // namespace sedef::test::nam
