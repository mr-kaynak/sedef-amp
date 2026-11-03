// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Installer.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: windows.h comes first, the headers after it need it
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
// clang-format on
#if defined(_MSC_VER)
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#endif
#else
#include <sys/stat.h>
#endif

namespace sedef::update
{
namespace
{
constexpr int kAttachTimeoutMs = 120000; // hdiutil checks the image's own checksums first
constexpr int kDetachTimeoutMs = 20000;
constexpr int kClosingDetachTimeoutMs = 4000; // the service is closing: never hold it up long
constexpr int kUserInstallTimeoutMs = 300000;

InstallResult outcome (InstallResult::Outcome o)
{
    InstallResult r;
    r.outcome = o;
    return r;
}

InstallResult failure (Error error, juce::String detail)
{
    InstallResult r;
    r.outcome = InstallResult::Outcome::Failed;
    r.error = error;
    r.detail = std::move (detail);
    return r;
}

/// Maps what the OS's prompt answered to an outcome; nullopt when the command ran and succeeded.
std::optional<InstallResult> fromElevated (const Elevated& e, Cancellation& cancel)
{
    if (cancel.isCancelled())
        return outcome (InstallResult::Outcome::Cancelled);
    switch (e.status)
    {
    case Elevated::Status::Ok:
        return std::nullopt;
    case Elevated::Status::Declined:
        return outcome (InstallResult::Outcome::Declined);
    case Elevated::Status::Unavailable:
        return failure (Error::ElevationFailed, e.process.summary());
    case Elevated::Status::Failed:
        break;
    }
    return failure (Error::InstallFailed, e.process.summary());
}

/// Runs `body` when it goes out of scope, unless dismissed first.
template <typename Body>
class Finally
{
public:
    explicit Finally (Body body) : body_ (std::move (body)) {}
    ~Finally()
    {
        if (armed_)
            body_();
    }
    Finally (const Finally&) = delete;
    Finally& operator= (const Finally&) = delete;
    void run()
    {
        if (std::exchange (armed_, false))
            body_();
    }

private:
    Body body_;
    bool armed_ = true;
};

bool isRegularFile (const juce::File& file)
{
#if JUCE_WINDOWS
    return file.existsAsFile();
#else
    struct stat info {};
    return lstat (file.getFullPathName().toRawUTF8(), &info) == 0 && S_ISREG (info.st_mode);
#endif
}

[[maybe_unused]] Elevated osascriptElevate (const juce::StringArray& command,
                                            const juce::String& prompt, Cancellation& cancel)
{
    Elevated e;
    // No timeout: the password dialog waits for the user.
    e.process =
        runProcess (osascriptArgv (command, prompt), 0, [&cancel] { return cancel.isCancelled(); });
    if (!e.process.started)
        e.status = Elevated::Status::Unavailable;
    else if (e.process.killed)
        e.status = Elevated::Status::Failed;
    else if (e.process.exitCode == 0)
        e.status = Elevated::Status::Ok;
    else if (const auto n = appleScriptErrorNumber (e.process.errors);
             n && *n == kAppleScriptUserCancelled)
        e.status = Elevated::Status::Declined;
    else if (n && *n > 0 && *n < 256)
        e.status = Elevated::Status::Failed; // do shell script reports the command's exit status
    else
        e.status = Elevated::Status::Unavailable; // no authorization (a sandbox, a policy)
    return e;
}

[[maybe_unused]] Elevated pkexecElevate (const juce::StringArray& command, const juce::String&,
                                         Cancellation& cancel)
{
    Elevated e;
    juce::StringArray argv { "/usr/bin/pkexec", "--disable-internal-agent" };
    argv.addArray (command);
    e.process = runProcess (argv, 0, [&cancel] { return cancel.isCancelled(); });
    if (!e.process.started)
        e.status = Elevated::Status::Unavailable;
    else if (e.process.killed)
        e.status = Elevated::Status::Failed;
    else if (e.process.exitCode == 0)
        e.status = Elevated::Status::Ok;
    else if (e.process.exitCode == 126) // the user dismissed the authentication dialog
        e.status = Elevated::Status::Declined;
    else if (e.process.exitCode == 127) // not authorized, or no authentication agent
        e.status = Elevated::Status::Unavailable;
    else
        e.status = Elevated::Status::Failed;
    return e;
}

// macOS -------------------------------------------------------------------------------------------

class MacInstaller final : public Installer
{
public:
    explicit MacInstaller (InstallerConfig config) : config_ (std::move (config)) {}

