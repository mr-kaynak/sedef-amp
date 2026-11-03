// SPDX-License-Identifier: AGPL-3.0-or-later
// Every .nam file is untrusted input from the internet. Malformed ones are rejected with a clear
// message, quickly, without crashing (the ASan/UBSan CI job runs these too).

#include "CaptureTestUtils.h"

#include <Eigen/Core>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>

using namespace sedef;
namespace nt = sedef::test::nam;
using nt::json;

namespace
{
/// A WaveNet in the trainer's layout, small so that the files that load measure quickly.
json standardJson()
{
    return json::parse (nt::smallWaveNet (1));
}

void expectRejected (const std::string& text, const std::string& fragment)
{
    const auto start = std::chrono::steady_clock::now();
    const auto result = dsp::CaptureFile::parse (text);
    const auto seconds =
        std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    CAPTURE (fragment, result.error, seconds);
    CHECK (result.file == nullptr);
    CHECK (result.error.find (fragment) != std::string::npos);
    CHECK (seconds < 5.0); // never hangs
}

void expectRejected (const json& j, const std::string& fragment)
{
    expectRejected (j.dump(), fragment);
}
} // namespace

TEST_CASE ("Files that aren't captures are rejected", "[capture][untrusted]")
{
    expectRejected (std::string(), "not valid JSON");
    expectRejected (std::string ("{\"version\": \"0.5.4\", "), "not valid JSON");
    expectRejected (std::string ("RIFF....WAVEfmt "), "not valid JSON");
    expectRejected (std::string ("[1, 2, 3]"), "not a JSON object");
    expectRejected (std::string ("{}"), "has no \"version\"");
    auto j = standardJson();
    j.erase ("weights");
    expectRejected (j, "has no \"weights\"");
    j = standardJson();
    j["config"] = 5;
    expectRejected (j, "has no \"config\"");
    j = standardJson();
    j["weights"] = "lots";
    expectRejected (j, "has no \"weights\"");
}

TEST_CASE ("Format versions outside 0.5-0.7 are rejected", "[capture][untrusted]")
{
    for (const auto& [version, fragment] :
         { std::pair<std::string, std::string> { "0.4.0", "older than Sedef reads" },
           { "0.8.0", "newer than Sedef reads" },
           { "1.0.0", "newer than Sedef reads" },
           { "0.5", "isn't valid" },
           { "0.5.x", "isn't valid" },
           { "v0.5.4", "isn't valid" },
           { "0.5.4.1", "isn't valid" } })
    {
        auto j = standardJson();
        j["version"] = version;
        expectRejected (j, fragment);
    }
    auto j = standardJson();
    j["version"] = 5;
    expectRejected (j, "no format \"version\"");
    // 0.7.x is read (partially supported by the core), 0.5.0 too.
    for (const char* ok : { "0.5.0", "0.6.1", "0.7.0", "0.7.9" })
    {
        j = standardJson();
        j["version"] = ok;
        CHECK (dsp::CaptureFile::parse (j.dump()).file != nullptr);
    }
}

TEST_CASE ("Unknown and unsupported architectures are named", "[capture][untrusted]")
{
    auto j = standardJson();
    j["architecture"] = "Transformer";
    expectRejected (j, "architecture Sedef doesn't know");
    j["architecture"] = "Sequential";
    expectRejected (j, "Sequential architecture, which Sedef can't load yet");
    j["architecture"] = "SlimmableContainer";
    expectRejected (j, "can't load yet");
    j["architecture"] = 12;
    expectRejected (j, "has no \"architecture\"");
}

