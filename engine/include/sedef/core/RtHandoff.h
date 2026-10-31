// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>

namespace sedef
{

/// Hands heavy objects (impulse responses, models) from a non-real-time thread to the audio
/// thread without the audio thread ever freeing memory.
///
///   publish()  - any non-RT thread (serialised by the caller)
///   acquire()  - audio thread, once per block; returns the current object (may be null)
///   collect()  - non-RT thread, periodically; frees retired objects
///
/// A newly published object waits in `incoming_` until the retire slot is empty, so the owner must
/// call collect() (before publishing, and on a timer). publish() deliberately does not collect by
/// itself: an owner may still be reading the retired object (CabSim crossfades out of it) and
/// guards collect() with its own ownership flag.
template <typename T>
class RtHandoff
{
public:
    ~RtHandoff()
    {
        delete incoming_.exchange (nullptr);
        delete retired_.exchange (nullptr);
        delete current_;
    }

    void publish (std::unique_ptr<T> object)
    {
        // A pending object the audio thread never picked up is replaced; acquire() takes
        // `incoming_` with an atomic exchange, so it can never be holding the one deleted here.
        delete incoming_.exchange (object.release(), std::memory_order_acq_rel);
    }

    /// Audio thread. Swaps in a newly published object if the retire slot is free.
    T* acquire() noexcept
    {
        if (incoming_.load (std::memory_order_acquire) != nullptr &&
            retired_.load (std::memory_order_acquire) == nullptr)
        {
            if (T* next = incoming_.exchange (nullptr, std::memory_order_acq_rel))
            {
                retired_.store (current_, std::memory_order_release);
                current_ = next;
            }
        }
        return current_;
    }

    void collect()
    {
        delete retired_.exchange (nullptr, std::memory_order_acq_rel);
    }

private:
    std::atomic<T*> incoming_ { nullptr };
    std::atomic<T*> retired_ { nullptr };
    T* current_ = nullptr; // audio thread only
};

} // namespace sedef
