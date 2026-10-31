// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace sedef::dsp
{
namespace cab
{

/// Owning, SIMD-aligned float array (pffft needs 16-byte alignment). Zero-filled on allocation.
class AlignedFloats
{
public:
    AlignedFloats() = default;
    explicit AlignedFloats (std::size_t count) { allocate (count); }
    ~AlignedFloats();
    AlignedFloats (AlignedFloats&& other) noexcept;
    AlignedFloats& operator= (AlignedFloats&& other) noexcept;
    AlignedFloats (const AlignedFloats&) = delete;
    AlignedFloats& operator= (const AlignedFloats&) = delete;

    /// Non-real-time. Replaces the contents with `count` zeros.
    void allocate (std::size_t count);
    void clear() noexcept;

    float* data() noexcept { return data_; }
    const float* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    float* data_ = nullptr;
    std::size_t size_ = 0;
};

/// Resamples an impulse response with a Kaiser-windowed sinc (32 zero crossings per side,
/// ~90 dB stop band, pass band to 95% of the lower Nyquist). Output amplitudes are scaled by
/// inRate / outRate so the filter's frequency response keeps its gain. At most `maxLength`
/// output samples are produced. Non-real-time.
std::vector<float> resampleIr (const float* ir, int length, double inRate, double outRate,
                               int maxLength);

/// Loudness index of a magnitude response: the mean, in dB, of the power in sixteen
/// third-octave bands centred from 157 Hz to 5 kHz. 0 dB for a unit impulse. Procedural mics and
/// custom IRs are both normalised to 0 dB on this scale so they sit at the same level.
template <typename PowerAtHz>
double loudnessIndexDb (PowerAtHz&& powerAt)
{
    constexpr int kBands = 16;
    constexpr int kPointsPerBand = 8;
    double sumDb = 0.0;
    for (int band = 0; band < kBands; ++band)
    {
        const double centre = 1000.0 * std::exp2 ((band - 8) / 3.0);
        double power = 0.0;
        for (int i = 0; i < kPointsPerBand; ++i)
        {
            // Log-spaced midpoints across [centre / 2^(1/6), centre * 2^(1/6)].
            const double t = (i + 0.5) / kPointsPerBand - 0.5;
            power += powerAt (centre * std::exp2 (t / 3.0));
        }
        sumDb += 10.0 * std::log10 (std::max (power / kPointsPerBand, 1.0e-30));
    }
    return sumDb / kBands;
}

/// Loudness index (see loudnessIndexDb) of an impulse response sampled at `sampleRate`.
double irLoudnessDb (const float* ir, int length, double sampleRate);

} // namespace cab

/// An immutable, convolution-ready impulse response: time-domain head for the zero-latency direct
/// part and pre-transformed tail partitions for uniformly partitioned FFT convolution.
/// Built off the audio thread (CabSim::makeKernel or fromTaps); read-only afterwards.
class ConvolutionKernel
{
public:
    static constexpr double kMaxSeconds = 1.0;

    /// An empty kernel. CabSim uses it to mean "no IR loaded".
    ConvolutionKernel();
    ~ConvolutionKernel();
    ConvolutionKernel (const ConvolutionKernel&) = delete;
    ConvolutionKernel& operator= (const ConvolutionKernel&) = delete;

    /// Non-real-time. Partitions `taps` as they are (no resampling, truncation or normalisation).
    /// `appliedGain` is recorded for reporting only. Returns null if length <= 0.
    static std::unique_ptr<ConvolutionKernel>
    fromTaps (const float* taps, int length, double sampleRate, float appliedGain = 1.0f);

    /// Partition size P used at a sample rate: 256 up to 50 kHz, 512 up to 100 kHz, else 1024.
    static int partitionSizeFor (double sampleRate) noexcept;

    bool empty() const noexcept { return taps_.empty(); }
    int length() const noexcept { return static_cast<int> (taps_.size()); }
    double sampleRate() const noexcept { return sampleRate_; }
    int partitionSize() const noexcept { return partition_; }
    int numTailPartitions() const noexcept { return numTail_; }
    /// Gain makeKernel applied to the source IR (normalisation).
    float normalisationGain() const noexcept { return gain_; }
    /// The taps the convolver realises (after resampling, truncation and normalisation).
    const float* taps() const noexcept { return taps_.data(); }

    /// First P taps, used by the direct-form head.
    const float* head() const noexcept { return head_.data(); }
    /// Spectrum of taps [(j+1)P, (j+2)P) zero-padded to 2P, in pffft's internal order and
    /// pre-scaled by 1 / (2P).
    const float* tailSpectrum (int j) const noexcept
    {
        return tail_.data() +
               static_cast<std::size_t> (j) * 2u * static_cast<std::size_t> (partition_);
    }

    /// Instrumentation for tests: kernels alive now, and kernels destroyed by the calling thread.
    static int liveInstances() noexcept;
    static int destroyedOnThisThread() noexcept;

private:
    std::vector<float> taps_;
    cab::AlignedFloats head_;
    cab::AlignedFloats tail_;
    double sampleRate_ = 0.0;
    int partition_ = 0;
    int numTail_ = 0;
    float gain_ = 1.0f;
};

} // namespace sedef::dsp
