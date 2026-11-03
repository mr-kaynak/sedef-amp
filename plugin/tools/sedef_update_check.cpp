// SPDX-License-Identifier: AGPL-3.0-or-later
//
// sedef-update-check: runs the plug-in's update checker (plugin/src/update) against the real
// GitHub API, without a plug-in or a host around it. It prints the release the plug-in would see,
// whether it would offer it and which installer it would download. With --download it fetches
// that installer, or --asset <name>, and SHA256SUMS.txt from the same release, and verifies the
// checksum before the file gets its final name, exactly as the plug-in does.
//
//   sedef-update-check [--current <version>] [--platform macOS|Windows|Linux] [--arm]
//                      [--download <folder>] [--asset <name>]
//   sedef-update-check --install <file> [--expect <version>] [--system] [--waitpid <pid>]
//                      [--log <file>]
//
// --install runs the plug-in's automatic install (plugin/src/update/Installer.cpp) on a package
// that was just built, with no API request: on macOS it mounts the disk image, picks the formats
// that are installed and runs Apple's installer; on Linux it unpacks the archive and runs
// install.sh (in your home folder, or --system); on Windows it starts the setup elevated with the
// update's switches, waiting for process --waitpid, and waits for it. It then checks the versions.
// The release workflow runs it on CI machines, which have passwordless sudo (macOS, Linux) or run
// elevated (Windows). This tool asks with `sudo -n` where the plug-in shows the OS's password
// prompt; that difference exists only here, at compile time, never in the plug-in.
//
// Exit status: 0 when every step that was asked for worked, 1 otherwise. Each check makes one API
// request (GitHub allows 60 an hour without a token).
#include "Installer.h"

#include <cstdio>

using namespace sedef::update;

namespace
{
void print (const juce::String& text)
{
    std::printf ("%s\n", text.toRawUTF8());
    std::fflush (stdout);
}

juce::String megabytes (juce::int64 bytes)
{
    return juce::String (static_cast<double> (bytes) / 1.0e6, 1) + " MB";
}

struct Options
{
    juce::String current = SEDEF_VERSION;
    Platform platform = currentPlatform();
    bool x64 = isX64();
    juce::File downloadTo;
    juce::String asset;
    juce::File install;
    juce::String expect = SEDEF_VERSION;
    bool system = false;
    juce::uint32 waitPid = 0;
    juce::File log;
};

bool parseArguments (int argc, char* argv[], Options& o)
{
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        const auto value = [&]() -> juce::String {
            if (arg.containsChar ('='))
                return arg.fromFirstOccurrenceOf ("=", false, false);
            return i + 1 < argc ? juce::String (argv[++i]) : juce::String();
        };
        const auto name = arg.upToFirstOccurrenceOf ("=", false, false);
        if (name == "--current")
            o.current = value();
        else if (name == "--platform")
        {
            const auto p = value();
            if (p == "macOS")
                o.platform = Platform::MacOS;
            else if (p == "Windows")
                o.platform = Platform::Windows;
            else if (p == "Linux")
                o.platform = Platform::Linux;
            else
                return false;
        }
        else if (name == "--arm")
            o.x64 = false;
        else if (name == "--download")
            o.downloadTo = juce::File::getCurrentWorkingDirectory().getChildFile (value());
        else if (name == "--asset")
            o.asset = value();
        else if (name == "--install")
            o.install = juce::File::getCurrentWorkingDirectory().getChildFile (value());
        else if (name == "--expect")
            o.expect = value();
        else if (name == "--system")
            o.system = true;
        else if (name == "--waitpid")
            o.waitPid = static_cast<juce::uint32> (value().getLargeIntValue());
        else if (name == "--log")
            o.log = juce::File::getCurrentWorkingDirectory().getChildFile (value());
        else
            return false;
    }
    return Version::parse (o.current).has_value() && Version::parse (o.expect).has_value();
}

/// The test seam: `sudo -n` instead of the OS's password prompt. It exists only in this tool.
Elevated sudoElevate (const juce::StringArray& command, const juce::String& prompt,
                      Cancellation& cancel)
{
    print ("  as root (" + prompt + "): " + command.joinIntoString (" "));
    juce::StringArray argv { "/usr/bin/sudo", "-n" };
    argv.addArray (command);
    Elevated e;
    e.process = runProcess (argv, 15 * 60 * 1000, [&cancel] { return cancel.isCancelled(); });
    if (e.process.output.isNotEmpty())
        print (e.process.output.trimEnd());
    if (e.process.errors.isNotEmpty())
        print (e.process.errors.trimEnd());
    e.status = !e.process.started      ? Elevated::Status::Unavailable
               : e.process.succeeded() ? Elevated::Status::Ok
                                       : Elevated::Status::Failed;
    return e;
}

juce::String outcomeName (InstallResult::Outcome outcome)
{
    switch (outcome)
    {
    case InstallResult::Outcome::Installed:
        return "installed";
    case InstallResult::Outcome::Scheduled:
        return "scheduled";
    case InstallResult::Outcome::Declined:
        return "declined";
    case InstallResult::Outcome::Cancelled:
        return "cancelled";
    case InstallResult::Outcome::Failed:
        break;
    }
    return "failed";
}

