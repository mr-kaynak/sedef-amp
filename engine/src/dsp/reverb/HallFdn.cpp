// SPDX-License-Identifier: AGPL-3.0-or-later
#include "HallFdn.h"

#include <bit>

namespace sedef::dsp::reverb
{
namespace
{
// Line lengths spread roughly geometrically over 31-102 ms; rounded up to primes at the session
// rate.
constexpr std::array<double, HallFdn::kLines> kLineMs { 31.7, 37.3, 43.9, 51.1,
                                                        59.9, 70.1, 83.3, 101.9 };
// Slow random wander per line: incommensurate rates so the lines never move together.
constexpr std::array<float, HallFdn::kLines> kWanderHz { 0.31f, 0.43f, 0.37f, 0.53f,
                                                         0.29f, 0.47f, 0.41f, 0.59f };
constexpr double kModDepthMs = 0.18;

// Input diffusers per channel (different lengths left and right for decorrelation).
constexpr std::array<std::array<double, 4>, 2> kDiffuserMs { { { 3.11, 4.73, 7.61, 11.23 },
                                                               { 3.37, 5.17, 8.09, 12.31 } } };
constexpr std::array<float, 4> kDiffusion { 0.7f, 0.7f, 0.625f, 0.625f };

// Sparse early reflections: times in ms, signed gains falling with time.
constexpr std::array<double, HallFdn::kReflections> kReflectMsL { 4.3,  9.7,  13.1, 19.9, 26.3,
                                                                  33.7, 41.3, 52.9, 64.1, 77.3 };
constexpr std::array<double, HallFdn::kReflections> kReflectMsR { 5.9,  8.3,  15.7, 22.1, 28.9,
                                                                  37.1, 45.7, 56.3, 69.7, 81.1 };
constexpr std::array<float, HallFdn::kReflections> kReflectGainL { 0.84f,  -0.71f, 0.62f,  0.55f,
                                                                   -0.47f, 0.40f,  -0.33f, 0.27f,
                                                                   -0.21f, 0.16f };
constexpr std::array<float, HallFdn::kReflections> kReflectGainR { 0.80f, -0.74f, 0.60f, -0.52f,
                                                                   0.45f, -0.38f, 0.31f, -0.25f,
                                                                   0.19f, -0.14f };
constexpr float kEarlyGain = 0.22f;
constexpr float kLateGain = 0.47f;

// Sylvester-Hadamard entry H8[row][col] = (-1)^popcount(row & col).
constexpr float hadamardSign (int row, int col)
{
    return (std::popcount (static_cast<unsigned> (row & col)) & 1) != 0 ? -1.0f : 1.0f;
}

template <int Row>
constexpr std::array<float, HallFdn::kLines> hadamardRow (float scale)
{
    std::array<float, HallFdn::kLines> r {};
    for (int c = 0; c < HallFdn::kLines; ++c)
        r[static_cast<size_t> (c)] = scale * hadamardSign (Row, c);
    return r;
}

// Orthogonal input and output patterns: left and right enter and leave the network along
// different Hadamard rows, so a mono source comes out decorrelated.
constexpr auto kInjectL = hadamardRow<2> (0.5f);
constexpr auto kInjectR = hadamardRow<6> (0.5f);
constexpr auto kTapL = hadamardRow<3> (1.0f);
constexpr auto kTapR = hadamardRow<5> (1.0f);

/// In-place fast Walsh-Hadamard transform, scaled to be orthonormal (energy preserving).
inline void hadamard8 (float* v) noexcept
{
    for (int h = 1; h < 8; h <<= 1)
        for (int i = 0; i < 8; i += h << 1)
            for (int j = i; j < i + h; ++j)
            {
                const float a = v[j];
                const float b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
    constexpr float norm = 0.35355339f; // 1/sqrt(8), rounds below the exact value
    for (int i = 0; i < 8; ++i)
        v[i] *= norm;
}
} // namespace

void HallFdn::prepare (double sampleRate)
{
    fs_ = sampleRate;
    const double controlRate = fs_ / kControlBlock;
    modDepth_ = static_cast<float> (kModDepthMs * 0.001 * fs_);
    const int headroom = static_cast<int> (std::ceil (modDepth_)) + 4;

    int previous = 0;
    for (size_t i = 0; i < lines_.size(); ++i)
    {
        auto& line = lines_[i];
        // Strictly increasing primes are pairwise coprime.
        const int target = static_cast<int> (std::lround (kLineMs[i] * 0.001 * fs_));
        line.length = nextPrime (std::max (target, previous + 1));
        previous = line.length;
        line.delay.prepare (line.length + headroom);
        line.wander.prepare (controlRate, 0xA11u + static_cast<std::uint32_t> (i) * 7919u);
        line.wander.setRate (kWanderHz[i]);
    }

    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t k = 0; k < 4; ++k)
            diffusers_[ch][k].prepare (
                static_cast<int> (std::lround (kDiffuserMs[ch][k] * 0.001 * fs_)));

