// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/record/FrameFifo.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace sedef::record
{

/// The real-time half of re-amp (docs/recorder.md, section 6): plays frames from a FIFO into the
/// rig's input, in place of the live input (Replace) or added to it (Mix).
///
/// The file is never faded when it starts, so the rig hears it exactly; Stop fades it out over
/// kFadeSeconds. The live input fades over the same time whenever Replace mutes or releases it.
/// While nothing plays and the live input is open, process() leaves the input untouched.
///
///   prepare()                          any non-real-time thread while the audio callback is
///                                      known not to run
///   requestFlush(), requestPlay(),     the reader thread (the FIFO's producer)
///   requestStop(), setEndAt()
///   setMix()                           any thread
///   process()                          audio thread, once per block, before the engine
class ReampPlayer
{
public:
    static constexpr double kFadeSeconds = 0.010;
    /// The disk stall the FIFO absorbs, like the recorder's.
    static constexpr double kFifoSeconds = 4.0;

    void prepare (double sampleRate, int channels, int maxBlock, double fifoSeconds = kFifoSeconds);
    double sampleRate() const noexcept { return sampleRate_; }
    int channels() const noexcept { return fifo_.channels(); }
    FrameFifo& fifo() noexcept { return fifo_; }

    /// Reader: asks the audio thread to stop and empty the FIFO. Returns the request's number;
    /// push nothing until flushed() says it is done.
    std::uint32_t requestFlush() noexcept;
    bool flushed (std::uint32_t request) const noexcept
    {
        return flushAck_.load (std::memory_order_acquire) == request;
    }
    /// Reader: starts playing what the FIFO holds, from its first frame, at the next block
    /// (after flush `request`).
    void requestPlay (std::uint32_t request) noexcept
    {
        playRequest_.store (request, std::memory_order_release);
    }
    /// Fades out and stops what the last requestPlay() started, or keeps it from starting.
    void requestStop() noexcept
    {
        stopRequest_.store (playRequest_.load (std::memory_order_acquire),
                            std::memory_order_release);
    }
    /// The stream frame where playing ends by itself; -1 for none (looping).
    void setEndAt (std::int64_t frame) noexcept { endAt_.store (frame, std::memory_order_release); }
    void setMix (bool mix) noexcept { mix_.store (mix, std::memory_order_relaxed); }
    bool mix() const noexcept { return mix_.load (std::memory_order_relaxed); }

    /// Audio thread. `inputs` are the rig's input channels, changed in place.
    void process (float* const* inputs, int numInputs, int n) noexcept;

    bool playing() const noexcept { return playingOut_.load (std::memory_order_acquire); }
    /// Frames played since the last flush.
    std::int64_t consumed() const noexcept { return consumedOut_.load (std::memory_order_acquire); }
    /// Blocks in which the FIFO ran dry before the file's end, since the last flush.
    std::int64_t underruns() const noexcept { return underruns_.load (std::memory_order_relaxed); }
    /// Plays ended by reaching setEndAt() (not by Stop or a flush).
    std::uint32_t endings() const noexcept { return endings_.load (std::memory_order_acquire); }

private:
    void processPart (float* const* inputs, int channels, int offset, int n) noexcept;

    FrameFifo fifo_;
    std::vector<float> scratch_;
    int scratchFrames_ = 0;
    double sampleRate_ = 48000.0;
    float step_ = 1.0f; // per-sample gain step of a fade

    std::atomic<std::uint32_t> flushRequest_ { 0 }, flushAck_ { 0 };
    std::atomic<std::uint32_t> playRequest_ { 0 };
    std::atomic<std::uint32_t> stopRequest_ { 0 };
    std::atomic<std::int64_t> endAt_ { -1 };
    std::atomic<bool> mix_ { false };

    // Audio thread.
    bool playing_ = false;
    bool fading_ = false;
    std::uint32_t startedFor_ = 0; // the play request now playing (or last played)
    float fileGain_ = 1.0f;
    float liveGain_ = 1.0f;
    std::int64_t consumed_ = 0;

    std::atomic<bool> playingOut_ { false };
    std::atomic<std::int64_t> consumedOut_ { 0 };
    std::atomic<std::int64_t> underruns_ { 0 };
    std::atomic<std::uint32_t> endings_ { 0 };
};

} // namespace sedef::record
