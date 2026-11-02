// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestUtils.h"
#include "sedef/dsp/capture/CaptureStage.h"
#include "sedef/dsp/capture/RateBridge.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <vector>

using namespace sedef;
using dsp::capture::RateBridge;

namespace
{
/// Host signal through host -> model -> host with an identity "model", in blocks of `block`.
std::vector<float> roundTrip (const std::vector<float>& in, double hostRate, double modelRate,
                              int block, int latency)
{
    RateBridge bridge;
    bridge.prepare (hostRate, modelRate, block, latency);
    std::vector<float> model (static_cast<std::size_t> (bridge.maxModelBlock()));
    std::vector<float> out (in.size());
    for (std::size_t s = 0; s < in.size(); s += static_cast<std::size_t> (block))
    {
        const int n = static_cast<int> (
            std::min<std::size_t> (static_cast<std::size_t> (block), in.size() - s));
        const int m = bridge.toModel (in.data() + s, n, model.data());
        REQUIRE (m <= bridge.maxModelBlock());
        bridge.fromModel (model.data(), m, out.data() + s, n);
    }
    return out;
}

double errorDb (const std::vector<float>& in, const std::vector<float>& out, int delay,
                std::size_t from)
{
    double e = 0.0, p = 0.0;
    for (std::size_t i = from; i < out.size(); ++i)
    {
        const double ref = in[i - static_cast<std::size_t> (delay)];
        e += (out[i] - ref) * (out[i] - ref);
        p += ref * ref;
    }
    return 10.0 * std::log10 (std::max (e, 1.0e-30) / p);
}
} // namespace

TEST_CASE ("Rate bridge latencies: the built-in amps' at 44.1/48 kHz, more above", "[capture]")
{
    // A 48 kHz capture at the host rates the plug-in sees; latencyFor() takes the larger of this
    // and the amp's oversampler latency (37 at 44.1/48 kHz, 31 at 88.2/96 kHz, 0 above).
    CHECK (RateBridge::minimumLatency (48000.0, 48000.0) == 0);
    CHECK (RateBridge::minimumLatency (44100.0, 48000.0) <= 37);
    CHECK (dsp::CaptureStage::latencyFor (44100.0, 37) == 37);
    CHECK (dsp::CaptureStage::latencyFor (48000.0, 37) == 37);
    CHECK (dsp::CaptureStage::latencyFor (96000.0, 31) ==
           RateBridge::minimumLatency (96000.0, 48000.0));
    // Every rate keeps a 16-zero-crossing filter for a 48 kHz model; a 44.1 kHz model at 48 kHz
    // still fits the amp's 37 samples with one crossing less at most.
    for (const double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        const int latency = dsp::CaptureStage::latencyFor (fs, fs <= 48000.0   ? 37
                                                               : fs <= 96000.0 ? 31
                                                                               : 0);
        CAPTURE (fs, latency);
        CHECK (RateBridge::zeroCrossingsFor (fs, 48000.0, latency) == 16);
        CHECK (RateBridge::zeroCrossingsFor (fs, 44100.0, latency) >= 14);
        CHECK (latency * 1000.0 / fs < 0.85); // ms
    }
}

TEST_CASE ("Rate bridge round trip is transparent in the pass band", "[capture]")
{
    const auto [host, model] =
        GENERATE (std::pair { 44100.0, 48000.0 }, std::pair { 88200.0, 48000.0 },
                  std::pair { 96000.0, 48000.0 }, std::pair { 192000.0, 48000.0 },
                  std::pair { 48000.0, 44100.0 }, std::pair { 48000.0, 96000.0 },
                  std::pair { 32000.0, 48000.0 });
    const int latency = RateBridge::minimumLatency (host, model);
    const double nyquist = 0.5 * std::min (host, model);
    for (const double hz : { 100.0, 1000.0, 5000.0, 10000.0, 0.68 * nyquist })
    {
        std::vector<float> in (16384);
        test::fillSine (in, hz, host, 0.5f);
        const auto out = roundTrip (in, host, model, 64, latency);
        const double gainDb =
            20.0 * std::log10 (test::toneMagnitude (out.data() + 8192, 8192, hz, host) /
                               test::toneMagnitude (in.data() + 8192, 8192, hz, host));
        const double err = errorDb (in, out, latency, 4096);
        CAPTURE (host, model, hz, latency, gainDb, err);
        CHECK (std::abs (gainDb) < 0.05);
        CHECK (err < -60.0);
    }
}

TEST_CASE ("Rate bridge latency is exact and linear phase", "[capture]")
{
    const auto [host, model] =
        GENERATE (std::pair { 44100.0, 48000.0 }, std::pair { 96000.0, 48000.0 },
                  std::pair { 48000.0, 44100.0 }, std::pair { 48000.0, 48000.0 });
    const int latency = std::max (37, RateBridge::minimumLatency (host, model));
    std::vector<float> in (4096, 0.0f);
    in[1000] = 1.0f;
    const auto out = roundTrip (in, host, model, 17, latency);
    const auto peak = std::max_element (
        out.begin(), out.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); });
    CAPTURE (host, model, latency);
    CHECK (peak - out.begin() == 1000 + latency);
    // Symmetric around the peak (a linear-phase round trip), to float precision.
    for (int k = 1; k < 40; ++k)
        CHECK (std::abs (out[static_cast<std::size_t> (1000 + latency - k)] -
                         out[static_cast<std::size_t> (1000 + latency + k)]) < 2.0e-4f);
}

TEST_CASE ("Rate bridge output does not depend on the block size", "[capture]")
{
    std::vector<float> in (20000);
    test::fillNoise (in, 0.5f, 3);
    const auto reference = roundTrip (in, 44100.0, 48000.0, 4096, 37);
    for (const int block : { 1, 7, 64, 333, 1024 })
    {
        const auto out = roundTrip (in, 44100.0, 48000.0, block, 37);
        CAPTURE (block);
        CHECK (out == reference); // positions are exact integers: bit for bit
    }
}

TEST_CASE ("Rate bridge rejects what the lower rate can't carry", "[capture]")
{
    // 96 kHz host, 48 kHz model: a 30 kHz tone must not reach the model (it would alias to 18 kHz).
    const double host = 96000.0;
    std::vector<float> in (32768);
    test::fillSine (in, 30000.0, host, 0.5f);
    const auto out = roundTrip (in, host, 48000.0, 64, RateBridge::minimumLatency (host, 48000.0));
    const double at30 = test::toneMagnitude (out.data() + 16384, 16384, 30000.0, host);
    const double at18 = test::toneMagnitude (out.data() + 16384, 16384, 18000.0, host);
    CAPTURE (20.0 * std::log10 (at30 / 0.5), 20.0 * std::log10 (at18 / 0.5));
    CHECK (20.0 * std::log10 (std::max (at30, 1e-12) / 0.5) < -70.0);
    CHECK (20.0 * std::log10 (std::max (at18, 1e-12) / 0.5) < -70.0);
}
