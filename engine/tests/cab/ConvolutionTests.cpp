// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CabTestUtils.h"
#include "sedef/dsp/cab/ConvolutionKernel.h"
#include "sedef/dsp/cab/PartitionedConvolver.h"

#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <thread>

using namespace sedef;
using namespace sedef::test;
using namespace sedef::test::cab;
using Catch::Approx;
using dsp::ConvolutionKernel;

namespace
{
std::vector<float> randomIr (int length, std::uint32_t seed, float decaySamples = 800.0f)
{
    std::vector<float> ir (static_cast<size_t> (length));
    Random r (seed);
    for (int i = 0; i < length; ++i)
        ir[static_cast<size_t> (i)] =
            r.bipolar() * std::exp (-static_cast<float> (i) / decaySamples);
    return ir;
}

std::vector<double> directConvolution (const std::vector<float>& x, const float* h, int taps)
{
    std::vector<double> y (x.size(), 0.0);
    for (size_t n = 0; n < x.size(); ++n)
    {
        double acc = 0.0;
        const int kMax = std::min (taps - 1, static_cast<int> (n));
        for (int k = 0; k <= kMax; ++k)
            acc += static_cast<double> (h[k]) * x[n - static_cast<size_t> (k)];
        y[n] = acc;
    }
    return y;
}

/// Max |a - b| relative to max |b|.
double relativeError (const std::vector<float>& a, const std::vector<double>& b)
{
    double err = 0.0, scale = 0.0;
    for (size_t i = 0; i < b.size(); ++i)
    {
        err = std::max (err, std::abs (static_cast<double> (a[i]) - b[i]));
        scale = std::max (scale, std::abs (b[i]));
    }
    return err / scale;
}

/// Feeds `x` through a mono CabSim (mic A only, centred) in blocks from `blocks`, cycling.
std::vector<float> runCabSim (CabSim& cab, const std::vector<float>& x,
                              const std::vector<int>& blocks)
{
    std::vector<float> y (x.size(), 0.0f);
    Buffer b (2, 512);
    size_t pos = 0, which = 0;
    while (pos < x.size())
    {
        const int n = std::min (blocks[which++ % blocks.size()], static_cast<int> (x.size() - pos));
        auto view = b.view().subView (0, n);
        cab.process (x.data() + pos, view);
        for (int i = 0; i < n; ++i)
            y[pos + static_cast<size_t> (i)] =
                (b[0][static_cast<size_t> (i)] + b[1][static_cast<size_t> (i)]) * 0.70710678f;
        pos += static_cast<size_t> (n);
    }
    return y;
}

CabSim::Settings customOnA (int cab = 0)
{
    auto s = soloMicA (cab, CabSim::kCustomIrType);
    return s;
}

/// Installs a kernel on mic A / cab 0 and lets the audio side pick it up without a crossfade.
void install (CabSim& cab, std::unique_ptr<ConvolutionKernel> k, const CabSim::Settings& s)
{
    cab.setCustomIr (0, s.cab, std::move (k));
    cab.setSettings (s);
    cab.reset(); // picks up the kernel and snaps the voice to it
}

double dominantFrequency (const std::vector<float>& v, double fs, double lo, double hi)
{
    double best = lo, bestMag = -1.0;
    for (double hz = lo; hz <= hi; hz += 0.25)
    {
        const double m = std::abs (responseAt (v, fs, hz));
        if (m > bestMag)
        {
            bestMag = m;
            best = hz;
        }
    }
    return best;
}
} // namespace

TEST_CASE ("Partitioned convolution matches direct convolution", "[cab][convolution]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto ir = randomIr (3000, 7);
    const auto kernel = ConvolutionKernel::fromTaps (ir.data(), 3000, fs);
    REQUIRE (kernel != nullptr);
    CHECK (kernel->numTailPartitions() ==
           (3000 + kernel->partitionSize() - 1) / kernel->partitionSize() - 1);

    std::vector<float> x (20000);
    fillNoise (x, 1.0f, 3);
    const auto expected = directConvolution (x, ir.data(), 3000);

    PFFFT_Setup* setup = pffft_new_setup (2 * kernel->partitionSize(), PFFFT_REAL);
    dsp::cab::PartitionedConvolver conv;
    conv.prepare (setup, kernel->partitionSize(), kernel->numTailPartitions());
    REQUIRE (conv.accepts (*kernel));

    const std::vector<std::vector<int>> schedules { { 1 },
                                                    { 333 },
                                                    { 1, 333, 17, 64, 5, 512, 2, 255, 256, 257 } };
    for (const auto& blocks : schedules)
    {
        conv.reset();
        std::vector<float> y (x.size());
        size_t pos = 0, which = 0;
        while (pos < x.size())
        {
            const int n =
                std::min (blocks[which++ % blocks.size()], static_cast<int> (x.size() - pos));
            conv.process (*kernel, x.data() + pos, y.data() + pos, n);
            pos += static_cast<size_t> (n);
        }
        CHECK (relativeError (y, expected) < 1.0e-5);
    }
    pffft_destroy_setup (setup);
}

