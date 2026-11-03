// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UpdateService.h"

namespace sedef::update
{
namespace
{
constexpr juce::uint32 kProgressMessageIntervalMs = 100; // the UI hears at most ~10 per second
constexpr int kStoreLockTimeoutMs = 2000;
constexpr int kClosingLockTimeoutMs = 250; // settings still unsaved when the service closes

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        o->setProperty (k, v);
    return juce::var (o);
}

/// "Install manually": what the OS does with a downloaded installer, as a browser would.
bool openInstaller (const juce::File& file, Platform platform)
{
    switch (platform)
    {
    case Platform::MacOS: // mounts the disk image and shows it, like `open`
        return file.startAsProcess();
    case Platform::Windows:
        // Interactive, never with /SILENT: the wizard asks for elevation, lists a DAW that holds
        // the plug-in and offers to close it.
        return file.startAsProcess();
    case Platform::Linux: // an archive: show it; install.sh and its README do the rest
        file.revealToUser();
        return true;
    }
    return false;
}

std::unique_ptr<Installer> installerFor (Service::Options& options)
{
    if (options.installer != nullptr)
        return std::move (options.installer);
    auto config = defaultInstallerConfig();
    if (options.cache != juce::File())
        config.cache = options.cache;
    return makeSystemInstaller (std::move (config));
}
} // namespace

Service::Service (Options options)
    : juce::Thread ("Sedef updates"),
      file_ (options.store),
      releasesUrl_ (options.releasesUrl),
      downloads_ (options.downloads),
      cache_ (options.cache != juce::File() ? options.cache : updateCacheFolder()),
      currentText_ (options.currentVersion),
      current_ (Version::parse (options.currentVersion).value_or (Version {})),
      platform_ (currentPlatform()),
      client_ (options.currentVersion, platform_, options.apiUrl, releasesUrl_),
      installer_ (installerFor (options)),
      open_ (std::move (options.open))
{
}

Service::~Service()
{
    cancelPendingUpdate();
    {
        // The worker makes cancel_ in the same step as it takes a job, so a job is either still
        // pending here or already cancellable. Cancelling kills any process the job waits on.
        const std::scoped_lock l (lock_);
        pending_ = Job::None;
        tidyPending_ = false;
        if (cancel_ != nullptr)
            cancel_->cancel();
    }
    signalThreadShouldExit();
    notify();
    // A cancelled request returns at once and a killed process at once; the margin covers a stack
    // that ignores cancel() and detaching a disk image.
    stopThread (Client::kTimeoutMs + 2000);
}

bool Service::automaticChecksAllowed()
{
    const auto off =
        juce::SystemStats::getEnvironmentVariable ("SEDEF_DISABLE_UPDATE_CHECK", {}).trim();
    if (off.isNotEmpty() && off != "0")
        return false;
    // The UI smoke test (tools/ci/ui-smoke.sh) opens the editor and must stay off the network.
    return juce::SystemStats::getEnvironmentVariable ("SEDEF_UI_SMOKE_TEST", {}).isEmpty();
}

ManualReason Service::manualReason()
{
    std::call_once (reasonOnce_, [this] { reason_ = installer_->manualReason(); });
    return reason_;
}

// State -------------------------------------------------------------------------------------------