TEST_CASE ("Weight counts must match the architecture", "[capture][untrusted]")
{
    auto j = standardJson();
    j["weights"].erase (j["weights"].size() - 1);
    expectRejected (j, "1003 weights, but its WaveNet needs 1004");
    j = standardJson();
    j["weights"].push_back (0.1);
    expectRejected (j, "1005 weights");
    j = standardJson();
    j["weights"][100] = "x";
    expectRejected (j, "a weight is not a number");
    j = standardJson();
    j["weights"][100] = 1.0e300;
    expectRejected (j, "a weight is out of range");
    j = standardJson();
    j["weights"][5] = nullptr;
    expectRejected (j, "a weight is not a number");

    const auto lstm = json::parse (
        nt::file ("LSTM", nt::lstmConfig (2, 4),
                  nt::randomWeights (static_cast<std::size_t> (nt::lstmWeights (2, 4) - 1), 1)));
    expectRejected (lstm, "needs"); // the core itself only asserts on this
    const auto conv = json::parse (nt::file (
        "ConvNet", nt::convNetConfig (3, { 1, 2 }, true),
        nt::randomWeights (static_cast<std::size_t> (nt::convNetWeights (3, 2, true) + 2), 1)));
    expectRejected (conv, "needs");
    expectRejected (json::parse (nt::file ("Linear", { { "receptive_field", 4 }, { "bias", true } },
                                           { 1, 2, 3, 4 })),
                    "needs 5");
}

TEST_CASE ("Missing or mistyped config fields are rejected before the core reads them",
           "[capture][untrusted]")
{
    // Several of these are read through const operator[] in the core, which is undefined
    // behaviour for a missing key.
    auto j = standardJson();
    j["config"].erase ("head_scale");
    expectRejected (j, "\"head_scale\" is missing");
    j = standardJson();
    j["config"].erase ("layers");
    expectRejected (j, "no layer arrays");
    j = standardJson();
    j["config"]["layers"][0].erase ("channels");
    expectRejected (j, "layer array 1: \"channels\" is missing");
    j = standardJson();
    j["config"]["layers"][1]["kernel_size"] = "three";
    expectRejected (j, "layer array 2: \"kernel_size\" must be a whole number");
    j = standardJson();
    j["config"]["layers"][0]["gated"] = 1;
    expectRejected (j, "\"gated\" must be true or false");
    j = standardJson();
    j["config"]["layers"][0]["activation"] = json::object();
    expectRejected (j, "an activation has no \"type\"");
    j = standardJson();
    j["config"]["layers"][0]["activation"] = "Swishy";
    expectRejected (j, "\"Swishy\" activation, which Sedef doesn't know");
    j = standardJson();
    j["config"]["layers"][0]["head1x1"] = { { "active", true } };
    expectRejected (j, "head1x1: \"out_channels\" is missing");
    j = standardJson();
    j["config"]["layers"][0]["conv_pre_film"] = true;
    expectRejected (j, "must be false or an object");

    for (const char* key : { "num_layers", "input_size", "hidden_size" })
    {
        auto c = nt::lstmConfig (1, 4);
        c.erase (key);
        expectRejected (
            json::parse (nt::file (
                "LSTM", c,
                nt::randomWeights (static_cast<std::size_t> (nt::lstmWeights (1, 4)), 1))),
            "is missing");
    }
    for (const char* key : { "channels", "dilations", "batchnorm", "activation" })
    {
        auto c = nt::convNetConfig (2, { 1, 2 }, false);
        c.erase (key);
        expectRejected (json::parse (nt::file (
                            "ConvNet", c,
                            nt::randomWeights (
                                static_cast<std::size_t> (nt::convNetWeights (2, 2, false)), 1))),
                        std::string ("\"") + key + "\"");
    }
    expectRejected (json::parse (nt::file ("Linear", { { "bias", false } }, { 1 })),
                    "\"receptive_field\" is missing");
}