int install (const Options& options)
{
    const auto& file = options.install;
    if (!file.existsAsFile())
    {
        print ("no such file: " + file.getFullPathName());
        return 1;
    }
    const auto version = Version::parse (options.expect)->toString();
    auto config = defaultInstallerConfig();
    config.running = juce::File(); // not an installed copy: the formats come from what is installed
    config.elevate = sudoElevate;
    if (options.system)
        config.scope = LinuxScope::System;
    else
        config.scope = LinuxScope::User;
    config.waitPid = options.waitPid;
    config.keepProcess = true;
    if (options.log != juce::File())
        config.extraSetupParameters = "/LOG=\"" + options.log.getFullPathName() + "\"";

    InstallRequest request;
    request.file = file;
    request.expectedHash = sha256Hex (file);
    request.version = version;
    print ("install " + file.getFullPathName() + " (sha256 " + request.expectedHash + ")");
    print ("expect version " + version);
    if (currentPlatform() == Platform::MacOS)
        print ("formats: " + macChoicesToInstall (config.root, {}).joinIntoString (", "));
    if (currentPlatform() == Platform::Windows)
        print ("Setup waits for process " + juce::String (options.waitPid));

    Cancellation cancel;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    auto installer = makeSystemInstaller (config);
    const auto result = installer->install (
        request, [] (InstallStep step) { print ("  step: " + toString (step)); }, cancel);
    print ("outcome: " + outcomeName (result.outcome) +
           (result.error != Error::None
                ? " (" + toString (result.error) + ": " + result.detail + ")"
                : juce::String()));

    if (currentPlatform() != Platform::Windows)
    {
        print (
            "took " +
            juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started)) +
            " ms");
        return result.outcome == InstallResult::Outcome::Installed ? 0 : 1;
    }

    // Windows: Setup runs once process --waitpid has exited; wait for it here.
    if (result.outcome != InstallResult::Outcome::Scheduled || result.process == nullptr)
        return 1;
    const int code = waitForProcess (result.process, 15 * 60 * 1000);
    closeProcess (result.process);
    const auto seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    print ("Setup exited with " + juce::String (code) + " after " + juce::String (seconds, 1) +
           " s");
    const auto vst3 = config.commonFiles.getChildFile ("VST3").getChildFile ("Sedef Amp.vst3");
    const auto found = vst3BundleVersion (vst3);
    print (vst3.getFullPathName() + ": " + (found.isEmpty() ? juce::String ("missing") : found));
    return code == 0 && (found.isEmpty() || found == version) ? 0 : 1;
}
} // namespace

int main (int argc, char* argv[])
{
    Options options;
    if (!parseArguments (argc, argv, options))
    {
        print ("usage: sedef-update-check [--current <version>] [--platform macOS|Windows|Linux] "
               "[--arm] [--download <folder>] [--asset <name>]\n"
               "       sedef-update-check --install <file> [--expect <version>] [--system] "
               "[--waitpid <pid>] [--log <file>]");
        return 2;
    }
    if (options.install != juce::File())
        return install (options);

    const Client client (options.current, options.platform);
    Cancellation cancel;
    print ("GET " + latestReleaseApiUrl());
    print ("User-Agent: " + client.userAgent());

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto check = client.fetchLatest (cancel);
    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - started;
    if (check.error != Error::None)
    {
        print ("check failed: " + toString (check.error) + " (" + check.detail + ")");
        return 1;
    }
    if (!check.release)
    {
        print ("no stable release published");
        return 0;
    }

    const auto& release = *check.release;
    print ("latest: " + release.tag + " \"" + release.name + "\", published " +
           release.publishedAt + " (" + juce::String (juce::roundToInt (elapsed)) + " ms)");
    print ("page: " + release.pageUrl);
    for (const auto& a : release.assets)
        print ("  asset " + a.name + " (" + megabytes (a.size) + ")");
    print ("notes: " + juce::String (release.notes.length()) + " characters");

    const auto current = *Version::parse (options.current);
    print ("current " + current.toString() + ": " +
           (isNewer (release, current) ? "the plug-in offers " + release.version.toString()
                                       : juce::String ("up to date")));

    const auto expected = installerAssetName (options.platform, options.x64);
    const auto* installer = selectInstaller (release, options.platform, options.x64);
    print ("installer for " + platformName (options.platform) + ": " +
           (installer != nullptr
                ? installer->name + " (" + megabytes (installer->size) + ")"
                : (expected.isEmpty() ? juce::String ("none for this processor")
                                      : expected + " is not in this release") +
                      " -> the plug-in offers \"Open releases page\" (" + release.pageUrl + ")"));

    if (options.downloadTo == juce::File())
        return 0;

    const auto* asset = options.asset.isNotEmpty() ? release.findAsset (options.asset) : installer;
    if (asset == nullptr)
    {
        print ("nothing to download" +
               (options.asset.isNotEmpty() ? ": no asset named " + options.asset : juce::String()));
        return 1;
    }

    print ("download " + asset->name + " -> " + options.downloadTo.getFullPathName());
    int lastTenth = -1;
    const auto result = client.downloadVerified (
        release, *asset, options.downloadTo,
        [&lastTenth] (DownloadStage stage, juce::int64 received, juce::int64 total) {
            if (stage == DownloadStage::Checksums)
                print ("  fetching " + juce::String (kChecksumsAssetName));
            else if (stage == DownloadStage::Verifying)
                print ("  verifying " + megabytes (received));
            else if (total > 0 && static_cast<int> (received * 10 / total) != lastTenth)
            {
                lastTenth = static_cast<int> (received * 10 / total);
                print ("  " + megabytes (received) + " of " + megabytes (total));
            }
        },
        cancel);
    if (result.error != Error::None)
    {
        print ("download failed: " + toString (result.error) + " (" + result.detail + ")");
        return 1;
    }
    print ("verified: " + result.file.getFullPathName());
    print ("sha256 " + sha256Hex (result.file) + " matches " + kChecksumsAssetName);
    return 0;
}
