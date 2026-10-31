// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/Math.h"
#include "sedef/core/Smoothed.h"

#include <array>

namespace sedef::dsp
{

/// Nine-band graphic EQ with 12 dB/oct low and high cuts. Bands are RBJ peaking filters with a
/// constant Q of 1.41 (one octave); cuts are 2nd-order Butterworth and are bypassed at their
/// extremes (low cut <= 20 Hz, high cut >= 20 kHz). The UI draws the same curve in TypeScript, so
/// keep these definitions in sync with ui/src/features/eq/response.ts.
class GraphicEq
{
public:
    static constexpr std::array<float, 9> kBandHz { 65.0f,   125.0f,  250.0f,  500.0f,  1000.0f,
                                                    2000.0f, 4000.0f, 8000.0f, 16000.0f };

    struct Settings
    {
        std::array<float, 9> bandsDb {}; ///< -12..+12 dB
        float lowCutHz = 20.0f;          ///< 20..500 Hz (20 = off)
        float highCutHz = 20000.0f;      ///< 1k..20k Hz (20k = off)
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;

    static constexpr float kBandQ = 1.41f;

    /// Magnitude response in dB for the given settings. Thread-safe (pure function).
    static float responseDb (const Settings& s, double sampleRate, float hz) noexcept;

private:
    static constexpr int kNumBands = 9;

    /// One TDF-II section (designed with design::*) with its own state, run block-wise with the
    /// state held in registers and both channels as independent chains: about 3x faster than
    /// per-sample Biquad calls for an 11-section cascade.
    ///
    /// While a parameter glides, coefficients move linearly sample by sample across each control
    /// block. Stepping a direct-form section every 32 samples leaves small output steps (worst near
    /// Nyquist); the interpolation stays stable because the biquad stability region is convex.
    struct Section
    {
        BiquadCoeffs coeffs, from, to;
        std::array<std::array<float, 2>, kMaxChannels> state {};
        bool ramping = false;
        int settleLeft = 0; ///< samples a band at 0 dB keeps running while its state rings out

        void reset() noexcept
        {
            state = {};
            settleLeft = 0;
        }
        void snap (const BiquadCoeffs& c) noexcept
        {
            coeffs = c;
            ramping = false;
        }
        void retarget (const BiquadCoeffs& c) noexcept
        {
            from = coeffs;
            to = c;
            ramping = true;
        }
        /// Filters [start, start + length) of every channel in place. With fade (one control
        /// block at most), blends from the dry input (0) to the filtered signal (1) per sample.
        void run (const AudioView& io, int start, int length, const float* fade) noexcept;
    };

    /// A cut fades against the dry signal when it enters or leaves its bypass extreme: switching
    /// even a 20 Hz high-pass in or out abruptly shifts low-frequency phase and thumps.
    struct Cut
    {
        Section filter;
        LinearSmoothed log2Hz; ///< glides in log-frequency so sweeps sound even
        LinearSmoothed mix;    ///< 0 = bypassed, 1 = fully filtered
        bool wantOn = false;
        bool running = false;
    };

    void applyTarget (bool snap) noexcept;
    void updateCut (Cut& cut, bool isLowCut, int advance) noexcept;
    void updateBands (int advance) noexcept;
    bool isSmoothing() const noexcept;
    void processBlock (const AudioView& io, int start, int length) noexcept;

    double sampleRate_ = 48000.0;
    Settings target_ {};
    bool snapNext_ = true;

    std::array<Section, kNumBands> bands_;
    std::array<LinearSmoothed, kNumBands> bandDb_;
    std::array<int, kNumBands> activeBands_ {};
    int numActiveBands_ = 0;
    bool bandsSettling_ = false;
    std::array<int, kNumBands> settleSamples_ {}; ///< per band: 120 dB of decay at 0 dB
    Cut lowCut_, highCut_;
    // Per-sample fade gains for one control block.
    std::array<float, kControlBlock> lowMix_ {}, highMix_ {};
};

} // namespace sedef::dsp
