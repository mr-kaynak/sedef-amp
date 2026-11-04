// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

/// Running the installer's commands: a program by absolute path with an argv, never through a
/// shell of ours. The call blocks until the program exits, the timeout passes or `cancelled` turns
/// true. A timeout kills the program and what it started; a cancellation does too, or leaves it to
/// finish (OnCancel::LetFinish). Either way the call returns at once: whatever can't be killed
/// because it runs as root now is left to finish, and the call never waits for it. macOS and Linux
/// only; on Windows runProcess reports that nothing started.
namespace sedef::update
{

struct ProcessResult
{
    bool started = false;
    bool killed = false;    // stopped waiting: by the timeout or a cancellation
    bool abandoned = false; // and it still runs: as root (no signal reaches it), or left to finish
    int exitCode = -1;      // when it exited normally
    juce::String output;    // stdout, at most kMaxProcessOutput bytes
    juce::String errors;    // stderr, likewise

    bool succeeded() const noexcept { return started && !killed && exitCode == 0; }
    /// The last lines of what it printed, for a log or an error's detail.
    juce::String summary() const;
};

inline constexpr int kMaxProcessOutput = 1 << 20;

/// What a cancellation does to the program: kill it and what it started (a password dialog goes
/// away with it), or leave it to finish on its own (an install that must not stop halfway).
enum class OnCancel
{
    Kill,
    LetFinish
};

/// Runs argv[0] (an absolute path) with the arguments that follow, stdin from /dev/null and no
/// other descriptor of this process. With `timeoutMs` 0 it waits as long as it takes. `cancelled`
/// is polled about every 50 ms.
///
/// The program runs under a small /bin/sh of ours ("$@" & wait), in a process group of its own.
/// That shell is this user's and always dies at a signal, so stopping never blocks: a kill goes to
/// the group, and a program that switched to root (pkexec once you have authenticated, or what
/// osascript started) does not get it and goes on, owned by init from then on.
ProcessResult runProcess (const juce::StringArray& argv, int timeoutMs,
                          const std::function<bool()>& cancelled = {},
                          OnCancel onCancel = OnCancel::Kill);

/// Starts argv[0] in its own session with its output discarded, no descriptor of this process,
/// and does not wait for it: for the helper that restarts the standalone app once this process has
/// quit.
bool spawnDetached (const juce::StringArray& argv);

/// This process's id.
int currentProcessId() noexcept;

} // namespace sedef::update