    ManualReason manualReason() const override
    {
        // A sandboxed host (or its out-of-process plug-in service) can't ask for a password.
        if (juce::SystemStats::getEnvironmentVariable ("APP_SANDBOX_CONTAINER_ID", {}).isNotEmpty())
            return ManualReason::Sandboxed;
        if (macChoiceFor (config_.running, config_.root).isEmpty())
            return ManualReason::Elsewhere;
        return ManualReason::None;
    }

    InstallResult install (const InstallRequest& request, const StepCallback& step,
                           Cancellation& cancel) override
    {
        const auto cancelled = [&cancel] {
            return cancel.isCancelled();
        };
        step (InstallStep::Preparing);
        if (!matchesChecksum (request.file, request.expectedHash))
            return failure (Error::ChecksumMismatch,
                            request.file.getFileName() + " changed after it was verified");

        // 1. Mount the image read-only, out of sight, in the private cache.
        if (!makePrivateFolder (config_.cache.getChildFile ("mnt")))
            return failure (Error::Disk, "cannot create the mount folder");
        const auto mounts = realPath (config_.cache.getChildFile ("mnt")); // as hdiutil reports it
        const auto attach = runProcess ({ "/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly",
                                          "-noautoopen", "-mountrandom", mounts.getFullPathName(),
                                          "-plist", request.file.getFullPathName() },
                                        kAttachTimeoutMs, cancelled);
        if (cancel.isCancelled())
            return outcome (InstallResult::Outcome::Cancelled); // cleanUp() detaches it later
        if (!attach.succeeded())
            return failure (Error::BadPackage, "hdiutil attach: " + attach.summary());
        const auto mount = parseMountPoint (attach.output);
        if (mount.isEmpty() || !juce::File (mount).isAChildOf (mounts))
        {
            cleanUp();
            return failure (Error::BadPackage, "the disk image has no single volume");
        }

        // The elevated step may still run after a cancel (the password was already given): then
        // the image is only detached if nothing uses it, and cleanUp() does the rest later.
        bool gentle = false;
        Finally detach ([&mount, &gentle] { detachImage (mount, gentle); });

        // 2. The pkg, and the formats to install.
        const auto pkg = juce::File (mount).getChildFile (kMacPackageName);
        if (!isRegularFile (pkg))
            return failure (Error::BadPackage,
                            juce::String (kMacPackageName) + " is not a file in the disk image");
        const auto choices = macChoicesToInstall (config_.root, config_.running);
        const auto choicesFile = config_.cache.getChildFile ("choices.xml");
        if (!choicesFile.replaceWithText (choiceChangesXml (choices)))
            return failure (Error::Disk, "cannot write " + choicesFile.getFullPathName());
        Finally removeChoices ([&choicesFile] { choicesFile.deleteFile(); });

        // 3. Apple's installer, as root, once macOS has asked for the password.
        step (InstallStep::Permission);
        const auto elevated =
            elevate() (installerArgv (pkg, choicesFile), installPrompt (request.version), cancel);
        if (cancel.isCancelled())
            gentle = true;
        if (auto stop = fromElevated (elevated, cancel))
            return *stop;

        // 4. Detach, then check that every format now has the release's version.
        step (InstallStep::Finishing);
        detach.run();
        juce::StringArray wrong;
        for (const auto& c : kMacComponents)
            if (choices.contains (c.choice))
                if (const auto found = bundleShortVersion (macBundlePath (c, config_.root));
                    found != request.version)
                    wrong.add (juce::String (c.bundle) + " is " +
                               (found.isEmpty() ? juce::String ("missing") : found));
        if (!wrong.isEmpty())
            return failure (Error::Unverified, wrong.joinIntoString ("; "));
        return outcome (InstallResult::Outcome::Installed);
    }

