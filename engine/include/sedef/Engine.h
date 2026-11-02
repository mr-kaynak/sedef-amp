// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/SpscRing.h"
#include "sedef/core/Transport.h"
#include "sedef/params/Parameters.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace sedef
{

namespace dsp
{
class CabSim;
class CaptureStage;
} // namespace dsp

/// The whole rig: routing, bypass crossfades, latency alignment, metering.
///
/// Threading: prepare()/reset() and the non-RT setters run on the message thread while audio is
/// stopped or between blocks as documented; process() runs on the audio thread and is RT-safe.
class Engine
{
public:
    /// Published by the audio thread, read by the UI timer.
    struct Telemetry
    {
        std::array<std::atomic<float>, 2> inputPeak {};  ///< linear, max since last read
        std::array<std::atomic<float>, 2> outputPeak {}; ///< linear, max since last read
        std::atomic<bool> inputClip { false };
        std::atomic<bool> outputClip { false };
        std::atomic<bool> gateOpen { false };
        std::atomic<float> gateReductionDb { 0.0f };
        std::atomic<float> tideReductionDb { 0.0f };
        std::atomic<int> metronomeStep { -1 };
        std::atomic<float> driftTimeMs { 0.0f };
    };

    Engine();
    ~Engine();
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    /// numInputs/numOutputs: 1 or 2. Allocates everything the audio thread will need.
    void prepare (double sampleRate, int maxBlockSize, int numInputs, int numOutputs);
    void reset();

    /// Audio thread. Any frame count; long blocks are processed in prepared-size chunks.
    void process (const float* const* inputs, float* const* outputs, int numFrames,
                  const ParamSnapshot& params, const Transport& transport) noexcept;

    /// Constant for a given prepare() (oversampled stages are always latency-aligned).
    /// These getters are safe from any thread.
    int latencySamples() const noexcept { return latency_.load (std::memory_order_acquire); }
    double sampleRate() const noexcept { return sampleRate_.load (std::memory_order_acquire); }
    int maxBlockSize() const noexcept { return maxBlockSize_.load (std::memory_order_acquire); }
    int numInputs() const noexcept { return numInputs_.load (std::memory_order_acquire); }
    int numOutputs() const noexcept { return numOutputs_.load (std::memory_order_acquire); }

    const Telemetry& telemetry() const noexcept { return telemetry_; }

    /// Raw instrument input (channel 0, or L+R in stereo modes) for pitch analysis. Written only
    /// while analysis is enabled. Consumer: the analysis thread.
    SpscRing<float>& analysisRing() noexcept { return analysisRing_; }
    void setAnalysisEnabled (bool enabled) noexcept { analysisEnabled_.store (enabled); }
    /// While analysis is enabled and the tunerMute parameter is on, the output is muted.

    /// The metronome only sounds when allowed (standalone builds).
    void setMetronomeAllowed (bool allowed) noexcept { metronomeAllowed_.store (allowed); }
    void setMetronomePattern (const std::array<std::uint8_t, 13>& steps) noexcept;

    /// Custom IRs must be installed on every chain's cab (dual mode has two).
    int numCabs() const noexcept;
    dsp::CabSim& cab (int index) noexcept;
    /// .nam captures likewise go to every chain's capture stage (numCabs() of them).
    dsp::CaptureStage& capture (int index) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    Telemetry telemetry_;
    SpscRing<float> analysisRing_ { 1 << 16 };
    std::atomic<bool> analysisEnabled_ { false };
    std::atomic<bool> metronomeAllowed_ { false };
    // 13 steps x 2 bits in one atomic word. Default: accented downbeat, every other step normal
    // (Metronome::StepState 2 and 1), the same as the plugin's default pattern.
    static constexpr std::uint32_t defaultMetronomePattern() noexcept
    {
        std::uint32_t packed = 2u;
        for (std::uint32_t i = 1; i < 13; ++i)
            packed |= 1u << (2 * i);
        return packed;
    }
    std::atomic<std::uint32_t> metronomePattern_ { defaultMetronomePattern() };

    std::atomic<double> sampleRate_ { 48000.0 };
    std::atomic<int> maxBlockSize_ { 512 };
    std::atomic<int> numInputs_ { 1 };
    std::atomic<int> numOutputs_ { 2 };
    std::atomic<int> latency_ { 0 };
};

} // namespace sedef
