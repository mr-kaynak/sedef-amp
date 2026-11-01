// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "UpdateClient.h"

#include <juce_events/juce_events.h>

namespace sedef::update
{

/// The update checker of this process: one per loaded plug-in binary, shared by every open editor
/// through juce::SharedResourcePointer, so a session with dozens of instances still runs one check.
/// update.json (with an inter-process lock) throttles checks across processes and keeps the
/// settings. Bridge calls arrive on the message thread; requests and file hashing run on the
/// service's own thread, so neither the audio thread nor the message thread ever waits for the
/// network. Change messages reach listeners on the message thread.
///
/// Constructing it does no I/O; nothing touches the network until an editor's page asks for the
/// state (pageOpened) or the user acts.
class Service final : public juce::ChangeBroadcaster,
                      private juce::Thread,
                      private juce::AsyncUpdater
{
public:
    /// The plug-in's: update.json in the per-user Sedef folder, the GitHub API.
    Service() : Service (storeFile(), latestReleaseApiUrl()) {}
    /// Tests: another file, another server.
    Service (juce::File store, juce::String apiUrl);
    ~Service() override;

    /// The snapshot the UI renders (UpdateState in ui/src/bridge/protocol.ts).
    juce::var state();

    /// An editor's page loaded: reloads update.json and runs the automatic check when it is
    /// enabled and due.
    void pageOpened();
    /// "Check now": ignores the throttle (not the in-flight check).
    void checkNow();
    void setAutoCheck (bool enabled);
    /// Stops offering `version`; an empty string clears it.
    void skip (const juce::String& version);
    /// Downloads, verifies and opens this OS's installer for the latest release.
    void download();
    void cancel();
    /// Shows the downloaded installer in the file manager.
    void reveal();

    /// False when SEDEF_DISABLE_UPDATE_CHECK is set (CI, validators, packagers' test runs) or
    /// the UI smoke test runs: then nothing checks without a click.
    static bool automaticChecksAllowed();
    /// <per-user Sedef folder>/update.json, next to Presets/ (defined in UpdateBridge.cpp, which
    /// links with the preset code).
    static juce::File storeFile();

private:
    enum class Phase
    {
        Idle,
        Checking,
        Downloading,
        Verifying,
        Done,
        Error
    };
    enum class Job
    {
        None,
        Check,
        ForcedCheck,
        Download
    };

    void run() override;
    void handleAsyncUpdate() override;

    bool isBusy() const noexcept; // lock_ held
    /// lock_ held. Sets up the automatic check when it is enabled and due; then call wake().
    bool startAutomaticCheckIfDue();
    /// Starts the worker on first use and hands it the pending job.
    void wake();
    void runCheck (bool forced);
    void runDownload();
    /// Read-modify-write of update.json, serialised within the process (fileMutex_) and across
    /// processes (fileLock_). Returns what the file holds afterwards.
    template <typename Modify>
    Store updateStore (int timeoutMs, Modify&& modify);

    const juce::File file_;
    const Platform platform_;
    const Client client_;
    std::mutex fileMutex_;
    juce::InterProcessLock fileLock_ { "SedefAmpUpdateStore" };

    mutable std::mutex lock_; // guards everything below
    Store store_;
    Phase phase_ = Phase::Idle;
    Error error_ = Error::None;
    juce::String detail_;
    juce::int64 received_ = 0, total_ = 0;
    juce::File installer_;
    juce::File toOpen_;
    bool opened_ = false;
    Job pending_ = Job::None;
    std::shared_ptr<Cancellation> cancel_;
    juce::uint32 lastProgressMessage_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Service)
};

} // namespace sedef::update
