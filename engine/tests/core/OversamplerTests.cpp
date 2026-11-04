// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestUtils.h"
#include "sedef/core/Oversampler.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <complex>
#include <cstdio>

using namespace sedef;
using Catch::Approx;

namespace
{
constexpr auto kLow = OversamplingMode::lowLatency;
constexpr auto kLinear = OversamplingMode::linearPhase;

/// Runs `in` through up+down with an identity in between, in blocks.
std::vector<float> roundTrip (Oversampler& os, const std::vector<float>& in, int block)
{
    std::vector<float> out (in.size());
    for (size_t start = 0; start < in.size(); start += static_cast<size_t> (block))
    {
        const int n =
            static_cast<int> (std::min<size_t> (static_cast<size_t> (block), in.size() - start));
        auto* inPtr = const_cast<float*> (in.data() + start);
        os.upsample (AudioView (&inPtr, 1, n));
        float* outPtr = out.data() + start;
        os.downsample (AudioView (&outPtr, 1, n));
    }
    return out;
}

std::vector<float> roundTrip (int factorLog2, OversamplingMode mode, const std::vector<float>& in,
                              int block = 64)
{
    Oversampler os;
    os.prepare (block, 1, factorLog2, mode);
    return roundTrip (os, in, block);
}

std::vector<float> sine (double hz, double fs, int frames, float amp = 0.5f)
{
    std::vector<float> v (static_cast<size_t> (frames));
    test::fillSine (v, hz, fs, amp);
    return v;
}

double db (double x)
{
    return 20.0 * std::log10 (std::max (x, 1.0e-12));
}

/// The base rates each factor serves (Oversampler::factorLog2For).
std::vector<double> ratesFor (int factorLog2)
{
    return factorLog2 == 2 ? std::vector<double> { 44100.0, 48000.0 }
                           : std::vector<double> { 88200.0, 96000.0 };
}
} // namespace

TEST_CASE ("Oversampler latency is reported exactly", "[core][oversampling]")
{
    for (const auto mode : { kLow, kLinear })
        for (int f = 0; f <= 2; ++f)
        {
            Oversampler os;
            os.prepare (64, 1, f, mode);
            const int expected = mode == kLinear ? std::array { 0, 31, 37 }[static_cast<size_t> (f)]
                                                 : std::array { 0, 3, 5 }[static_cast<size_t> (f)];
            CAPTURE (f, mode == kLinear);
            CHECK (os.latencySamples() == expected);
            CHECK (Oversampler::latencyFor (f, mode) == expected);
            CHECK (os.factor() == 1 << f);
        }
}

TEST_CASE ("Linear phase: an impulse comes out centred on the reported latency",
           "[core][oversampling]")
{
    const int factorLog2 = GENERATE (1, 2);
    Oversampler os;
    os.prepare (64, 1, factorLog2, kLinear);
    const int latency = os.latencySamples();
    CHECK (latency == (factorLog2 == 2 ? 37 : 31));

    std::vector<float> in (256, 0.0f);
    in[10] = 1.0f;
    const auto out = roundTrip (os, in, 64);
    const auto peakIt = std::max_element (out.begin(), out.end());
    CHECK (std::distance (out.begin(), peakIt) == 10 + latency);
    CHECK (*peakIt > 0.75f);
}

TEST_CASE ("Minimum phase: the group delay at DC is the reported whole number",
           "[core][oversampling]")
{
    for (int f = 1; f <= 2; ++f)
    {
        Oversampler os;
        os.prepare (64, 1, f, kLow);
        CAPTURE (f);
        // The designs are tuned so the analytic delay is a whole number of base-rate samples.
        CHECK (os.groupDelayAtDc() == Approx (os.latencySamples()).margin (1.0e-6));

        // And a measured low tone lags by exactly that: guitar fundamentals and their first
        // harmonics are where the timing is felt.
        for (const double fs : ratesFor (f))
            for (const double hz : { 82.0, 330.0, 1000.0 })
            {
                const auto in = sine (hz, fs, 32768);
                const auto out = roundTrip (f, kLow, in);
                const double d = test::toneDelaySamples (in.data() + 16384, out.data() + 16384,
                                                         16384, hz, fs, os.latencySamples());
                CAPTURE (fs, hz, d);
                CHECK (d == Approx (os.latencySamples()).margin (0.01));
            }
    }
}