    bool relaunchAfterExit() override
    {
        const auto app = macBundlePath (kMacComponents[0], config_.root);
        return app.isDirectory() &&
               spawnDetached (waitThenRunArgv (currentProcessId(),
                                               { "/usr/bin/open", app.getFullPathName() }));
    }

    void cleanUp() override
    {
        const auto mounts = realPath (config_.cache.getChildFile ("mnt"));
        if (mounts.findChildFiles (juce::File::findDirectories, false).isEmpty())
            return;
        const auto info = runProcess ({ "/usr/bin/hdiutil", "info", "-plist" }, kDetachTimeoutMs);
        for (const auto& point : mountPointsUnder (info.output, mounts))
            detachImage (point, false);
    }

private:
    Elevate elevate() const { return config_.elevate ? config_.elevate : systemElevate(); }

    static void detachImage (const juce::String& mount, bool gentle)
    {
        const int timeout = gentle ? kClosingDetachTimeoutMs : kDetachTimeoutMs;
        if (runProcess ({ "/usr/bin/hdiutil", "detach", mount }, timeout).succeeded() || gentle)
            return;
        runProcess ({ "/usr/bin/hdiutil", "detach", mount, "-force" }, timeout);
    }

    InstallerConfig config_;
};

// Linux -------------------------------------------------------------------------------------------

class LinuxInstaller final : public Installer
{
public:
    explicit LinuxInstaller (InstallerConfig config) : config_ (std::move (config)) {}

    ManualReason manualReason() const override
    {
        switch (scope())
        {
        case LinuxScope::User:
            return ManualReason::None;
        case LinuxScope::System:
            return config_.elevate || juce::File ("/usr/bin/pkexec").existsAsFile()
                       ? ManualReason::None
                       : ManualReason::NoElevation;
        case LinuxScope::Elsewhere:
            break;
        }
        return ManualReason::Elsewhere;
    }

    InstallResult install (const InstallRequest& request, const StepCallback& step,
                           Cancellation& cancel) override
    {
        step (InstallStep::Preparing);
        if (!matchesChecksum (request.file, request.expectedHash))
            return failure (Error::ChecksumMismatch,
                            request.file.getFileName() + " changed after it was verified");

        // 1. Unpack into a fresh private folder, refusing anything but plain files and folders.
        const auto work = config_.cache.getChildFile ("unpacked");
        work.deleteRecursively();
        if (!makePrivateFolder (work))
            return failure (Error::Disk, "cannot create " + work.getFullPathName());
        Finally removeWork ([&work] { work.deleteRecursively(); });
        juce::String error, top;
        if (!extractTarGz (request.file, work, error, &top))
            return failure (Error::BadPackage, error);
        if (cancel.isCancelled())
            return outcome (InstallResult::Outcome::Cancelled);
        const auto script = work.getChildFile (top).getChildFile ("install.sh");

        // 2. install.sh: as you in your own folders, through pkexec for the system's.
        const auto where = scope();
        if (where == LinuxScope::System)
        {
            step (InstallStep::Permission);
            const auto elevated = elevate() ({ script.getFullPathName(), "--system" },
                                             installPrompt (request.version), cancel);
            if (auto stop = fromElevated (elevated, cancel))
                return *stop;
        }
        else
        {
            step (InstallStep::Installing);
            const auto run = runProcess ({ script.getFullPathName() }, kUserInstallTimeoutMs,
                                         [&cancel] { return cancel.isCancelled(); });
            if (cancel.isCancelled())
                return outcome (InstallResult::Outcome::Cancelled);
            if (!run.succeeded())
                return failure (Error::InstallFailed, "install.sh: " + run.summary());
        }

        // 3. The VST3 install.sh wrote must carry the release's version.
        step (InstallStep::Finishing);
        const auto bundle = linuxVst3Bundle (where, config_.home);
        if (const auto found = vst3BundleVersion (bundle); found != request.version)
            return failure (Error::Unverified,
                            bundle.getFullPathName() + " is " +
                                (found.isEmpty() ? juce::String ("missing") : found));
        return outcome (InstallResult::Outcome::Installed);
    }