juce::var Service::state()
{
    static constexpr const char* kPhases[] = { "idle",       "checking", "downloading", "verifying",
                                               "installing", "done",     "error" };
    const auto reason = manualReason();

    const std::scoped_lock l (lock_);
    juce::var latest, installer;
    bool available = false;
    if (store_.latest)
    {
        const auto& r = *store_.latest;
        if (const auto* a = selectInstaller (r, platform_, isX64()))
            installer = object ({ { "name", a->name }, { "size", a->size } });
        latest = object ({ { "version", r.version.toString() },
                           { "tag", r.tag },
                           { "name", r.name },
                           { "notes", r.notes },
                           { "url", r.pageUrl },
                           { "publishedAt", r.publishedAt },
                           { "installer", installer },
                           { "hasChecksums", r.findAsset (kChecksumsAssetName) != nullptr } });
        available = isNewer (r, current_);
    }
    const bool transferring = phase_ == Phase::Downloading || phase_ == Phase::Verifying;

    // Idle can mean "installed, restart to use it" (this process or another one installed it) or,
    // on Windows, "Setup waits for this process to exit".
    juce::String phase = kPhases[static_cast<int> (phase_)];
    juce::var installedVersion;
    bool canRestart = false;
    if (phase_ == Phase::Idle && scheduledVersion_.isNotEmpty())
    {
        phase = "scheduled";
        installedVersion = scheduledVersion_;
        canRestart = standalone_ && scheduledRelaunch_;
    }
    else if (phase_ == Phase::Idle && restartNeeded (store_, current_))
    {
        phase = "installed";
        installedVersion = store_.installedVersion;
        canRestart = standalone_;
    }

    return object (
        { { "supported", true },
          { "currentVersion", currentText_ },
          { "platform", platformName (platform_) },
          { "autoCheck", store_.autoCheck },
          { "autoCheckAllowed", automaticChecksAllowed() },
          { "skippedVersion", store_.skippedVersion },
          { "lastChecked",
            store_.lastSuccessMs > 0 ? juce::var (store_.lastSuccessMs) : juce::var() },
          { "phase", phase },
          { "latest", latest },
          { "updateAvailable", available },
          { "error", error_ == Error::None
                         ? juce::var()
                         : object ({ { "code", toString (error_) }, { "detail", detail_ } }) },
          { "progress", transferring ? object ({ { "received", received_ }, { "total", total_ } })
                                     : juce::var() },
          { "install",
            phase_ == Phase::Installing ? object ({ { "step", toString (step_) } }) : juce::var() },
          { "installedVersion", installedVersion },
          { "host", hostName_.isNotEmpty() ? hostName_ : juce::String ("your DAW") },
          { "standalone", standalone_ },
          { "canRestart", canRestart },
          { "autoInstall", object ({ { "available", reason == ManualReason::None },
                                     { "reason", reason == ManualReason::None
                                                     ? juce::var()
                                                     : juce::var (toString (reason)) } }) },
          { "download",
            downloaded_ == juce::File()
                ? juce::var()
                : object ({ { "fileName", downloaded_.getFileName() }, { "opened", opened_ } }) },
          { "releasesUrl", releasesUrl_ } });
}

// Requests from the UI (message thread) -----------------------------------------------------------

bool Service::isBusy() const noexcept
{
    return phase_ == Phase::Checking || phase_ == Phase::Downloading ||
           phase_ == Phase::Verifying || phase_ == Phase::Installing;
}

bool Service::startAutomaticCheckIfDue()
{
    if (!automaticChecksAllowed() || !store_.autoCheck || isBusy() ||
        !isCheckDue (juce::Time::currentTimeMillis(), store_.lastAttemptMs, store_.lastSuccessMs))
        return false;
    phase_ = Phase::Checking;
    error_ = Error::None;
    detail_.clear();
    pending_ = Job::Check;
    return true;
}

void Service::pageOpened()
{
    // Another process may have checked, changed a setting or installed an update since this one
    // last looked. The file is replaced by renaming, so reading it needs no lock.
    auto store = readStore (file_, releasesUrl_);
    const auto recorded = Version::parse (store.installedVersion);
    const bool stale = recorded && compare (*recorded, current_) <= 0;
    bool start = false;
    {
        const std::scoped_lock l (lock_);
        adopt (std::move (store));
        start = startAutomaticCheckIfDue();
        if (!tidied_ || stale)
        {
            tidied_ = true;
            tidyPending_ = start = true;
        }
    }
    if (start)
        wake();
    sendChangeMessage();
}

void Service::checkNow()
{
    {
        const std::scoped_lock l (lock_);
        if (isBusy())
            return;
        phase_ = Phase::Checking;
        error_ = Error::None;
        detail_.clear();
        pending_ = Job::ForcedCheck;
    }
    wake();
    sendChangeMessage();
}

void Service::setAutoCheck (bool enabled)
{
    {
        const std::scoped_lock l (lock_);
        unsaved_.autoCheck = enabled;
        unsaved_.applyTo (store_);
    }
    bool start = !saveSettings (0); // the file is busy: the worker saves it
    {
        const std::scoped_lock l (lock_);
        start = startAutomaticCheckIfDue() || start;
    }
    if (start)
        wake();
    sendChangeMessage();
}

void Service::skip (const juce::String& version)
{
    const auto parsed = Version::parse (version);
    {
        const std::scoped_lock l (lock_);
        unsaved_.skippedVersion = parsed ? parsed->toString() : juce::String();
        unsaved_.applyTo (store_);
    }
    if (!saveSettings (0))
        wake();
    sendChangeMessage();
}

void Service::install()
{
    startDownload (manualReason() == ManualReason::None ? Job::Install : Job::Manual);
}

void Service::installManually()
{
    startDownload (Job::Manual);
}

