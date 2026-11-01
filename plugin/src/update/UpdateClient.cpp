// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UpdateClient.h"

namespace sedef::update
{
namespace
{
constexpr juce::int64 kMaxApiResponseBytes = 2 << 20;
constexpr juce::int64 kMaxChecksumListBytes = 1 << 20;
constexpr const char* kApiHeaders = "Accept: application/vnd.github+json\r\n"
                                    "X-GitHub-Api-Version: 2022-11-28";
constexpr const char* kBinaryHeaders = "Accept: application/octet-stream";

Error errorForStatus (int status)
{
    if (status == 403 || status == 429)
        return Error::RateLimited;
    if (status >= 500 || status == 0)
        return Error::Network;
    return Error::BadResponse;
}

juce::String hostOf (const juce::String& url)
{
    return juce::URL (url).getDomain();
}

/// "Name (2).ext", "Name (3).ext", ... the way file managers number copies.
juce::File numberedSibling (const juce::File& file)
{
    const auto name = file.getFileName();
    const auto ext =
        name.endsWithIgnoreCase (".tar.gz") ? name.getLastCharacters (7) : file.getFileExtension();
    const auto stem = name.dropLastCharacters (ext.length());
    for (int n = 2;; ++n)
        if (const auto candidate = file.getSiblingFile (stem + " (" + juce::String (n) + ")" + ext);
            !candidate.exists())
            return candidate;
}
} // namespace

juce::String toString (Error error)
{
    switch (error)
    {
    case Error::None:
        return {};
    case Error::Network:
        return "network";
    case Error::RateLimited:
        return "rateLimited";
    case Error::BadResponse:
        return "badResponse";
    case Error::NoInstaller:
        return "noInstaller";
    case Error::NoChecksum:
        return "noChecksum";
    case Error::ChecksumMismatch:
        return "checksumMismatch";
    case Error::Incomplete:
        return "incomplete";
    case Error::Disk:
        return "disk";
    case Error::OpenFailed:
        return "openFailed";
    case Error::Cancelled:
        return "cancelled";
    }
    return {};
}

// Cancellation ------------------------------------------------------------------------------------

void Cancellation::cancel()
{
    cancelled_ = true;
    const std::scoped_lock lock (lock_);
    if (stream_ != nullptr)
        stream_->cancel();
}

bool Cancellation::attach (juce::WebInputStream* stream)
{
    const std::scoped_lock lock (lock_);
    stream_ = stream;
    return !cancelled_;
}

void Cancellation::detach()
{
    const std::scoped_lock lock (lock_);
    stream_ = nullptr;
}

// Client ------------------------------------------------------------------------------------------

Client::Client (juce::String appVersion, Platform platform, juce::String apiUrl)
    : appVersion_ (std::move (appVersion)),
      platform_ (platform),
      apiUrl_ (std::move (apiUrl))
{
}

juce::String Client::userAgent() const
{
    return "SedefAmp/" + appVersion_ + " (" + platformName (platform_) + ")";
}

std::unique_ptr<juce::WebInputStream>
Client::open (const juce::String& url, const juce::String& headers, int timeoutMs) const
{
    auto stream = std::make_unique<juce::WebInputStream> (juce::URL (url), false);
    stream->withExtraHeaders (headers + "\r\nUser-Agent: " + userAgent())
        .withConnectionTimeout (timeoutMs)
        .withNumRedirectsToFollow (5); // asset downloads redirect to GitHub's CDN
    return stream;
}

Client::Response Client::get (const juce::String& url, const juce::String& headers,
                              juce::int64 maxBytes, Cancellation& cancel) const
{
    Response r;
    const auto fail = [&r, &cancel] (Error error, juce::String detail) {
        r.error = cancel.isCancelled() ? Error::Cancelled : error;
        r.detail = std::move (detail);
        return r;
    };
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + kTimeoutMs;

    auto stream = open (url, headers, kTimeoutMs);
    if (!cancel.attach (stream.get()))
        return fail (Error::Cancelled, {});
    const juce::ScopeGuard detach { [&cancel] { cancel.detach(); } };

    if (!stream->connect (nullptr))
        return fail (Error::Network, "could not connect to " + hostOf (url));
    r.status = stream->getStatusCode();
    if (r.status != 200)
        return fail (errorForStatus (r.status), "HTTP " + juce::String (r.status));
    if (stream->getTotalLength() > maxBytes)
        return fail (Error::BadResponse, "the response is too large");

    char buffer[16384];
    for (;;)
    {
        if (cancel.isCancelled())
            return fail (Error::Cancelled, {});
        if (juce::Time::getMillisecondCounterHiRes() > deadline)
            return fail (Error::Network,
                         "timed out after " + juce::String (kTimeoutMs / 1000) + " s");
        const int n = stream->read (buffer, static_cast<int> (sizeof (buffer)));
        if (n <= 0)
            break;
        r.body.append (buffer, static_cast<size_t> (n));
        if (static_cast<juce::int64> (r.body.getSize()) > maxBytes)
            return fail (Error::BadResponse, "the response is too large");
    }
    if (cancel.isCancelled())
        return fail (Error::Cancelled, {});
    return r;
}

CheckResult Client::fetchLatest (Cancellation& cancel) const
{
    CheckResult result;
    const auto r = get (apiUrl_, kApiHeaders, kMaxApiResponseBytes, cancel);
    if (r.status == 404)
        return result; // no stable release published yet
    if (r.error != Error::None)
    {
        result.error = r.error;
        result.detail = r.detail;
        return result;
    }

    auto parsed = parseRelease (juce::JSON::parse (r.body.toString()));
    switch (parsed.status)
    {
    case ParsedRelease::Status::Ok:
        result.release = std::move (parsed.release);
        break;
    case ParsedRelease::Status::Ignored:
        break; // the endpoint should never return one; treat it as "nothing newer"
    case ParsedRelease::Status::Invalid:
        result.error = Error::BadResponse;
        result.detail = parsed.reason;
        break;
    }
    return result;
}

DownloadResult Client::downloadVerified (const Release& release, const Asset& asset,
                                         const juce::File& folder, const DownloadProgress& progress,
                                         Cancellation& cancel) const
{
    DownloadResult result;
    const auto fail = [&result, &cancel] (Error error, juce::String detail) {
        result.error = cancel.isCancelled() ? Error::Cancelled : error;
        result.detail = std::move (detail);
        return result;
    };
    const auto report = [&progress, &asset] (DownloadStage stage, juce::int64 received) {
        if (progress)
            progress (stage, received, asset.size);
    };

    // 1. The expected checksum, from the same release.
    const auto* sums = release.findAsset (kChecksumsAssetName);
    if (sums == nullptr)
        return fail (Error::NoChecksum, juce::String ("the release has no ") + kChecksumsAssetName);
    report (DownloadStage::Checksums, 0);
    const auto list = get (sums->url, kBinaryHeaders, kMaxChecksumListBytes, cancel);
    if (list.error != Error::None)
        return fail (list.error, juce::String (kChecksumsAssetName) + ": " + list.detail);
    const auto table = parseChecksums (list.body.toString());
    const auto entry = table.find (asset.name);
    if (entry == table.end())
        return fail (Error::NoChecksum,
                     juce::String (kChecksumsAssetName) + " has no line for " + asset.name);
    const auto expected = entry->second;

    // 2. Where it goes. Never overwrite a file we did not verify.
    if (!folder.createDirectory())
        return fail (Error::Disk, "cannot create " + folder.getFullPathName());
    auto target = folder.getChildFile (asset.name);
    if (target.existsAsFile())
    {
        if (matchesChecksum (target, expected))
        {
            result.file = target;
            return result;
        }
        target = numberedSibling (target);
    }
    if (asset.size > 0 && folder.getBytesFreeOnVolume() < asset.size)
        return fail (Error::Disk, "not enough free space in " + folder.getFullPathName());
    const auto part = target.getSiblingFile (target.getFileName() + ".part");
    const juce::ScopeGuard removePart { [&part] { part.deleteFile(); } }; // a no-op once renamed

    // 3. Download.
    auto stream = open (asset.url, kBinaryHeaders, kTimeoutMs);
    if (!cancel.attach (stream.get()))
        return fail (Error::Cancelled, {});
    const juce::ScopeGuard detach { [&cancel] { cancel.detach(); } };
    if (!stream->connect (nullptr))
        return fail (Error::Network, "could not connect to " + hostOf (asset.url));
    if (const int status = stream->getStatusCode(); status != 200)
        return fail (errorForStatus (status), "HTTP " + juce::String (status));

    juce::int64 received = 0;
    {
        part.deleteFile();
        juce::FileOutputStream out (part);
        if (!out.openedOk())
            return fail (Error::Disk, out.getStatus().getErrorMessage());
        std::vector<char> buffer (1 << 16);
        for (;;)
        {
            if (cancel.isCancelled())
                return fail (Error::Cancelled, {});
            const int n = stream->read (buffer.data(), static_cast<int> (buffer.size()));
            if (n <= 0)
                break;
            if (!out.write (buffer.data(), static_cast<size_t> (n)))
                return fail (Error::Disk, "could not write " + part.getFullPathName());
            received += n;
            if (asset.size > 0 && received > asset.size)
                return fail (Error::Incomplete, "more data than the release lists");
            report (DownloadStage::Downloading, received);
        }
        out.flush();
        if (out.getStatus().failed())
            return fail (Error::Disk, out.getStatus().getErrorMessage());
    }
    if (cancel.isCancelled())
        return fail (Error::Cancelled, {});
    if (asset.size > 0 ? received != asset.size : received == 0)
        return fail (Error::Incomplete, "received " + juce::String (received) + " of " +
                                            juce::String (asset.size) + " bytes");

    // 4. Verify, then give it its real name.
    report (DownloadStage::Verifying, received);
    const auto actual = sha256Hex (part);
    if (actual != expected)
        return fail (Error::ChecksumMismatch, "expected " + expected + ", got " + actual);
    if (!part.moveFileTo (target))
        return fail (Error::Disk, "could not rename " + part.getFullPathName());
    result.file = target;
    return result;
}

juce::File downloadsFolder()
{
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
#if JUCE_LINUX || JUCE_BSD
    // xdg-user-dirs names it per desktop and language: XDG_DOWNLOAD_DIR="$HOME/Downloads".
    auto config = juce::SystemStats::getEnvironmentVariable ("XDG_CONFIG_HOME", {});
    if (!juce::File::isAbsolutePath (config))
        config = home.getChildFile (".config").getFullPathName();
    const auto dirs = juce::File (config).getChildFile ("user-dirs.dirs").loadFileAsString();
    for (const auto& line : juce::StringArray::fromLines (dirs))
        if (line.trim().startsWith ("XDG_DOWNLOAD_DIR="))
        {
            const auto value = line.fromFirstOccurrenceOf ("=", false, false)
                                   .trim()
                                   .unquoted()
                                   .replace ("$HOME", home.getFullPathName());
            if (juce::File::isAbsolutePath (value) && juce::File (value).isDirectory())
                return juce::File (value);
        }
#endif
    const auto downloads = home.getChildFile ("Downloads");
    return downloads.isDirectory() ? downloads : home;
}

} // namespace sedef::update
