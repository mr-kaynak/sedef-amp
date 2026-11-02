// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/dsp/cab/ConvolutionKernel.h" // complete type for makeKernel's unique_ptr

#include <array>
#include <memory>

namespace sedef::dsp
{

/// A user impulse response, resampled to the session rate and partitioned for FFT convolution.
/// Built off the audio thread with CabSim::makeKernel().
class ConvolutionKernel;

/// Cabinet + microphone simulation. Three cabinets (1x12 open, 2x12 open, 4x12 closed), each
/// picked up by two microphones (A and B) and an optional room microphone per channel.
/// Microphones are procedural models (speaker + cabinet + mic filters) or a user IR.
class CabSim
{
public:
    static constexpr int kNumCabs = 3;
    static constexpr int kNumMics = 2;
    static constexpr int kNumMicTypes = 11;
    static constexpr int kCustomIrType = 10;

    struct MicSettings
    {
        bool enabled = true;
        int type = 0;          ///< 0..10, 10 = custom IR for this (mic, cab) slot
        float position = 0.5f; ///< 0 = cone centre (bright), 1 = cone edge (dark)
        /// Where around the cone, in degrees clockwise from 12 o'clock as you face the cab.
        /// The miked speaker is the left one of the 2x12 and the bottom-left one of the 4x12, so
        /// this decides how close the mic sits to the other speakers and to the floor.
        float azimuthDeg = 60.0f;
        float distance = 0.0f;   ///< 0 = on the grille, 1 = far (less proximity, more room)
        float offAxisDeg = 0.0f; ///< capsule tilt towards the dust cap, 0..60 degrees
        float levelDb = 0.0f;    ///< -40..+6
        float pan = 0.0f;        ///< -1 (left) .. +1 (right)
        bool invertPolarity = false;
        bool roomEnabled = false;
        float roomLevelDb = -6.0f;
    };

    struct Settings
    {
        int cab = 0; ///< 0..2
        /// Mic A at 2 o'clock and mic B at 4 o'clock, as in the parameter schema.
        std::array<MicSettings, kNumMics> mics { MicSettings {},
                                                 MicSettings { .azimuthDeg = 120.0f } };
    };

    CabSim();
    ~CabSim();
    CabSim (const CabSim&) = delete;
    CabSim& operator= (const CabSim&) = delete;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;

    /// Mono in, stereo out (mics panned). `out` must have 2 channels and in's frame count.
    void process (const float* in, const AudioView& out) noexcept;
    int latencySamples() const noexcept;

    /// Non-real-time: builds a kernel from raw IR samples (mono), resampling to targetRate.
    /// Returns null if the IR is empty. Long IRs are truncated to 1 second.
    static std::unique_ptr<ConvolutionKernel> makeKernel (const float* ir, int length,
                                                          double irSampleRate, double targetRate,
                                                          int maxBlockSize);

    /// Non-real-time: installs (or clears, with null) the custom IR for a (mic, cab) slot.
    void setCustomIr (int mic, int cab, std::unique_ptr<ConvolutionKernel> kernel);
    /// Non-real-time: frees kernels the audio thread has retired. Call periodically.
    void collectGarbage();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace sedef::dsp
