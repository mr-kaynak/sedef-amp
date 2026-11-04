// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_core/juce_core.h>
#include <optional>
#include <vector>

/// The installer's pure logic: where updates are cached, which formats to install and how to ask
/// for them, the scripts that install as root, the Windows setup's switches, what a Linux archive
/// may contain, and the argv of every command the installer runs. Nothing here starts a process,
/// so plugin/tests/InstallTests.cpp covers all of it. See docs/updates.md.
namespace sedef::update
{

// The private cache -------------------------------------------------------------------------------

/// Where updates are downloaded and installed from, per user: ~/Library/Caches/Sedef/Updates
/// (macOS), %LOCALAPPDATA%\Sedef\Updates (Windows), ${XDG_CACHE_HOME:-~/.cache}/sedef/updates.
juce::File updateCacheFolder();

/// Creates `folder` with its parents and makes it private: mode 0700 on macOS and Linux, where it
/// also refuses a symbolic link or a folder another user owns. Windows' per-user folders are
/// private already.
bool makePrivateFolder (const juce::File& folder);

// The install scripts
// ------------------------------------------------------------------------------

/// The install, from the verified file to the installed formats, is one fixed /bin/sh script per
/// OS, run as root (macOS, a Linux system-wide install) or as you (a Linux install in your home
/// folder). The script is constant text: the file, its SHA-256 and the rest travel as its
/// arguments. It works in a folder of its own that only its user can open (mktemp -d, umask 077):
/// it copies the file there, checks the copy's SHA-256, unpacks or mounts that copy and installs
/// from it, and removes the folder (and detaches the image) on every way out. Nothing another
/// process of yours can write is used once the password has been given. Its output goes to a log
/// in that folder; the last of it is printed on stderr at the end.
///
/// Its exit status, beyond the installer's own success (0):
enum class ScriptExit : int
{
    Interrupted = 80, // a signal stopped it
    Staging = 81,     // its folder, or the copy in it, could not be made
    Checksum = 82,    // the copy does not have the SHA-256 Sedef verified
    Unpack = 83,      // the disk image does not attach, or the archive does not unpack
    NoInstaller = 84, // no pkg (macOS) or install.sh (Linux) as a plain file in it
    Failed = 85       // Apple's installer or install.sh failed
};

// macOS -------------------------------------------------------------------------------------------

/// One format in "Install Sedef Amp.pkg": its choice id in distribution.xml.in, its bundle and
/// where the pkg puts it (tools/packaging/macos/build-pkg.sh).
struct MacComponent
{
    const char* choice;
    const char* bundle;
    const char* location;
};

inline constexpr MacComponent kMacComponents[] = {
    { "app", "Sedef Amp.app", "Applications" },
    { "vst3", "Sedef Amp.vst3", "Library/Audio/Plug-Ins/VST3" },
    { "au", "Sedef Amp.component", "Library/Audio/Plug-Ins/Components" },
    { "clap", "Sedef Amp.clap", "Library/Audio/Plug-Ins/CLAP" },
};

inline constexpr const char* kMacPackageName = "Install Sedef Amp.pkg";

/// The bundle the pkg installs for `component`, under `root` ("/" except in tests).
juce::File macBundlePath (const MacComponent& component, const juce::File& root);

/// The pkg's choice id for the bundle this process runs from, or empty when the pkg does not
/// write there (a build folder, a copy in ~/Library/Audio/Plug-Ins).
juce::String macChoiceFor (const juce::File& runningBundle, const juce::File& root);

/// The formats to install: those installed under `root` now, plus the one this process runs
/// from; all four when none is found. In kMacComponents order.
juce::StringArray macChoicesToInstall (const juce::File& root, const juce::File& runningBundle);

/// The `installer -applyChoiceChangesXML` file that selects exactly `selected` of the four
/// choices and deselects the others.
juce::String choiceChangesXml (const juce::StringArray& selected);

/// An XML property list as a var: dict (DynamicObject), array, string, integer (int64), real,
/// true and false; date and data as their text. Void for anything else.
juce::var parsePlist (const juce::String& xml);

/// CFBundleShortVersionString from <bundle>/Contents/Info.plist (XML, or binary on macOS); empty
/// when it cannot be read.
juce::String bundleShortVersion (const juce::File& bundle);

/// The argv that runs `command` (absolute program path first) as root after macOS asks for an
/// administrator's password, showing `prompt`:
///   /usr/bin/osascript -e <constant script> -- <prompt> <command...>
/// The script never changes. The prompt and every argument travel as arguments, and the script
/// hands each one to the shell through `quoted form of`, so none is ever read as script or shell
/// code.
juce::StringArray osascriptArgv (const juce::StringArray& command, const juce::String& prompt);

/// The AppleScript error number at the end of osascript's error output ("... (-128)").
std::optional<int> appleScriptErrorNumber (const juce::String& errorOutput);
inline constexpr int kAppleScriptUserCancelled = -128;

/// The macOS install script (see ScriptExit): copies the disk image, checks it, attaches it
/// read-only, requires "Install Sedef Amp.pkg" to be a plain file in it, writes the choice changes
/// and runs /usr/sbin/installer -pkg ... -applyChoiceChangesXML ... -target /.
juce::String macInstallScript();

/// /bin/sh -c <macInstallScript()> sedef <dmg> <sha256> <choice changes XML>
juce::StringArray macInstallCommand (const juce::File& dmg, const juce::String& sha256,
                                     const juce::String& choiceChanges);

/// The password prompt: "Sedef Amp wants to install version 0.5.0."
juce::String installPrompt (const juce::String& version);

// Windows -----------------------------------------------------------------------------------------

/// The setup's AppId (sedef-amp.iss): its uninstall key is "<AppId>_is1".
inline constexpr const char* kWindowsAppId = "{8F234EBD-87BC-4821-9789-4BF2E8D8AB79}";

/// The setup's components (sedef-amp.iss) to install: those installed now (the VST3 and CLAP in
/// `commonFiles`, the app in `appFolder`), plus the one this process runs from; all three when
/// none is found. In the setup's order.
juce::StringArray windowsComponentsToInstall (const juce::File& commonFiles,
                                              const juce::File& appFolder,
                                              const juce::File& runningModule);

/// The setup's switches for an update: no wizard and no message boxes, no restart, no closing of
/// programs, a restart reported as exit code 3010, exactly `components`, and wait for process
/// `waitPid` (the host) to exit first, if it is still the process created at `waitPidCreated` (a
/// FILETIME; 0 when unknown) and not a new one with the same id (sedef-amp.iss). `relaunch` opens
/// the standalone app afterwards.
juce::String windowsSetupParameters (juce::uint32 waitPid, juce::uint64 waitPidCreated,
                                     const juce::StringArray& components, bool relaunch);

/// Whether the setup updates the file this process runs from: the VST3 or CLAP in `commonFiles`
/// (C:\Program Files\Common Files), or "Sedef Amp.exe" in `appFolder`, where the setup put the app.
bool windowsSetupUpdates (const juce::File& runningModule, const juce::File& commonFiles,
                          const juce::File& appFolder);

// Linux -------------------------------------------------------------------------------------------

struct TarEntry
{
    enum class Type
    {
        File,
        Directory,
        Symlink,
        Hardlink,
        Device,
        Fifo,
        Other
    };
    juce::String path; // as stored, after a GNU long name or a pax "path" record
    Type type = Type::Other;
    juce::int64 size = 0;
    int mode = 0;
    juce::String sha256; // of a file's data
};

struct TarListing
{
    juce::String error; // empty when the archive could be read to its end
    std::vector<TarEntry> entries;
};

/// The maximum an archive may unpack to, and the most headers it may have (long names and pax
/// headers count, and their bytes count towards the size).
inline constexpr juce::int64 kMaxUnpackedBytes = juce::int64 { 1 } << 30;
inline constexpr int kMaxTarEntries = 20000;

/// Reads a gzip-compressed tar (ustar, GNU, GNU long names, pax paths): every entry, with the
/// SHA-256 of each file's data. Only files have data. `cancelled` is asked before each header.
TarListing listTarGz (juce::InputStream& gzipped, const std::function<bool()>& cancelled = {});

/// Checks a listing before anything is extracted: only files and folders, relative paths without
/// "..", no duplicates, all under one top folder, within the size limits, and the layout install.sh
/// expects. Returns the top folder's name, or empty with `error` set.
juce::String checkLinuxArchive (const std::vector<TarEntry>& entries, juce::String& error);

/// Where this process runs from: your own folders (~/.vst3, ~/.clap, ~/.local/bin), the system's
/// (/usr/lib/vst3, /usr/lib/clap, /usr/local/bin), or somewhere install.sh does not write.
enum class LinuxScope
{
    User,
    System,
    Elsewhere
};
LinuxScope linuxScopeOf (const juce::File& running, const juce::File& home);

/// The VST3 bundle, CLAP and standalone binary install.sh writes for a scope.
juce::File linuxVst3Bundle (LinuxScope scope, const juce::File& home);
juce::File linuxClap (LinuxScope scope, const juce::File& home);
juce::File linuxStandalone (LinuxScope scope, const juce::File& home);

/// A file install.sh copies, where it goes, and the SHA-256 it must have there.
struct InstalledFile
{
    juce::File file;
    juce::String sha256;
};

/// Every file of the checked archive's VST3 bundle, CLAP and standalone app (under `top`), at the
/// place install.sh copies it to for `scope`.
std::vector<InstalledFile> linuxInstalledFiles (const std::vector<TarEntry>& entries,
                                                const juce::String& top, LinuxScope scope,
                                                const juce::File& home);

/// The Linux install script (see ScriptExit): copies the archive, checks it, unpacks it with tar
/// (no owners, permissions within the umask) and runs its install.sh, with --system for `scope`
/// System.
juce::String linuxInstallScript();

/// /bin/sh -c <linuxInstallScript()> sedef <archive> <sha256> <system|user>
juce::StringArray linuxInstallCommand (const juce::File& archive, const juce::String& sha256,
                                       LinuxScope scope);

/// "Version" from a VST3 bundle's Contents/Resources/moduleinfo.json; empty when unreadable.
juce::String moduleInfoVersion (const juce::String& json);
juce::String vst3BundleVersion (const juce::File& bundle);

// Restarting the standalone app -------------------------------------------------------------------

/// The argv of a /bin/sh that waits (at most a minute) for process `pid` to exit, then runs
/// `command`. The script is constant; the pid and the command are its arguments.
juce::StringArray waitThenRunArgv (int pid, const juce::StringArray& command);

} // namespace sedef::update