    bool relaunchAfterExit() override
    {
        const auto binary = linuxStandalone (scope(), config_.home);
        return binary.existsAsFile() &&
               spawnDetached (waitThenRunArgv (currentProcessId(), { binary.getFullPathName() }));
    }

private:
    LinuxScope scope() const
    {
        return config_.scope ? *config_.scope : linuxScopeOf (config_.running, config_.home);
    }
    Elevate elevate() const { return config_.elevate ? config_.elevate : systemElevate(); }

    InstallerConfig config_;
};

// Windows -----------------------------------------------------------------------------------------

class WindowsInstaller final : public Installer
{
public:
    explicit WindowsInstaller (InstallerConfig config) : config_ (std::move (config)) {}

    ManualReason manualReason() const override
    {
        return windowsSetupUpdates (config_.running, config_.commonFiles) ? ManualReason::None
                                                                          : ManualReason::Elsewhere;
    }

    InstallResult install (const InstallRequest& request, const StepCallback& step,
                           Cancellation& cancel) override
    {
        step (InstallStep::Preparing);
        if (!matchesChecksum (request.file, request.expectedHash))
            return failure (Error::ChecksumMismatch,
                            request.file.getFileName() + " changed after it was verified");
        if (cancel.isCancelled())
            return outcome (InstallResult::Outcome::Cancelled);
        // The host keeps the plug-in loaded and locked, so Setup waits for it to exit.
        step (InstallStep::Permission);
        const auto pid =
            config_.waitPid != 0 ? config_.waitPid : static_cast<juce::uint32> (currentProcessId());
        auto parameters = windowsSetupParameters (pid, request.relaunch);
        if (config_.extraSetupParameters.isNotEmpty())
            parameters << " " << config_.extraSetupParameters;
        const auto launch =
            launchElevated (request.file, parameters, request.window, config_.keepProcess);
        switch (launch.status)
        {
        case Elevated::Status::Ok:
        {
            auto r = outcome (InstallResult::Outcome::Scheduled);
            r.process = launch.process;
            return r;
        }
        case Elevated::Status::Declined:
            return outcome (InstallResult::Outcome::Declined);
        case Elevated::Status::Unavailable:
        case Elevated::Status::Failed:
            break;
        }
        return failure (Error::ElevationFailed, launch.detail);
    }