TEST_CASE ("CabSim custom IR matches direct convolution for any block size", "[cab][convolution]")
{
    const double fs = 48000.0;
    const auto ir = randomIr (3000, 11);
    auto kernel = CabSim::makeKernel (ir.data(), 3000, fs, fs, 512);
    REQUIRE (kernel != nullptr);
    REQUIRE (kernel->length() == 3000); // same rate: no resampling, nothing trimmed
    const std::vector<float> taps (kernel->taps(), kernel->taps() + kernel->length());

    std::vector<float> x (24000);
    fillNoise (x, 0.5f, 5);
    const auto expected = directConvolution (x, taps.data(), static_cast<int> (taps.size()));

    CabSim cab;
    cab.prepare ({ fs, 512, 2 });
    install (cab, std::move (kernel), customOnA());

    Random r (99);
    std::vector<int> randomBlocks (64);
    for (auto& b : randomBlocks)
        b = 1 + static_cast<int> (r.unipolar() * 511.0f);
    randomBlocks[3] = 1;
    randomBlocks[9] = 333;

    for (const auto& blocks : { std::vector<int> { 1 }, std::vector<int> { 333 }, randomBlocks })
    {
        cab.reset();
        const auto y = runCabSim (cab, x, blocks);
        CHECK (relativeError (y, expected) < 1.0e-5);
    }
}

TEST_CASE ("An impulse IR passes the input through with zero latency", "[cab][convolution]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const float one = 1.0f;
    auto kernel = CabSim::makeKernel (&one, 1, fs, fs, 512);
    REQUIRE (kernel != nullptr);
    CHECK (kernel->normalisationGain() == Approx (1.0f).margin (1e-5));

    CabSim cab;
    cab.prepare ({ fs, 512, 2 });
    CHECK (cab.latencySamples() == 0);
    install (cab, std::move (kernel), customOnA());

    std::vector<float> x (6000);
    fillNoise (x, 1.0f, 8);
    for (int block : { 1, 17, 512 })
    {
        cab.reset();
        const auto y = runCabSim (cab, x, { block });
        float err = 0.0f;
        for (size_t i = 0; i < x.size(); ++i)
            err = std::max (err, std::abs (y[i] - x[i]));
        CHECK (err < 1.0e-5f);
    }
}

TEST_CASE ("A delayed impulse crossing the head/tail split delays exactly", "[cab][convolution]")
{
    const double fs = 48000.0;
    const int p = ConvolutionKernel::partitionSizeFor (fs);
    for (int delay : { p - 1, p, p + 1, 3 * p + 7 })
    {
        std::vector<float> ir (static_cast<size_t> (delay + 1), 0.0f);
        ir.back() = 1.0f;
        auto kernel = ConvolutionKernel::fromTaps (ir.data(), delay + 1, fs);
        CabSim cab;
        cab.prepare ({ fs, 512, 2 });
        install (cab, std::move (kernel), customOnA());

        std::vector<float> x (4000);
        fillNoise (x, 1.0f, static_cast<std::uint32_t> (delay));
        const auto y = runCabSim (cab, x, { 37 });
        float err = 0.0f;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const float want =
                i >= static_cast<size_t> (delay) ? x[i - static_cast<size_t> (delay)] : 0.0f;
            err = std::max (err, std::abs (y[i] - want));
        }
        CHECK (err < 1.0e-5f);
    }
}

TEST_CASE ("makeKernel resampling keeps frequencies and the IR's response", "[cab][convolution]")
{
    struct Case
    {
        double from, to;
    };
    const auto c =
        GENERATE (Case { 44100.0, 48000.0 }, Case { 48000.0, 44100.0 }, Case { 96000.0, 48000.0 });

    // A 1 kHz Hann-windowed tone burst, 0.1 s long.
    const int len = static_cast<int> (0.1 * c.from);
    std::vector<float> ir (static_cast<size_t> (len));
    for (int i = 0; i < len; ++i)
        ir[static_cast<size_t> (i)] =
            static_cast<float> (std::sin (kTwoPiD * 1000.0 * i / c.from) *
                                (0.5 - 0.5 * std::cos (kTwoPiD * i / (len - 1))));

    auto kernel = CabSim::makeKernel (ir.data(), len, c.from, c.to, 512);
    REQUIRE (kernel != nullptr);
    CHECK (kernel->sampleRate() == c.to);
    CHECK (std::abs (kernel->length() - 0.1 * c.to) < 0.002 * c.to);

    const std::vector<float> taps (kernel->taps(), kernel->taps() + kernel->length());
    CHECK (dominantFrequency (taps, c.to, 900.0, 1100.0) == Approx (1000.0).margin (1.0));

    // Undo the normalisation: the response at 1 kHz must match the source's.
    const double source = std::abs (responseAt (ir, c.from, 1000.0));
    const double resampled =
        std::abs (responseAt (taps, c.to, 1000.0)) / kernel->normalisationGain();
    CHECK (20.0 * std::log10 (resampled / source) == Approx (0.0).margin (0.05));
}

