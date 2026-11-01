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
//
// Exit status: 0 when every step that was asked for worked, 1 otherwise. Each run makes one API
// request (GitHub allows 60 an hour without a token).
#include "UpdateClient.h"

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
        else
            return false;
    }
    return Version::parse (o.current).has_value();
}
} // namespace

int main (int argc, char* argv[])
{
    Options options;
    if (!parseArguments (argc, argv, options))
    {
        print ("usage: sedef-update-check [--current <version>] [--platform macOS|Windows|Linux] "
               "[--arm] [--download <folder>] [--asset <name>]");
        return 2;
    }

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