void Service::startDownload (Job job)
{
    bool start = false;
    {
        const std::scoped_lock l (lock_);
        if (isBusy() || !store_.latest)
            return;
        // Installed already, or Setup already waits: a restart is what's left to do.
        if (job == Job::Install &&
            (restartNeeded (store_, current_) || scheduledVersion_.isNotEmpty()))
            return;
        error_ = Error::None;
        detail_.clear();
        downloaded_ = juce::File();
        opened_ = false;
        if (const auto* asset = selectInstaller (*store_.latest, platform_, isX64()))
        {
            phase_ = Phase::Downloading;
            received_ = 0;
            total_ = asset->size;
            pending_ = job;
            window_ = foregroundWindow(); // the permission prompt's owner (Windows)
            start = true;
        }
        else
        {
            phase_ = Phase::Error;
            error_ = Error::NoInstaller;
            detail_ = store_.latest->tag + " has no " +
                      (installerAssetName (platform_, isX64()).isNotEmpty()
                           ? installerAssetName (platform_, isX64())
                           : "installer for this processor");
        }
    }
    if (start)
        wake();
    sendChangeMessage();
}

void Service::cancel()
{
    {
        const std::scoped_lock l (lock_);
        // Only a download: once the OS's installer runs, its own prompt is where to cancel.
        if (phase_ != Phase::Downloading && phase_ != Phase::Verifying)
            return;
        if (cancel_ != nullptr)
            cancel_->cancel();
        if (pending_ != Job::None) // not started yet
        {
            pending_ = Job::None;
            phase_ = Phase::Idle;
        }
    }
    sendChangeMessage();
}

void Service::reveal()
{
    juce::File file;
    {
        const std::scoped_lock l (lock_);
        file = downloaded_;
    }
    if (file.existsAsFile())
        file.revealToUser();
}

void Service::setHost (juce::String name, bool standalone)
{
    const std::scoped_lock l (lock_);
    hostName_ = std::move (name);
    standalone_ = standalone;
}

bool Service::prepareRestart()
{
    {
        const std::scoped_lock l (lock_);
        if (!standalone_ || phase_ != Phase::Idle)
            return false;
        if (scheduledVersion_.isNotEmpty())
            return scheduledRelaunch_; // Setup installs once this process exits, then opens it
        if (!restartNeeded (store_, current_))
            return false;
    }
    return installer_->relaunchAfterExit();
}

void Service::handleAsyncUpdate()
{
    juce::File file;
    {
        const std::scoped_lock l (lock_);
        file = std::exchange (toOpen_, juce::File());
    }
    if (file == juce::File())
        return;
    // On the message thread: launching goes through the shell, which wants a GUI thread.
    const bool ok = open_ ? open_ (file) : openInstaller (file, platform_);
    {
        const std::scoped_lock l (lock_);
        opened_ = ok;
        if (!ok)
        {
            phase_ = Phase::Error;
            error_ = Error::OpenFailed;
            detail_ = file.getFullPathName();
        }
    }
    sendChangeMessage();
}

// Worker thread -----------------------------------------------------------------------------------

void Service::wake()
{
    // Normal, not low: the message thread joins this thread when the last editor closes, and
    // must not wait behind a starved low-priority thread. It mostly sleeps on the network.
    if (!isThreadRunning())
        startThread (juce::Thread::Priority::normal);
    notify();
}

void Service::run()
{
    while (!threadShouldExit())
    {
        Job job = Job::None;
        bool unsaved = false, tidying = false;
        std::shared_ptr<Cancellation> cancel;
        {
            const std::scoped_lock l (lock_);
            job = std::exchange (pending_, Job::None);
            unsaved = unsaved_.any();
            tidying = std::exchange (tidyPending_, false);
            if (job != Job::None)
                cancel_ = cancel = std::make_shared<Cancellation>();
        }
        if (unsaved)
            saveSettings (kStoreLockTimeoutMs);
        if (tidying && !threadShouldExit())
            tidy();
        switch (job)
        {
        case Job::None:
            if (!unsaved && !tidying)
                wait (-1);
            break;
        case Job::Check:
            runCheck (false, *cancel);
            break;
        case Job::ForcedCheck:
            runCheck (true, *cancel);
            break;
        case Job::Install:
        case Job::Manual:
            runDownload (job, *cancel);
            break;
        }
    }
    saveSettings (kClosingLockTimeoutMs); // closing: a setting the user changed is not lost
}

void Service::Unsaved::applyTo (Store& store) const
{
    if (autoCheck)
        store.autoCheck = *autoCheck;
    if (skippedVersion)
        store.skippedVersion = *skippedVersion;
}

void Service::adopt (Store store)
{
    store_ = std::move (store);
    unsaved_.applyTo (store_);
}

