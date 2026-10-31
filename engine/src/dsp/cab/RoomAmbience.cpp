// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/cab/RoomAmbience.h"

#include "sedef/core/Math.h"

#include <cmath>
#include <vector>

namespace sedef::dsp::cab
{
namespace
{
// Early reflections (ms, gain): walls, ceiling and floor of a ~4 x 5 m room, alternating sides.
struct TapSpec
{
    float ms;
    float gain;
    bool fromLeft;
};
constexpr TapSpec kTapsL[] = { { 4.3f, 0.62f, true },  { 6.8f, 0.50f, false },
                               { 9.7f, -0.45f, true }, { 13.1f, -0.38f, false },
                               { 16.9f, 0.34f, true }, { 21.7f, 0.27f, false } };
constexpr TapSpec kTapsR[] = { { 4.9f, 0.60f, false },   { 7.4f, 0.49f, true },
                               { 10.6f, -0.44f, false }, { 12.4f, -0.37f, true },
                               { 18.2f, 0.33f, false },  { 23.3f, 0.26f, true } };
constexpr float kEarlyMaxMs = 30.0f;
constexpr float kPreDelayMs = 9.0f;

// Mutually incommensurate lengths around 8-21 ms: dense enough for a small room without the
// metallic ring of a single short loop.
constexpr float kLineMs[] = { 7.9f, 9.3f, 11.1f, 12.7f, 14.6f, 16.4f, 18.9f, 21.3f };
constexpr float kDampHz = 4500.0f;

// Orthogonal output sign patterns keep left and right decorrelated.
constexpr float kOutL[] = { 1, -1, 1, -1, 1, -1, 1, -1 };
constexpr float kOutR[] = { 1, 1, -1, -1, 1, 1, -1, -1 };
constexpr float kInSign[] = { 1, 1, -1, 1, 1, -1, -1, 1 };

int msToSamples (float ms, double fs)
{
    return std::max (1, static_cast<int> (std::lround (ms * 0.001 * fs)));
}
} // namespace

void RoomAmbience::prepare (double fs)
{
    inputHp_.setCoeffs (design::highpass1 (fs, 90.0));
    inputLp_.setCoeffs (design::lowpass (fs, 4500.0, 0.6));

    const int earlyMax = msToSamples (kEarlyMaxMs, fs);
    earlyL_.prepare (earlyMax);
    earlyR_.prepare (earlyMax);
    for (int i = 0; i < kTaps; ++i)
    {
        const auto idx = static_cast<size_t> (i);
        tapsL_[idx] = { msToSamples (kTapsL[i].ms, fs), kTapsL[i].gain, kTapsL[i].fromLeft };
        tapsR_[idx] = { msToSamples (kTapsR[i].ms, fs), kTapsR[i].gain, kTapsR[i].fromLeft };
    }
    preDelay_ = msToSamples (kPreDelayMs, fs);

    for (int k = 0; k < kLines; ++k)
    {
        const auto idx = static_cast<size_t> (k);
        lengths_[idx] = msToSamples (kLineMs[k], fs);
        lines_[idx].prepare (lengths_[idx]);
        // -60 dB after kRt60 seconds: g = 10^(-3 L / (fs RT60)).
        feedback_[idx] = static_cast<float> (std::pow (10.0, -3.0 * lengths_[idx] / (fs * kRt60)));
    }
    dampCoeff_ = static_cast<float> (1.0 - std::exp (-kTwoPiD * kDampHz / fs));
    tailSamples_ = static_cast<int> (2.0 * kRt60 * fs) + earlyMax;

    // Normalise the early and late energies of the impulse response to -3 dB each, so a room
    // level of 0 dB carries about as much energy as the close mic it is fed from.
    earlyGain_ = lateGain_ = 1.0f;
    reset();
    const int len = tailSamples_;
    std::vector<float> inL (static_cast<size_t> (len), 0.0f), zeros (inL.size(), 0.0f);
    std::vector<float> early (inL.size(), 0.0f), late (inL.size(), 0.0f), dummy (inL.size(), 0.0f);
    inL[0] = 1.0f;
    earlyGain_ = 1.0f;
    lateGain_ = 0.0f;
    process (inL.data(), zeros.data(), early.data(), dummy.data(), len);
    reset();
    earlyGain_ = 0.0f;
    lateGain_ = 1.0f;
    process (inL.data(), zeros.data(), late.data(), dummy.data(), len);
    double eEarly = 0.0, eLate = 0.0;
    for (int i = 0; i < len; ++i)
    {
        eEarly +=
            static_cast<double> (early[static_cast<size_t> (i)]) * early[static_cast<size_t> (i)];
        eLate +=
            static_cast<double> (late[static_cast<size_t> (i)]) * late[static_cast<size_t> (i)];
    }
    earlyGain_ = static_cast<float> (std::sqrt (0.5 / std::max (eEarly, 1.0e-12)));
    lateGain_ = static_cast<float> (std::sqrt (0.5 / std::max (eLate, 1.0e-12)));
    reset();
}

void RoomAmbience::reset() noexcept
{
    inputHp_.reset();
    inputLp_.reset();
    earlyL_.reset();
    earlyR_.reset();
    for (auto& l : lines_)
        l.reset();
    damping_.fill (0.0f);
}

void RoomAmbience::process (const float* inL, const float* inR, float* outL, float* outR,
                            int n) noexcept
{
    constexpr float kMix = 0.35355339f; // 1 / sqrt (8): orthonormal Hadamard
    for (int i = 0; i < n; ++i)
    {
        const float xl = inputLp_.processSample (0, inputHp_.processSample (0, inL[i]));
        const float xr = inputLp_.processSample (1, inputHp_.processSample (1, inR[i]));
        earlyL_.push (xl);
        earlyR_.push (xr);

        float erL = 0.0f, erR = 0.0f;
        for (int t = 0; t < kTaps; ++t)
        {
            const Tap& a = tapsL_[static_cast<size_t> (t)];
            const Tap& b = tapsR_[static_cast<size_t> (t)];
            erL += a.gain * (a.fromLeft ? earlyL_ : earlyR_).read (a.delay);
            erR += b.gain * (b.fromLeft ? earlyL_ : earlyR_).read (b.delay);
        }

        std::array<float, kLines> o {}, v {};
        for (int k = 0; k < kLines; ++k)
        {
            const auto idx = static_cast<size_t> (k);
            o[idx] = lines_[idx].read (lengths_[idx] - 1);
            damping_[idx] += dampCoeff_ * (o[idx] - damping_[idx]);
            v[idx] = feedback_[idx] * damping_[idx];
        }

        // Fast Walsh-Hadamard transform (3 butterfly stages).
        for (int h = 1; h < kLines; h <<= 1)
            for (int j = 0; j < kLines; j += 2 * h)
                for (int k = j; k < j + h; ++k)
                {
                    const float a = v[static_cast<size_t> (k)];
                    const float b = v[static_cast<size_t> (k + h)];
                    v[static_cast<size_t> (k)] = a + b;
                    v[static_cast<size_t> (k + h)] = a - b;
                }

        const float injL = earlyL_.read (preDelay_);
        const float injR = earlyR_.read (preDelay_);
        float lateL = 0.0f, lateR = 0.0f;
        for (int k = 0; k < kLines; ++k)
        {
            const auto idx = static_cast<size_t> (k);
            const float inject = kInSign[k] * ((k & 1) == 0 ? injL : injR);
            lines_[idx].push (kMix * v[idx] + inject);
            lateL += kOutL[k] * o[idx];
            lateR += kOutR[k] * o[idx];
        }

        outL[i] += earlyGain_ * erL + lateGain_ * lateL;
        outR[i] += earlyGain_ * erR + lateGain_ * lateR;
    }

    for (auto& d : damping_)
        d = flushToZero (d);
}

} // namespace sedef::dsp::cab