TEST_CASE ("makeKernel normalises IRs to a 0 dB loudness index", "[cab][convolution]")
{
    const double fs = 48000.0;
    SECTION ("scaled impulses get the inverse gain")
    {
        for (float a : { 0.25f, 1.0f, 3.0f })
        {
            auto k = CabSim::makeKernel (&a, 1, fs, fs, 64);
            REQUIRE (k != nullptr);
            CHECK (k->normalisationGain() == Approx (1.0f / a).epsilon (1e-4));
            CHECK (k->taps()[0] == Approx (1.0f).epsilon (1e-4));
        }
    }
    SECTION ("arbitrary IRs land on 0 dB regardless of their level")
    {
        for (float scale : { 0.01f, 1.0f, 20.0f })
        {
            auto ir = randomIr (4000, 21, 400.0f);
            for (auto& v : ir)
                v *= scale;
            auto k = CabSim::makeKernel (ir.data(), 4000, 44100.0, fs, 64);
            REQUIRE (k != nullptr);
            CHECK (dsp::cab::irLoudnessDb (k->taps(), k->length(), fs) ==
                   Approx (0.0).margin (0.05));
        }
    }
    SECTION ("an impulse sits at the same loudness index as the procedural mics")
    {
        CabSim cab;
        for (int c = 0; c < CabSim::kNumCabs; ++c)
            for (int t = 0; t < 10; ++t)
            {
                const auto ir = impulseResponse (cab, soloMicA (c, t), fs, 16384);
                CHECK (dsp::cab::irLoudnessDb (ir.data(), 16384, fs) == Approx (0.0).margin (0.3));
            }
    }
}

TEST_CASE ("makeKernel truncates long IRs to one second with a fade-out", "[cab][convolution]")
{
    const double fs = 48000.0;
    std::vector<float> ir (static_cast<size_t> (2.5 * fs));
    fillNoise (ir, 0.5f, 4);
    auto k = CabSim::makeKernel (ir.data(), static_cast<int> (ir.size()), fs, fs, 512);
    REQUIRE (k != nullptr);
    // One second, less the faded-out last samples that fall below the -120 dB trim.
    CHECK (k->length() <= static_cast<int> (fs));
    CHECK (k->length() > static_cast<int> (0.995 * fs));
    const float* t = k->taps();
    const float g = k->normalisationGain();
    CHECK (std::abs (t[k->length() - 1]) < 1.0e-6f);
    // Untouched before the 10 ms fade, attenuated inside it.
    const int fadeStart = k->length() - static_cast<int> (0.01 * fs);
    CHECK (t[fadeStart - 10] ==
           Approx (ir[static_cast<size_t> (fadeStart - 10)] * g).margin (1e-6));
    const int mid = fadeStart + static_cast<int> (0.005 * fs);
    CHECK (std::abs (t[mid]) <= std::abs (ir[static_cast<size_t> (mid)] * g) * 0.51f + 1.0e-6f);

    // Resampling path too: 44.1k -> 48k of a 3 s IR.
    std::vector<float> longIr (static_cast<size_t> (3 * 44100));
    fillNoise (longIr, 0.5f, 6);
    auto k2 =
        CabSim::makeKernel (longIr.data(), static_cast<int> (longIr.size()), 44100.0, fs, 512);
    REQUIRE (k2 != nullptr);
    CHECK (k2->length() <= static_cast<int> (fs));
    CHECK (std::abs (k2->taps()[k2->length() - 1]) < 1.0e-6f);
}

TEST_CASE ("makeKernel rejects empty input", "[cab][convolution]")
{
    const float v = 1.0f;
    CHECK (CabSim::makeKernel (nullptr, 10, 48000.0, 48000.0, 64) == nullptr);
    CHECK (CabSim::makeKernel (&v, 0, 48000.0, 48000.0, 64) == nullptr);
    CHECK (CabSim::makeKernel (&v, 1, 0.0, 48000.0, 64) == nullptr);
}

