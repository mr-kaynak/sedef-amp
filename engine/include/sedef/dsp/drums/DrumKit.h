// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Random.h"
#include "sedef/dsp/drums/Grooves.h"

#include <array>
#include <cstdint>

namespace sedef::dsp::drums
{

/// A first-order low- or high-pass from the bilinear transform with a pre-warped cut-off, so it
/// has the same response at every sample rate up to near Nyquist. Integer: Q30 coefficients, Q23
/// signal (DrumKit's internals).
struct KitFilter
{
    std::int64_t b0 = 0, a1 = 0; ///< Q30
    std::int64_t x1 = 0, y1 = 0; ///< Q23
    bool high = false;
    std::int64_t process (std::int64_t x) noexcept
    {
        // Rounded toward zero, so with no input the state falls to zero instead of resting a few
        // LSB off it (rounding down keeps a negative state stuck).
        const std::int64_t y =
            ((high ? x - x1 : x + x1) * b0 - a1 * y1) / (std::int64_t { 1 } << 30);
        x1 = x;
        y1 = y;
        return y;
    }
};

/// The drum kit, synthesised in integer arithmetic so a render is the same bits on every platform
/// (docs/drums.md, section 5). One modal bank per instrument; strikes add to what rings.
///
/// Signals are Q23 (1.0 = 2^23) in int32, coefficients Q30. prepare() is the only place that uses
/// floating point.
class DrumKit
{
public:
    static constexpr int kMaxModes = 8;
    static constexpr int kNumBanks = 8; ///< the hats are one bank, struck open or closed

    void prepare (double sampleRate);
    /// Silence, and the noise generators back to their seeds.
    void reset() noexcept;
    /// The noise generators back to their seeds (a transport start), leaving what rings.
    void reseed() noexcept;

    /// A stroke on the next rendered sample. velocity 1..127.
    void strike (Instrument instrument, int velocity) noexcept;

    /// Renders `numFrames` samples (Q23), overwriting `left` and `right`.
    void render (std::int32_t* left, std::int32_t* right, int numFrames) noexcept;

    bool sounding() const noexcept;

    /// The largest |sample| the kit can produce on either channel, in Q23 (docs/drums.md, 5.6).
    std::int64_t peakBound() const noexcept { return peakBound_; }

    /// Read-outs for the tests.
    struct BankInfo
    {
        int numModes = 0;
        std::array<double, kMaxModes> hz {};
        double fundamentalAmplitude = 0.0; ///< |z| of the first mode, Q23 units
        double fundamentalPhase = 0.0;     ///< arg z of the first mode, radians
        double glide = 0.0;                ///< current pitch raise of the modes (0.1 = +10 %)
        double wireDrive = 0.0;            ///< snare: how far the head is over the wires' threshold
        bool active = false;
    };
    BankInfo bankInfo (Instrument instrument) const noexcept;

private:
    /// A decaying amplitude: Q38 so slow decays stay exponential far below the output's LSB
    /// (docs/drums.md, section 5.1); the decay factor is Q24.
    struct Envelope
    {
        std::int64_t value = 0; ///< Q38
        std::int64_t decay = 0; ///< Q24
        void step() noexcept { value = (value * decay) >> 24; }
        std::int64_t q23() const noexcept { return value >> 15; }
    };

    struct Mode
    {
        std::int64_t x = 0, y = 0;         ///< Q35 phasor
        std::int64_t c = 0, s = 0;         ///< Q26 r·cos ω, r·sin ω (the damping in use)
        std::int64_t cOpen = 0, sOpen = 0; ///< hats: open and closed damping
        std::int64_t cClosed = 0, sClosed = 0;
        std::int64_t omega = 0;  ///< Q30 radians per sample
        std::int64_t capMag = 0; ///< Q23: |z| may not exceed this after a strike
        double hz = 0.0;
    };

    enum class NoiseKind : std::uint8_t
    {
        none,
        wires, ///< driven by the head's fundamental above a threshold
        wash,  ///< its own envelope, adding power at each strike
    };

    struct Bank
    {
        int numModes = 0;
        std::array<Mode, kMaxModes> modes {};
        std::array<std::array<std::int32_t, kMaxModes>, 128> strikeAmp {}; ///< Q23 per velocity

        // The fundamental's amplitude, tracked as it decays (glide, wires).
        Envelope e1;
        std::int64_t r1Open = 0, r1Closed = 0; ///< Q24
        std::int64_t glideK = 0, glideMax = 0; ///< glide = min(max, K·e1²), Q30

        NoiseKind noise = NoiseKind::none;
        Envelope wash;
        std::int64_t washOut = 0;                  ///< Q23, the bloom's follower
        std::int64_t washOpen = 0, washClosed = 0; ///< Q24
        std::int64_t washRise = 0;                 ///< Q30; 0 = no bloom
        std::int64_t washCap = 0;                  ///< Q23
        std::array<std::int32_t, 128> washAmp {};  ///< Q23 per velocity
        std::int64_t wireThreshold = 0;            ///< Q23
        std::int64_t wireGain = 0;                 ///< Q30
        int highPoles = 0;                         ///< one-pole high-passes
        bool lowPass = false;
        KitFilter noiseHp1, noiseHp2, noiseLp;

        Envelope click;
        std::int64_t clickCap = 0;                                ///< Q23
        std::array<std::int32_t, 128> clickAmp {};                ///< Q23 per velocity
        std::array<std::int32_t, 128> clickLpB0 {}, clickLpA1 {}; ///< Q30 per velocity
        KitFilter clickLp, clickHp;

        std::int64_t panL = 0, panR = 0; ///< Q30
        std::uint32_t seed = 1;
        Random rng { 1u };
        bool active = false;
        bool closed = false;    ///< hats: the damping in use
        std::int64_t bound = 0; ///< Q23: the largest |output| (docs/drums.md, 5.6)

        std::int64_t next() noexcept;
        void setClosed (bool shut) noexcept;
        bool quiet() const noexcept;
        void silence() noexcept;
    };

    static int bankOf (Instrument instrument) noexcept;

    std::array<Bank, kNumBanks> banks_ {};
    std::int64_t peakBound_ = 0;
    std::uint32_t clock_ = 0; ///< samples rendered while sounding (the stop test's grid)
};

} // namespace sedef::dsp::drums