bool Service::saveSettings (int timeoutMs)
{
    Unsaved settings;
    {
        const std::scoped_lock l (lock_);
        settings = unsaved_;
    }
    if (!settings.any())
        return true;
    auto store = updateStore (timeoutMs, [&settings] (Store& s) {
        settings.applyTo (s);
        return true;
    });
    if (!store)
        return false;
    {
        const std::scoped_lock l (lock_);
        // Only what was written: the user may have changed a setting again meanwhile.
        if (unsaved_.autoCheck == settings.autoCheck)
            unsaved_.autoCheck.reset();
        if (unsaved_.skippedVersion == settings.skippedVersion)
            unsaved_.skippedVersion.reset();
        adopt (std::move (*store));
    }
    if (timeoutMs > 0)
        sendChangeMessage();
    return true;
}

template <typename Modify>
std::optional<Store> Service::updateStore (int timeoutMs, Modify&& modify)
{
    std::unique_lock l (fileMutex_, std::defer_lock);
    if (timeoutMs > 0)
        l.lock();
    else if (!l.try_lock())
        return std::nullopt;
    // Best effort across processes once we have waited: the write itself is atomic either way.
    const bool locked = fileLock_.enter (timeoutMs);
    if (!locked && timeoutMs <= 0)
        return std::nullopt;
    auto store = readStore (file_, releasesUrl_);
    if (modify (store))
        writeStore (file_, store);
    if (locked)
        fileLock_.exit();
    return store;
}

void Service::tidy()
{
    installer_->cleanUp();
    auto store = updateStore (kStoreLockTimeoutMs, [this] (Store& s) {
        const auto recorded = Version::parse (s.installedVersion);
        if (!recorded || compare (*recorded, current_) > 0)
            return false;
        s.installedVersion.clear(); // this version runs now: nothing waits for a restart
        return true;
    });
    if (store)
    {
        {
            const std::scoped_lock l (lock_);
            adopt (std::move (*store));
        }
        sendChangeMessage();
    }
}

void Service::clearCache (const juce::String& keep)
{
    for (const auto& item : cache_.findChildFiles (juce::File::findFilesAndDirectories, false))
    {
        if (item.getFileName() == keep || item.getFileName() == "mnt")
            continue; // mount points go with their image (Installer::cleanUp)
        if (item.isDirectory())
            item.deleteRecursively();
        else
            item.deleteFile(); // fails for a setup that still runs (Windows): it stays
    }
}

void Service::runCheck (bool forced, Cancellation& cancel)
{
    // Claim the check before the request, so other processes that open an editor meanwhile
    // see a recent attempt and skip theirs.
    const auto now = juce::Time::currentTimeMillis();
    bool due = false;
    std::optional<Store> store;
    if (!threadShouldExit() && !cancel.isCancelled())
        store = updateStore (kStoreLockTimeoutMs, [&] (Store& s) {
            due = forced || isCheckDue (now, s.lastAttemptMs, s.lastSuccessMs);
            if (due)
                s.lastAttemptMs = now;
            return due;
        });

    CheckResult result;
    if (due && !threadShouldExit())
    {
        result = client_.fetchLatest (cancel);
        if (result.error == Error::None && !threadShouldExit())
            store = updateStore (kStoreLockTimeoutMs, [&result] (Store& s) {
                s.lastSuccessMs = juce::Time::currentTimeMillis();
                s.latest = result.release;
                return true;
            });
    }

    bool upToDate = false;
    {
        const std::scoped_lock l (lock_);
        cancel_.reset();
        if (store)
            adopt (std::move (*store));
        const bool failed = result.error != Error::None && result.error != Error::Cancelled;
        phase_ = failed ? Phase::Error : Phase::Idle;
        error_ = failed ? result.error : Error::None;
        detail_ = failed ? result.detail : juce::String();
        upToDate = due && !failed && (!store_.latest || !isNewer (*store_.latest, current_)) &&
                   scheduledVersion_.isEmpty();
    }
    if (upToDate)
        clearCache(); // nothing newer to install: no installer needs keeping
    sendChangeMessage();
}