    // Setup opens the app itself when it was started with /RELAUNCH (sedef-amp.iss).
    bool relaunchAfterExit() override { return true; }

private:
    InstallerConfig config_;
};
} // namespace

juce::String toString (InstallStep step)
{
    switch (step)
    {
    case InstallStep::Preparing:
        return "preparing";
    case InstallStep::Permission:
        return "permission";
    case InstallStep::Installing:
        return "installing";
    case InstallStep::Finishing:
        return "finishing";
    }
    return {};
}

juce::String toString (ManualReason reason)
{
    switch (reason)
    {
    case ManualReason::None:
        return {};
    case ManualReason::Elsewhere:
        return "elsewhere";
    case ManualReason::Sandboxed:
        return "sandboxed";
    case ManualReason::NoElevation:
        return "noElevation";
    case ManualReason::Unsupported:
        return "unsupported";
    }
    return {};
}

Elevate systemElevate()
{
#if JUCE_MAC
    return osascriptElevate;
#elif JUCE_LINUX || JUCE_BSD
    return pkexecElevate;
#else
    return [] (const juce::StringArray&, const juce::String&, Cancellation&) {
        Elevated e;
        e.status = Elevated::Status::Unavailable;
        return e;
    };
#endif
}

InstallerConfig defaultInstallerConfig()
{
    InstallerConfig c;
    c.cache = updateCacheFolder();
    c.root = juce::File ("/");
    c.home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
#if JUCE_MAC
    c.running = juce::File::getSpecialLocation (juce::File::currentApplicationFile); // the bundle
#else
    c.running = juce::File::getSpecialLocation (juce::File::currentExecutableFile); // the module
#endif
#if JUCE_WINDOWS
    for (const auto* name : { "CommonProgramW6432", "CommonProgramFiles" })
        if (const auto path = juce::SystemStats::getEnvironmentVariable (name, {});
            juce::File::isAbsolutePath (path))
        {
            c.commonFiles = juce::File (path);
            break;
        }
#endif
    return c;
}

std::unique_ptr<Installer> makeInstaller (Platform platform, InstallerConfig config)
{
    switch (platform)
    {
    case Platform::MacOS:
        return std::make_unique<MacInstaller> (std::move (config));
    case Platform::Windows:
        return std::make_unique<WindowsInstaller> (std::move (config));
    case Platform::Linux:
        break;
    }
    return std::make_unique<LinuxInstaller> (std::move (config));
}

std::unique_ptr<Installer> makeSystemInstaller (InstallerConfig config)
{
    return makeInstaller (currentPlatform(), std::move (config));
}

#if JUCE_WINDOWS

LaunchResult launchElevated (const juce::File& setup, const juce::String& parameters, void* window,
                             bool keepProcess)
{
    LaunchResult r;
    // ShellExecuteEx wants COM; this runs on the service's own thread.
    const auto com = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const auto path = setup.getFullPathName();
    SHELLEXECUTEINFOW info {};
    info.cbSize = sizeof (info);
    // NOZONECHECKS: the cache copy has no Mark of the Web and Sedef verified it.
    info.fMask =
        SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOZONECHECKS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.hwnd = static_cast<HWND> (window);
    info.lpVerb = L"runas";
    info.lpFile = path.toWideCharPointer();
    info.lpParameters = parameters.toWideCharPointer();
    info.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW (&info) != FALSE;
    const auto error = GetLastError();
    if (SUCCEEDED (com))
        CoUninitialize();
    if (!ok)
    {
        r.status =
            error == ERROR_CANCELLED ? Elevated::Status::Declined : Elevated::Status::Unavailable;
        r.detail = "ShellExecuteEx failed with error " + juce::String (static_cast<int> (error));
        return r;
    }
    r.status = Elevated::Status::Ok;
    if (keepProcess)
        r.process = info.hProcess;
    else if (info.hProcess != nullptr)
        CloseHandle (info.hProcess);
    return r;
}

int waitForProcess (void* process, int timeoutMs)
{
    if (process == nullptr)
        return -1;
    if (WaitForSingleObject (static_cast<HANDLE> (process),
                             timeoutMs > 0 ? static_cast<DWORD> (timeoutMs) : INFINITE) !=
        WAIT_OBJECT_0)
        return -1;
    DWORD code = 0;
    return GetExitCodeProcess (static_cast<HANDLE> (process), &code) ? static_cast<int> (code) : -1;
}

void closeProcess (void* process)
{
    if (process != nullptr)
        CloseHandle (static_cast<HANDLE> (process));
}

void* foregroundWindow()
{
    return GetForegroundWindow();
}

#else

LaunchResult launchElevated (const juce::File&, const juce::String&, void*, bool)
{
    LaunchResult r;
    r.status = Elevated::Status::Unavailable;
    r.detail = "Setup programs run on Windows only";
    return r;
}

int waitForProcess (void*, int)
{
    return -1;
}

void closeProcess (void*) {}

void* foregroundWindow()
{
    return nullptr;
}

#endif

} // namespace sedef::update
