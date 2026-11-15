// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace sedef
{

/// Keeps the audio thread out of its block while another thread changes what the block uses
/// (docs/robustness.md, section 1). The audio thread never waits: it enters, or, while a hold is
/// raised, it does not, and plays silence for that block.
///
/// One atomic word: the low 16 bits count blocks inside, the high 16 bits count raised holds.
/// Both sides change it with one read-modify-write each, so either the block sees the hold or
/// the holder sees the block.
class ProcessHold
{
public:
    // Audio thread ------------------------------------------------------------------------------

    /// Wait-free. True when the block may run; it must then call leave() when it is done. False
    /// while a hold is raised: the block must not touch what the hold protects.
    bool enter() noexcept
    {
        const auto s = word_.fetch_add (1, std::memory_order_acq_rel);
        if ((s & kHoldMask) == 0)
            return true;
        word_.fetch_sub (1, std::memory_order_acq_rel);
        return false;
    }

    void leave() noexcept { word_.fetch_sub (1, std::memory_order_acq_rel); }

    /// enter() and leave() for a scope.
    class Pass
    {
    public:
        explicit Pass (ProcessHold& hold) noexcept : hold_ (hold), entered_ (hold.enter()) {}
        ~Pass()
        {
            if (entered_)
                hold_.leave();
        }
        explicit operator bool() const noexcept { return entered_; }
        Pass (const Pass&) = delete;
        Pass& operator= (const Pass&) = delete;

    private:
        ProcessHold& hold_;
        bool entered_;
    };

    // Any other thread --------------------------------------------------------------------------

    /// Raises a hold: blocks that start from now on stay out. Holds nest.
    void raise() noexcept { word_.fetch_add (kHoldUnit, std::memory_order_acq_rel); }
    void lower() noexcept { word_.fetch_sub (kHoldUnit, std::memory_order_acq_rel); }

    /// True when no block is inside. With a hold raised, it stays true until the hold is lowered.
    bool audioIsOut() const noexcept
    {
        return (word_.load (std::memory_order_acquire) & kInsideMask) == 0;
    }
    bool isHeld() const noexcept
    {
        return (word_.load (std::memory_order_acquire) & kHoldMask) != 0;
    }

    /// Calls `wait (round)` until no block is inside or it returns false. True once the audio is
    /// out. (Tests pass a `wait` that decides deterministically.)
    template <typename Wait>
    bool waitUntilOut (Wait&& wait)
    {
        for (int round = 0;; ++round)
        {
            if (audioIsOut())
                return true;
            if (!wait (round))
                return audioIsOut();
        }
    }

    /// Waits at most `timeout`: a yield for the first rounds, then 100 µs sleeps.
    bool waitUntilOut (std::chrono::nanoseconds timeout)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        return waitUntilOut ([end] (int round) {
            if (std::chrono::steady_clock::now() >= end)
                return false;
            if (round < kSpinRounds)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for (std::chrono::microseconds (100));
            return true;
        });
    }

    /// Raises a hold for a scope and waits (bounded) for the block inside to leave.
    /// audioIsOut() says whether it did; the hold stays raised either way until the scope ends.
    class Scope
    {
    public:
        Scope (ProcessHold& hold, std::chrono::nanoseconds timeout) : hold_ (hold)
        {
            hold_.raise();
            out_ = hold_.waitUntilOut (timeout);
        }
        ~Scope() { hold_.lower(); }
        bool audioIsOut() const noexcept { return out_; }
        Scope (const Scope&) = delete;
        Scope& operator= (const Scope&) = delete;

    private:
        ProcessHold& hold_;
        bool out_ = false;
    };

    static constexpr int kSpinRounds = 64;

private:
    static constexpr std::uint32_t kInsideMask = 0xffffu;
    static constexpr std::uint32_t kHoldUnit = 0x10000u;
    static constexpr std::uint32_t kHoldMask = 0xffff0000u;

    std::atomic<std::uint32_t> word_ { 0 };
};

} // namespace sedef
