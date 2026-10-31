// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Transport.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace sedef::dsp
{

/// Synthesised click track (no samples) with odd meters and per-step accents.
class Metronome
{
public:
    static constexpr int kMaxSteps = 13;

    enum StepState : std::uint8_t
    {
        Off = 0,
        Normal = 1,
        Accent = 2,
        Soft = 3
    };

    struct Settings
    {
        bool enabled = false;
        float volumeDb = 0.0f;
        int signature = 2; ///< 2/4, 3/4, 4/4, 5/4, 6/8, 7/8, 9/8, 12/8
        int sound = 0;     ///< Blip, Wood, Clave, Cowbell, Rim, Shaker
        float bpm = 120.0f;
        std::array<std::uint8_t, kMaxSteps> steps {}; ///< StepState per step; step 0 = downbeat
    };

    /// Steps per bar for a signature index.
    static int stepsFor (int signature) noexcept;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// Adds the click into a stereo buffer.
    void process (const AudioView& io) noexcept;

    /// Index of the step that sounded last, or -1 when stopped (telemetry for the UI).
    int currentStep() const noexcept;
};

} // namespace sedef::dsp
