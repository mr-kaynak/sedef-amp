// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace sedef::record
{

/// Wait-free single-producer, single-consumer FIFO of interleaved audio frames, copied in at
/// most two pieces. The consumer can also drop everything it holds (discard()).
class FrameFifo
{
public:
    /// Not thread-safe: while neither side runs. Rounds the capacity up to a power of two.
    void prepare (int channels, std::size_t capacityFrames)
    {
        channels_ = std::max (1, channels);
        std::size_t size = 1;
        while (size < capacityFrames)
            size <<= 1;
        buffer_.assign (size * static_cast<std::size_t> (channels_), 0.0f);
        mask_ = size - 1;
        head_.store (0);
        tail_.store (0);
    }

    int channels() const noexcept { return channels_; }
    std::size_t capacity() const noexcept { return mask_ + 1; }

    /// Producer.
    std::size_t freeFrames() const noexcept
    {
        return capacity() -
               (head_.load (std::memory_order_relaxed) - tail_.load (std::memory_order_acquire));
    }

    /// Producer. Returns the frames written (fewer when full).
    std::size_t push (const float* frames, std::size_t count) noexcept
    {
        const auto head = head_.load (std::memory_order_relaxed);
        const auto n = std::min (count, freeFrames());
        if (n > 0)
            write (frames, head, n);
        head_.store (head + n, std::memory_order_release);
        return n;
    }

    /// Consumer.
    std::size_t availableFrames() const noexcept
    {
        return head_.load (std::memory_order_acquire) - tail_.load (std::memory_order_relaxed);
    }

    /// Consumer. Returns the frames read.
    std::size_t pop (float* frames, std::size_t count) noexcept
    {
        const auto tail = tail_.load (std::memory_order_relaxed);
        const auto n = std::min (count, availableFrames());
        if (n > 0)
            read (frames, tail, n);
        tail_.store (tail + n, std::memory_order_release);
        return n;
    }

    /// Consumer: drops every frame pushed so far.
    void discard() noexcept
    {
        tail_.store (head_.load (std::memory_order_acquire), std::memory_order_release);
    }

private:
    void write (const float* src, std::size_t at, std::size_t n) noexcept
    {
        const auto ch = static_cast<std::size_t> (channels_);
        const std::size_t start = at & mask_;
        const std::size_t first = std::min (n, capacity() - start);
        std::memcpy (buffer_.data() + start * ch, src, first * ch * sizeof (float));
        std::memcpy (buffer_.data(), src + first * ch, (n - first) * ch * sizeof (float));
    }

    void read (float* dst, std::size_t at, std::size_t n) const noexcept
    {
        const auto ch = static_cast<std::size_t> (channels_);
        const std::size_t start = at & mask_;
        const std::size_t first = std::min (n, capacity() - start);
        std::memcpy (dst, buffer_.data() + start * ch, first * ch * sizeof (float));
        std::memcpy (dst + first * ch, buffer_.data(), (n - first) * ch * sizeof (float));
    }

    std::vector<float> buffer_;
    std::size_t mask_ = 0;
    int channels_ = 1;
    alignas (64) std::atomic<std::size_t> head_ { 0 };
    alignas (64) std::atomic<std::size_t> tail_ { 0 };
};

} // namespace sedef::record
