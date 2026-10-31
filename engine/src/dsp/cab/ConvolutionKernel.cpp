// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/cab/ConvolutionKernel.h"

#include "sedef/core/Math.h"

#include <atomic>
#include <cstring>
#include <new>
#include <pffft/pffft.h>

namespace sedef::dsp
{
namespace cab
{

AlignedFloats::~AlignedFloats()
{
    pffft_aligned_free (data_);
}

AlignedFloats::AlignedFloats (AlignedFloats&& other) noexcept
    : data_ (other.data_),
      size_ (other.size_)
{
    other.data_ = nullptr;
    other.size_ = 0;
}

AlignedFloats& AlignedFloats::operator= (AlignedFloats&& other) noexcept
{
    if (this != &other)
    {
        pffft_aligned_free (data_);
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void AlignedFloats::allocate (std::size_t count)
{
    pffft_aligned_free (data_);
    data_ = nullptr;
    size_ = 0;
    if (count == 0)
        return;
    data_ = static_cast<float*> (pffft_aligned_malloc (count * sizeof (float)));
    if (data_ == nullptr)
        throw std::bad_alloc();
    size_ = count;
    clear();
}

void AlignedFloats::clear() noexcept
{
    if (data_ != nullptr)
        std::memset (data_, 0, size_ * sizeof (float));
}

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

/// RAII pffft setup for the non-real-time helpers.
struct FftSetup
{
    explicit FftSetup (int n) : setup (pffft_new_setup (n, PFFFT_REAL))
    {
        if (setup == nullptr)
            throw std::bad_alloc();
    }
    ~FftSetup() { pffft_destroy_setup (setup); }
    FftSetup (const FftSetup&) = delete;
    FftSetup& operator= (const FftSetup&) = delete;
    PFFFT_Setup* setup;
};
} // namespace

std::vector<float> resampleIr (const float* ir, int length, double inRate, double outRate,
                               int maxLength)
{
    std::vector<float> out;
    if (ir == nullptr || length <= 0 || maxLength <= 0 || inRate <= 0.0 || outRate <= 0.0)
        return out;

    if (std::abs (inRate - outRate) <= 1.0e-9 * outRate)
    {
        out.assign (ir, ir + std::min (length, maxLength));
        return out;
    }

    constexpr int kZeroCrossings = 32;
    constexpr int kTableSteps = 512; // table entries per zero crossing
    constexpr double kBeta = 9.0;
    constexpr double kRolloff = 0.95;

    const double ratio = outRate / inRate;
    const double scale = std::min (1.0, ratio) * kRolloff; // cut-off relative to input Nyquist
    const double halfWidth = kZeroCrossings / scale;       // in input samples

    // Windowed sinc as a function of v = scale * |t - k|, v in [0, kZeroCrossings].
    std::vector<double> table (static_cast<size_t> (kZeroCrossings * kTableSteps + 2), 0.0);
    const double i0Beta = besselI0 (kBeta);
    for (int i = 0; i <= kZeroCrossings * kTableSteps; ++i)
    {
        const double v = static_cast<double> (i) / kTableSteps;
        const double sinc = i == 0 ? 1.0 : std::sin (kPiD * v) / (kPiD * v);
        const double r = v / kZeroCrossings;
        table[static_cast<size_t> (i)] =
            sinc * besselI0 (kBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
    }

    const auto outLength =
        static_cast<int> (std::min<double> (maxLength, std::ceil (length * ratio)));
    out.assign (static_cast<size_t> (std::max (outLength, 1)), 0.0f);
    // Dividing by the ratio keeps the IR's frequency response (sum of taps) unchanged.
    const double amplitude = scale / ratio;
    for (int n = 0; n < outLength; ++n)
    {
        const double t = n / ratio;
        const int first = std::max (0, static_cast<int> (std::ceil (t - halfWidth)));
        const int last = std::min (length - 1, static_cast<int> (std::floor (t + halfWidth)));
        double acc = 0.0;
        for (int k = first; k <= last; ++k)
        {
            const double pos = std::abs (t - k) * scale * kTableSteps;
            const auto idx = static_cast<size_t> (pos);
            if (idx >= table.size() - 1)
                continue;
            const double frac = pos - static_cast<double> (idx);
            const double w = table[idx] + frac * (table[idx + 1] - table[idx]);
            acc += static_cast<double> (ir[k]) * w;
        }
        out[static_cast<size_t> (n)] = static_cast<float> (acc * amplitude);
    }
    return out;
}

double irLoudnessDb (const float* ir, int length, double sampleRate)
{
    if (ir == nullptr || length <= 0 || sampleRate <= 0.0)
        return -300.0;

    // Zero-pad to at least 16k points so the lowest band still spans many bins.
    int n = 16384;
    while (n < 2 * length)
        n *= 2;

    FftSetup fft (n);
    AlignedFloats buffer (static_cast<size_t> (n));
    AlignedFloats work (static_cast<size_t> (n));
    std::memcpy (buffer.data(), ir, sizeof (float) * static_cast<size_t> (length));
    pffft_transform_ordered (fft.setup, buffer.data(), buffer.data(), work.data(), PFFFT_FORWARD);

    const float* spec = buffer.data();
    const auto binPower = [&] (int k) -> double {
        if (k <= 0)
            return static_cast<double> (spec[0]) * spec[0];
        if (k >= n / 2)
            return static_cast<double> (spec[1]) * spec[1];
        const double re = spec[2 * k], im = spec[2 * k + 1];
        return re * re + im * im;
    };

    return loudnessIndexDb ([&] (double hz) {
        const double bin = hz * n / sampleRate;
        const int k = static_cast<int> (bin);
        const double frac = bin - k;
        return binPower (k) + frac * (binPower (k + 1) - binPower (k));
    });
}

} // namespace cab

namespace
{
std::atomic<int> gLiveKernels { 0 };
thread_local int tDestroyedKernels = 0;
} // namespace

ConvolutionKernel::ConvolutionKernel()
{
    gLiveKernels.fetch_add (1, std::memory_order_relaxed);
}

ConvolutionKernel::~ConvolutionKernel()
{
    gLiveKernels.fetch_sub (1, std::memory_order_relaxed);
    ++tDestroyedKernels;
}

int ConvolutionKernel::liveInstances() noexcept
{
    return gLiveKernels.load (std::memory_order_relaxed);
}

int ConvolutionKernel::destroyedOnThisThread() noexcept
{
    return tDestroyedKernels;
}

int ConvolutionKernel::partitionSizeFor (double sampleRate) noexcept
{
    // P near sqrt(3 * taps) balances the direct head (P MACs per sample) against the tail
    // (about 3 * taps / P operations per sample) for a one-second IR.
    if (sampleRate <= 50000.0)
        return 256;
    if (sampleRate <= 100000.0)
        return 512;
    return 1024;
}

std::unique_ptr<ConvolutionKernel>
ConvolutionKernel::fromTaps (const float* taps, int length, double sampleRate, float appliedGain)
{
    if (taps == nullptr || length <= 0 || sampleRate <= 0.0)
        return nullptr;

    auto k = std::make_unique<ConvolutionKernel>();
    const int p = partitionSizeFor (sampleRate);
    const int fftSize = 2 * p;
    k->taps_.assign (taps, taps + length);
    k->sampleRate_ = sampleRate;
    k->partition_ = p;
    k->gain_ = appliedGain;
    k->numTail_ = std::max (0, (length + p - 1) / p - 1);

    k->head_.allocate (static_cast<size_t> (p));
    std::memcpy (k->head_.data(), taps,
                 sizeof (float) * static_cast<size_t> (std::min (p, length)));

    if (k->numTail_ > 0)
    {
        cab::FftSetup fft (fftSize);
        cab::AlignedFloats work (static_cast<size_t> (fftSize));
        k->tail_.allocate (static_cast<size_t> (k->numTail_) * static_cast<size_t> (fftSize));
        const float scale = 1.0f / static_cast<float> (fftSize); // folds in the inverse FFT's 1/N
        for (int j = 0; j < k->numTail_; ++j)
        {
            float* dst = k->tail_.data() + static_cast<size_t> (j) * static_cast<size_t> (fftSize);
            const int start = (j + 1) * p;
            const int count = std::min (p, length - start);
            for (int i = 0; i < count; ++i)
                dst[i] = taps[start + i] * scale;
            // dst[count .. 2P) is already zero.
            pffft_transform (fft.setup, dst, dst, work.data(), PFFFT_FORWARD);
        }
    }
    return k;
}

} // namespace sedef::dsp
