// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/core/Oversampler.h"

#include "sedef/core/Math.h"

#include <cassert>
#include <cmath>
#include <cstring>

namespace sedef
{
namespace
{
double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double halfX = 0.5 * x;
    for (int k = 1; k < 64; ++k)
    {
        term *= (halfX / k) * (halfX / k);
        sum += term;
        if (term < sum * 1.0e-17)
            break;
    }
    return sum;
}
} // namespace

void HalfBandStage::design (int numTaps, double beta)
{
    assert (numTaps % 2 == 1);
    numTaps_ = numTaps;
    centre_ = (numTaps - 1) / 2;
    taps_.assign (static_cast<size_t> (numTaps), 0.0);

    const double i0Beta = besselI0 (beta);
    for (int n = 0; n < numTaps; ++n)
    {
        const int k = n - centre_;
        double sinc = 0.5;
        if (k != 0)
        {
            if (k % 2 == 0)
                continue; // exact zero of the half-band response
            const double x = kPiD * k * 0.5;
            sinc = 0.5 * std::sin (x) / x;
        }
        const double r = 2.0 * n / (numTaps - 1) - 1.0;
        const double window = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        taps_[static_cast<size_t> (n)] = sinc * window;
    }

    // Normalise to exactly unity DC gain.
    double sum = 0.0;
    for (double t : taps_)
        sum += t;
    for (double& t : taps_)
        t /= sum;

    for (auto& p : phaseTaps_)
        p.clear();
    for (int n = 0; n < numTaps; ++n)
        if (taps_[static_cast<size_t> (n)] != 0.0)
            phaseTaps_[static_cast<size_t> (n & 1)].push_back (
                { n, static_cast<float> (taps_[static_cast<size_t> (n)]) });
}

void HalfBandStage::prepare (int maxInputFrames, int numChannels)
{
    // History buffers hold (taps + 2 * frames) samples so a whole block can be appended and read
    // linearly; they are compacted at the start of each call.
    const auto upSize = static_cast<size_t> (numTaps_ + maxInputFrames + 1);
    const auto downSize = static_cast<size_t> (numTaps_ + 2 * maxInputFrames + 1);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        upHistory_[static_cast<size_t> (c)].data.assign (c < numChannels ? upSize : 0, 0.0f);
        downHistory_[static_cast<size_t> (c)].data.assign (c < numChannels ? downSize : 0, 0.0f);
    }
    reset();
}

void HalfBandStage::reset() noexcept
{
    for (auto* group : { &upHistory_, &downHistory_ })
        for (auto& h : *group)
        {
            std::fill (h.data.begin(), h.data.end(), 0.0f);
            h.pos = numTaps_;
        }
}

void HalfBandStage::up (int channel, const float* in, float* out, int n) noexcept
{
    // Zero-stuffing puts x[m] at high-rate index 2m, so output 2m + p only sees taps i with
    // (i - p) even: y[2m + p] = 2 * sum h[i] * x[m - (i - p) / 2].
    auto& h = upHistory_[static_cast<size_t> (channel)];
    float* hist = h.data.data();
    std::memmove (hist, hist + h.pos - numTaps_, sizeof (float) * static_cast<size_t> (numTaps_));
    h.pos = numTaps_;
    std::memcpy (hist + h.pos, in, sizeof (float) * static_cast<size_t> (n));

    for (int m = 0; m < n; ++m)
    {
        const float* x = hist + h.pos + m; // x[0] is the newest input sample for this frame
        for (int p = 0; p < 2; ++p)
        {
            float acc = 0.0f;
            for (const auto& t : phaseTaps_[static_cast<size_t> (p)])
                acc += t.coeff * x[-((t.index - p) >> 1)];
            out[2 * m + p] = 2.0f * acc;
        }
    }
    h.pos += n;
}