TEST_CASE ("Minimum phase: the delay rises only in the top octave", "[core][oversampling]")
{
    // The price of minimum phase: the group delay is not flat. Measured phase delay at 48 kHz,
    // 4x (docs/dsp/oversampling.md has the table): within 0.1 samples of 5 up to 5 kHz.
    const double fs = 48000.0;
    for (const double hz : { 2000.0, 5000.0, 10000.0, 15000.0 })
    {
        const auto in = sine (hz, fs, 32768);
        const auto out = roundTrip (2, kLow, in);
        const double d =
            test::toneDelaySamples (in.data() + 16384, out.data() + 16384, 16384, hz, fs, 5.0);
        CAPTURE (hz, d);
        CHECK (d >= 5.0);
        CHECK (d <= (hz <= 5000.0 ? 5.1 : 6.0));
    }
}

TEST_CASE ("Oversampler passband is flat and the round trip preserves level",
           "[core][oversampling]")
{
    SECTION ("Minimum phase: flat to 20 kHz at every base rate")
    {
        for (int f = 1; f <= 2; ++f)
            for (const double fs : ratesFor (f))
                for (double hz : { 20.0, 100.0, 1000.0, 10000.0, 18000.0, 20000.0 })
                {
                    const auto out = roundTrip (f, kLow, sine (hz, fs, 32768));
                    const double mag = test::toneMagnitude (out.data() + 16384, 16384, hz, fs);
                    CAPTURE (f, fs, hz);
                    CHECK (db (mag / 0.5) == Approx (0.0).margin (0.01));
                }
    }
    SECTION ("Linear phase: flat to 18 kHz at 48 kHz")
    {
        const double fs = 48000.0;
        const int factorLog2 = GENERATE (1, 2);
        for (double hz : { 100.0, 1000.0, 10000.0, 18000.0 })
        {
            const auto out = roundTrip (factorLog2, kLinear, sine (hz, fs, 16384));
            const double mag = test::toneMagnitude (out.data() + 8192, 8192, hz, fs);
            CHECK (db (mag / 0.5) == Approx (0.0).margin (0.1));
        }
    }
}

TEST_CASE ("Upsampled images are rejected", "[core][oversampling]")
{
    SECTION ("Linear phase: by at least 80 dB")
    {
        const double fs = 48000.0;
        Oversampler os;
        const int block = 512;
        os.prepare (block, 1, 2, kLinear);
        auto in = sine (1000.0, fs, block * 32);
        std::vector<float> high;
        for (int s = 0; s < static_cast<int> (in.size()); s += block)
        {
            float* p = in.data() + s;
            const auto up = os.upsample (AudioView (&p, 1, block));
            high.insert (high.end(), up.channel (0), up.channel (0) + up.numFrames());
            os.downsample (AudioView (&p, 1, block));
        }
        const double fsHigh = fs * 4.0;
        const int n = 65536;
        const double wanted =
            test::toneMagnitude (high.data() + high.size() - n, n, 1000.0, fsHigh);
        const double image =
            test::toneMagnitude (high.data() + high.size() - n, n, 47000.0, fsHigh);
        CHECK (db (image / wanted) < -80.0);
    }
    SECTION ("Minimum phase: by at least 90 dB for every tone up to 20 kHz")
    {
        for (int f = 1; f <= 2; ++f)
            for (const double fs : ratesFor (f))
                for (const double hz : { 1000.0, 5000.0, 10000.0, 15000.0, 20000.0 })
                {
                    Oversampler os;
                    const int block = 512;
                    os.prepare (block, 1, f, kLow);
                    auto in = sine (hz, fs, block * 64);
                    std::vector<float> high;
                    for (int s = 0; s < static_cast<int> (in.size()); s += block)
                    {
                        float* p = in.data() + s;
                        const auto up = os.upsample (AudioView (&p, 1, block));
                        high.insert (high.end(), up.channel (0), up.channel (0) + up.numFrames());
                        os.downsample (AudioView (&p, 1, block));
                    }
                    const double fsHigh = fs * os.factor();
                    const int n = static_cast<int> (high.size() / 2); // steady state
                    const float* tail = high.data() + high.size() - n;
                    const double wanted = test::toneMagnitude (tail, n, hz, fsHigh);
                    double worst = 0.0;
                    // Images at k fs +- hz, below the high rate's Nyquist.
                    for (int k = 1; k < os.factor(); ++k)
                        for (const double image : { k * fs - hz, k * fs + hz })
                            if (image < fsHigh / 2.0)
                                worst =
                                    std::max (worst, test::toneMagnitude (tail, n, image, fsHigh));
                    CAPTURE (f, fs, hz, db (worst / wanted));
                    CHECK (db (worst / wanted) < -90.0);
                }
    }
}

