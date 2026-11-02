// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AmpTestUtils.h"
#include "sedef/core/Oversampler.h"
#include "sedef/dsp/amp/AmpStages.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace sedef;
using dsp::amp::AdaaTriodeStage;
using dsp::amp::TriodeCurve;

namespace
{
constexpr TriodeCurve kCold { 1.2f, 0.6f, 0.6f };

/// The curve AdaaTriodeStage integrates, sampled directly (no anti-aliasing, no bias shift).
float sampledCurve (float x, const TriodeCurve& c)
{
    const auto f = [&] (float u) {
        const float lim = u >= 0.0f ? c.soft : c.hard;
        return u / std::sqrt (1.0f + (u / lim) * (u / lim));
    };
    return f (x + c.bias) - f (c.bias);
}

} // namespace

TEST_CASE ("ADAA triode stage follows its curve and stays silent at rest", "[amp]")
{
    AdaaTriodeStage stage;
    stage.configure (kCold, 0.0f, 192000.0);
    // Digital silence is exactly silent.
    for (int i = 0; i < 64; ++i)
        REQUIRE (stage.process (0, 0.0f) == 0.0f);
    // A held input settles on the static curve, on both sides of the operating point.
    for (float x : { -5.0f, -0.7f, -0.1f, 0.05f, 0.4f, 2.0f, 30.0f })
    {
        float y = 0.0f;
        for (int i = 0; i < 4; ++i)
            y = stage.process (0, x);
        CAPTURE (x, y, sampledCurve (x, kCold));
        CHECK (std::abs (y - sampledCurve (x, kCold)) < 1.0e-5f);
    }
    // Slow signals: the mean over each step equals the curve to within the half-sample delay.
    stage.reset();
    const double fs = 192000.0;
    double err = 0.0;
    float prevX = 0.0f;
    for (int i = 0; i < 19200; ++i)
    {
        const auto x = static_cast<float> (3.0 * std::sin (kTwoPiD * 50.0 * i / fs));
        const float y = stage.process (0, x);
        err = std::max (
            err, static_cast<double> (std::abs (y - sampledCurve (0.5f * (x + prevX), kCold))));
        prevX = x;
    }
    CAPTURE (err);
    CHECK (err < 1.0e-4);
}

TEST_CASE ("ADAA triode stage aliases far less than the sampled curve", "[amp]")
{
    // A 4.3 kHz sine driven 30 dB into a cold clipper inside the amps' 4x oversampler at 48 kHz.
    // The images of harmonics above 96 kHz fold into the audio band.
    const double fs = 48000.0, f0 = 4321.0;
    const int block = 64, blocks = 2048;
    const auto render = [&] (bool antialiased) {
        Oversampler os;
        os.prepare (block, 1, 2);
        AdaaTriodeStage adaa;
        adaa.configure (kCold, 0.0f, 4.0 * fs);
        std::vector<float> out;
        std::vector<float> buf (static_cast<size_t> (block));
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
                buf[static_cast<size_t> (i)] =
                    static_cast<float> (20.0 * std::sin (kTwoPiD * f0 * (b * block + i) / fs));
            float* p = buf.data();
            const AudioView io (&p, 1, block);
            const AudioView up = os.upsample (io);
            float* d = up.channel (0);
            for (int i = 0; i < up.numFrames(); ++i)
                d[i] = antialiased ? adaa.process (0, d[i]) : sampledCurve (d[i], kCold);
            os.downsample (io);
            out.insert (out.end(), buf.begin(), buf.end());
        }
        return out;
    };
    const double naiveDb = test::worstAliasDb (render (false), f0, fs);
    const double adaaDb = test::worstAliasDb (render (true), f0, fs);
    CAPTURE (naiveDb, adaaDb);
    CHECK (adaaDb < naiveDb - 10.0);
}
