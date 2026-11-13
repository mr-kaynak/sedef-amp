// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/SpscRing.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

namespace sedef::record
{

/// A run of consecutive frames for one of the take's two files.
struct TakeChunk
{
    float* data = nullptr;     ///< planar: channel c at data + c * TakeRecorder::kChunkFrames
    std::int64_t position = 0; ///< the first frame's index in its file
    int frames = 0;
    int channels = 0;
    std::uint8_t stream = 0; ///< TakeRecorder::kDi or TakeRecorder::kOutput
};

/// The real-time half of the recorder (docs/recorder.md, section 3): the audio thread copies
/// the DI and the output into preallocated chunks and publishes them; the writer thread
/// (`drain()`) writes them and hands them back. The audio thread never waits: without a free
/// chunk it drops samples and counts them, and the writer turns the hole into silence.
///
///   start(), drain(), release()      the owner (the writer thread), one thread
///   requestStop()                    any thread
///   finishWithoutAudio()             any non-real-time thread while the audio callback is
///                                    known not to run (prepare, release, shutdown)
///   captureInput(), captureOutput()  audio thread, once each per block, in that order
class TakeRecorder
{
public:
    static constexpr int kChunkFrames = 2048;
    static constexpr int kMaxChannels = 2;
    /// The disk stall the FIFO absorbs (docs/recorder.md, "FIFO size").
    static constexpr double kStallSeconds = 4.0;
    static constexpr std::uint8_t kDi = 0;
    static constexpr std::uint8_t kOutput = 1;

    enum class Phase : int
    {
        idle,  ///< no take; the audio thread touches nothing
        armed, ///< memory ready; the take starts at the next block
        recording,
        stopRequested, ///< the DI ends at the next block
        stopping,      ///< the output catches up with the DI
        finished       ///< every chunk is published; the writer completes the files
    };

    enum class EndReason : int
    {
        none,
        stopped,      ///< the player pressed Stop
        sizeLimit,    ///< a file reached the WAV size limit
        audioStopped, ///< the audio device stopped or restarted
    };

    struct Config
    {
        double sampleRate = 48000.0;
        int diChannels = 1;
        int outChannels = 2;
        double stallSeconds = kStallSeconds;
        /// The take stops before either file passes this many frames.
        std::int64_t maxFrames = std::numeric_limits<std::int64_t>::max();
    };

    /// Chunks per stream for a rate and stall: enough for the stall, plus one being filled and
    /// one being written.
    static int chunksPerStream (double sampleRate, double stallSeconds) noexcept;

    /// Owner. Reserves the memory and arms a take. False unless idle.
    bool start (const Config& config);
    /// Any thread. Ends an armed or recording take (the output still catches up).
    void requestStop() noexcept;
    /// While the audio callback is known not to run: ends the take here, publishing what the
    /// audio thread had not yet published.
    void finishWithoutAudio() noexcept;

    /// Owner. Calls write (const TakeChunk&) for each published chunk, oldest first, then hands
    /// it back to the audio thread. Returns how many.
    template <typename Write>
    int drain (Write&& write)
    {
        int count = 0;
        int index = 0;
        while (filled_.pop (&index, 1) == 1)
        {
            write (static_cast<const TakeChunk&> (chunks_[static_cast<std::size_t> (index)]));
            free_.push (&index, 1);
            ++count;
        }
        return count;
    }

    /// Owner, once finished and drained: frees the memory and goes back to idle.
    void release();

    Phase phase() const noexcept { return phase_.load (std::memory_order_acquire); }
    const Config& config() const noexcept { return config_; }

    // Audio thread ---------------------------------------------------------------------------
    void captureInput (const float* const* inputs, int channels, int n) noexcept;
    /// `latency`: the rig's latency for this block's output (Engine::latencySamples()).
    void captureOutput (const float* const* outputs, int channels, int n, int latency) noexcept;

    // Any thread -----------------------------------------------------------------------------
    /// Frames of DI so far (the take's length once finished).
    std::int64_t frames() const noexcept { return frames_.load (std::memory_order_acquire); }
    std::int64_t droppedBlocks() const noexcept { return droppedBlocks_.load(); }
    std::int64_t droppedFrames (std::uint8_t stream) const noexcept
    {
        return droppedFrames_[stream].load();
    }
    /// The rig's latency when the take started, and whether it changed during the take.
    int startLatency() const noexcept { return startLatency_.load(); }
    bool latencyChanged() const noexcept { return latencyChanged_.load(); }
    EndReason endReason() const noexcept { return endReason_.load(); }
    /// The highest |sample| of each file since the last call (linear).
    float takePeak (std::uint8_t stream) noexcept { return peaks_[stream].exchange (0.0f); }
    std::size_t numChunks() const noexcept { return chunks_.size(); }

private:
    void store (std::uint8_t stream, std::int64_t position, const float* const* src, int channels,
                int offset, int count) noexcept;
    void publish (std::uint8_t stream) noexcept;
    void finish (EndReason reason) noexcept;
    static void raise (std::atomic<float>& peak, float value) noexcept;

    Config config_;
    std::vector<float> storage_;
    std::vector<TakeChunk> chunks_;
    SpscRing<int> free_ { 1 };   // writer -> audio
    SpscRing<int> filled_ { 1 }; // audio -> writer

    std::atomic<Phase> phase_ { Phase::idle };

    // Audio thread (or finishWithoutAudio while it does not run).
    std::array<int, 2> current_ { -1, -1 }; // chunk being filled per stream, -1 none
    std::int64_t time_ = 0;                 // stream samples since the take started
    std::int64_t diEnd_ = std::numeric_limits<std::int64_t>::max();
    std::int64_t outCovered_ = 0; // one past the highest output frame stored or dropped
    std::int64_t stopTime_ = 0;   // time_ when stopping began
    int lastLatency_ = -1;
    bool blockDropped_ = false;
    bool blockActive_ = false; // captureInput saw a take in this block
    EndReason pendingReason_ = EndReason::none;

    std::atomic<std::int64_t> frames_ { 0 };
    std::atomic<std::int64_t> droppedBlocks_ { 0 };
    std::array<std::atomic<std::int64_t>, 2> droppedFrames_ {};
    std::atomic<int> startLatency_ { 0 };
    std::atomic<bool> latencyChanged_ { false };
    std::atomic<EndReason> endReason_ { EndReason::none };
    std::array<std::atomic<float>, 2> peaks_ {};
};

} // namespace sedef::record
