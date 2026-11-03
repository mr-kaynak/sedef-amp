// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Math.h"
#include "sedef/core/Random.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace sedef::test
{

/// Owns planar audio and hands out AudioViews.
///
/// `ptrs` always points at this buffer's own channels: it is re-read from `data` on every access,
/// so assigning a new vector to a channel (`b[0] = makeSignal()`) or copying the buffer
/// (`Buffer copy = b;`) can never leave a view pointing at freed or foreign memory.
struct Buffer
{
    Buffer (int channels, int frames) : data (static_cast<size_t> (channels))
    {
        for (auto& ch : data)
            ch.assign (static_cast<size_t> (frames), 0.0f);
    }
    Buffer (const Buffer& other) : data (other.data) {}
    Buffer (Buffer&& other) noexcept : data (std::move (other.data)) {}
    Buffer& operator= (const Buffer& other)
    {
        data = other.data;
        return *this;
    }
    Buffer& operator= (Buffer&& other) noexcept
    {
        data = std::move (other.data);
        return *this;
    }

    AudioView view() { return AudioView (ptrs.data(), channels(), frames()); }
    int channels() const { return static_cast<int> (data.size()); }
    int frames() const { return static_cast<int> (data[0].size()); }
    std::vector<float>& operator[] (int c) { return data[static_cast<size_t> (c)]; }

    /// Channel pointers of the buffer that owns it, refreshed on every access.
    class Pointers
    {
    public:
        explicit Pointers (Buffer& owner) : owner_ (&owner) {}
        Pointers (const Pointers&) = delete;
        Pointers& operator= (const Pointers&) = delete;

        float* operator[] (size_t c) { return refresh()[c]; }
        float** data() { return refresh().data(); }

    private:
        std::array<float*, kMaxChannels>& refresh()
        {
            cache_.fill (nullptr);
            for (size_t c = 0; c < owner_->data.size() && c < cache_.size(); ++c)
                cache_[c] = owner_->data[c].data();
            return cache_;
        }

        Buffer* owner_;
        std::array<float*, kMaxChannels> cache_ {};
    };

    std::vector<std::vector<float>> data;
    Pointers ptrs { *this };
};

inline void fillSine (std::vector<float>& v, double hz, double fs, float amp = 1.0f,
                      double phase = 0.0)
{
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = amp *
               static_cast<float> (std::sin (kTwoPiD * hz * static_cast<double> (i) / fs + phase));
}

inline void fillNoise (std::vector<float>& v, float amp = 1.0f, std::uint32_t seed = 1)
{
    Random r (seed);
    for (auto& s : v)
        s = amp * r.bipolar();
}

inline float rms (const float* d, int n)
{
    double acc = 0.0;
    for (int i = 0; i < n; ++i)
        acc += static_cast<double> (d[i]) * d[i];
    return n > 0 ? static_cast<float> (std::sqrt (acc / n)) : 0.0f;
}

inline float rms (const std::vector<float>& v, size_t start = 0)
{
    return rms (v.data() + start, static_cast<int> (v.size() - start));
}

inline float peak (const std::vector<float>& v)
{
    float p = 0.0f;
    for (float s : v)
        p = std::max (p, std::abs (s));
    return p;
}

inline bool allFinite (const std::vector<float>& v)
{
    return std::all_of (v.begin(), v.end(), [] (float s) { return std::isfinite (s); });
}

/// Magnitude of a single frequency component (Goertzel), normalised so a full-scale sine reads 1.
inline double toneMagnitude (const float* d, int n, double hz, double fs)
{
    const double w = kTwoPiD * hz / fs;
    const double coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i)
    {
        // Hann window to limit leakage.
        const double win = 0.5 - 0.5 * std::cos (kTwoPiD * i / (n - 1));
        const double s0 = d[i] * win + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt (std::max (power, 0.0)) / (0.5 * n);
}

/// Steady-state gain (dB) of a mono process at one frequency.
template <typename ProcessFn>
double measureGainDb (ProcessFn&& process, double hz, double fs, int frames = 16384,
                      float amp = 0.1f)
{
    Buffer b (1, frames);
    fillSine (b[0], hz, fs, amp);
    process (b.view());
    const int skip = frames / 2;
    const double out = toneMagnitude (b[0].data() + skip, frames - skip, hz, fs);
    return 20.0 * std::log10 (std::max (out, 1e-12) / amp);
}

} // namespace sedef::test