    for (size_t k = 0; k < kReflections; ++k)
    {
        reflectTapsL_[k] = static_cast<int> (std::lround (kReflectMsL[k] * 0.001 * fs_));
        reflectTapsR_[k] = static_cast<int> (std::lround (kReflectMsR[k] * 0.001 * fs_));
    }
    reflectL_.prepare (reflectTapsL_.back() + 1);
    reflectR_.prepare (reflectTapsR_.back() + 1);

    setDecay (2.5f, 5000.0f);
    clear();
}

void HallFdn::clear() noexcept
{
    for (auto& line : lines_)
    {
        line.delay.reset();
        line.state = 0.0f;
        line.mod = line.modStep = 0.0f;
    }
    for (auto& chain : diffusers_)
        for (auto& ap : chain)
            ap.clear();
    reflectL_.reset();
    reflectR_.reset();
}

void HallFdn::setDecay (float rt60Seconds, float dampingHz) noexcept
{
    const double rt60 = std::max (static_cast<double> (rt60Seconds), 0.05);
    const double k = std::tan (kPiD * clampFrequency (dampingHz, fs_) / fs_);

    for (auto& line : lines_)
    {
        // Jot & Chaigne: a line of length L gets gain 10^(-3 L / (fs T60(w))), so every mode of
        // the lossless prototype decays at the same rate. The shelf goes from gDc at low
        // frequencies to gHf above the damping corner.
        const double seconds = line.length / fs_;
        const double gDc = loopGainForRt60 (seconds, rt60);
        const double gHf = loopGainForRt60 (seconds, rt60 * kHighRatio);
        const double g = std::sqrt (gHf / gDc); // shelf H(0) = 1, H(nyquist) = g^2
        const double a0 = 1.0 / g + k;
        line.b0 = static_cast<float> (gDc * (g + k) / a0);
        line.b1 = static_cast<float> (gDc * (k - g) / a0);
        line.a1 = static_cast<float> ((k - 1.0 / g) / a0);
        line.dcGain = static_cast<float> (gDc);
    }
}

float HallFdn::maxLineGain() const noexcept
{
    float g = 0.0f;
    for (const auto& line : lines_)
        g = std::max (g, (line.b0 + line.b1) / (1.0f + line.a1));
    return g;
}

void HallFdn::controlTick() noexcept
{
    constexpr float inv = 1.0f / static_cast<float> (kControlBlock);
    for (auto& line : lines_)
    {
        const float target = modDepth_ * line.wander.next();
        line.modStep = (target - line.mod) * inv;
        line.state = flushToZero (line.state);
    }
}

void HallFdn::process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        reflectL_.push (inL[i]);
        reflectR_.push (inR[i]);
        float earlyL = 0.0f, earlyR = 0.0f;
        for (size_t k = 0; k < kReflections; ++k)
        {
            earlyL += kReflectGainL[k] * reflectL_.read (reflectTapsL_[k]);
            earlyR += kReflectGainR[k] * reflectR_.read (reflectTapsR_[k]);
        }

        float dl = inL[i];
        float dr = inR[i];
        for (size_t k = 0; k < 4; ++k)
        {
            dl = diffusers_[0][k].process (dl, kDiffusion[k]);
            dr = diffusers_[1][k].process (dr, kDiffusion[k]);
        }

        std::array<float, kLines> y;
        for (size_t j = 0; j < kLines; ++j)
        {
            auto& line = lines_[j];
            line.mod += line.modStep;
            const float x = line.delay.readCubic (static_cast<float> (line.length - 1) + line.mod);
            const float out = line.b0 * x + line.state;
            line.state = line.b1 * x - line.a1 * out;
            y[j] = out;
        }

        float lateL = 0.0f, lateR = 0.0f;
        for (size_t j = 0; j < kLines; ++j)
        {
            lateL += kTapL[j] * y[j];
            lateR += kTapR[j] * y[j];
        }

        hadamard8 (y.data());
        for (size_t j = 0; j < kLines; ++j)
            lines_[j].delay.push (flushToZero (y[j] + kInjectL[j] * dl + kInjectR[j] * dr));

        outL[i] = kEarlyGain * earlyL + kLateGain * lateL;
        outR[i] = kEarlyGain * earlyR + kLateGain * lateR;
    }
}

} // namespace sedef::dsp::reverb
