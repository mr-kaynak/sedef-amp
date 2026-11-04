// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "InstallLogic.h"
#include "InstallProcess.h"
#include "UpdateClient.h"

/// Installs a verified update through the OS's own installer: Apple's /usr/sbin/installer on the
/// pkg inside the disk image (after an administrator's password), the Inno Setup program once the
/// host has quit (after a UAC prompt), or the archive's install.sh (with pkexec for a system-wide
/// install). Sedef never writes into plug-in folders itself. On macOS and Linux one fixed script
/// does the whole install from its own private copy of the file (InstallLogic.h, ScriptExit); on
/// Windows the setup file is held open, so it can't change, from the check until this process
/// exits, and Setup holds it from then on. Everything here blocks; UpdateService runs it on its own
/// thread, and a Cancellation ends any wait at once (an install already under way as root goes on).
namespace sedef::update
{

enum class InstallStep
{
    Preparing,  // checking the file again, mounting or unpacking it
    Permission, // the OS asks for a password or permission; the install runs right after
    Installing, // installing, with nothing to ask (a Linux install in your home folder)
    Finishing   // detaching, checking the installed versions
};
juce::String toString (InstallStep step);

/// Why the automatic route can't run in this process; the panel offers Install manually instead.
enum class ManualReason
{
    None,
    Elsewhere,   // this copy runs from a folder the installer does not write
    Sandboxed,   // the host's sandbox keeps Sedef from asking for permission
    NoElevation, // nothing to ask for permission with (Linux without pkexec)
    Unsupported  // no automatic route on this system
};
juce::String toString (ManualReason reason);

struct InstallRequest
{
    juce::File file;           // the verified installer, in the private cache
    juce::String expectedHash; // its SHA-256, checked again right before it is used
    juce::String version;      // the release's, e.g. "0.5.0"
    bool relaunch = false;     // Windows: open the standalone app once Setup has run
    void* window = nullptr;    // Windows: the window the permission prompt belongs to
};

struct InstallResult
{
    enum class Outcome
    {
        Installed, // and verified
        Scheduled, // Windows: Setup waits for this process to exit
        Declined,  // the user cancelled the OS's prompt: ready to try again
        Cancelled, // the service is closing (an install under way may still finish: `detail`)
        Failed
    };
    Outcome outcome = Outcome::Failed;
    Error error = Error::None; // when Failed
    juce::String detail;
    void* process =
        nullptr; // Windows, Scheduled, with InstallerConfig::keepProcess: Setup's HANDLE
};

using StepCallback = std::function<void (InstallStep)>;

class Installer
{
public:
    virtual ~Installer() = default;
    /// Whether the automatic route can run here. Cheap (a few file checks); asked once.
    virtual ManualReason manualReason() const = 0;
    virtual InstallResult install (const InstallRequest& request, const StepCallback& step,
                                   Cancellation& cancel) = 0;
    /// The standalone app: arranges for the new version to open once this process has quit.
    virtual bool relaunchAfterExit() = 0;
};

/// Runs `command` with administrator rights once the OS has asked (`prompt`).
struct Elevated
{
    enum class Status
    {
        Ok,
        Declined,    // the user cancelled the prompt
        Unavailable, // could not ask (no agent, a sandbox, osascript refused)
        Failed       // it ran and failed
    };
    Status status = Status::Failed;
    int exitCode = -1; // the command's own exit status, when known (Failed): see ScriptExit
    ProcessResult process;
};
using Elevate = std::function<Elevated (const juce::StringArray& command,
                                        const juce::String& prompt, Cancellation& cancel)>;

/// The plug-in's way to ask: osascript's "with administrator privileges" on macOS, pkexec on Linux.
Elevate systemElevate();

struct InstallerConfig
{
    juce::File root;        // macOS: the volume the pkg installs to ("/"; tests: a fake root)
    juce::File running;     // the bundle or binary this process runs from
    juce::File home;        // Linux: the user's home folder
    juce::File commonFiles; // Windows: C:\Program Files\Common Files
    juce::File appFolder;   // Windows: where Setup put the app (C:\Program Files\Sedef Amp)
    std::optional<LinuxScope> scope;   // Linux: overrides the scope `running` implies (the CLI)
    Elevate elevate;                   // empty: systemElevate()
    juce::uint32 waitPid = 0;          // Windows: the process Setup waits for (0: this one)
    juce::uint64 waitPidCreated = 0;   // Windows: its creation time (0: looked up)
    bool keepProcess = false;          // Windows: hand back Setup's process (the CLI waits for it)
    juce::String extraSetupParameters; // Windows: e.g. /LOG="..." (the CLI)
};

/// This process's configuration: the real root, location, folders and way to ask.
InstallerConfig defaultInstallerConfig();
/// This OS's installer.
std::unique_ptr<Installer> makeSystemInstaller (InstallerConfig config);
/// Another OS's (tests: the Linux route runs on macOS too, with a fake way to ask).
std::unique_ptr<Installer> makeInstaller (Platform platform, InstallerConfig config);

/// Windows: starts `setup` elevated (UAC asks) with `parameters`, on a thread of its own that
/// keeps the plug-in's module loaded until it is done: the prompt can stay up for as long as the
/// user likes, and `cancelled` ends the wait for it at once (the prompt stays, and if the user
/// agrees later, Setup runs). The CLI keeps the process to wait for it; the plug-in lets it go.
struct LaunchResult
{
    Elevated::Status status = Elevated::Status::Failed;
    bool cancelled = false; // stopped waiting for the prompt
    juce::String detail;
    void* process = nullptr; // a HANDLE when asked for; close it with closeProcess
};
LaunchResult launchElevated (const juce::File& setup, const juce::String& parameters, void* window,
                             bool keepProcess, const std::function<bool()>& cancelled = {});
/// Waits for a process from launchElevated; its exit code, or -1 on a timeout.
int waitForProcess (void* process, int timeoutMs);
void closeProcess (void* process);
/// The window that has the focus (Windows), for the permission prompt to come to the front.
void* foregroundWindow();

/// Windows: a file opened for reading that nobody can write, move or delete while it is open (no
/// write or delete sharing). Elsewhere it never opens.
class HeldFile
{
public:
    explicit HeldFile (const juce::File& file);
    ~HeldFile();
    HeldFile (const HeldFile&) = delete;
    HeldFile& operator= (const HeldFile&) = delete;

    bool isOpen() const noexcept { return handle_ != nullptr; }
    /// The SHA-256 of what the handle reads (lower-case hex); empty when it can't be read.
    juce::String sha256();
    /// Keeps the file held until this process exits.
    void keepUntilExit();

private:
    void* handle_ = nullptr;
};

/// Windows: when process `pid` was created, as a FILETIME; 0 when unknown.
juce::uint64 processCreationTime (juce::uint32 pid);

/// Windows: once process `pid` has exited (or after a minute), opens `app`, through a hidden
/// PowerShell that runs a constant script; `pid` and `app` reach it in its environment.
bool openAfterExit (juce::uint32 pid, const juce::File& app);

} // namespace sedef::update
