// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/record/RateConverter.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>

namespace sedef::record
{

namespace
{
double besselI0 (double x)
{
    // Power series: sum of ((x/2)^k / k!)^2, to double precision.
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 200; ++k)
    {
        term *= q / (static_cast<double> (k) * static_cast<double> (k));
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

/// sinc(u) * kaiser(u / Z), u in zero crossings.
double prototype (double u)
{
    constexpr double z = RateConverter::kZeroCrossings;
    const double r = u / z;
    if (r <= -1.0 || r >= 1.0)
        return 0.0;
    static const double i0Beta = besselI0 (RateConverter::kKaiserBeta);
    const double window = besselI0 (RateConverter::kKaiserBeta * std::sqrt (1.0 - r * r)) / i0Beta;
    const double sinc = u == 0.0 ? 1.0 : std::sin (kPiD * u) / (kPiD * u);
    return sinc * window;
}

std::int64_t wholeHz (double rate)
{
    return std::max<std::int64_t> (1, std::llround (rate));
}

double cutoff (std::int64_t from, std::int64_t to)
{
    // 2 fc / from: the pass band's edge relative to the source's Nyquist frequency.
    return RateConverter::kPassband * static_cast<double> (std::min (from, to)) /
           static_cast<double> (from);
}

/// Source frames kept around the frames being read, refilled in large blocks.
class Window
{
public:
    Window (FrameSource& source, int margin)
        : source_ (source),
          channels_ (std::max (1, source.channels())),
          capacity_ (kBlock + 2 * margin + 2),
          data_ (static_cast<std::size_t> (channels_),
                 std::vector<float> (static_cast<std::size_t> (capacity_))),
          read_ (static_cast<std::size_t> (channels_),
                 std::vector<float> (static_cast<std::size_t> (kBlock)))
    {
    }

    /// Makes source frames lo..hi (inclusive) available; frames outside the source are 0.
    bool ensure (std::int64_t lo, std::int64_t hi)
    {
        if (len_ == 0)
            start_ = lo;
        if (hi < start_ + len_)
            return true;
        const std::int64_t drop = std::clamp<std::int64_t> (lo - start_, 0, len_);
        for (auto& ch : data_)
            std::memmove (ch.data(), ch.data() + drop,
                          static_cast<std::size_t> (len_ - drop) * sizeof (float));
        start_ += drop;
        len_ -= drop;
        while (hi >= start_ + len_)
        {
            const std::int64_t from = start_ + len_;
            const int n = static_cast<int> (std::min<std::int64_t> (capacity_ - len_, kBlock));
            if (n <= 0)
                return false;
            for (auto& ch : read_)
                std::fill (ch.begin(), ch.begin() + n, 0.0f);
            const std::int64_t a = std::clamp<std::int64_t> (from, 0, source_.length());
            const std::int64_t b = std::clamp<std::int64_t> (from + n, 0, source_.length());
            if (b > a)
            {
                std::array<float*, 8> ptrs {};
                for (int c = 0; c < channels_ && c < 8; ++c)
                    ptrs[static_cast<std::size_t> (c)] =
                        read_[static_cast<std::size_t> (c)].data() + (a - from);
                if (channels_ > 8 || !source_.read (ptrs.data(), a, static_cast<int> (b - a)))
                    return false;
            }
            for (int c = 0; c < channels_; ++c)
                std::memcpy (data_[static_cast<std::size_t> (c)].data() + len_,
                             read_[static_cast<std::size_t> (c)].data(),
                             static_cast<std::size_t> (n) * sizeof (float));
            len_ += n;
        }
        return true;
    }

    const float* at (int channel, std::int64_t frame) const
    {
        return data_[static_cast<std::size_t> (channel)].data() + (frame - start_);
    }
    int channels() const { return channels_; }

private:
    static constexpr int kBlock = 1 << 16;
    FrameSource& source_;
    int channels_;
    std::int64_t capacity_;
    std::int64_t start_ = 0, len_ = 0;
    std::vector<std::vector<float>> data_, read_;
};
} // namespace

int RateConverter::taps (double fromRate, double toRate)
{
    const double c = cutoff (wholeHz (fromRate), wholeHz (toRate));
    return 2 * static_cast<int> (std::ceil (kZeroCrossings / c));
}

void RateConverter::weights (double fromRate, double toRate, double frac, std::vector<double>& out)
{
    const double c = cutoff (wholeHz (fromRate), wholeHz (toRate));
    const int t = taps (fromRate, toRate);
    out.assign (static_cast<std::size_t> (t), 0.0);
    double sum = 0.0;
    for (int m = 0; m < t; ++m)
    {
        const double x = static_cast<double> (m - t / 2 + 1) - frac; // source frame - time
        const double w = c * prototype (c * x);
        out[static_cast<std::size_t> (m)] = w;
        sum += w;
    }
    // Exactly unity at DC for every phase (the window leaves it a few parts per million off).
    for (auto& w : out)
        w /= sum;
}

std::int64_t RateConverter::outputLength (std::int64_t inputLength, double fromRate, double toRate)
{
    const std::int64_t from = wholeHz (fromRate), to = wholeHz (toRate);
    // Frames k with k * from / to < inputLength.
    return (inputLength * to + from - 1) / from;
}

bool RateConverter::convert (FrameSource& source, double toRate, const Write& write,
                             const Progress& progress)
{
    const std::int64_t from = wholeHz (source.sampleRate()), to = wholeHz (toRate);
    const std::int64_t length = outputLength (source.length(), source.sampleRate(), toRate);
    const int channels = std::max (1, source.channels());
    const int t = taps (source.sampleRate(), toRate);
    const int half = t / 2;

    if (from == to)
    {
        // Nothing to convert: the frames pass unchanged.
        constexpr int kBlock = 4096;
        std::vector<std::vector<float>> block (static_cast<std::size_t> (channels),
                                               std::vector<float> (kBlock));
        std::vector<float*> p (static_cast<std::size_t> (channels));
        for (int c = 0; c < channels; ++c)
            p[static_cast<std::size_t> (c)] = block[static_cast<std::size_t> (c)].data();
        for (std::int64_t k0 = 0; k0 < length; k0 += kBlock)
        {
            const int n = static_cast<int> (std::min<std::int64_t> (kBlock, length - k0));
            if (!source.read (p.data(), k0, n) || !write (p.data(), n))
                return false;
            if (progress && !progress (static_cast<double> (k0 + n) / static_cast<double> (length)))
                return false;
        }
        return true;
    }

    // Output k's phase repeats every to / gcd frames: precompute those weights when few.
    const std::int64_t period = to / std::gcd (from, to);
    const bool cached = period <= 8192;
    std::vector<std::vector<double>> table;
    if (cached)
    {
        table.resize (static_cast<std::size_t> (period));
        for (std::int64_t p = 0; p < period; ++p)
            weights (source.sampleRate(), toRate,
                     static_cast<double> ((p * from) % to) / static_cast<double> (to),
                     table[static_cast<std::size_t> (p)]);
    }
    std::vector<double> scratch;

    Window window (source, half + 1);
    constexpr int kOut = 4096;
    std::vector<std::vector<float>> out (static_cast<std::size_t> (channels),
                                         std::vector<float> (kOut));
    std::vector<float*> ptrs (static_cast<std::size_t> (channels));
    for (int c = 0; c < channels; ++c)
        ptrs[static_cast<std::size_t> (c)] = out[static_cast<std::size_t> (c)].data();

    for (std::int64_t k0 = 0; k0 < length; k0 += kOut)
    {
        const int n = static_cast<int> (std::min<std::int64_t> (kOut, length - k0));
        for (int i = 0; i < n; ++i)
        {
            const std::int64_t k = k0 + i;
            const std::int64_t i0 = (k * from) / to;
            const std::vector<double>* w = nullptr;
            if (cached)
            {
                w = &table[static_cast<std::size_t> (k % period)];
            }
            else
            {
                weights (source.sampleRate(), toRate,
                         static_cast<double> ((k * from) % to) / static_cast<double> (to), scratch);
                w = &scratch;
            }
            const std::int64_t lo = i0 - half + 1;
            if (!window.ensure (lo, i0 + half))
                return false;
            for (int c = 0; c < channels; ++c)
            {
                const float* x = window.at (c, lo);
                double acc = 0.0;
                for (int m = 0; m < t; ++m)
                    acc += (*w)[static_cast<std::size_t> (m)] * static_cast<double> (x[m]);
                out[static_cast<std::size_t> (c)][static_cast<std::size_t> (i)] =
                    static_cast<float> (acc);
            }
        }
        if (!write (ptrs.data(), n))
            return false;
        if (progress && !progress (static_cast<double> (k0 + n) / static_cast<double> (length)))
            return false;
    }
    return true;
}

} // namespace sedef::record