void Service::runDownload (Job job, Cancellation& cancel)
{
    std::optional<Release> release;
    {
        const std::scoped_lock l (lock_);
        release = store_.latest;
    }

    DownloadResult result;
    const auto* asset = release ? selectInstaller (*release, platform_, isX64()) : nullptr;
    if (threadShouldExit() || cancel.isCancelled())
    {
        result.error = Error::Cancelled;
    }
    else if (asset == nullptr)
    {
        result.error = Error::NoInstaller;
    }
    else if (!makePrivateFolder (cache_))
    {
        result.error = Error::Disk;
        result.detail = "cannot create " + cache_.getFullPathName();
    }
    else
    {
        const auto onProgress = [this] (DownloadStage stage, juce::int64 received,
                                        juce::int64 total) {
            {
                const std::scoped_lock l (lock_);
                phase_ = stage == DownloadStage::Verifying ? Phase::Verifying : Phase::Downloading;
                received_ = received;
                total_ = total;
            }
            const auto now = juce::Time::getMillisecondCounter();
            if (stage != DownloadStage::Downloading ||
                now - lastProgressMessage_ >= kProgressMessageIntervalMs)
            {
                lastProgressMessage_ = now;
                sendChangeMessage();
            }
        };
        // The private cache holds the current installer only.
        clearCache (asset->name);
        DownloadOptions options;
        options.mark = false;
        options.replace = true;
        result = client_.downloadVerified (*release, *asset, cache_, onProgress, cancel, options);
        if (result.error == Error::None && job == Job::Manual)
            result = copyVerified (result.file, result.sha256,
                                   downloads_ == juce::File() ? downloadsFolder() : downloads_,
                                   asset->url, release->pageUrl);
    }

    if (result.error == Error::None && job == Job::Install && !cancel.isCancelled())
    {
        runInstall (*release, result, cancel);
        return;
    }

    {
        const std::scoped_lock l (lock_);
        cancel_.reset();
        switch (result.error)
        {
        case Error::None:
            if (job == Job::Manual)
            {
                phase_ = Phase::Done;
                downloaded_ = result.file;
                toOpen_ = result.file;
            }
            else
            {
                phase_ = Phase::Idle; // cancelled between the download and the install
            }
            break;
        case Error::Cancelled:
            phase_ = Phase::Idle;
            break;
        case Error::Network:
        case Error::RateLimited:
        case Error::BadResponse:
        case Error::NoInstaller:
        case Error::NoChecksum:
        case Error::ChecksumMismatch:
        case Error::Incomplete:
        case Error::Disk:
        case Error::OpenFailed:
        case Error::BadPackage:
        case Error::ElevationFailed:
        case Error::InstallFailed:
        case Error::Unverified:
            phase_ = Phase::Error;
            error_ = result.error;
            detail_ = result.detail;
            break;
        }
    }
    if (result.error == Error::None && job == Job::Manual)
        triggerAsyncUpdate(); // opens it on the message thread
    sendChangeMessage();
}

void Service::runInstall (const Release& release, const DownloadResult& download,
                          Cancellation& cancel)
{
    InstallRequest request;
    request.file = download.file;
    request.expectedHash = download.sha256;
    request.version = release.version.toString();
    {
        const std::scoped_lock l (lock_);
        phase_ = Phase::Installing;
        step_ = InstallStep::Preparing;
        // The standalone app on Windows: Setup reopens it after installing (sedef-amp.iss).
        request.relaunch = standalone_ && platform_ == Platform::Windows;
        request.window = window_;
    }
    sendChangeMessage();

    const auto result = installer_->install (
        request,
        [this] (InstallStep step) {
            {
                const std::scoped_lock l (lock_);
                step_ = step;
            }
            sendChangeMessage();
        },
        cancel);

    std::optional<Store> store;
    if (result.outcome == InstallResult::Outcome::Installed)
    {
        download.file.deleteFile(); // installed: the cache keeps nothing
        // Every process that runs an older version now asks for a restart instead.
        store = updateStore (kStoreLockTimeoutMs, [&request] (Store& s) {
            s.installedVersion = request.version;
            return true;
        });
    }

    {
        const std::scoped_lock l (lock_);
        cancel_.reset();
        if (store)
            adopt (std::move (*store));
        switch (result.outcome)
        {
        case InstallResult::Outcome::Installed:
            store_.installedVersion = request.version; // also when update.json can't be written
            phase_ = Phase::Idle;
            break;
        case InstallResult::Outcome::Scheduled:
            scheduledVersion_ = request.version;
            scheduledRelaunch_ = request.relaunch;
            phase_ = Phase::Idle;
            break;
        case InstallResult::Outcome::Declined: // the user said no: ready to try again
        case InstallResult::Outcome::Cancelled:
            phase_ = Phase::Idle;
            break;
        case InstallResult::Outcome::Failed:
            phase_ = Phase::Error;
            error_ = result.error;
            detail_ = result.detail;
            break;
        }
    }
    sendChangeMessage();
}

} // namespace sedef::update
