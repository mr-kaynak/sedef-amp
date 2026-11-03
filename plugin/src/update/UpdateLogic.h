// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <juce_core/juce_core.h>
#include <map>
#include <optional>
#include <vector>

/// The update checker's pure logic: version comparison, the GitHub release model, installer
/// selection, checksum lists, the check throttle and the file that persists settings and the last
/// result. Nothing here touches the network or starts a thread, so plugin/tests/UpdateTests.cpp
/// covers all of it. See docs/updates.md.
namespace sedef::update
{

inline constexpr const char* kRepository = "mr-kaynak/sedef-amp";
inline constexpr const char* kChecksumsAssetName = "SHA256SUMS.txt";

/// https://api.github.com/repos/<repository>/releases/latest
juce::String latestReleaseApiUrl();
/// https://github.com/<repository>/releases
juce::String releasesPageUrl();

// Versions ----------------------------------------------------------------------------------------

/// A semantic version (semver.org 2.0). Parsing accepts a "v" prefix and a missing minor or patch
/// number ("v1.2" is 1.2.0) and drops build metadata ("+abc"), which has no precedence but must
/// still be well-formed: a parsed tag only ever holds [0-9A-Za-z.+-], so it is safe in a URL path.
struct Version
{
    std::array<int, 3> numbers {}; // major, minor, patch
    juce::StringArray prerelease;  // dot-separated identifiers after '-', e.g. { "rc", "1" }

    static std::optional<Version> parse (juce::String text);
    bool isPrerelease() const noexcept { return !prerelease.isEmpty(); }
    juce::String toString() const;
};

/// Semver precedence: negative when a < b, zero when equal, positive when a > b.
int compare (const Version& a, const Version& b);

// Releases ----------------------------------------------------------------------------------------

struct Asset
{
    juce::String name;
    juce::int64 size = 0;
    /// Always under https://github.com/<repository>/releases/download/ (parseRelease checks).
    juce::String url;
};

struct Release
{
    Version version;
    juce::String tag, name, notes, pageUrl, publishedAt;
    std::vector<Asset> assets;

    const Asset* findAsset (const juce::String& assetName) const;
};

struct ParsedRelease
{
    enum class Status
    {
        Ok,
        Ignored, // a draft or a pre-release: never offered
        Invalid  // not a release object, or no usable version in its tag
    };
    Status status = Status::Invalid;
    Release release;
    juce::String reason; // why it was ignored or invalid
};

/// Reads a release object as the GitHub REST API returns it (tag_name, name, body, html_url,
/// published_at, draft, prerelease, assets[] with name, size and browser_download_url). The same
/// shape is used for the cached copy (toVar). Assets that do not download from this repository's
/// releases, or whose names are not plain file names, are dropped. Only tests pass another
/// `releasesUrl`.
ParsedRelease parseRelease (const juce::var& json,
                            const juce::String& releasesUrl = releasesPageUrl());
juce::var toVar (const Release& release);

/// Whether `latest` is newer than the running version.
bool isNewer (const Release& latest, const Version& current);
/// Newer, and not the version the user chose to skip.
bool shouldNotify (const Release& latest, const Version& current, const juce::String& skipped);

/// Where a redirect from `from` with this Location header leads, if it may be followed: over https
/// to GitHub's download hosts (github.com, objects.githubusercontent.com,
/// release-assets.githubusercontent.com), or anywhere on the origin `from` is on. Empty for
/// anything else, including relative paths that do not start with "/" and URLs with credentials.
juce::String redirectTarget (const juce::String& from, const juce::String& location);

// Installers --------------------------------------------------------------------------------------

enum class Platform
{
    MacOS,
    Windows,
    Linux
};

Platform currentPlatform() noexcept;
bool isX64() noexcept;
/// "macOS", "Windows" or "Linux": in the User-Agent and in the UI.
juce::String platformName (Platform platform);
/// The installer each release publishes for an OS (.github/workflows/release.yml); empty when
/// there is none (Linux builds exist only for x86-64).
juce::String installerAssetName (Platform platform, bool x64);

/// No installer of ours comes near this; a release that lists more is not trusted with the disk.
inline constexpr juce::int64 kMaxInstallerBytes = juce::int64 { 1 } << 30;

/// The release's installer for this OS, or nullptr. An installer that lists no size (0), or more
/// than kMaxInstallerBytes, counts as none: the download could not be checked against it.
const Asset* selectInstaller (const Release& release, Platform platform, bool x64);

// Checksums ---------------------------------------------------------------------------------------

/// Parses `sha256sum` output: "<64 hex digits>  <name>" per line ("*<name>" in binary mode).
/// Returns name -> lower-case hex. Blank, comment and malformed lines are skipped, white space
/// around a line is ignored, and a name listed twice with different hashes is left out (it has no
/// trustworthy checksum).
std::map<juce::String, juce::String> parseChecksums (const juce::String& text);
bool isSha256Hex (const juce::String& text);
/// Lower-case hex SHA-256 of a file's contents; empty when it cannot be read.
juce::String sha256Hex (const juce::File& file);
bool matchesChecksum (const juce::File& file, const juce::String& expectedHex);

// Throttle ----------------------------------------------------------------------------------------

inline constexpr juce::int64 kCheckIntervalMs = 24 * 60 * 60 * 1000;
inline constexpr juce::int64 kRetryAfterFailureMs = 60 * 60 * 1000;

/// Whether an automatic check is due. `lastAttemptMs` is when the last check started (0: never),
/// `lastSuccessMs` when one last succeeded. After a success the next check waits a day, after a
/// failure (or a check another process has not finished) an hour. A clock that went backwards
/// makes a check due rather than suppressing checks until it catches up.
bool isCheckDue (juce::int64 nowMs, juce::int64 lastAttemptMs, juce::int64 lastSuccessMs);

// Persisted state ---------------------------------------------------------------------------------

/// Everything update.json holds: the user's settings and the last check. Global per user, never
/// part of a DAW project.
struct Store
{
    bool autoCheck = true;
    juce::String skippedVersion;
    juce::int64 lastAttemptMs = 0;
    juce::int64 lastSuccessMs = 0;
    /// The latest stable release from the last successful check; empty when there was none.
    std::optional<Release> latest;
    /// The version an automatic update installed, until a process runs it: every process that
    /// runs an older one asks for a restart instead of offering the update again. Empty: none.
    juce::String installedVersion;
};

/// Whether `store` says a newer version than `current` is installed and waits for a restart (and
/// no release newer than that one is known).
bool restartNeeded (const Store& store, const Version& current);

juce::var toVar (const Store& store);
/// Tolerant: anything missing or malformed falls back to the default.
Store storeFromVar (const juce::var& json, const juce::String& releasesUrl = releasesPageUrl());

Store readStore (const juce::File& file, const juce::String& releasesUrl = releasesPageUrl());
/// Writes to a temporary file and renames it over the target, so readers never see a torn file.
bool writeStore (const juce::File& file, const Store& store);

} // namespace sedef::update
