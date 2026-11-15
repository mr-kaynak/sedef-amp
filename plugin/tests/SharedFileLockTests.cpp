// SPDX-License-Identifier: AGPL-3.0-or-later
// Files every instance shares (docs/robustness.md, section 3): instances in one host are one
// process, which JUCE's InterProcessLock does not tell apart on macOS and Linux.

#include "SharedFileLock.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>

using sedef::plugin::SharedFileLock;

#if !JUCE_WINDOWS
TEST_CASE ("JUCE's InterProcessLock lets a second holder in the same process in")
{
    // The evidence for the suspicion: an fcntl lock belongs to the process, so two instances'
    // locks of one name are both "held" at once. (On Windows it is a named mutex, which keeps
    // the second out.)
    const auto name = "SedefLockTest" + juce::Uuid().toString();
    juce::InterProcessLock first (name), second (name);
    REQUIRE (first.enter (0));
    CHECK (second.enter (0)); // the defect SharedFileLock works around
    second.exit();
    first.exit();
}
#endif

TEST_CASE ("SharedFileLock keeps a second holder in the same process out until the first lets go")
{
    const auto name = "SedefLockTest" + juce::Uuid().toString();
    SharedFileLock a (name), b (name); // two instances' locks for one file
    std::atomic<int> stage { 0 };
    std::atomic<bool> bInside { false }, bInsideWhileAHeld { false };

    std::thread other;
    {
        const SharedFileLock::Scoped held (a);
        REQUIRE (held.isLocked());
        other = std::thread ([&] {
            stage.store (1); // about to ask
            const SharedFileLock::Scoped lock (b);
            bInside.store (lock.isLocked());
            if (stage.load() < 2)
                bInsideWhileAHeld.store (true);
        });
        while (stage.load() < 1)
            std::this_thread::yield();
        for (int i = 0; i < 2000; ++i)
            std::this_thread::yield(); // b is asking all this time
        CHECK_FALSE (bInside.load());
        stage.store (2); // a lets go now
    }
    other.join();
    CHECK (bInside.load());
    CHECK_FALSE (bInsideWhileAHeld.load());
}
