// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>

namespace sedef
{

/// Small, fast, real-time safe PRNG (xorshift32).
class Random
{
public:
    explicit Random (std::uint32_t seed = 0x9E3779B9u) noexcept { this->seed (seed); }

    void seed (std::uint32_t s) noexcept { state_ = s != 0 ? s : 0x9E3779B9u; }

    std::uint32_t nextU32() noexcept
    {
        std::uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }

    /// Uniform in [0, 1).
    float unipolar() noexcept { return static_cast<float> (nextU32() >> 8) * (1.0f / 16777216.0f); }

    /// Uniform in [-1, 1).
    float bipolar() noexcept { return 2.0f * unipolar() - 1.0f; }

private:
    std::uint32_t state_ = 0x9E3779B9u;
};

} // namespace sedef