TEST_CASE ("Shapes that would make the core divide by zero or read out of bounds are rejected",
           "[capture][untrusted]")
{
    auto j = standardJson();
    j["config"]["layers"][0]["groups_input"] = 0;
    expectRejected (j, "must be from 1");
    j = standardJson();
    j["config"]["layers"][0]["groups_input"] = 3; // 16 channels
    expectRejected (j, "3 groups don't divide 4 inputs");
    j = standardJson();
    j["config"]["layers"][0]["channels"] = 0;
    expectRejected (j, "\"channels\" must be from 1");
    j = standardJson();
    j["config"]["layers"][0]["channels"] = -16;
    expectRejected (j, "\"channels\" must be from 1");
    j = standardJson();
    j["config"]["layers"][0]["kernel_size"] = 0;
    expectRejected (j, "\"kernel_size\" must be from 1");
    j = standardJson();
    j["config"]["layers"][0]["dilations"] = json::array();
    expectRejected (j, "\"dilations\" must be a non-empty list");
    j = standardJson();
    j["config"]["layers"][1]["input_size"] = 8; // previous array has 16 channels
    expectRejected (j, "doesn't match the previous array's channels");
    j = standardJson();
    j["config"]["layers"][0]["condition_size"] = 2;
    expectRejected (j, "the condition has 1 channels");
    j = standardJson();
    j["config"]["layers"][1]["head_size"] = 2;
    expectRejected (j, "2 outputs; Sedef plays captures with one input and one output");
    j = standardJson();
    j["config"]["in_channels"] = 2;
    expectRejected (j, "2 inputs");
    j = standardJson();
    j["config"]["layers"][0]["bottleneck"] = 8;
    j["config"]["layers"][0]["layer1x1"] = { { "active", false }, { "groups", 1 } };
    expectRejected (j, "without layer1x1 the bottleneck must equal the channels");
    j = standardJson();
    j["config"]["layers"][0]["activation"] = { { "type", "PReLU" },
                                               { "negative_slopes", { 0.1, 0.2 } } };
    expectRejected (j, "PReLU has 2 slopes for 4 channels");
    j["config"]["layers"][0]["activation"] = { { "type", "PReLU" },
                                               { "negative_slopes", json::array() } };
    expectRejected (j, "PReLU slopes must be a non-empty list");
    j["config"]["layers"][0]["activation"] = { { "type", "LeakyReLU" },
                                               { "negative_slope", 1.0e30 } };
    expectRejected (j, "out of range");

    auto lstm = nt::lstmConfig (1, 4);
    lstm["input_size"] = 3; // the core would copy one input sample into a three-element vector
    expectRejected (json::parse (nt::file ("LSTM", lstm, nt::randomWeights (500, 1))),
                    "\"input_size\" doesn't match");
    auto conv = nt::convNetConfig (4, { 1, 2 }, false);
    conv["groups"] = 2; // the first block has one input
    expectRejected (json::parse (nt::file ("ConvNet", conv, nt::randomWeights (100, 1))),
                    "groups don't divide");
    expectRejected (
        json::parse (nt::file ("Linear", { { "receptive_field", 0 }, { "bias", false } }, {})),
        "must be from 1");
}

TEST_CASE ("Absurd sizes are rejected as too large, without allocating them",
           "[capture][untrusted]")
{
    auto j = standardJson();
    j["config"]["layers"][0]["channels"] = 100000;
    expectRejected (j, "too large to play in real time");
    j = standardJson();
    j["config"]["layers"][0]["dilations"] = std::vector<int> (5000, 1);
    expectRejected (j, "too large");
    j = standardJson();
    j["config"]["layers"][0]["dilations"] = { 1, 1000000000 };
    expectRejected (j, "too large");
    j = standardJson();
    j["config"]["layers"] = std::vector<json> (100, j["config"]["layers"][0]);
    expectRejected (j, "100 layer arrays");
    // Plausible fields, impossible cost: 64 channels x 64 layers.
    j = standardJson();
    j["config"]["layers"][0]["channels"] = 64;
    j["config"]["layers"][0]["dilations"] = std::vector<int> (64, 1);
    expectRejected (j, "operations per second");
    // Wider than a matrix product may be.
    j["config"]["layers"][0]["channels"] = 512;
    expectRejected (j, "mixes 512 channels at once");
    // A receptive field of minutes.
    j = standardJson();
    j["config"]["layers"][0]["dilations"] = std::vector<int> (40, 32768);
    expectRejected (j, "samples back");
    expectRejected (json::parse (nt::file ("LSTM", nt::lstmConfig (16, 512), {})),
                    "operations per second");
    expectRejected (json::parse (nt::file (
                        "Linear", { { "receptive_field", 100000000 }, { "bias", false } }, {})),
                    "too large");
}

