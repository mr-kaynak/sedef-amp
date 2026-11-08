// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <type_traits>

namespace sedef
{

/// Hands small plain values (the pedal order) from one non-real-time thread to the audio thread,
/// newest value wins. A triple buffer: both sides are wait-free and nothing allocates.
///
///   write() - the producer (callers serialise it, e.g. under their own lock)
///   read()  - the audio thread; true and the newest value when one arrived since the last read
template <typename T>
class RtMailbox
{
    static_assert (std::is_trivially_copyable_v<T>);

public:
    void write (const T& value) noexcept
    {
        slots_[static_cast<std::size_t> (back_)] = value;
        back_ = middle_.exchange (back_ | kFresh, std::memory_order_acq_rel) & kIndex;
    }

    bool read (T& out) noexcept
    {
        if ((middle_.load (std::memory_order_acquire) & kFresh) == 0)
            return false;
        front_ = middle_.exchange (front_, std::memory_order_acq_rel) & kIndex;
        out = slots_[static_cast<std::size_t> (front_)];
        return true;
    }

private:
    static constexpr int kIndex = 3;
    static constexpr int kFresh = 4;

    std::array<T, 3> slots_ {};
    std::atomic<int> middle_ { 1 }; // the slot between the two sides, | kFresh when unread
    int back_ = 0;                  // producer only
    int front_ = 2;                 // consumer only
};

} // namespace sedef
