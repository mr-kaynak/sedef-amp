// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Installer.h"

#include <juce_cryptography/juce_cryptography.h>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: windows.h comes first, the headers after it need it
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
// clang-format on
#include <cwchar>
#include <mutex>
#include <string>
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
constexpr int kUserInstallTimeoutMs = 300000;
constexpr juce::int64 kMaxArchiveBytes = juce::int64 { 512 } << 20;

InstallResult outcome (InstallResult::Outcome o, juce::String detail = {})
{
    InstallResult r;
    r.outcome = o;
    r.detail = std::move (detail);
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

/// A stopped wait: the service is closing. What runs as root by then finishes on its own.
InstallResult cancelled (const ProcessResult& process)
{
    return outcome (InstallResult::Outcome::Cancelled,
                    process.abandoned ? "the install goes on and finishes by itself" : "");
}

/// What the install script's exit status means (ScriptExit).
InstallResult fromScript (int exitCode, const juce::String& detail)
{
    switch (exitCode)
    {
    case static_cast<int> (ScriptExit::Checksum):
        return failure (Error::ChecksumMismatch,
                        "the copy taken for the install changed: " + detail);
    case static_cast<int> (ScriptExit::Unpack):
    case static_cast<int> (ScriptExit::NoInstaller):
        return failure (Error::BadPackage, detail);
    default:
        break;
    }
    return failure (Error::InstallFailed, detail);
}

/// Maps what the OS's prompt answered to an outcome; nullopt when the command ran and succeeded.
std::optional<InstallResult> fromElevated (const Elevated& e, Cancellation& cancel)
{
    if (cancel.isCancelled())
        return cancelled (e.process);
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
    return fromScript (e.exitCode, e.process.summary());
}

[[maybe_unused]] Elevated osascriptElevate (const juce::StringArray& command,
                                            const juce::String& prompt, Cancellation& cancel)
{
    Elevated e;
    // No timeout: the password dialog waits for the user. A cancel takes the dialog away; once
    // the password is given, what runs as root goes on (InstallProcess.h).
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
    {
        e.status = Elevated::Status::Failed; // do shell script reports the command's exit status
        e.exitCode = *n;
    }
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
    {
        e.status = Elevated::Status::Failed;
        e.exitCode = e.process.exitCode;
    }
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
        // A quick look first, so a broken file never gets as far as the password dialog. The
        // script checks its own copy again; that check is the one that counts.
        step (InstallStep::Preparing);
        if (!matchesChecksum (request.file, request.expectedHash))
            return failure (Error::ChecksumMismatch,
                            request.file.getFileName() + " changed after it was verified");
        if (cancel.isCancelled())
            return outcome (InstallResult::Outcome::Cancelled);

        // The formats come from the bundles on disk, so nothing needs mounting before then.
        const auto choices = macChoicesToInstall (config_.root, config_.running);
        step (InstallStep::Permission);
        const auto elevated = elevate() (
            macInstallCommand (request.file, request.expectedHash, choiceChangesXml (choices)),
            installPrompt (request.version), cancel);
        if (auto stop = fromElevated (elevated, cancel))
            return *stop;

        // Every format it installed must now have the release's version.
        step (InstallStep::Finishing);
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

private:
    Elevate elevate() const { return config_.elevate ? config_.elevate : systemElevate(); }

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
        const auto isCancelled = [&cancel] {
            return cancel.isCancelled();
        };

        // 1. Check the archive from the very bytes that were hashed: only plain files and
        //    folders, inside one top folder, with what install.sh needs. The script then installs
        //    from its own copy only if that copy has this hash, so it unpacks what was checked.
        step (InstallStep::Preparing);
        juce::MemoryBlock bytes;
        if (request.file.getSize() > kMaxArchiveBytes || !request.file.loadFileAsData (bytes))
            return failure (Error::BadPackage, "cannot read " + request.file.getFullPathName());
        if (juce::SHA256 (bytes).toHexString().toLowerCase() != request.expectedHash.toLowerCase())
            return failure (Error::ChecksumMismatch,
                            request.file.getFileName() + " changed after it was verified");
        juce::MemoryInputStream in (bytes, false);
        const auto listing = listTarGz (in, isCancelled);
        if (cancel.isCancelled())
            return outcome (InstallResult::Outcome::Cancelled);
        if (listing.error.isNotEmpty())
            return failure (Error::BadPackage, listing.error);
        juce::String error;
        const auto top = checkLinuxArchive (listing.entries, error);
        if (top.isEmpty())
            return failure (Error::BadPackage, error);

        // 2. The script: as root through pkexec for the system's folders, as you for your own.
        const auto where = scope();
        const auto command = linuxInstallCommand (request.file, request.expectedHash, where);
        if (where == LinuxScope::System)
        {
            step (InstallStep::Permission);
            const auto elevated = elevate() (command, installPrompt (request.version), cancel);
            if (auto stop = fromElevated (elevated, cancel))
                return *stop;
        }
        else
        {
            // Half an install is worse than a late one: a cancel stops the wait, not install.sh.
            step (InstallStep::Installing);
            const auto run =
                runProcess (command, kUserInstallTimeoutMs, isCancelled, OnCancel::LetFinish);
            if (cancel.isCancelled())
                return cancelled (run);
            if (!run.succeeded())
                return run.killed ? failure (Error::InstallFailed, "install.sh did not finish")
                                  : fromScript (run.exitCode, run.summary());
        }

        // 3. Every file it copied is the archive's, and the VST3 says the release's version.
        step (InstallStep::Finishing);
        juce::StringArray wrong;
        for (const auto& f : linuxInstalledFiles (listing.entries, top, where, config_.home))
        {
            if (sha256Hex (f.file) != f.sha256)
                wrong.add (f.file.getFullPathName() +
                           (f.file.existsAsFile() ? " differs from the release" : " is missing"));
            else if (where == LinuxScope::System && !everyoneReads (f.file))
                wrong.add (f.file.getFullPathName() + " is not readable by every user");
        }
        const auto bundle = linuxVst3Bundle (where, config_.home);
        if (const auto found = vst3BundleVersion (bundle); found != request.version)
            wrong.insert (0, bundle.getFullPathName() + " is " +
                                 (found.isEmpty() ? juce::String ("missing") : found));
        if (!wrong.isEmpty())
            return failure (Error::Unverified, wrong.joinIntoString ("; "));
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

    /// A system-wide install is for every user: its files, and the folder each is in.
    static bool everyoneReads ([[maybe_unused]] const juce::File& file)
    {
#if JUCE_WINDOWS
        return true;
#else
        struct stat f {}, folder {};
        return stat (file.getFullPathName().toRawUTF8(), &f) == 0 && (f.st_mode & S_IROTH) != 0 &&
               stat (file.getParentDirectory().getFullPathName().toRawUTF8(), &folder) == 0 &&
               (folder.st_mode & S_IXOTH) != 0;
#endif
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
        return windowsSetupUpdates (config_.running, config_.commonFiles, config_.appFolder)
                   ? ManualReason::None
                   : ManualReason::Elsewhere;
    }

    InstallResult install (const InstallRequest& request, const StepCallback& step,
                           Cancellation& cancel) override
    {
        // Held from the check on: nobody can change, move or delete the file until this process
        // exits, and by then Setup holds it itself (sedef-amp.iss, InitializeSetup).
        step (InstallStep::Preparing);
        HeldFile held (request.file);
        if (!held.isOpen())
            return failure (Error::Disk, "cannot open " + request.file.getFullPathName());
        if (held.sha256() != request.expectedHash.toLowerCase())
            return failure (Error::ChecksumMismatch,
                            request.file.getFileName() + " changed after it was verified");
        held.keepUntilExit();
        if (cancel.isCancelled())
            return outcome (InstallResult::Outcome::Cancelled);

        // The host keeps the plug-in loaded and locked, so Setup waits for it to exit.
        step (InstallStep::Permission);
        const auto pid =
            config_.waitPid != 0 ? config_.waitPid : static_cast<juce::uint32> (currentProcessId());
        const auto created =
            config_.waitPidCreated != 0 ? config_.waitPidCreated : processCreationTime (pid);
        auto parameters = windowsSetupParameters (
            pid, created,
            windowsComponentsToInstall (config_.commonFiles, config_.appFolder, config_.running),
            request.relaunch);
        if (config_.extraSetupParameters.isNotEmpty())
            parameters << " " << config_.extraSetupParameters;
        const auto launch =
            launchElevated (request.file, parameters, request.window, config_.keepProcess,
                            [&cancel] { return cancel.isCancelled(); });
        if (launch.cancelled)
            return outcome (InstallResult::Outcome::Cancelled,
                            "if the prompt is answered with Yes later, Setup runs");
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

    // Reached when no Setup from this process reopens the app (UpdateService::prepareRestart):
    // another process installed the update.
    bool relaunchAfterExit() override
    {
        const auto app = config_.appFolder.getChildFile ("Sedef Amp.exe");
        return config_.appFolder != juce::File() && app.existsAsFile() &&
               openAfterExit (static_cast<juce::uint32> (currentProcessId()), app);
    }

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
    c.root = juce::File ("/");
    c.home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
#if JUCE_MAC
    c.running = juce::File::getSpecialLocation (juce::File::currentApplicationFile); // the bundle
#else
    c.running = juce::File::getSpecialLocation (juce::File::currentExecutableFile); // the module
#endif
#if JUCE_WINDOWS
    const auto folder = [] (std::initializer_list<const char*> names) {
        for (const auto* name : names)
            if (const auto path = juce::SystemStats::getEnvironmentVariable (name, {});
                juce::File::isAbsolutePath (path))
                return juce::File (path);
        return juce::File();
    };
    c.commonFiles = folder ({ "CommonProgramW6432", "CommonProgramFiles" });
    // Where Setup put the app: its uninstall entry says, else the default ({autopf}\Sedef Amp).
    const auto location = juce::WindowsRegistry::getValue (
        juce::String (
            "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") +
            kWindowsAppId + "_is1\\InstallLocation",
        {}, juce::WindowsRegistry::WoW64_64bit);
    if (juce::File::isAbsolutePath (location.trim()))
        c.appFolder = juce::File (location.trim());
    else if (const auto programs = folder ({ "ProgramW6432", "ProgramFiles" });
             programs != juce::File())
        c.appFolder = programs.getChildFile ("Sedef Amp");
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

namespace
{
/// One elevated launch, shared by the thread that waits in ShellExecuteEx and the one that waits
/// for it; whichever is last frees it.
struct LaunchJob
{
    std::wstring file, parameters;
    HWND window = nullptr;
    bool keepProcess = false;
    std::mutex lock;
    bool done = false, abandoned = false;
    LaunchResult result;
};

struct ThreadStart
{
    HMODULE module;
    std::shared_ptr<LaunchJob>* job;
};

void runLaunch (std::shared_ptr<LaunchJob>* holder)
{
    const std::shared_ptr<LaunchJob> job (std::move (*holder));
    delete holder;
    LaunchResult r;
    // ShellExecuteEx wants COM.
    const auto com = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW info {};
    info.cbSize = sizeof (info);
    // NOZONECHECKS: the cache copy has no Mark of the Web and Sedef verified it. NOASYNC: this
    // thread waits for the prompt.
    info.fMask =
        SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOZONECHECKS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.hwnd = job->window;
    info.lpVerb = L"runas";
    info.lpFile = job->file.c_str();
    info.lpParameters = job->parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW (&info) != FALSE;
    const auto error = GetLastError();
    if (SUCCEEDED (com))
        CoUninitialize();
    if (ok)
    {
        r.status = Elevated::Status::Ok;
        r.process = info.hProcess;
    }
    else
    {
        r.status =
            error == ERROR_CANCELLED ? Elevated::Status::Declined : Elevated::Status::Unavailable;
        r.detail = "ShellExecuteEx failed with error " + juce::String (static_cast<int> (error));
    }

    const std::scoped_lock l (job->lock);
    job->done = true;
    if ((job->abandoned || !job->keepProcess) && r.process != nullptr)
    {
        CloseHandle (static_cast<HANDLE> (r.process));
        r.process = nullptr;
    }
    job->result = std::move (r);
}

DWORD WINAPI launchThread (void* parameter)
{
    const auto* start = static_cast<ThreadStart*> (parameter);
    const auto module = start->module;
    auto* job = start->job;
    delete start;
    runLaunch (job); // every C++ object is gone when it returns
    if (module != nullptr)
        FreeLibraryAndExitThread (module, 0); // the plug-in may have been unloaded meanwhile
    return 0;
}
} // namespace

LaunchResult launchElevated (const juce::File& setup, const juce::String& parameters, void* window,
                             bool keepProcess, const std::function<bool()>& cancelled)
{
    auto job = std::make_shared<LaunchJob>();
    job->file = setup.getFullPathName().toWideCharPointer();
    job->parameters = parameters.toWideCharPointer();
    job->window = static_cast<HWND> (window);
    job->keepProcess = keepProcess;

    // The thread keeps this module loaded until it is done, even if the host unloads the plug-in.
    HMODULE module = nullptr;
    GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                        reinterpret_cast<LPCWSTR> (&launchThread), &module);
    auto* start = new ThreadStart { module, new std::shared_ptr<LaunchJob> (job) };
    if (const auto thread = CreateThread (nullptr, 0, launchThread, start, 0, nullptr))
    {
        CloseHandle (thread);
    }
    else
    {
        delete start->job;
        delete start;
        if (module != nullptr)
            FreeLibrary (module);
        LaunchResult r;
        r.status = Elevated::Status::Unavailable;
        r.detail = "cannot start a thread";
        return r;
    }

    for (;;)
    {
        {
            const std::scoped_lock l (job->lock);
            if (job->done)
                return job->result;
            if (cancelled && cancelled())
            {
                job->abandoned = true; // the thread closes Setup's handle, if it gets one
                LaunchResult r;
                r.cancelled = true;
                return r;
            }
        }
        Sleep (20);
    }
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

HeldFile::HeldFile (const juce::File& file)
{
    const auto h = CreateFileW (file.getFullPathName().toWideCharPointer(), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h != INVALID_HANDLE_VALUE)
        handle_ = h;
}

HeldFile::~HeldFile()
{
    if (handle_ != nullptr)
        CloseHandle (static_cast<HANDLE> (handle_));
}

juce::String HeldFile::sha256()
{
    if (handle_ == nullptr)
        return {};
    // Reads through this handle, from the start.
    class Reader final : public juce::InputStream
    {
    public:
        explicit Reader (HANDLE h) : h_ (h) {}
        juce::int64 getTotalLength() override
        {
            LARGE_INTEGER size {};
            return GetFileSizeEx (h_, &size) ? size.QuadPart : -1;
        }
        bool isExhausted() override { return end_; }
        juce::int64 getPosition() override { return position_; }
        // BufferedInputStream sets the position before every read it makes.
        bool setPosition (juce::int64 position) override
        {
            LARGE_INTEGER to {};
            to.QuadPart = position;
            if (!SetFilePointerEx (h_, to, nullptr, FILE_BEGIN))
                return false;
            position_ = position;
            end_ = false;
            return true;
        }
        int read (void* buffer, int bytes) override
        {
            DWORD got = 0;
            if (bytes <= 0 || !ReadFile (h_, buffer, static_cast<DWORD> (bytes), &got, nullptr) ||
                got == 0)
            {
                end_ = true;
                return 0;
            }
            position_ += got;
            return static_cast<int> (got);
        }

    private:
        HANDLE h_;
        juce::int64 position_ = 0;
        bool end_ = false;
    };
    Reader reader (static_cast<HANDLE> (handle_));
    if (!reader.setPosition (0))
        return {};
    juce::BufferedInputStream buffered (reader, 1 << 16);
    return juce::SHA256 (buffered).toHexString().toLowerCase();
}

void HeldFile::keepUntilExit()
{
    handle_ = nullptr; // never closed: the process's exit closes it
}

juce::uint64 processCreationTime (juce::uint32 pid)
{
    const auto process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
        return 0;
    FILETIME created {}, exited {}, kernel {}, user {};
    const bool ok = GetProcessTimes (process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle (process);
    return ok ? (static_cast<juce::uint64> (created.dwHighDateTime) << 32) | created.dwLowDateTime
              : 0;
}

bool openAfterExit (juce::uint32 pid, const juce::File& app)
{
    wchar_t system[MAX_PATH] {};
    if (GetSystemDirectoryW (system, MAX_PATH) == 0)
        return false;
    const auto powershell =
        juce::File (juce::String (system)).getChildFile ("WindowsPowerShell\\v1.0\\powershell.exe");
    if (!powershell.existsAsFile())
        return false;
    // Constant; the values come from the environment, never from the command line.
    const juce::String script =
        "$p = [int]$env:SEDEF_WAIT_PID; "
        "try { Wait-Process -Id $p -Timeout 60 -ErrorAction Stop } catch {}; "
        "Start-Process -FilePath $env:SEDEF_OPEN";
    juce::MemoryOutputStream utf16;
    for (auto p = script.getCharPointer(); !p.isEmpty(); ++p)
    {
        const auto c = static_cast<juce::uint32> (*p);
        utf16.writeByte (static_cast<char> (c & 0xff));
        utf16.writeByte (static_cast<char> ((c >> 8) & 0xff));
    }
    const auto encoded = juce::Base64::toBase64 (utf16.getData(), utf16.getDataSize());
    auto command = "\"" + powershell.getFullPathName() +
                   "\" -NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand " + encoded;

    // This process's environment, plus the two values.
    std::wstring environment;
    if (auto* block = GetEnvironmentStringsW())
    {
        for (const wchar_t* p = block; *p != 0; p += wcslen (p) + 1)
            if (_wcsnicmp (p, L"SEDEF_WAIT_PID=", 15) != 0 &&
                _wcsnicmp (p, L"SEDEF_OPEN=", 11) != 0)
                environment.append (p).push_back (L'\0');
        FreeEnvironmentStringsW (block);
    }
    environment.append (L"SEDEF_WAIT_PID=" + std::to_wstring (pid)).push_back (L'\0');
    environment.append (L"SEDEF_OPEN=")
        .append (app.getFullPathName().toWideCharPointer())
        .push_back (L'\0');
    environment.push_back (L'\0');

    std::wstring line (command.toWideCharPointer());
    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    PROCESS_INFORMATION info {};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    // Out of this process's job if it may (a job can end its processes when this one exits).
    if (!CreateProcessW (nullptr, line.data(), nullptr, nullptr, FALSE,
                         flags | CREATE_BREAKAWAY_FROM_JOB, environment.data(), nullptr, &startup,
                         &info) &&
        !CreateProcessW (nullptr, line.data(), nullptr, nullptr, FALSE, flags, environment.data(),
                         nullptr, &startup, &info))
        return false;
    CloseHandle (info.hThread);
    CloseHandle (info.hProcess);
    return true;
}

#else

LaunchResult launchElevated (const juce::File&, const juce::String&, void*, bool,
                             const std::function<bool()>&)
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

HeldFile::HeldFile (const juce::File&) {}

HeldFile::~HeldFile() = default;

juce::String HeldFile::sha256()
{
    return {};
}

void HeldFile::keepUntilExit() {}

juce::uint64 processCreationTime (juce::uint32)
{
    return 0;
}

bool openAfterExit (juce::uint32, const juce::File&)
{
    return false;
}

#endif

} // namespace sedef::update
