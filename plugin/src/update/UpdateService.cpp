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

bool openInstaller (const juce::File& file, Platform platform)
{
    switch (platform)
    {
    case Platform::MacOS: // mounts the disk image and shows it, like `open`
        return file.startAsProcess();
    case Platform::Windows:
        // Interactive, never with /SILENT: the wizard asks for elevation, lists a DAW that holds
        // the plug-in and offers to close it, which a silent install would do unasked.
        return file.startAsProcess();
    case Platform::Linux: // an archive: show it; install.sh and its README do the rest
        file.revealToUser();
        return true;
    }
    return false;
}
} // namespace

Service::Service (juce::File store, juce::String apiUrl, juce::String releasesUrl,
                  juce::File downloads)
    : juce::Thread ("Sedef updates"),
      file_ (std::move (store)),
      releasesUrl_ (std::move (releasesUrl)),
      downloads_ (std::move (downloads)),
      platform_ (currentPlatform()),
      client_ (SEDEF_VERSION, platform_, std::move (apiUrl), releasesUrl_)
{
}

Service::~Service()
{
    cancelPendingUpdate();
    {
        // The worker makes cancel_ in the same step as it takes a job, so a job is either still
        // pending here or already cancellable.
        const std::scoped_lock l (lock_);
        pending_ = Job::None;
        if (cancel_ != nullptr)
            cancel_->cancel();
    }
    signalThreadShouldExit();
    notify();
    // A cancelled request returns at once; the margin only covers a stack that ignores cancel().
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

// State -------------------------------------------------------------------------------------------

juce::var Service::state()
{
    static constexpr const char* kPhases[] = { "idle",      "checking", "downloading",
                                               "verifying", "done",     "error" };
    const auto current = Version::parse (SEDEF_VERSION).value_or (Version {});

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
        available = isNewer (r, current);
    }
    const bool transferring = phase_ == Phase::Downloading || phase_ == Phase::Verifying;

    return object (
        { { "supported", true },
          { "currentVersion", SEDEF_VERSION },
          { "platform", platformName (platform_) },
          { "autoCheck", store_.autoCheck },
          { "autoCheckAllowed", automaticChecksAllowed() },
          { "skippedVersion", store_.skippedVersion },
          { "lastChecked",
            store_.lastSuccessMs > 0 ? juce::var (store_.lastSuccessMs) : juce::var() },
          { "phase", kPhases[static_cast<int> (phase_)] },
          { "latest", latest },
          { "updateAvailable", available },
          { "error", error_ == Error::None
                         ? juce::var()
                         : object ({ { "code", toString (error_) }, { "detail", detail_ } }) },
          { "progress", transferring ? object ({ { "received", received_ }, { "total", total_ } })
                                     : juce::var() },
          { "download",
            installer_ == juce::File()
                ? juce::var()
                : object ({ { "fileName", installer_.getFileName() }, { "opened", opened_ } }) },
          { "releasesUrl", releasesUrl_ } });
}

// Requests from the UI (message thread) -----------------------------------------------------------

bool Service::isBusy() const noexcept
{
    return phase_ == Phase::Checking || phase_ == Phase::Downloading || phase_ == Phase::Verifying;
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
    // Another process may have checked, or changed a setting, since this one last looked. The
    // file is replaced by renaming, so reading it needs no lock.
    auto store = readStore (file_, releasesUrl_);
    bool start = false;
    {
        const std::scoped_lock l (lock_);
        adopt (std::move (store));
        start = startAutomaticCheckIfDue();
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

void Service::download()
{
    bool start = false;
    {
        const std::scoped_lock l (lock_);
        if (isBusy() || !store_.latest)
            return;
        error_ = Error::None;
        detail_.clear();
        installer_ = juce::File();
        opened_ = false;
        if (const auto* asset = selectInstaller (*store_.latest, platform_, isX64()))
        {
            phase_ = Phase::Downloading;
            received_ = 0;
            total_ = asset->size;
            pending_ = Job::Download;
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
        file = installer_;
    }
    if (file.existsAsFile())
        file.revealToUser();
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
    const bool ok = openInstaller (file, platform_);
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
        bool unsaved = false;
        std::shared_ptr<Cancellation> cancel;
        {
            const std::scoped_lock l (lock_);
            job = std::exchange (pending_, Job::None);
            unsaved = unsaved_.any();
            if (job != Job::None)
                cancel_ = cancel = std::make_shared<Cancellation>();
        }
        if (unsaved)
            saveSettings (kStoreLockTimeoutMs);
        switch (job)
        {
        case Job::None:
            if (!unsaved)
                wait (-1);
            break;
        case Job::Check:
            runCheck (false, *cancel);
            break;
        case Job::ForcedCheck:
            runCheck (true, *cancel);
            break;
        case Job::Download:
            runDownload (*cancel);
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

    {
        const std::scoped_lock l (lock_);
        cancel_.reset();
        if (store)
            adopt (std::move (*store));
        const bool failed = result.error != Error::None && result.error != Error::Cancelled;
        phase_ = failed ? Phase::Error : Phase::Idle;
        error_ = failed ? result.error : Error::None;
        detail_ = failed ? result.detail : juce::String();
    }
    sendChangeMessage();
}

void Service::runDownload (Cancellation& cancel)
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
        result = client_.downloadVerified (
            *release, *asset, downloads_ == juce::File() ? downloadsFolder() : downloads_,
            onProgress, cancel);
    }

    {
        const std::scoped_lock l (lock_);
        cancel_.reset();
        switch (result.error)
        {
        case Error::None:
            phase_ = Phase::Done;
            installer_ = result.file;
            toOpen_ = result.file;
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
            phase_ = Phase::Error;
            error_ = result.error;
            detail_ = result.detail;
            break;
        }
    }
    if (result.error == Error::None)
        triggerAsyncUpdate(); // opens it on the message thread
    sendChangeMessage();
}

} // namespace sedef::update