TEST_CASE ("Hostile JSON can't exhaust the stack, memory or time", "[capture][untrusted]")
{
    // Deep nesting: the parser recurses per level.
    expectRejected (std::string (200000, '[') + std::string (200000, ']'), "nested more than 64");
    expectRejected ("{\"a\":" + std::string (100, '{') + std::string (100, '}') + "}",
                    "nested more than 64");
    // Millions of tiny values.
    std::string many = "[0";
    many.reserve (6'000'000);
    for (int i = 0; i < 2'600'000; ++i)
        many += ",0";
    many += "]";
    expectRejected (many, "more than 2500000 numbers");
    // A file over the size limit.
    expectRejected (std::string (dsp::CaptureFile::kMaxBytes + 1, ' '), "the file is 24 MB");
    // Strings with brackets and escapes don't confuse the pre-scan.
    auto j = standardJson();
    j["metadata"] = { { "name", std::string (1000, '[') + "\\\"]]]" } };
    const auto ok = dsp::CaptureFile::parse (j.dump());
    CHECK (ok.file != nullptr);
    CHECK (ok.file->info().name.size() <= 120);
}

TEST_CASE ("Bad numbers are rejected or ignored, never trusted", "[capture][untrusted]")
{
    auto j = standardJson();
    j["sample_rate"] = 8000;
    expectRejected (j, "made at 8000 Hz; Sedef plays captures made at 16 to 192 kHz");
    j["sample_rate"] = 1.0e12;
    expectRejected (j, "Hz; Sedef plays captures made at 16 to 192 kHz");
    j["sample_rate"] = "fast";
    expectRejected (j, "\"sample_rate\" must be a number");
    j["sample_rate"] = -1; // the core's "unknown": played at 48 kHz
    auto unknown = dsp::CaptureFile::parse (j.dump());
    REQUIRE (unknown.file != nullptr);
    CHECK_FALSE (unknown.file->info().sampleRateKnown);
    CHECK (unknown.file->info().sampleRate == 48000.0);

    // Metadata of the wrong type or out of range is ignored, not passed to the core.
    j = standardJson();
    j["metadata"] = { { "loudness", "loud" },
                      { "input_level_dbu", 1.0e9 },
                      { "output_level_dbu", nullptr },
                      { "name", 42 },
                      { "gear_type", "toaster" } };
    const auto r = dsp::CaptureFile::parse (j.dump());
    REQUIRE (r.file != nullptr);
    CHECK_FALSE (r.file->info().loudnessDb.has_value());
    CHECK_FALSE (r.file->info().inputLevelDbu.has_value());
    CHECK_FALSE (r.file->info().outputLevelDbu.has_value());
    CHECK (r.file->info().name.empty());
    CHECK (r.file->info().kind == dsp::CaptureKind::Unknown);
    j["metadata"] = "not an object";
    CHECK (dsp::CaptureFile::parse (j.dump()).file != nullptr);
}

TEST_CASE ("Captures that produce silence or garbage are rejected", "[capture][untrusted]")
{
    expectRejected (nt::linear ({ 0.0f, 0.0f, 0.0f }), "silent");
    // Batch norm with eps + variance = 0 divides by zero: infinite output.
    auto w = nt::convNetWeightsFor (2, 1, true, 3);
    // Block layout: conv (2*2*1), mean (2), var (2), weight (2), bias (2), eps (1).
    w[4 + 2] = -1.0e-5f;
    w[4 + 3] = -1.0e-5f;
    expectRejected (nt::file ("ConvNet", nt::convNetConfig (2, { 1 }, true), w), "invalid output");
    // Huge but finite weights: far louder than any amp.
    expectRejected (nt::linear ({ 1.0e6f }), "far louder");
}

