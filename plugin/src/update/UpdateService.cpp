// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UpdateService.h"

namespace sedef::update
{
namespace
{
constexpr juce::uint32 kProgressMessageIntervalMs = 100; // the UI hears at most ~10 per second

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
    case Platform::MacOS:   // mounts the disk image and shows it, like `open`
    case Platform::Windows: // runs the setup program, which asks for elevation itself
        return file.startAsProcess();
    case Platform::Linux: // an archive: show it; its README explains the install
        file.revealToUser();
        return true;
    }
    return false;
}
} // namespace

Service::Service (juce::File store, juce::String apiUrl)
    : juce::Thread ("Sedef updates"),
      file_ (std::move (store)),
      platform_ (currentPlatform()),
      client_ (SEDEF_VERSION, platform_, std::move (apiUrl))
{
}

Service::~Service()
{
    cancelPendingUpdate();
    {
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
          { "releasesUrl", releasesPageUrl() } });
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
    // Another process may have checked, or changed a setting, since this one last looked.
    auto store = readStore (file_);
    bool start = false;
    {
        const std::scoped_lock l (lock_);
        store_ = std::move (store);
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
    auto store = updateStore (500, [enabled] (Store& s) {
        s.autoCheck = enabled;
        return true;
    });
    bool start = false;
    {
        const std::scoped_lock l (lock_);
        store_ = std::move (store);
        start = startAutomaticCheckIfDue();
    }
    if (start)
        wake();
    sendChangeMessage();
}

void Service::skip (const juce::String& version)
{
    const auto parsed = Version::parse (version);
    const auto value = parsed ? parsed->toString() : juce::String();
    auto store = updateStore (500, [&value] (Store& s) {
        s.skippedVersion = value;
        return true;
    });
    {
        const std::scoped_lock l (lock_);
        store_ = std::move (store);
    }
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
        {
            const std::scoped_lock l (lock_);
            job = std::exchange (pending_, Job::None);
        }
        switch (job)
        {
        case Job::None:
            wait (-1);
            break;
        case Job::Check:
            runCheck (false);
            break;
        case Job::ForcedCheck:
            runCheck (true);
            break;
        case Job::Download:
            runDownload();
            break;
        }
    }
}

template <typename Modify>
Store Service::updateStore (int timeoutMs, Modify&& modify)
{
    const std::scoped_lock l (fileMutex_);
    // Best effort across processes: the write itself is atomic either way.
    const bool locked = fileLock_.enter (timeoutMs);
    auto store = readStore (file_);
    if (modify (store))
        writeStore (file_, store);
    if (locked)
        fileLock_.exit();
    return store;
}

void Service::runCheck (bool forced)
{
    auto cancel = std::make_shared<Cancellation>();
    {
        const std::scoped_lock l (lock_);
        cancel_ = cancel;
    }

    // Claim the check before the request, so other processes that open an editor meanwhile
    // see a recent attempt and skip theirs.
    const auto now = juce::Time::currentTimeMillis();
    bool due = true;
    auto store = updateStore (2000, [&] (Store& s) {
        due = forced || isCheckDue (now, s.lastAttemptMs, s.lastSuccessMs);
        if (due)
            s.lastAttemptMs = now;
        return due;
    });

    CheckResult result;
    if (due)
    {
        result = client_.fetchLatest (*cancel);
        if (result.error == Error::None)
            store = updateStore (2000, [&result] (Store& s) {
                s.lastSuccessMs = juce::Time::currentTimeMillis();
                s.latest = result.release;
                return true;
            });
    }

    {
        const std::scoped_lock l (lock_);
        cancel_.reset();
        store_ = std::move (store);
        const bool failed = result.error != Error::None && result.error != Error::Cancelled;
        phase_ = failed ? Phase::Error : Phase::Idle;
        error_ = failed ? result.error : Error::None;
        detail_ = failed ? result.detail : juce::String();
    }
    sendChangeMessage();
}

void Service::runDownload()
{
    auto cancel = std::make_shared<Cancellation>();
    std::optional<Release> release;
    {
        const std::scoped_lock l (lock_);
        cancel_ = cancel;
        release = store_.latest;
    }

    DownloadResult result;
    const auto* asset = release ? selectInstaller (*release, platform_, isX64()) : nullptr;
    if (asset == nullptr)
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
        result =
            client_.downloadVerified (*release, *asset, downloadsFolder(), onProgress, *cancel);
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
