// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/record/TakeRecorder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sedef::record
{

int TakeRecorder::chunksPerStream (double sampleRate, double stallSeconds) noexcept
{
    const double frames = std::max (0.0, stallSeconds) * std::max (1.0, sampleRate);
    return static_cast<int> (std::ceil (frames / kChunkFrames)) + 2;
}

bool TakeRecorder::start (const Config& config)
{
    if (phase() != Phase::idle)
        return false;

    config_ = config;
    config_.diChannels = std::clamp (config.diChannels, 1, kMaxChannels);
    config_.outChannels = std::clamp (config.outChannels, 1, kMaxChannels);

    const int total = 2 * chunksPerStream (config_.sampleRate, config_.stallSeconds);
    const auto chunkFloats = static_cast<std::size_t> (kMaxChannels) * kChunkFrames;
    storage_.assign (static_cast<std::size_t> (total) * chunkFloats, 0.0f);
    chunks_.assign (static_cast<std::size_t> (total), TakeChunk {});
    for (int i = 0; i < total; ++i)
        chunks_[static_cast<std::size_t> (i)].data =
            storage_.data() + static_cast<std::size_t> (i) * chunkFloats;
    free_.resize (static_cast<std::size_t> (total));
    filled_.resize (static_cast<std::size_t> (total));
    for (int i = 0; i < total; ++i)
        free_.push (&i, 1);

    current_ = { -1, -1 };
    time_ = 0;
    diEnd_ = std::numeric_limits<std::int64_t>::max();
    outCovered_ = 0;
    stopTime_ = 0;
    lastLatency_ = -1;
    blockDropped_ = false;
    blockActive_ = false;
    pendingReason_ = EndReason::none;

    frames_.store (0);
    droppedBlocks_.store (0);
    for (auto& d : droppedFrames_)
        d.store (0);
    startLatency_.store (0);
    latencyChanged_.store (false);
    endReason_.store (EndReason::none);
    for (auto& p : peaks_)
        p.store (0.0f);

    // Everything above happens before the audio thread can see the take.
    phase_.store (Phase::armed, std::memory_order_release);
    return true;
}

void TakeRecorder::requestStop() noexcept
{
    auto p = phase_.load (std::memory_order_acquire);
    while (p == Phase::armed || p == Phase::recording)
        if (phase_.compare_exchange_weak (p, Phase::stopRequested, std::memory_order_acq_rel))
            return;
}

void TakeRecorder::finishWithoutAudio() noexcept
{
    const auto p = phase();
    if (p == Phase::idle || p == Phase::finished)
        return;
    auto reason = EndReason::audioStopped;
    if (p == Phase::armed)
        diEnd_ = 0;
    else if (diEnd_ == std::numeric_limits<std::int64_t>::max())
        diEnd_ = time_; // between blocks, time_ is the DI's length
    if (p == Phase::stopRequested)
        reason = EndReason::stopped;
    else if (p == Phase::stopping)
        reason = pendingReason_;
    frames_.store (diEnd_, std::memory_order_release);
    finish (reason);
}

void TakeRecorder::release()
{
    if (phase() != Phase::finished)
        return;
    std::vector<float>().swap (storage_);
    chunks_.clear();
    phase_.store (Phase::idle, std::memory_order_release);
}

// Audio thread ------------------------------------------------------------------------------------

void TakeRecorder::raise (std::atomic<float>& peak, float value) noexcept
{
    if (value > peak.load (std::memory_order_relaxed))
        peak.store (value, std::memory_order_relaxed);
}

void TakeRecorder::publish (std::uint8_t stream) noexcept
{
    int& index = current_[stream];
    if (index < 0 || chunks_[static_cast<std::size_t> (index)].frames == 0)
        return;               // an empty chunk stays ours: only the writer pushes to the free queue
    filled_.push (&index, 1); // never full: it holds every chunk
    index = -1;
}

void TakeRecorder::store (std::uint8_t stream, std::int64_t position, const float* const* src,
                          int channels, int offset, int count) noexcept
{
    int& index = current_[stream];
    const int fileChannels = stream == kDi ? config_.diChannels : config_.outChannels;
    while (count > 0)
    {
        if (index >= 0)
        {
            auto& c = chunks_[static_cast<std::size_t> (index)];
            if (c.frames > 0 && c.position + c.frames != position)
                publish (stream); // a gap or a latency step: the next frames start a new chunk
            else if (c.frames == 0)
                c.position = position;
        }
        if (index < 0)
        {
            int next = -1;
            if (free_.pop (&next, 1) != 1)
            {
                // No room: drop the rest of this block for this file, keep counting time.
                droppedFrames_[stream].fetch_add (count, std::memory_order_relaxed);
                blockDropped_ = true;
                return;
            }
            index = next;
            auto& c = chunks_[static_cast<std::size_t> (index)];
            c.position = position;
            c.frames = 0;
            c.channels = fileChannels;
            c.stream = stream;
        }

        auto& c = chunks_[static_cast<std::size_t> (index)];
        const int m = std::min (count, kChunkFrames - c.frames);
        for (int ch = 0; ch < fileChannels; ++ch)
        {
            float* dst = c.data + static_cast<std::size_t> (ch) * kChunkFrames + c.frames;
            if (ch < channels && src[ch] != nullptr)
                std::memcpy (dst, src[ch] + offset, static_cast<std::size_t> (m) * sizeof (float));
            else
                std::memset (dst, 0, static_cast<std::size_t> (m) * sizeof (float));
        }
        c.frames += m;
        position += m;
        offset += m;
        count -= m;
        if (c.frames == kChunkFrames)
            publish (stream);
    }
}

void TakeRecorder::finish (EndReason reason) noexcept
{
    publish (kDi);
    publish (kOutput);
    endReason_.store (reason, std::memory_order_relaxed);
    phase_.store (Phase::finished, std::memory_order_release);
}

void TakeRecorder::captureInput (const float* const* inputs, int channels, int n) noexcept
{
    blockActive_ = false;
    auto p = phase_.load (std::memory_order_acquire);
    if (p == Phase::idle || p == Phase::finished || n <= 0)
        return;

    if (p == Phase::armed &&
        phase_.compare_exchange_strong (p, Phase::recording, std::memory_order_acq_rel))
        p = Phase::recording;
    // (A failed exchange leaves p == stopRequested: stopped before it started.)

    if (p == Phase::recording && time_ + n > config_.maxFrames)
    {
        if (phase_.compare_exchange_strong (p, Phase::stopping, std::memory_order_acq_rel))
        {
            diEnd_ = time_;
            stopTime_ = time_;
            pendingReason_ = EndReason::sizeLimit;
            frames_.store (diEnd_, std::memory_order_release);
            p = Phase::stopping;
        }
    }
    if (p == Phase::stopRequested)
    {
        phase_.store (Phase::stopping, std::memory_order_release);
        diEnd_ = time_;
        stopTime_ = time_;
        pendingReason_ = EndReason::stopped;
        frames_.store (diEnd_, std::memory_order_release);
        p = Phase::stopping;
    }

    blockActive_ = true;
    blockDropped_ = false;
    if (p != Phase::recording)
        return;

    store (kDi, time_, inputs, channels, 0, n);
    float peak = 0.0f;
    for (int c = 0; c < std::min (channels, config_.diChannels); ++c)
        if (inputs[c] != nullptr)
            for (int i = 0; i < n; ++i)
                peak = std::max (peak, std::abs (inputs[c][i]));
    raise (peaks_[kDi], peak);
    frames_.store (time_ + n, std::memory_order_release);
}

void TakeRecorder::captureOutput (const float* const* outputs, int channels, int n,
                                  int latency) noexcept
{
    if (!blockActive_)
        return;
    blockActive_ = false;

    const int l = std::max (0, latency);
    if (lastLatency_ < 0)
        startLatency_.store (l, std::memory_order_relaxed);
    else if (l != lastLatency_)
        latencyChanged_.store (true, std::memory_order_relaxed);
    lastLatency_ = l;

    // Output sample time_ + i belongs to DI sample time_ + i - l (docs/recorder.md, section 5).
    const std::int64_t first = time_ - l;
    const auto i0 = static_cast<int> (std::clamp<std::int64_t> (-first, 0, n));
    const bool stopping = diEnd_ != std::numeric_limits<std::int64_t>::max();
    const auto i1 =
        stopping ? static_cast<int> (std::clamp<std::int64_t> (diEnd_ - first, i0, n)) : n;
    if (i1 > i0)
    {
        store (kOutput, first + i0, outputs, channels, i0, i1 - i0);
        float peak = 0.0f;
        for (int c = 0; c < std::min (channels, config_.outChannels); ++c)
            if (outputs[c] != nullptr)
                for (int i = i0; i < i1; ++i)
                    peak = std::max (peak, std::abs (outputs[c][i]));
        raise (peaks_[kOutput], peak);
        outCovered_ = std::max (outCovered_, first + i1);
    }
    time_ += n;
    if (blockDropped_)
        droppedBlocks_.fetch_add (1, std::memory_order_relaxed);

    if (stopping)
    {
        // Stopping: done once the output has caught up with the DI. A latency longer than a
        // second cannot occur; the cap only guarantees the take always ends.
        const bool caughtUp = outCovered_ >= diEnd_;
        const bool tooLong = static_cast<double> (time_ - stopTime_) > config_.sampleRate;
        if (caughtUp || tooLong)
            finish (pendingReason_);
    }
}

} // namespace sedef::record