TEST_CASE ("Random corruption of a valid capture never crashes", "[capture][untrusted]")
{
    // Small WaveNet so each attempt is quick; flips, truncations and duplications at random
    // positions. Every result is either a capture or an error message.
    const std::string valid =
        nt::file ("WaveNet", nt::tinyWaveNetConfig (3, { 1, 2, 4 }),
                  nt::randomWeights (static_cast<std::size_t> (nt::tinyWaveNetWeights (3, 3)), 2));
    REQUIRE (dsp::CaptureFile::parse (valid).file != nullptr);
    Random r (2026);
    const std::string alphabet = "{}[]\",:0123456789-.eE truefalsn";
    int accepted = 0;
    for (int round = 0; round < 200; ++round)
    {
        std::string text = valid;
        const int edits = 1 + static_cast<int> (r.nextU32() % 4);
        for (int e = 0; e < edits; ++e)
        {
            const auto pos = static_cast<std::size_t> (r.nextU32() % text.size());
            switch (r.nextU32() % 4)
            {
            case 0:
                text[pos] = alphabet[r.nextU32() % alphabet.size()];
                break;
            case 1:
                text.resize (pos);
                break;
            case 2:
                text.insert (pos, text.substr (pos, std::min<std::size_t> (20, text.size() - pos)));
                break;
            default:
                text.erase (pos, 1 + r.nextU32() % 8);
                break;
            }
            if (text.empty())
                text = "{";
        }
        const auto result = dsp::CaptureFile::parse (text);
        if (result.file != nullptr)
        {
            ++accepted;
            // Whatever was accepted plays finite audio.
            auto m = result.file->instantiate ({ 44100.0, 64, 37 });
            REQUIRE (m != nullptr);
            std::vector<float> v (512);
            test::fillNoise (v, 0.5f, static_cast<std::uint32_t> (round + 1));
            for (std::size_t s = 0; s < v.size(); s += 64)
                m->process (v.data() + s, v.data() + s, 64);
            CHECK (test::allFinite (v));
        }
        else
            CHECK_FALSE (result.error.empty());
    }
    CAPTURE (accepted);
    SUCCEED();
}

namespace
{
/// One layer array of `channels` channels and kernel `kernel`, one layer per dilation, with the
/// convolution and the layer 1x1 split into `groups` groups; the weight count to match.
std::pair<json, std::vector<float>> groupedWaveNet (int channels, int groups, int kernel,
                                                    const std::vector<int>& dilations,
                                                    std::uint32_t seed)
{
    auto config = nt::tinyWaveNetConfig (channels, dilations);
    auto& a = config["layers"][0];
    a["kernel_size"] = kernel;
    a["groups_input"] = groups;
    a["layer1x1"] = { { "active", true }, { "groups", groups } };
    const auto layers = static_cast<int> (dilations.size());
    const int perLayer = kernel * channels * channels / groups + channels // convolution
                         + channels                                       // input mixin
                         + channels * channels / groups + channels;       // layer 1x1
    auto w = nt::randomWeights (
        static_cast<std::size_t> (channels + layers * perLayer + channels + 1 + 1), seed, 0.2f);
    w.back() = 0.5f;
    return { config, w };
}
} // namespace

TEST_CASE ("A PReLU in a gated or blended layer needs one slope per channel",
           "[capture][untrusted]")
{
    // Gated layers run their activations over every channel without wrapping around the slopes,
    // so a single slope would be read past its end on every sample.
    const auto gated = [] (const json& activation, const char* mode) {
        auto config = nt::tinyWaveNetConfig (4, { 1, 2 }, "Tanh", true);
        auto& a = config["layers"][0];
        if (std::string (mode) == "secondary")
        {
            a.erase ("gated");
            a["gating_mode"] = "blended";
            a["secondary_activation"] = activation;
        }
        else
            a["activation"] = activation;
        auto w = nt::randomWeights (static_cast<std::size_t> (nt::tinyWaveNetWeights (4, 2, true)),
                                    6, 0.3f);
        return nt::file ("WaveNet", config, w);
    };
    const json single = { { "type", "PReLU" }, { "negative_slope", 0.1 } };
    const json one = { { "type", "PReLU" }, { "negative_slopes", { 0.1 } } };
    const json none = { { "type", "PReLU" } };
    const json name = "PReLU";
    const json perChannel = { { "type", "PReLU" }, { "negative_slopes", { 0.1, 0.2, 0.3, 0.4 } } };
    for (const char* mode : { "primary", "secondary" })
    {
        CAPTURE (mode);
        for (const auto& a : { single, one, none, name })
            expectRejected (gated (a, mode), "one slope for each of its 4 channels");
        const auto ok = dsp::CaptureFile::parse (gated (perChannel, mode));
        CAPTURE (ok.error);
        CHECK (ok.file != nullptr);
    }
    // Without gating the core wraps around, so one slope still works there.
    auto plain = standardJson();
    plain["config"]["layers"][0]["activation"] = single;
    CHECK (dsp::CaptureFile::parse (plain.dump()).file != nullptr);
}

