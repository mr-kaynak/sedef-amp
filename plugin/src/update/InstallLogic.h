// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>
#include <optional>
#include <vector>

/// The installer's pure logic: where updates are cached, which macOS formats to install and how
/// to ask for them, what `hdiutil` printed, the Windows setup's switches, what a Linux archive may
/// contain, and the argv of every command the installer runs. Nothing here starts a process, so
/// plugin/tests/InstallTests.cpp covers all of it. See docs/updates.md.
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

/// `file` with every symbolic link in its path resolved (/var -> /private/var on macOS), as
/// hdiutil reports mount points; `file` itself when that fails.
juce::File realPath (const juce::File& file);

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

/// The one mount point in `hdiutil attach -plist` output; empty when there is none, or more than
/// one (Sedef's disk image has a single volume).
juce::String parseMountPoint (const juce::String& plist);

/// The mount points `hdiutil info -plist` lists under `folder`: disk images an earlier run left
/// attached there.
juce::StringArray mountPointsUnder (const juce::String& infoPlist, const juce::File& folder);

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

/// /usr/sbin/installer -pkg <pkg> -applyChoiceChangesXML <choices> -target /
juce::StringArray installerArgv (const juce::File& pkg, const juce::File& choices);

/// The password prompt: "Sedef Amp wants to install version 0.3.1."
juce::String installPrompt (const juce::String& version);

// Windows -----------------------------------------------------------------------------------------

/// The setup's switches for an update: no wizard and no message boxes, no restart, no closing of
/// programs, a restart reported as exit code 3010, and wait for process `waitPid` (the host) to
/// exit first (sedef-amp.iss). `relaunch` opens the standalone app afterwards.
juce::String windowsSetupParameters (juce::uint32 waitPid, bool relaunch);

/// Whether the setup updates the file this process runs from: the VST3 or CLAP in
/// `commonFiles` (C:\Program Files\Common Files), or the standalone app (wherever it was put).
bool windowsSetupUpdates (const juce::File& runningModule, const juce::File& commonFiles);

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
};

struct TarListing
{
    juce::String error; // empty when the archive could be read to its end
    std::vector<TarEntry> entries;
};

/// The maximum an archive may unpack to, and the most entries it may have.
inline constexpr juce::int64 kMaxUnpackedBytes = juce::int64 { 1 } << 30;
inline constexpr int kMaxTarEntries = 20000;

/// Reads a gzip-compressed tar's headers (ustar, GNU, GNU long names, pax paths) and nothing else.
TarListing listTarGz (juce::InputStream& gzipped);
TarListing listTarGz (const juce::File& archive);

/// Checks a listing before anything is extracted: only files and folders, relative paths without
/// "..", no duplicates, all under one top folder, within the size limits, and the layout install.sh
/// expects. Returns the top folder's name, or empty with `error` set.
juce::String checkLinuxArchive (const std::vector<TarEntry>& entries, juce::String& error);

/// Extracts a checked archive into `destination` (an empty private folder) and sets `top` to its
/// top folder. Every entry is checked again as it is written; anything unexpected stops it. Files
/// keep their executable bit only.
bool extractTarGz (const juce::File& archive, const juce::File& destination, juce::String& error,
                   juce::String* top = nullptr);

/// Where this process runs from: your own folders (~/.vst3, ~/.clap, ~/.local/bin), the system's
/// (/usr/lib/vst3, /usr/lib/clap, /usr/local/bin), or somewhere install.sh does not write.
enum class LinuxScope
{
    User,
    System,
    Elsewhere
};
LinuxScope linuxScopeOf (const juce::File& running, const juce::File& home);

/// The VST3 bundle and standalone binary install.sh writes for a scope.
juce::File linuxVst3Bundle (LinuxScope scope, const juce::File& home);
juce::File linuxStandalone (LinuxScope scope, const juce::File& home);

/// "Version" from a VST3 bundle's Contents/Resources/moduleinfo.json; empty when unreadable.
juce::String moduleInfoVersion (const juce::String& json);
juce::String vst3BundleVersion (const juce::File& bundle);

// Restarting the standalone app -------------------------------------------------------------------

/// The argv of a /bin/sh that waits (at most a minute) for process `pid` to exit, then runs
/// `command`. The script is constant; the pid and the command are its arguments.
juce::StringArray waitThenRunArgv (int pid, const juce::StringArray& command);

} // namespace sedef::update
