// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// A lock for a file every instance shares (docs/robustness.md, section 3). JUCE's
// InterProcessLock is an fcntl record lock on macOS and Linux: it belongs to the process, so two
// instances in one host both "hold" it at once, and the first to let go releases it for both.
// SharedFileLock adds a process-wide mutex per name in front of it.

#include <juce_core/juce_core.h>
#include <map>
#include <memory>
#include <mutex>

namespace sedef::plugin
{

class SharedFileLock
{
public:
    explicit SharedFileLock (const juce::String& name) : name_ (name), local_ (mutexFor (name)) {}

    /// Holds the lock for a scope. isLocked() is false when another process kept it for longer
    /// than the timeout; the caller then leaves the file alone.
    class Scoped
    {
    public:
        explicit Scoped (SharedFileLock& lock, int timeoutMs = 500)
            : local_ (*lock.local_),
              file_ (lock.name_)
        {
            locked_ = file_.enter (timeoutMs);
        }
        ~Scoped()
        {
            if (locked_)
                file_.exit();
        }
        bool isLocked() const noexcept { return locked_; }
        Scoped (const Scoped&) = delete;
        Scoped& operator= (const Scoped&) = delete;

    private:
        std::scoped_lock<std::mutex> local_; // instances in this process wait here
        // A fresh InterProcessLock each time: JUCE's counts re-entries per object, and the
        // process-wide mutex already keeps this process to one holder.
        juce::InterProcessLock file_;
        bool locked_ = false;
    };

private:
    static std::shared_ptr<std::mutex> mutexFor (const juce::String& name)
    {
        static std::mutex registryLock;
        static std::map<juce::String, std::weak_ptr<std::mutex>> registry;
        const std::scoped_lock lock (registryLock);
        auto& slot = registry[name];
        auto m = slot.lock();
        if (m == nullptr)
        {
            m = std::make_shared<std::mutex>();
            slot = m;
        }
        return m;
    }

    juce::String name_;
    std::shared_ptr<std::mutex> local_;
};

} // namespace sedef::plugin
