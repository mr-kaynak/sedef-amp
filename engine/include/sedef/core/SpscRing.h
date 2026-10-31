// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace sedef
{

/// Wait-free single-producer / single-consumer ring of trivially copyable items.
template <typename T>
class SpscRing
{
public:
    explicit SpscRing (std::size_t capacityPow2 = 1 << 15) { resize (capacityPow2); }

    /// Not thread-safe; call before either side starts.
    void resize (std::size_t capacityPow2)
    {
        std::size_t size = 1;
        while (size < capacityPow2)
            size <<= 1;
        buffer_.assign (size, T {});
        mask_ = size - 1;
        head_.store (0);
        tail_.store (0);
    }

    /// Producer side. Returns the number of items written (drops the rest when full).
    std::size_t push (const T* items, std::size_t count) noexcept
    {
        const auto head = head_.load (std::memory_order_relaxed);
        const auto tail = tail_.load (std::memory_order_acquire);
        const std::size_t free = buffer_.size() - (head - tail);
        const std::size_t n = std::min (count, free);
        for (std::size_t i = 0; i < n; ++i)
            buffer_[(head + i) & mask_] = items[i];
        head_.store (head + n, std::memory_order_release);
        return n;
    }

    /// Consumer side. Returns the number of items read.
    std::size_t pop (T* items, std::size_t maxCount) noexcept
    {
        const auto tail = tail_.load (std::memory_order_relaxed);
        const auto head = head_.load (std::memory_order_acquire);
        const std::size_t n = std::min (maxCount, head - tail);
        for (std::size_t i = 0; i < n; ++i)
            items[i] = buffer_[(tail + i) & mask_];
        tail_.store (tail + n, std::memory_order_release);
        return n;
    }

    std::size_t available() const noexcept
    {
        return head_.load (std::memory_order_acquire) - tail_.load (std::memory_order_acquire);
    }

private:
    std::vector<T> buffer_;
    std::size_t mask_ = 0;
    alignas (64) std::atomic<std::size_t> head_ { 0 };
    alignas (64) std::atomic<std::size_t> tail_ { 0 };
};

} // namespace sedef
