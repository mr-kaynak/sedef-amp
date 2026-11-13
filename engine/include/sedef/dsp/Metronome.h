// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/Random.h"
#include "sedef/core/Smoothed.h"
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

    /// Counting, or about to start at the next process() (after setSettings).
    bool isRunning() const noexcept { return settings_.enabled && (running_ || startPending_); }
    /// Samples per bar at the current settings (steps x interval).
    double barSamples() const noexcept;
    /// The samples, in the next `frames` that process() will be given, on which step 0 (a
    /// downbeat) will sound: the same scheduler run forward on a copy of the state, so the result
    /// is exactly what process() does. Call after setSettings(). Returns how many it wrote.
    int downbeats (int frames, int* offsets, int maxOffsets) const noexcept;
    /// A loop wraps on this sample of the next process() (docs/looper.md, "Lock"): a bar starts
    /// there, its step 0 sounds there and the grid continues from it. A step that sounded less
    /// than half a step before counts as that downbeat, so the click never doubles.
    void alignDownbeat (int offset) noexcept;

private:
    static constexpr int kMaxVoices = 4;
    static constexpr int kMaxPartials = 3;

    /// One sounding click: up to three decaying partials (optionally pitch-bent, optionally
    /// square-ish, optionally through a body band-pass) plus a filtered noise burst.
    struct Voice
    {
        bool active = false;
        int age = 0;
        int numPartials = 0;
        float gain = 0.0f;
        float attack = 0.0f, attackStep = 1.0f;
        std::array<float, kMaxPartials> phase {}, increment {}, bend {}, bendCoeff {};
        std::array<float, kMaxPartials> level {}, fast {}, fastCoeff {}, slow {}, slowCoeff {};
        std::array<float, kMaxPartials> slowMix {};
        std::array<bool, kMaxPartials> square {};
        bool hasBody = false;
        Biquad body;
        float noiseGain = 0.0f, noiseEnv = 0.0f, noiseCoeff = 0.0f;
        int noiseAttack = 0;
        Biquad noise;
        int settleAge = 0; ///< no early stop before every attack has finished
        int length = 0;    ///< a short fade-out starts here at the latest
        float fade = 1.0f, fadeStep = 1.0f;

        float next (Random& rng) noexcept;
    };

    double samplesPerStep() const noexcept;
    long long nextTrigger() const noexcept;
    void trigger (long long when, long long scheduled) noexcept;
    void startVoice (StepState state) noexcept;
    void render (const AudioView& io, int start, int end) noexcept;

    double sampleRate_ = 48000.0;
    Settings settings_ {};
    std::array<Voice, kMaxVoices> voices_ {};
    Random rng_ { 0xC11C4u };
    LinearSmoothed volume_;
    bool snapNext_ = true;

    // Scheduling is anchored to an absolute sample count: step k after the anchor falls on
    // ceil(anchor + k * samplesPerStep), so rounding never accumulates.
    bool startPending_ = false;
    bool running_ = false;
    long long now_ = 0;
    long long anchor_ = 0;
    long long stepsSinceAnchor_ = 0;
    long long lastTrigger_ = 0;
    long long startedAt_ = 0; ///< where the current run started (a trigger before it is stale)
    double interval_ = 0.0;
    int nextStep_ = 0;
    std::atomic<int> currentStep_ { -1 };
};

} // namespace sedef::dsp