void HalfBandStage::down (int channel, const float* in, float* out, int n) noexcept
{
    // y[m] = sum_i h[i] * u[2m - i], evaluated on non-zero taps only. Keeping the even phase makes
    // the up+down round trip an integer number of low-rate samples.
    auto& h = downHistory_[static_cast<size_t> (channel)];
    float* hist = h.data.data();
    std::memmove (hist, hist + h.pos - numTaps_, sizeof (float) * static_cast<size_t> (numTaps_));
    h.pos = numTaps_;
    std::memcpy (hist + h.pos, in, sizeof (float) * static_cast<size_t> (2 * n));

    for (int m = 0; m < n; ++m)
    {
        const float* u = hist + h.pos + 2 * m;
        float acc = 0.0f;
        for (const auto& phase : phaseTaps_)
            for (const auto& t : phase)
                acc += t.coeff * u[-t.index];
        out[m] = acc;
    }
    h.pos += 2 * n;
}

int Oversampler::factorLog2For (double sampleRate) noexcept
{
    if (sampleRate <= 50000.0)
        return 2;
    if (sampleRate <= 100000.0)
        return 1;
    return 0;
}

void Oversampler::prepare (int maxBlockSize, int numChannels, int factorLog2)
{
    assert (factorLog2 >= 0 && factorLog2 <= 2);
    factorLog2_ = factorLog2;
    numChannels_ = numChannels;

    // Stage 1 does the heavy lifting (passband to ~0.41 fs). Stage 2 only has to reject the
    // images of an already band-limited signal, so it can be much shorter. Tap counts are chosen
    // so the round-trip latency is a whole number of base-rate samples.
    stages_[0].design (63, 8.5);
    stages_[1].design (25, 7.0);

    stages_[0].prepare (maxBlockSize, numChannels);
    stages_[1].prepare (maxBlockSize * 2, numChannels);

    for (int k = 0; k < 2; ++k)
        for (int c = 0; c < kMaxChannels; ++c)
            buffers_[static_cast<size_t> (k)][static_cast<size_t> (c)].assign (
                c < numChannels ? static_cast<size_t> (maxBlockSize << (k + 1)) : 0, 0.0f);

    latency_ = 0;
    if (factorLog2_ >= 1)
        latency_ += stages_[0].roundTripLatency();
    if (factorLog2_ >= 2)
        latency_ += stages_[1].roundTripLatency() / 2;
}

void Oversampler::reset() noexcept
{
    for (auto& s : stages_)
        s.reset();
}

AudioView Oversampler::upsample (const AudioView& in) noexcept
{
    lastFrames_ = in.numFrames();
    const int channels = std::min (in.numChannels(), numChannels_);

    if (factorLog2_ == 0)
        return in;

    for (int c = 0; c < channels; ++c)
    {
        auto& b0 = buffers_[0][static_cast<size_t> (c)];
        stages_[0].up (c, in.channel (c), b0.data(), lastFrames_);
        float* result = b0.data();
        if (factorLog2_ == 2)
        {
            auto& b1 = buffers_[1][static_cast<size_t> (c)];
            stages_[1].up (c, b0.data(), b1.data(), lastFrames_ * 2);
            result = b1.data();
        }
        viewPtrs_[static_cast<size_t> (c)] = result;
    }
    return AudioView (viewPtrs_.data(), channels, lastFrames_ << factorLog2_);
}

void Oversampler::downsample (const AudioView& out) noexcept
{
    assert (out.numFrames() == lastFrames_);
    if (factorLog2_ == 0)
        return;

    const int channels = std::min (out.numChannels(), numChannels_);
    for (int c = 0; c < channels; ++c)
    {
        auto& b0 = buffers_[0][static_cast<size_t> (c)];
        if (factorLog2_ == 2)
        {
            auto& b1 = buffers_[1][static_cast<size_t> (c)];
            stages_[1].down (c, b1.data(), b0.data(), lastFrames_ * 2);
        }
        stages_[0].down (c, b0.data(), out.channel (c), lastFrames_);
    }
}

} // namespace sedef
