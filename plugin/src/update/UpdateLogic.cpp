// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UpdateLogic.h"

#include <juce_cryptography/juce_cryptography.h>

namespace sedef::update
{
namespace
{
constexpr int kMaxNotesLength = 64 * 1024; // GitHub allows 125,000; the panel needs far less
constexpr int kMaxAssetNameLength = 128;
constexpr const char* kDigits = "0123456789";

bool isNumeric (const juce::String& s)
{
    return s.isNotEmpty() && s.containsOnly (kDigits);
}

/// A name we may create in the Downloads folder: no separators, no leading dot.
bool isPlainFileName (const juce::String& name)
{
    return name.isNotEmpty() && name.length() <= kMaxAssetNameLength &&
           !name.startsWithChar ('.') &&
           name.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-");
}

juce::String downloadPrefix (const juce::String& tag)
{
    return releasesPageUrl() + "/download/" + tag + "/";
}

juce::int64 int64Property (const juce::var& v, const char* name)
{
    const auto& p = v[name];
    return p.isInt() || p.isInt64() || p.isDouble() ? static_cast<juce::int64> (p) : 0;
}
} // namespace

juce::String latestReleaseApiUrl()
{
    return juce::String ("https://api.github.com/repos/") + kRepository + "/releases/latest";
}

juce::String releasesPageUrl()
{
    return juce::String ("https://github.com/") + kRepository + "/releases";
}

// Versions ----------------------------------------------------------------------------------------

std::optional<Version> Version::parse (juce::String text)
{
    text = text.trim();
    if (text.startsWithChar ('v') || text.startsWithChar ('V'))
        text = text.substring (1);
    text = text.upToFirstOccurrenceOf ("+", false, false); // build metadata has no precedence

    const auto core = text.upToFirstOccurrenceOf ("-", false, false);
    juce::StringArray parts;
    parts.addTokens (core, ".", {});
    if (parts.isEmpty() || parts.size() > 3)
        return std::nullopt;

    Version v;
    for (int i = 0; i < parts.size(); ++i)
    {
        // Nine digits keep the number inside an int.
        if (!isNumeric (parts[i]) || parts[i].length() > 9)
            return std::nullopt;
        v.numbers[static_cast<size_t> (i)] = parts[i].getIntValue();
    }

    if (text.containsChar ('-'))
    {
        v.prerelease.addTokens (text.fromFirstOccurrenceOf ("-", false, false), ".", {});
        if (v.prerelease.isEmpty())
            return std::nullopt;
        for (const auto& id : v.prerelease)
            if (id.isEmpty() ||
                !id.containsOnly (
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-"))
                return std::nullopt;
    }
    return v;
}

juce::String Version::toString() const
{
    auto s = juce::String (numbers[0]) + "." + juce::String (numbers[1]) + "." +
             juce::String (numbers[2]);
    if (isPrerelease())
        s << "-" << prerelease.joinIntoString (".");
    return s;
}

int compare (const Version& a, const Version& b)
{
    for (size_t i = 0; i < a.numbers.size(); ++i)
        if (a.numbers[i] != b.numbers[i])
            return a.numbers[i] < b.numbers[i] ? -1 : 1;

    // A pre-release ranks below its release: 1.0.0-rc.1 < 1.0.0.
    if (a.isPrerelease() != b.isPrerelease())
        return a.isPrerelease() ? -1 : 1;

    const int common = juce::jmin (a.prerelease.size(), b.prerelease.size());
    for (int i = 0; i < common; ++i)
    {
        const auto& x = a.prerelease[i];
        const auto& y = b.prerelease[i];
        const bool xNum = isNumeric (x), yNum = isNumeric (y);
        if (xNum && yNum)
        {
            const auto xv = x.getLargeIntValue(), yv = y.getLargeIntValue();
            if (xv != yv)
                return xv < yv ? -1 : 1;
        }
        else if (xNum != yNum)
        {
            return xNum ? -1 : 1; // numeric identifiers rank below alphanumeric ones
        }
        else if (const int c = x.compare (y); c != 0)
        {
            return c < 0 ? -1 : 1; // ASCII order
        }
    }
    if (a.prerelease.size() != b.prerelease.size())
        return a.prerelease.size() < b.prerelease.size() ? -1 : 1;
    return 0;
}

// Releases ----------------------------------------------------------------------------------------

const Asset* Release::findAsset (const juce::String& assetName) const
{
    for (const auto& a : assets)
        if (a.name == assetName)
            return &a;
    return nullptr;
}

ParsedRelease parseRelease (const juce::var& json)
{
    ParsedRelease out;
    if (json.getDynamicObject() == nullptr)
    {
        out.reason = "the response is not a release object";
        return out;
    }

    auto& r = out.release;
    r.tag = json["tag_name"].toString().trim();
    const auto version = Version::parse (r.tag);
    if (r.tag.isEmpty() || !version)
    {
        out.reason = "the release tag \"" + r.tag + "\" is not a version";
        return out;
    }
    r.version = *version;
    r.name = json["name"].toString().trim();
    if (r.name.isEmpty())
        r.name = r.tag;
    r.notes = json["body"].toString().substring (0, kMaxNotesLength);
    r.publishedAt = json["published_at"].toString().trim();

    // Only ever link to and download from this repository's releases.
    const auto page = json["html_url"].toString().trim();
    r.pageUrl = page.startsWith (releasesPageUrl() + "/") ? page : releasesPageUrl();

    if (const auto* list = json["assets"].getArray())
        for (const auto& item : *list)
        {
            Asset a { item["name"].toString(), int64Property (item, "size"),
                      item["browser_download_url"].toString().trim() };
            if (isPlainFileName (a.name) && a.size >= 0 &&
                a.url.startsWith (downloadPrefix (r.tag)))
                r.assets.push_back (std::move (a));
        }

    if (static_cast<bool> (json["draft"]))
    {
        out.status = ParsedRelease::Status::Ignored;
        out.reason = "a draft";
        return out;
    }
    // Stable builds are only offered stable releases, also when a pre-release tag was published
    // without GitHub's pre-release flag.
    if (static_cast<bool> (json["prerelease"]) || r.version.isPrerelease())
    {
        out.status = ParsedRelease::Status::Ignored;
        out.reason = "a pre-release";
        return out;
    }
    out.status = ParsedRelease::Status::Ok;
    return out;
}

juce::var toVar (const Release& release)
{
    juce::Array<juce::var> assets;
    for (const auto& a : release.assets)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", a.name);
        o->setProperty ("size", a.size);
        o->setProperty ("browser_download_url", a.url);
        assets.add (juce::var (o));
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("tag_name", release.tag);
    o->setProperty ("name", release.name);
    o->setProperty ("body", release.notes);
    o->setProperty ("html_url", release.pageUrl);
    o->setProperty ("published_at", release.publishedAt);
    o->setProperty ("draft", false);
    o->setProperty ("prerelease", false);
    o->setProperty ("assets", assets);
    return juce::var (o);
}

bool isNewer (const Release& latest, const Version& current)
{
    return compare (latest.version, current) > 0;
}

bool shouldNotify (const Release& latest, const Version& current, const juce::String& skipped)
{
    if (!isNewer (latest, current))
        return false;
    const auto skip = Version::parse (skipped);
    return !skip || compare (*skip, latest.version) != 0;
}

// Installers --------------------------------------------------------------------------------------

Platform currentPlatform() noexcept
{
#if JUCE_MAC
    return Platform::MacOS;
#elif JUCE_WINDOWS
    return Platform::Windows;
#else
    return Platform::Linux;
#endif
}

bool isX64() noexcept
{
#if JUCE_INTEL && JUCE_64BIT
    return true;
#else
    return false;
#endif
}

juce::String platformName (Platform platform)
{
    switch (platform)
    {
    case Platform::MacOS:
        return "macOS";
    case Platform::Windows:
        return "Windows";
    case Platform::Linux:
        return "Linux";
    }
    return {};
}

juce::String installerAssetName (Platform platform, bool x64)
{
    switch (platform)
    {
    case Platform::MacOS:
        return "Sedef-Amp-macOS.dmg"; // universal binaries
    case Platform::Windows:
        return x64 ? "Sedef-Amp-Windows-Setup.exe" : juce::String();
    case Platform::Linux:
        return x64 ? "Sedef-Amp-Linux-x86_64.tar.gz" : juce::String();
    }
    return {};
}

const Asset* selectInstaller (const Release& release, Platform platform, bool x64)
{
    const auto name = installerAssetName (platform, x64);
    return name.isEmpty() ? nullptr : release.findAsset (name);
}

// Checksums ---------------------------------------------------------------------------------------

bool isSha256Hex (const juce::String& text)
{
    return text.length() == 64 && text.containsOnly ("0123456789abcdefABCDEF");
}

std::map<juce::String, juce::String> parseChecksums (const juce::String& text)
{
    std::map<juce::String, juce::String> out;
    for (const auto& raw : juce::StringArray::fromLines (text))
    {
        const auto line = raw.trimEnd();
        const auto hex = line.substring (0, 64);
        if (!isSha256Hex (hex) || line[64] != ' ')
            continue;
        // "<hex>  <name>" (text mode) or "<hex> *<name>" (binary mode).
        auto name = line.substring (65);
        if (name.startsWithChar (' ') || name.startsWithChar ('*'))
            name = name.substring (1);
        if (name.isNotEmpty())
            out[name] = hex.toLowerCase();
    }
    return out;
}

juce::String sha256Hex (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (!in.openedOk())
        return {};
    juce::BufferedInputStream buffered (in, 1 << 16);
    return juce::SHA256 (buffered).toHexString().toLowerCase();
}

bool matchesChecksum (const juce::File& file, const juce::String& expectedHex)
{
    return isSha256Hex (expectedHex) && sha256Hex (file) == expectedHex.toLowerCase();
}

// Throttle ----------------------------------------------------------------------------------------

bool isCheckDue (juce::int64 nowMs, juce::int64 lastAttemptMs, juce::int64 lastSuccessMs)
{
    if (lastAttemptMs <= 0 || nowMs < lastAttemptMs)
        return true;
    const bool lastSucceeded = lastSuccessMs >= lastAttemptMs;
    return nowMs - lastAttemptMs >= (lastSucceeded ? kCheckIntervalMs : kRetryAfterFailureMs);
}

// Persisted state ---------------------------------------------------------------------------------

juce::var toVar (const Store& store)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", 1);
    o->setProperty ("autoCheck", store.autoCheck);
    o->setProperty ("skippedVersion", store.skippedVersion);
    o->setProperty ("lastAttempt", store.lastAttemptMs);
    o->setProperty ("lastSuccess", store.lastSuccessMs);
    o->setProperty ("latest", store.latest ? toVar (*store.latest) : juce::var());
    return juce::var (o);
}

Store storeFromVar (const juce::var& json)
{
    Store s;
    if (json.getDynamicObject() == nullptr)
        return s;
    if (const auto& v = json["autoCheck"]; v.isBool())
        s.autoCheck = static_cast<bool> (v);
    s.skippedVersion = json["skippedVersion"].toString().substring (0, 64);
    s.lastAttemptMs = juce::jmax<juce::int64> (0, int64Property (json, "lastAttempt"));
    s.lastSuccessMs = juce::jmax<juce::int64> (0, int64Property (json, "lastSuccess"));
    if (auto parsed = parseRelease (json["latest"]); parsed.status == ParsedRelease::Status::Ok)
        s.latest = std::move (parsed.release);
    return s;
}

Store readStore (const juce::File& file)
{
    return file.existsAsFile() ? storeFromVar (juce::JSON::parse (file)) : Store {};
}

bool writeStore (const juce::File& file, const Store& store)
{
    if (!file.getParentDirectory().createDirectory())
        return false;
    juce::TemporaryFile temp (file, juce::TemporaryFile::useHiddenFile);
    if (!temp.getFile().appendText (juce::JSON::toString (toVar (store))))
        return false;
    return temp.overwriteTargetFileWithTemporary();
}

} // namespace sedef::update
