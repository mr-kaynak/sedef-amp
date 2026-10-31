// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

#include <array>
#include <vector>

namespace sedef
{

/// One 2x up/down stage built from a linear-phase, Kaiser-windowed half-band FIR.
/// Only the non-zero taps are evaluated (polyphase form).
class HalfBandStage
{
public:
    /// numTaps must be odd. beta is the Kaiser window shape (8.5 ~ 85-90 dB stopband).
    void design (int numTaps, double beta);
    void prepare (int maxInputFrames, int numChannels);
    void reset() noexcept;

    /// in: n frames at the low rate -> out: 2n frames at the high rate.
    void up (int channel, const float* in, float* out, int n) noexcept;
    /// in: 2n frames at the high rate -> out: n frames at the low rate.
    void down (int channel, const float* in, float* out, int n) noexcept;

    /// Group delay of up+down, in low-rate samples.
    int roundTripLatency() const noexcept { return centre_; }
    const std::vector<double>& taps() const noexcept { return taps_; }

private:
    struct Tap
    {
        int index;
        float coeff;
    };

    std::vector<double> taps_;
    std::array<std::vector<Tap>, 2> phaseTaps_; // non-zero taps split by index parity
    int numTaps_ = 0;
    int centre_ = 0;

    // Per-channel history (linear buffers with a mirrored tail, so reads never wrap).
    struct History
    {
        std::vector<float> data;
        int pos = 0;
    };
    std::array<History, kMaxChannels> upHistory_;
    std::array<History, kMaxChannels> downHistory_;
};

/// Cascaded half-band oversampler, factor 1x, 2x or 4x.
class Oversampler
{
public:
    /// Picks 4x up to 48 kHz, 2x up to 96 kHz, 1x above.
    static int factorLog2For (double sampleRate) noexcept;

    void prepare (int maxBlockSize, int numChannels, int factorLog2);
    void reset() noexcept;

    int factor() const noexcept { return 1 << factorLog2_; }
    int latencySamples() const noexcept { return latency_; }

    /// Upsamples into an internal buffer and returns a view of it (numFrames * factor frames).
    AudioView upsample (const AudioView& in) noexcept;
    /// Downsamples the internal buffer into out (must have the frame count of the last upsample).
    void downsample (const AudioView& out) noexcept;

private:
    int factorLog2_ = 0;
    int numChannels_ = 0;
    int latency_ = 0;
    int lastFrames_ = 0;
    std::array<HalfBandStage, 2> stages_;
    // buffers_[k][ch]: signal at rate 2^(k+1)
    std::array<std::array<std::vector<float>, kMaxChannels>, 2> buffers_;
    std::array<float*, kMaxChannels> viewPtrs_ {};
};

} // namespace sedef
