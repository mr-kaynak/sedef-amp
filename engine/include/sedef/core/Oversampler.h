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

/// One 2x up/down stage built from a minimum-phase polyphase IIR half-band:
///
///   H(z) = 1/2 [A0(z^2) + z^-1 A1(z^2)],   A(z^2) = prod_k (a_k + z^-2) / (1 + a_k z^-2)
///
/// Two branches of first-order all-pass sections that run at the low rate (Valenzuela and
/// Constantinides 1983; Regalia, Mitra and Vaidyanathan 1988; harris, "Multirate Signal
/// Processing", ch. 10). The coefficients are those of an elliptic half-band, from the
/// closed-form design in docs/dsp/oversampling.md. One multiply per coefficient per low-rate
/// sample in each direction.
class AllpassHalfBand
{
public:
    static constexpr int kMaxCoefs = 12;

    /// Elliptic half-band with `numCoefs` coefficients (order 2 numCoefs + 1). `transition` is
    /// the normalised half-width of the transition band: the passband ends at (1/4 - transition)
    /// of the high rate and the stopband starts at (1/4 + transition).
    void design (int numCoefs, double transition);
    void reset() noexcept;

    /// in: n frames at the low rate -> out: 2n frames at the high rate.
    void up (int channel, const float* in, float* out, int n) noexcept;
    /// in: 2n frames at the high rate -> out: n frames at the low rate.
    void down (int channel, const float* in, float* out, int n) noexcept;

    /// Group delay of one pass (up or down) at DC, in high-rate samples:
    /// 1/2 + sum_k (1 - a_k) / (1 + a_k).
    double groupDelayAtDc() const noexcept { return groupDelayAtDc (coefs_); }
    static double groupDelayAtDc (const std::vector<double>& coefs) noexcept;
    /// Stopband attenuation of the design, in dB (positive).
    double attenuationDb() const noexcept { return attenuationDb_; }
    double transition() const noexcept { return transition_; }
    const std::vector<double>& coefficients() const noexcept { return coefs_; }

    /// The elliptic design's coefficients and attenuation (dB), without preparing a stage.
    static std::vector<double> coefficientsFor (int numCoefs, double transition);
    static double attenuationFor (int numCoefs, double transition) noexcept;

private:
    /// One branch: first-order all-pass sections in series. `x1` is the branch's previous
    /// input; y[s] the previous output of section s (and so the previous input of section s + 1).
    struct Branch
    {
        std::array<float, kMaxCoefs> y {};
        float x1 = 0.0f;
    };
    struct Channel
    {
        std::array<Branch, 2> up, down;
        float downOdd = 0.0f; // odd high-rate sample of the previous pair: branch 1's next input
    };

    float run (int branch, Branch& s, float x) const noexcept;
    void flush (Channel& c) const noexcept;

    std::vector<double> coefs_;
    std::array<std::array<float, kMaxCoefs>, 2> branchCoefs_ {}; // a_0, a_2, ... | a_1, a_3, ...
    std::array<int, 2> branchSize_ {};
    double attenuationDb_ = 0.0;
    double transition_ = 0.0;
    std::array<Channel, kMaxChannels> state_ {};
};

/// Cascaded half-band oversampler, factor 1x, 2x or 4x, minimum phase
/// (OversamplingMode::lowLatency, the default) or linear phase. docs/dsp/oversampling.md has
/// the designs and their measurements.
class Oversampler
{
public:
    /// Picks 4x up to 48 kHz, 2x up to 96 kHz, 1x above.
    static int factorLog2For (double sampleRate) noexcept;

    /// What latencySamples() reports once prepared with these settings.
    static int latencyFor (int factorLog2, OversamplingMode mode) noexcept;

    void prepare (int maxBlockSize, int numChannels, int factorLog2,
                  OversamplingMode mode = OversamplingMode::lowLatency);
    void reset() noexcept;

    int factor() const noexcept { return 1 << factorLog2_; }
    OversamplingMode mode() const noexcept { return mode_; }

    /// Whole base-rate samples of delay through upsample() + downsample(). Linear phase: the
    /// FIRs' exact delay at every frequency. Minimum phase: the round trip's group delay at DC,
    /// which the designs make a whole number; it rises towards the top octave
    /// (docs/dsp/oversampling.md).
    int latencySamples() const noexcept { return latency_; }
    /// The round trip's group delay at DC in base-rate samples, before rounding.
    double groupDelayAtDc() const noexcept;

    /// Upsamples into an internal buffer and returns a view of it (numFrames * factor frames).
    AudioView upsample (const AudioView& in) noexcept;
    /// Downsamples the internal buffer into out (must have the frame count of the last upsample).
    void downsample (const AudioView& out) noexcept;

    /// The minimum-phase stages (0 runs at 2x, 1 at 4x). For tests and docs.
    const AllpassHalfBand& allpassStage (int i) const noexcept
    {
        return iir_[static_cast<size_t> (i)];
    }

private:
    int factorLog2_ = 0;
    int numChannels_ = 0;
    int latency_ = 0;
    int lastFrames_ = 0;
    OversamplingMode mode_ = OversamplingMode::lowLatency;
    std::array<HalfBandStage, 2> stages_;
    std::array<AllpassHalfBand, 2> iir_;
    // buffers_[k][ch]: signal at rate 2^(k+1)
    std::array<std::array<std::vector<float>, kMaxChannels>, 2> buffers_;
    std::array<float*, kMaxChannels> viewPtrs_ {};
};

} // namespace sedef
