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
                              int block, int latency, bool minimumPhase = false)
{
    RateBridge bridge;
    if (minimumPhase)
        bridge.prepareMinimumPhase (hostRate, modelRate, block, latency);
    else
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

TEST_CASE ("Rate bridge latencies: the built-in amps' at 44.1 kHz, a little more above",
           "[capture]")
{
    constexpr auto kLinear = OversamplingMode::linearPhase;
    CHECK (RateBridge::minimumLatency (48000.0, 48000.0) == 0);
    CHECK (dsp::CaptureStage::latencyFor (44100.0, 37, kLinear) == 37);
    // Captures made at any of the common rates get the full 16-zero-crossing filter at every host
    // rate, whatever the built-in amp's latency (37 at 44.1/48 kHz, 31 at 88.2/96 kHz, 0 above).
    for (const double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        for (const int amp : { fs <= 48000.0 ? 37 : fs <= 96000.0 ? 31 : 0, 0 })
        {
            const int latency = dsp::CaptureStage::latencyFor (fs, amp, kLinear);
            CAPTURE (fs, amp, latency);
            for (const double model : { 44100.0, 48000.0, 88200.0, 96000.0 })
            {
                CAPTURE (model);
                CHECK (RateBridge::zeroCrossingsFor (fs, model, latency) == 16);
            }
            CHECK (latency >= amp);
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

// -------------------------------------------------------------------------------------------------
// Minimum phase (OversamplingMode::lowLatency)

TEST_CASE ("Minimum-phase kernel: the prototype's magnitude, all of it causal", "[capture]")
{
    const auto& k = dsp::capture::MinimumPhaseKernel::standard();
    CHECK (k.delayZc() == Catch::Approx (dsp::capture::MinimumPhaseKernel::kDelayZc).margin (2e-3));

    dsp::capture::SincKernel linear;
    linear.design (dsp::capture::MinimumPhaseKernel::kZeroCrossings, RateBridge::kKaiserBeta);
    const int phases = dsp::capture::MinimumPhaseKernel::kPhases;
    const int z = dsp::capture::MinimumPhaseKernel::kZeroCrossings;
    // |G(f)|, f in cycles per zero crossing (the cut-off is at 0.5), both tables as filters at
    // `phases` samples per zero crossing.
    const auto magnitude = [&] (double f, bool minimum) {
        double re = 0.0, im = 0.0;
        const int n = minimum ? 2 * z * phases : 2 * z * phases + 1;
        for (int i = 0; i < n; ++i)
        {
            const double g = minimum ? k.atTableIndex (i)
                                     : linear.atTableIndex (static_cast<double> (i - z * phases));
            re += g * std::cos (kTwoPiD * f * i / phases);
            im -= g * std::sin (kTwoPiD * f * i / phases);
        }
        return std::hypot (re, im);
    };
    const double dc = magnitude (0.0, false);
    CHECK (magnitude (0.0, true) == Catch::Approx (dc).epsilon (1e-4));
    for (const double f : { 0.1, 0.3, 0.4, 0.45 })
    {
        CAPTURE (f);
        CHECK (20.0 * std::log10 (magnitude (f, true) / magnitude (f, false)) ==
               Catch::Approx (0.0).margin (0.01));
    }
    for (const double f : { 0.62, 0.8, 1.5, 3.0 })
    {
        CAPTURE (f);
        CHECK (20.0 * std::log10 (magnitude (f, true) / dc) < -78.0);
    }
    // Causal and front-loaded: 95 % of its energy has arrived after 8 zero crossings, half the
    // linear prototype's look-ahead (its centre is 16 in).
    double head = 0.0, all = 0.0;
    for (int i = 0; i < 2 * z * phases; ++i)
    {
        const double g = k.atTableIndex (i);
        all += g * g;
        if (i < 8 * phases)
            head += g * g;
    }
    CHECK (head / all > 0.95);
}

TEST_CASE ("Minimum-phase rate bridge: about 0.12 ms at every rate", "[capture]")
{
    constexpr auto kLow = OversamplingMode::lowLatency;
    CHECK (RateBridge::minimumPhaseLatency (48000.0, 48000.0) == 0);
    CHECK (RateBridge::minimumPhaseLatency (48000.0, 44100.0) == 6);
    CHECK (RateBridge::minimumPhaseLatency (44100.0, 48000.0) == 5);
    // The stage: the amp's 5 samples at 44.1 kHz, one more at 48 kHz for 44.1 kHz captures.
    CHECK (dsp::CaptureStage::latencyFor (44100.0, 5, kLow) == 5);
    CHECK (dsp::CaptureStage::latencyFor (48000.0, 5, kLow) == 6);
    CHECK (dsp::CaptureStage::latencyFor (96000.0, 3, kLow) == 11);
    CHECK (dsp::CaptureStage::latencyFor (192000.0, 0, kLow) == 23);
    for (const double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        for (const double model : { 44100.0, 48000.0, 88200.0, 96000.0 })
        {
            CAPTURE (fs, model);
            CHECK (RateBridge::minimumPhaseDelay (fs, model) * 1000.0 / fs < 0.13); // ms
            CHECK (RateBridge::minimumPhaseLatency (fs, model) <=
                   dsp::CaptureStage::latencyFor (fs, 0, kLow));
        }
}

TEST_CASE ("Minimum-phase rate bridge: flat in the pass band, its delay where it says", "[capture]")
{
    const auto [host, model] =
        GENERATE (std::pair { 44100.0, 48000.0 }, std::pair { 88200.0, 48000.0 },
                  std::pair { 96000.0, 48000.0 }, std::pair { 192000.0, 48000.0 },
                  std::pair { 48000.0, 44100.0 }, std::pair { 48000.0, 96000.0 },
                  std::pair { 32000.0, 48000.0 });
    const int latency = dsp::CaptureStage::latencyFor (host, 0);
    const double nyquist = 0.5 * std::min (host, model);
    for (const double hz : { 100.0, 1000.0, 5000.0, 10000.0, 0.68 * nyquist })
    {
        std::vector<float> in (32768);
        test::fillSine (in, hz, host, 0.5f);
        const auto out = roundTrip (in, host, model, 64, latency, true);
        const double gainDb =
            20.0 * std::log10 (test::toneMagnitude (out.data() + 16384, 16384, hz, host) /
                               test::toneMagnitude (in.data() + 16384, 16384, hz, host));
        CAPTURE (host, model, hz, latency, gainDb);
        CHECK (std::abs (gainDb) < 0.05);
        if (hz <= 1000.0)
        {
            const double d = test::toneDelaySamples (in.data() + 16384, out.data() + 16384, 16384,
                                                     hz, host, latency);
            CAPTURE (d);
            CHECK (std::abs (d - latency) <= 0.5);
        }
    }
}

TEST_CASE ("Minimum-phase rate bridge: block size and what the lower rate can't carry", "[capture]")
{
    std::vector<float> in (20000);
    test::fillNoise (in, 0.5f, 3);
    const auto reference = roundTrip (in, 44100.0, 48000.0, 4096, 5, true);
    for (const int block : { 1, 7, 64, 333, 1024 })
    {
        const auto out = roundTrip (in, 44100.0, 48000.0, block, 5, true);
        CAPTURE (block);
        CHECK (out == reference);
    }

    const double host = 96000.0;
    std::vector<float> tone (32768);
    test::fillSine (tone, 30000.0, host, 0.5f);
    const auto out = roundTrip (tone, host, 48000.0, 64, 11, true);
    const double at30 = test::toneMagnitude (out.data() + 16384, 16384, 30000.0, host);
    const double at18 = test::toneMagnitude (out.data() + 16384, 16384, 18000.0, host);
    CAPTURE (20.0 * std::log10 (at30 / 0.5), 20.0 * std::log10 (at18 / 0.5));
    CHECK (20.0 * std::log10 (std::max (at30, 1e-12) / 0.5) < -70.0);
    CHECK (20.0 * std::log10 (std::max (at18, 1e-12) / 0.5) < -70.0);
}

TEST_CASE ("Minimum-phase rate bridge: a capture made at 16 kHz plays, a little late", "[capture]")
{
    // The low-latency stage budgets for captures made at 44.1 to 96 kHz. One made at 16 kHz
    // needs about 16 samples at 48 kHz; it plays with the full filter, that much later.
    RateBridge bridge;
    bridge.prepareMinimumPhase (48000.0, 16000.0, 64, 6);
    CHECK (bridge.latency() == RateBridge::minimumPhaseLatency (48000.0, 16000.0));
    CHECK (bridge.latency() > 6);
    CHECK (bridge.latency() * 1000.0 / 48000.0 < 0.35); // ms
}