TEST_CASE ("Minimum phase: what folds into the audible band on the way down is rejected",
           "[core][oversampling]")
{
    // A tone written straight into the oversampled stream (what a waveshaper's harmonics are)
    // above the base rate's Nyquist folds down on decimation. Everything that would land at or
    // below 20 kHz must be at least 90 dB down.
    for (int f = 1; f <= 2; ++f)
        for (const double fs : ratesFor (f))
        {
            const int factor = 1 << f;
            const double fsHigh = fs * factor;
            double worstDb = -300.0, worstAt = 0.0;
            for (double hf = fs - 20000.0; hf < fsHigh / 2.0; hf += 997.0)
            {
                // Where it folds to at the base rate.
                double folded = std::fmod (hf, fs);
                if (folded > fs / 2.0)
                    folded = fs - folded;
                if (folded > 20000.0 || folded < 20.0)
                    continue;
                Oversampler os;
                const int block = 256;
                os.prepare (block, 1, f, kLow);
                const int frames = block * 96;
                std::vector<float> out (static_cast<size_t> (frames));
                std::vector<float> dummy (static_cast<size_t> (block), 0.0f);
                long t = 0;
                for (int s = 0; s < frames; s += block)
                {
                    float* d = dummy.data();
                    const auto up = os.upsample (AudioView (&d, 1, block));
                    for (int i = 0; i < up.numFrames(); ++i, ++t)
                        up.channel (0)[i] =
                            0.5f * static_cast<float> (
                                       std::sin (kTwoPiD * hf * static_cast<double> (t) / fsHigh));
                    float* o = out.data() + s;
                    os.downsample (AudioView (&o, 1, block));
                }
                const int n = 16384;
                const double alias =
                    db (test::toneMagnitude (out.data() + frames - n, n, folded, fs) / 0.5);
                if (alias > worstDb)
                {
                    worstDb = alias;
                    worstAt = hf;
                }
            }
            CAPTURE (f, fs, worstAt, worstDb);
            CHECK (worstDb < -90.0);
        }
}

TEST_CASE ("Oversampler output does not depend on the block size", "[core][oversampling]")
{
    const auto mode = GENERATE (kLow, kLinear);
    const int factorLog2 = GENERATE (1, 2);
    std::vector<float> in (8192);
    test::fillNoise (in, 0.5f, 7);
    const auto reference = roundTrip (factorLog2, mode, in, 512);
    for (const int block : { 1, 7, 64, 333 })
    {
        const auto out = roundTrip (factorLog2, mode, in, block);
        float diff = 0.0f;
        for (size_t i = 0; i < in.size(); ++i)
            diff = std::max (diff, std::abs (out[i] - reference[i]));
        CAPTURE (block);
        CHECK (diff == 0.0f);
    }
}

TEST_CASE ("Minimum phase: channels are independent and silence decays to zero",
           "[core][oversampling]")
{
    Oversampler os;
    const int block = 64;
    os.prepare (block, 2, 2, kLow);
    test::Buffer b (2, block);
    std::vector<float> left, right;
    for (int s = 0; s < 4800; s += block)
    {
        for (int i = 0; i < block; ++i)
        {
            b[0][static_cast<size_t> (i)] = s < 480 ? 0.5f : 0.0f; // a burst, then silence
            b[1][static_cast<size_t> (i)] = 0.0f;
        }
        os.upsample (b.view());
        os.downsample (b.view());
        left.insert (left.end(), b[0].begin(), b[0].end());
        right.insert (right.end(), b[1].begin(), b[1].end());
    }
    CHECK (test::peak (right) == 0.0f);
    CHECK (test::peak (left) > 0.4f);
    // 90 ms after the burst the tail is gone entirely (no denormal crawl).
    CHECK (*std::max_element (left.end() - block, left.end()) == 0.0f);
    CHECK (*std::min_element (left.end() - block, left.end()) == 0.0f);
}

