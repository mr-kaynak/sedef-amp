// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestUtils.h"
#include "sedef/core/Oversampler.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace sedef;
using Catch::Approx;

namespace
{
/// Runs a sine through up+down with an identity in between.
std::vector<float> roundTrip (int factorLog2, double hz, double fs, int frames, int block = 64)
{
    Oversampler os;
    os.prepare (block, 1, factorLog2);
    test::Buffer in (1, frames);
    test::fillSine (in[0], hz, fs, 0.5f);
    std::vector<float> out (static_cast<size_t> (frames));
    for (int start = 0; start < frames; start += block)
    {
        const int n = std::min (block, frames - start);
        float* inPtr = in[0].data() + start;
        AudioView v (&inPtr, 1, n);
        os.upsample (v);
        float* outPtr = out.data() + start;
        os.downsample (AudioView (&outPtr, 1, n));
    }
    return out;
}
} // namespace

TEST_CASE ("Oversampler latency is reported exactly", "[core][oversampling]")
{
    const int factorLog2 = GENERATE (1, 2);
    Oversampler os;
    os.prepare (64, 1, factorLog2);
    const int latency = os.latencySamples();
    CHECK (latency == (factorLog2 == 2 ? 37 : 31));

    // An impulse comes out band-limited and centred on the reported latency.
    const int frames = 256;
    std::vector<float> in (frames, 0.0f), out (frames, 0.0f);
    in[10] = 1.0f;
    for (int s = 0; s < frames; s += 64)
    {
        float* a = in.data() + s;
        float* b = out.data() + s;
        os.upsample (AudioView (&a, 1, 64));
        os.downsample (AudioView (&b, 1, 64));
    }
    const auto peakIt = std::max_element (out.begin(), out.end());
    CHECK (std::distance (out.begin(), peakIt) == 10 + latency);
    CHECK (*peakIt > 0.75f);
}

TEST_CASE ("Oversampler passband is flat and the round trip preserves level",
           "[core][oversampling]")
{
    const double fs = 48000.0;
    const int factorLog2 = GENERATE (1, 2);
    for (double hz : { 100.0, 1000.0, 10000.0, 18000.0 })
    {
        const auto out = roundTrip (factorLog2, hz, fs, 16384);
        const double mag = test::toneMagnitude (out.data() + 8192, 8192, hz, fs);
        CHECK (20.0 * std::log10 (mag / 0.5) == Approx (0.0).margin (0.1));
    }
}

TEST_CASE ("Upsampled images are rejected by at least 80 dB", "[core][oversampling]")
{
    const double fs = 48000.0;
    Oversampler os;
    const int block = 512;
    os.prepare (block, 1, 2);
    test::Buffer in (1, block * 32);
    test::fillSine (in[0], 1000.0, fs, 0.5f);
    std::vector<float> high;
    for (int s = 0; s < in.frames(); s += block)
    {
        float* p = in[0].data() + s;
        const auto up = os.upsample (AudioView (&p, 1, block));
        high.insert (high.end(), up.channel (0), up.channel (0) + up.numFrames());
        float* q = in[0].data() + s;
        os.downsample (AudioView (&q, 1, block));
    }
    const double fsHigh = fs * 4.0;
    const int n = 65536;
    const double wanted = test::toneMagnitude (high.data() + high.size() - n, n, 1000.0, fsHigh);
    const double image = test::toneMagnitude (high.data() + high.size() - n, n, 47000.0, fsHigh);
    CHECK (20.0 * std::log10 (image / wanted) < -80.0);
}
