// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/record/ReampPlayer.h"

#include <algorithm>
#include <cmath>

namespace sedef::record
{

void ReampPlayer::prepare (double sampleRate, int channels, int maxBlock, double fifoSeconds)
{
    sampleRate_ = std::max (1.0, sampleRate);
    const auto ch = std::clamp (channels, 1, 2);
    fifo_.prepare (ch, static_cast<std::size_t> (std::ceil (fifoSeconds * sampleRate_)));
    scratchFrames_ = std::clamp (maxBlock, 64, 8192);
    scratch_.assign (static_cast<std::size_t> (scratchFrames_) * static_cast<std::size_t> (ch),
                     0.0f);
    step_ = 1.0f / static_cast<float> (std::max (1L, std::lround (kFadeSeconds * sampleRate_)));

    flushRequest_.store (0);
    flushAck_.store (0);
    playRequest_.store (0);
    stopRequest_.store (0);
    endAt_.store (-1);
    playing_ = false;
    fading_ = false;
    startedFor_ = 0;
    fileGain_ = 1.0f;
    liveGain_ = 1.0f;
    consumed_ = 0;
    playingOut_.store (false);
    consumedOut_.store (0);
}

std::uint32_t ReampPlayer::requestFlush() noexcept
{
    auto request = flushRequest_.load (std::memory_order_relaxed) + 1;
    if (request == 0)
        request = 1; // 0 means "none"
    flushRequest_.store (request, std::memory_order_release);
    return request;
}

void ReampPlayer::process (float* const* inputs, int numInputs, int n) noexcept
{
    const auto flush = flushRequest_.load (std::memory_order_acquire);
    if (flush != flushAck_.load (std::memory_order_relaxed))
    {
        fifo_.discard();
        playing_ = false;
        fading_ = false;
        consumed_ = 0;
        underruns_.store (0, std::memory_order_relaxed); // counted per play, shown while it plays
        flushAck_.store (flush, std::memory_order_release);
    }

    const auto play = playRequest_.load (std::memory_order_acquire);
    const auto stop = stopRequest_.load (std::memory_order_acquire);
    if (!playing_ && play != 0 && play == flush && play != startedFor_ && stop != play)
    {
        playing_ = true;
        fading_ = false;
        fileGain_ = 1.0f;
        startedFor_ = play;
    }
    if (playing_ && !fading_ && stop == startedFor_)
        fading_ = true;

    const bool active = numInputs == fifo_.channels() && (playing_ || liveGain_ != 1.0f);
    if (active)
        for (int offset = 0; offset < n; offset += scratchFrames_)
            processPart (inputs, numInputs, offset, std::min (scratchFrames_, n - offset));

    playingOut_.store (playing_, std::memory_order_release);
    consumedOut_.store (consumed_, std::memory_order_release);
}

void ReampPlayer::processPart (float* const* inputs, int channels, int offset, int n) noexcept
{
    int want = 0;
    if (playing_)
    {
        const auto endAt = endAt_.load (std::memory_order_acquire);
        const std::int64_t left = endAt >= 0 ? std::max<std::int64_t> (0, endAt - consumed_) : n;
        want = static_cast<int> (std::min<std::int64_t> (n, left));
    }
    const int got =
        want > 0 ? static_cast<int> (fifo_.pop (scratch_.data(), static_cast<std::size_t> (want)))
                 : 0;
    if (got < want)
        underruns_.fetch_add (1, std::memory_order_relaxed); // the reader fell behind

    const float liveTarget = playing_ && !mix_.load (std::memory_order_relaxed) ? 0.0f : 1.0f;
    for (int i = 0; i < n; ++i)
    {
        if (liveGain_ != liveTarget)
            liveGain_ = liveTarget > liveGain_ ? std::min (liveTarget, liveGain_ + step_)
                                               : std::max (liveTarget, liveGain_ - step_);
        if (fading_ && i < got)
            fileGain_ = std::max (0.0f, fileGain_ - step_);

        for (int c = 0; c < channels; ++c)
        {
            float& x = inputs[c][offset + i];
            const float live = liveGain_ == 1.0f ? x : (liveGain_ == 0.0f ? 0.0f : x * liveGain_);
            if (i < got)
            {
                // The file's samples pass unchanged unless they are fading out.
                float f = scratch_[static_cast<std::size_t> (i * channels + c)];
                if (fading_)
                    f *= fileGain_;
                x = liveGain_ == 0.0f ? f : live + f;
            }
            else
            {
                x = live;
            }
        }
    }
    consumed_ += got;

    if (playing_)
    {
        const auto endAt = endAt_.load (std::memory_order_acquire);
        if (endAt >= 0 && consumed_ >= endAt)
        {
            playing_ = false;
            fading_ = false;
            endings_.fetch_add (1, std::memory_order_acq_rel);
        }
        else if (fading_ && fileGain_ <= 0.0f)
        {
            playing_ = false;
            fading_ = false;
        }
    }
}

} // namespace sedef::record