TEST_CASE ("Grouped convolutions count at their real cost", "[capture][untrusted]")
{
    // The core keeps a grouped convolution's full matrix (zeros between the groups) and multiplies
    // all of it, so a grouped layer costs what the same layer ungrouped does.
    const auto [groupedConfig, groupedWeights] = groupedWaveNet (8, 4, 2, { 1, 2 }, 3);
    const auto [denseConfig, denseWeights] = groupedWaveNet (8, 1, 2, { 1, 2 }, 3);
    const auto grouped = nt::parseOrFail (nt::file ("WaveNet", groupedConfig, groupedWeights));
    const auto dense = nt::parseOrFail (nt::file ("WaveNet", denseConfig, denseWeights));
    CHECK (grouped->info().numWeights < dense->info().numWeights);
    CHECK (grouped->info().macsPerSample == dense->info().macsPerSample);

    // Small in the file, too slow to play: about 75,000 multiply-adds per sample.
    const auto [config, weights] = groupedWaveNet (32, 16, 8, std::vector<int> (8, 1), 4);
    expectRejected (nt::file ("WaveNet", config, weights), "operations per second");
}

TEST_CASE ("The CPU cap counts operations per second at the capture's own rate",
           "[capture][untrusted]")
{
    // 54,000 multiply-adds per sample: playable at 48 kHz, more than three cores' worth of a
    // standard capture at 96 kHz.
    expectRejected (nt::waveNet (32, 16, 2, 0.2f, 96000.0), "operations per second");
    CHECK (dsp::CaptureFile::parse (nt::linear ({ 0.5f, 0.25f }, 192000.0)).file != nullptr);
}

TEST_CASE ("A capture that takes too long to start is refused", "[capture][untrusted]")
{
    // Every instance runs its receptive field once before it plays: 40,000 samples of a model
    // with 100,000 multiply-adds per sample are 4 billion operations, for each instance.
    auto config = nt::tinyWaveNetConfig (64, { 32768, 7232, 1, 1, 1, 1, 1, 1 });
    auto w =
        nt::randomWeights (static_cast<std::size_t> (nt::tinyWaveNetWeights (64, 8)), 5, 0.05f);
    expectRejected (nt::file ("WaveNet", config, w, 16000.0), "operations to start");
}

TEST_CASE ("Products wider than Eigen's stack buffers are refused", "[capture][untrusted]")
{
    // Eigen packs each matrix product into buffers it takes from the stack only up to
    // EIGEN_STACK_ALLOCATION_LIMIT, and from the heap beyond: on the audio thread. A layer that
    // mixes 128 channels at once crosses it at 512-sample blocks.
    auto config = nt::tinyWaveNetConfig (128, { 1 });
    config["layers"][0]["kernel_size"] = 1;
    const int weights = 128 + (128 * 128 + 128 + 128 + 128 * 128 + 128) + 128 + 1 + 1;
    expectRejected (nt::file ("WaveNet", config,
                              nt::randomWeights (static_cast<std::size_t> (weights), 7, 0.05f)),
                    "mixes 128 channels at once");

    // What the validator allows (at most 64 channels into a product, 512 out, 512-sample blocks)
    // fits on every CPU: blocking only ever shrinks a dimension.
    Eigen::Index k = 64, m = 512, n = 512;
    Eigen::internal::computeProductBlockingSizes<float, float, 1> (k, m, n, Eigen::Index (1));
    CHECK (k * m * static_cast<Eigen::Index> (sizeof (float)) <= EIGEN_STACK_ALLOCATION_LIMIT);
    CHECK (k * n * static_cast<Eigen::Index> (sizeof (float)) <= EIGEN_STACK_ALLOCATION_LIMIT);
}

TEST_CASE ("A conditioning model must be one Sedef runs in real time", "[capture][untrusted]")
{
    // The core builds a nested ConvNet itself, and its ConvNet allocates on every block.
    auto j = standardJson();
    json inner;
    inner["version"] = "0.6.0";
    inner["architecture"] = "ConvNet";
    inner["config"] = nt::convNetConfig (4, { 1, 2 }, false);
    inner["weights"] = nt::convNetWeightsFor (4, 2, false, 9);
    inner["sample_rate"] = 48000;
    j["config"]["condition_dsp"] = inner;
    j["version"] = "0.6.0";
    expectRejected (j, "conditioning model is a ConvNet");
}