TEST_CASE ("A kernel for another sample rate falls back to Dyn Classic", "[cab][convolution]")
{
    const float one = 1.0f;
    CabSim custom, reference;
    custom.prepare ({ 48000.0, 256, 2 });
    install (custom, CabSim::makeKernel (&one, 1, 44100.0, 44100.0, 256), customOnA());
    reference.prepare ({ 48000.0, 256, 2 });
    reference.setSettings (soloMicA (0, 0));
    reference.reset();

    std::vector<float> x (4096);
    fillNoise (x, 0.5f, 12);
    const auto a = runCabSim (custom, x, { 256 });
    const auto b = runCabSim (reference, x, { 256 });
    CHECK (a == b);
}

TEST_CASE ("Clearing a custom IR crossfades to the Dyn Classic fallback", "[cab][convolution]")
{
    const double fs = 48000.0;
    const float one = 1.0f;
    CabSim cab, reference;
    cab.prepare ({ fs, 128, 2 });
    install (cab, CabSim::makeKernel (&one, 1, fs, fs, 128), customOnA());
    reference.prepare ({ fs, 128, 2 });
    reference.setSettings (soloMicA (0, 0));
    reference.reset();

    std::vector<float> x (static_cast<size_t> (fs));
    fillNoise (x, 0.5f, 13);
    std::vector<float> head (x.begin(), x.begin() + 4096);
    const auto passed = runCabSim (cab, head, { 128 }); // impulse IR: identity
    float identityErr = 0.0f;
    for (size_t i = 0; i < head.size(); ++i)
        identityErr = std::max (identityErr, std::abs (passed[i] - head[i]));
    CHECK (identityErr < 1.0e-6f);

    cab.setCustomIr (0, 0, nullptr);
    const auto y = runCabSim (cab, x, { 128 });
    const auto want = runCabSim (reference, x, { 128 });
    // After the 30 ms crossfade and the procedural chain's short memory, both agree.
    float err = 0.0f;
    for (size_t i = x.size() / 2; i < x.size(); ++i)
        err = std::max (err, std::abs (y[i] - want[i]));
    CHECK (err < 1.0e-4f);
    cab.collectGarbage();
}

TEST_CASE ("setCustomIr from another thread is safe and the audio thread never frees",
           "[cab][convolution][concurrency]")
{
    const int liveBefore = ConvolutionKernel::liveInstances();
    {
        const double fs = 48000.0;
        CabSim cab;
        cab.prepare ({ fs, 64, 2 });
        CabSim::Settings s;
        s.mics[0].type = CabSim::kCustomIrType;
        s.mics[1].type = CabSim::kCustomIrType;
        s.mics[1].roomEnabled = true;
        cab.setSettings (s);
        cab.reset();

        std::atomic<bool> stop { false };
        std::atomic<int> audioFrees { -1 };
        std::atomic<bool> finite { true };
        std::atomic<long> blocks { 0 };
        std::thread audio (
            [&]
            {
                Buffer in (1, 64), out (2, 64);
                fillNoise (in[0], 0.5f, 77);
                int cabIndex = 0;
                while (!stop.load())
                {
                    if ((blocks.load() % 50) == 0)
                    {
                        s.cab = cabIndex = (cabIndex + 1) % CabSim::kNumCabs;
                        cab.setSettings (s);
                    }
                    cab.process (in[0].data(), out.view());
                    if (!allFinite (out[0]) || !allFinite (out[1]))
                        finite = false;
                    blocks.fetch_add (1);
                }
                audioFrees = ConvolutionKernel::destroyedOnThisThread();
            });

        Random r (5);
        const auto start = std::chrono::steady_clock::now();
        int published = 0;
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds (600))
        {
            const int mic = static_cast<int> (r.nextU32() % 2);
            const int slot = static_cast<int> (r.nextU32() % 3);
            if (r.unipolar() < 0.2f)
            {
                cab.setCustomIr (mic, slot, nullptr);
            }
            else
            {
                const auto ir =
                    randomIr (500 + static_cast<int> (r.nextU32() % 3000), r.nextU32(), 300.0f);
                cab.setCustomIr (
                    mic, slot,
                    CabSim::makeKernel (ir.data(), static_cast<int> (ir.size()), 44100.0, fs, 64));
            }
            ++published;
            cab.collectGarbage();
            std::this_thread::sleep_for (std::chrono::microseconds (200 + r.nextU32() % 800));
        }
        stop = true;
        audio.join();

        CHECK (published > 50);
        CHECK (blocks.load() > 100);
        CHECK (finite.load());
        CHECK (audioFrees.load() == 0);
        cab.collectGarbage();
    }
    CHECK (ConvolutionKernel::liveInstances() == liveBefore);
}