TEST_CASE ("Bypass crossfade: the minimum-phase comb stays in the top octave",
           "[core][oversampling]")
{
    // A slot switching a pedal fades over 10 ms between the dry path, delayed by the whole
    // samples latencySamples() reports, and the pedal behind its oversampler. With minimum phase
    // the two differ in phase in the top octave, so mid-fade they partly cancel there. The
    // bare oversampler (a pedal that changes nothing) is the worst case: measured here, the dip
    // below the steady level during a fade. Linear phase has none.
    const double fs = GENERATE (44100.0, 48000.0);
    // The wet path against the dry one at hz, measured on steady sines, gives the output's level
    // at every point of the linear fade: |(1 - a) + a W / D|. The dip is its lowest.
    const auto dipDb = [fs] (OversamplingMode mode, double hz) {
        Oversampler os;
        os.prepare (64, 1, 2, mode);
        const auto latency = static_cast<size_t> (os.latencySamples());
        std::vector<float> in (32768);
        test::fillSine (in, hz, fs, 0.5f);
        const auto wet = roundTrip (os, in, 64);
        const auto phasor = [hz, fs] (const float* d, int n) {
            std::complex<double> acc;
            for (int i = 0; i < n; ++i)
                acc += static_cast<double> (d[i]) * std::polar (1.0, -kTwoPiD * hz * i / fs);
            return acc;
        };
        const int n = 16384;
        const auto ratio = phasor (wet.data() + 16384, n) / phasor (in.data() + 16384 - latency, n);
        double lowest = 1.0e9;
        for (int k = 0; k <= 1000; ++k)
        {
            const double a = k / 1000.0;
            lowest = std::min (lowest, std::abs ((1.0 - a) + a * ratio));
        }
        return db (lowest);
    };
    for (const double hz : { 100.0, 1000.0, 5000.0, 10000.0, 12000.0 })
    {
        const double low = dipDb (kLow, hz), linear = dipDb (kLinear, hz);
        CAPTURE (fs, hz, low, linear);
        CHECK (linear > -0.05);
        CHECK (low > (hz <= 5000.0 ? -0.05 : hz <= 10000.0 ? -0.35 : -1.1));
    }
}

TEST_CASE ("AlignedDryPath is the oversampler's round trip with nothing in between",
           "[core][oversampling]")
{
    // Minimum phase: exactly the up/down pair; linear phase: exactly the FIRs' delay.
    const int factorLog2 = GENERATE (0, 1, 2);
    std::vector<float> in (4096);
    test::fillNoise (in, 0.5f, 11);
    for (const auto mode : { kLow, kLinear })
    {
        AlignedDryPath dry;
        dry.prepare (64, 1, factorLog2, mode);
        auto out = in;
        for (size_t s = 0; s < out.size(); s += 64)
        {
            float* p = out.data() + s;
            dry.process (AudioView (&p, 1, 64));
        }
        const int latency = Oversampler::latencyFor (factorLog2, mode);
        if (mode == kLow && factorLog2 > 0)
            CHECK (out == roundTrip (factorLog2, mode, in));
        else
            for (size_t i = static_cast<size_t> (latency); i < in.size(); ++i)
                CHECK (out[i] == in[i - static_cast<size_t> (latency)]);
    }
}

TEST_CASE ("Oversampler designs", "[.report][oversampling]")
{
    // Prints the minimum-phase designs for docs/dsp/oversampling.md.
    for (int f = 1; f <= 2; ++f)
    {
        Oversampler os;
        os.prepare (64, 1, f, kLow);
        std::printf ("%dx: latency %d, group delay at DC %.6f\n", 1 << f, os.latencySamples(),
                     os.groupDelayAtDc());
        for (int s = 0; s < f; ++s)
        {
            const auto& st = os.allpassStage (s);
            std::printf ("  stage %d (x%d): %zu coefs, transition %.6f, attenuation %.1f dB, "
                         "delay %.4f high-rate samples\n   ",
                         s, 2 << s, st.coefficients().size(), st.transition(), st.attenuationDb(),
                         st.groupDelayAtDc());
            for (double a : st.coefficients())
                std::printf (" %.9f", a);
            std::printf ("\n");
        }
        for (const double fs : ratesFor (f))
        {
            std::printf ("  %.1f kHz phase delay:", fs / 1000.0);
            for (const double hz : { 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 20000.0 })
            {
                const auto in = sine (hz, fs, 32768);
                const auto out = roundTrip (f, kLow, in);
                std::printf (" %.0f Hz %.2f,", hz,
                             test::toneDelaySamples (in.data() + 16384, out.data() + 16384, 16384,
                                                     hz, fs, os.latencySamples()));
            }
            std::printf ("\n");
        }
    }
}
