// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/board/Board.h"
#include "sedef/core/AudioView.h"
#include "sedef/core/RtMailbox.h"
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
struct LoopMemory;
} // namespace dsp

/// What the looper's main button and its other actions ask for (dsp::Looper::Command).
enum class LooperCommand : std::uint8_t
{
    main,
    stop,
    undo,
    redo,
    clear
};

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
        /// The same for the second Gate and the second Tide (docs/board.md).
        std::atomic<bool> gate2Open { false };
        std::atomic<float> tide2ReductionDb { 0.0f };
        std::atomic<int> metronomeStep { -1 };
        std::atomic<float> driftTimeMs { 0.0f };
        /// Per copy of a pedal (slotIndex: the first copies first): its module processed audio in
        /// the last block (on the board, on either side of the amp, and not bypassed).
        std::array<std::atomic<bool>, kNumSlots> pedalRan {};
        /// A board change is still on its way in (bypass fades or the duck).
        std::atomic<bool> boardChanging { false };
        /// The looper (docs/looper.md): dsp::Looper::State, its flags (kLooper*), the loop's length
        /// (or the take's so far) and position in samples, its bars and the downbeats still to
        /// wait for before an armed start.
        std::atomic<int> looperState { 0 };
        std::atomic<int> looperFlags { 0 };
        std::atomic<int> looperLength { 0 };
        std::atomic<int> looperPosition { 0 };
        std::atomic<int> looperBars { 0 };
        std::atomic<int> looperWait { 0 };
    };
    static constexpr int kLooperCanUndo = 1;
    static constexpr int kLooperCanRedo = 2;
    static constexpr int kLooperUndoIsClear = 4;
    static constexpr int kLooperSynced = 8;
    static constexpr int kLooperArmedForPlay = 16;

    Engine();
    ~Engine();
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    /// numInputs/numOutputs: 1 or 2. Allocates everything the audio thread will need.
    void prepare (double sampleRate, int maxBlockSize, int numInputs, int numOutputs);
    void reset();

    /// How the nonlinear stages oversample (docs/dsp/oversampling.md). Not real time: it takes
    /// effect at the next prepare(), which changes latencySamples().
    void setOversampling (OversamplingMode mode) noexcept { oversampling_.store (mode); }
    OversamplingMode oversampling() const noexcept { return oversampling_.load(); }

    /// Audio thread. Any frame count; long blocks are processed in prepared-size chunks.
    void process (const float* const* inputs, float* const* outputs, int numFrames,
                  const ParamSnapshot& params, const Transport& transport) noexcept;

    /// The low-frequency delay of the whole rig, whatever is switched on (oversampled stages are
    /// always latency-aligned). Constant for a given prepare(), except for Transpose's share,
    /// which follows its setting: 0 when off, more when shifting up than down
    /// (docs/dsp/transpose.md, section 5). Hosts are told on a change.
    /// These getters are safe from any thread.
    int latencySamples() const noexcept
    {
        return latency_.load (std::memory_order_acquire) +
               transposeLatency_.load (std::memory_order_acquire);
    }
    /// After prepare(), before the first block: the transpose setting (in semitones) that block
    /// will bring, so latencySamples() is right from the start. Message thread, audio stopped.
    void expectTranspose (float semitones) noexcept;
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

    /// The pedalboard: which pedals play, in what order, on which side of the amp (board/Board.h).
    /// Any non-real-time thread; callers serialise their calls. The newest board applies at the
    /// next block boundary: pedals without latency join and leave (or cross the amp) through
    /// their bypass fades, anything else (a new order on a side, a pedal with latency) under a
    /// short duck of that side. The latency never changes. Before the first prepare() or reset()
    /// the board is Board::legacy().
    void setBoard (const Board& board) noexcept;

    /// The metronome only sounds when allowed (standalone builds).
    void setMetronomeAllowed (bool allowed) noexcept { metronomeAllowed_.store (allowed); }
    void setMetronomePattern (const std::array<std::uint8_t, 13>& steps) noexcept;

    /// The looper (docs/looper.md). Its memory (about 92 MB) is reserved the first time this is
    /// called, on the calling (non-real-time) thread, and the audio thread picks it up at its
    /// next block; until then the looper does nothing. Returns true once it is reserved.
    bool prepareLooper();
    bool looperReady() const noexcept
    {
        return looperMemory_.load (std::memory_order_acquire) != nullptr;
    }
    /// Any one non-real-time thread: queued for the start of the next block. Dropped while the
    /// looper has no memory.
    void looperCommand (LooperCommand command) noexcept;
    /// The audio device's input + output latency in samples (the standalone app knows it); the
    /// looper adds the rig's own latency and writes what it records that much earlier.
    void setLooperDeviceLatency (int samples) noexcept { looperDeviceLatency_.store (samples); }

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
    RtMailbox<Board> board_;
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

    std::unique_ptr<dsp::LoopMemory> looperMemoryOwner_; // freed with the engine, never before
    std::atomic<dsp::LoopMemory*> looperMemory_ { nullptr };
    SpscRing<std::uint8_t> looperCommands_ { 64 };
    std::atomic<int> looperDeviceLatency_ { 0 };

    std::atomic<double> sampleRate_ { 48000.0 };
    std::atomic<int> maxBlockSize_ { 512 };
    std::atomic<int> numInputs_ { 1 };
    std::atomic<int> numOutputs_ { 2 };
    std::atomic<int> latency_ { 0 };          // everything but Transpose
    std::atomic<int> transposeLatency_ { 0 }; // updated by the audio thread every block
    std::atomic<OversamplingMode> oversampling_ { OversamplingMode::lowLatency };
};

} // namespace sedef
