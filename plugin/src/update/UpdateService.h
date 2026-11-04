// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Installer.h"
#include "UpdateClient.h"

#include <juce_events/juce_events.h>

namespace sedef::update
{

/// The update checker of this process: one per loaded plug-in binary, shared by every open editor
/// through juce::SharedResourcePointer, so a session with dozens of instances still runs one check.
/// update.json (with an inter-process lock) throttles checks across processes, keeps the settings
/// and records an installed update until a process runs it. Bridge calls arrive on the message
/// thread; requests, file hashing, the install (mounting, unpacking, the OS's installer) and any
/// wait for update.json's lock run on the service's own thread, so neither the audio thread nor
/// the message thread ever waits for the network, an installer or another process. Change
/// messages reach listeners on the message thread.
///
/// Constructing it does no I/O; nothing touches the network until an editor's page asks for the
/// state (pageOpened) or the user acts.
class Service final : public juce::ChangeBroadcaster,
                      private juce::Thread,
                      private juce::AsyncUpdater
{
public:
    struct Options
    {
        juce::File store;
        juce::String apiUrl = latestReleaseApiUrl();
        juce::String releasesUrl = releasesPageUrl();
        juce::File downloads; // empty: the user's Downloads folder
        juce::File cache;     // empty: updateCacheFolder()
        juce::String currentVersion = SEDEF_VERSION;
        std::unique_ptr<Installer> installer; // empty: this OS's (makeSystemInstaller)
        /// "Install manually" hands the file to this; empty: the OS opens or shows it.
        std::function<bool (const juce::File&)> open;
    };

    /// The plug-in's: update.json in the per-user Sedef folder, the GitHub API, the OS installer.
    Service() : Service (pluginOptions()) {}
    /// Tests: another file, server, folders, version and installer.
    explicit Service (Options options);
    ~Service() override;

    /// The snapshot the UI renders (UpdateState in ui/src/bridge/protocol.ts).
    juce::var state();

    /// An editor's page loaded: reloads update.json and runs the automatic check when it is
    /// enabled and due.
    void pageOpened();
    /// "Check now": ignores the throttle (not the in-flight check).
    void checkNow();
    /// Settings take effect at once and reach update.json right away, or from the worker when
    /// another thread or process holds the file.
    void setAutoCheck (bool enabled);
    /// Stops offering `version`; an empty string clears it.
    void skip (const juce::String& version);
    /// "Update now": downloads and verifies this OS's installer into the private cache, then
    /// installs it through the OS's installer. Where that can't run, does installManually().
    void install();
    /// "Install manually": the verified installer in the Downloads folder, opened (or shown).
    void installManually();
    /// Stops a download in progress.
    void cancel();
    /// Shows the installer "Install manually" saved in the file manager.
    void reveal();
    /// The host's name for "Restart <host>", and whether this process is the standalone app.
    void setHost (juce::String name, bool standalone);
    /// The standalone app, once an update is installed (or, on Windows, waits for it to quit):
    /// arranges for the new version to open after this process exits. True when the caller should
    /// quit now.
    bool prepareRestart();

    /// False when SEDEF_DISABLE_UPDATE_CHECK is set (CI, validators, packagers' test runs) or
    /// the UI smoke test runs: then nothing checks without a click.
    static bool automaticChecksAllowed();
    /// <per-user Sedef folder>/update.json, next to Presets/ (defined in UpdateBridge.cpp, which
    /// links with the preset code).
    static juce::File storeFile();

private:
    static Options pluginOptions()
    {
        Options o;
        o.store = storeFile();
        return o;
    }

    enum class Phase
    {
        Idle,
        Checking,
        Downloading,
        Verifying,
        Installing,
        Done, // "Install manually": the installer is in Downloads
        Error
    };
    enum class Job
    {
        None,
        Check,
        ForcedCheck,
        Install,
        Manual
    };
    /// Settings changed in memory and not yet in update.json.
    struct Unsaved
    {
        std::optional<bool> autoCheck;
        std::optional<juce::String> skippedVersion;

        bool any() const noexcept { return autoCheck || skippedVersion; }
        void applyTo (Store& store) const;
    };

    void run() override;
    void handleAsyncUpdate() override;

    bool isBusy() const noexcept; // lock_ held
    /// lock_ held. Sets up the automatic check when it is enabled and due; then call wake().
    bool startAutomaticCheckIfDue();
    void startDownload (Job job);
    /// Starts the worker on first use and hands it the pending job.
    void wake();
    void runCheck (bool forced, Cancellation& cancel);
    void runDownload (Job job, Cancellation& cancel);
    void runInstall (const Release& release, const DownloadResult& download, Cancellation& cancel);
    /// Drops an installed-version record this version has caught up with.
    void tidy();
    /// Deletes everything in the cache but `keep` (and what can't go: a held setup, on Windows).
    void clearCache (const juce::String& keep = {});
    ManualReason manualReason();
    /// lock_ held. Takes `store` as the current one, with the unsaved settings on top.
    void adopt (Store store);
    /// Writes the unsaved settings. With `timeoutMs` 0 it only tries (the message thread) and
    /// returns false when the file is busy; otherwise it waits that long for the other processes
    /// and then writes anyway.
    bool saveSettings (int timeoutMs);
    /// Read-modify-write of update.json, serialised within the process (fileMutex_) and across
    /// processes (fileLock_). Returns what the file holds afterwards; with `timeoutMs` 0, nothing
    /// when another thread or process holds it.
    template <typename Modify>
    std::optional<Store> updateStore (int timeoutMs, Modify&& modify);

    const juce::File file_;
    const juce::String releasesUrl_;
    const juce::File downloads_;
    const juce::File cache_;
    const juce::String currentText_;
    const Version current_;
    const Platform platform_;
    const Client client_;
    const std::unique_ptr<Installer> installer_;
    const std::function<bool (const juce::File&)> open_;
    std::mutex fileMutex_;
    juce::InterProcessLock fileLock_ { "SedefAmpUpdateStore" };
    std::once_flag reasonOnce_;
    ManualReason reason_ = ManualReason::None;

    mutable std::mutex lock_; // guards everything below
    Store store_;
    Unsaved unsaved_;
    Phase phase_ = Phase::Idle;
    InstallStep step_ = InstallStep::Preparing;
    Error error_ = Error::None;
    juce::String detail_;
    juce::int64 received_ = 0, total_ = 0;
    juce::File downloaded_; // "Install manually": the copy in Downloads
    juce::File toOpen_;
    bool opened_ = false;
    juce::String scheduledVersion_; // Windows: Setup waits for this process to exit
    bool scheduledRelaunch_ = false;
    juce::String hostName_;
    bool standalone_ = false;
    void* window_ = nullptr;
    bool tidyPending_ = false;
    Job pending_ = Job::None;
    std::shared_ptr<Cancellation> cancel_; // the running job's, made when the worker takes it
    juce::uint32 lastProgressMessage_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Service)
};

} // namespace sedef::update
