// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UpdateClient.h"

#include "DownloadMark.h"

#include <chrono>
#include <condition_variable>
#include <thread>

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

bool isRedirect (int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
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

/// Calls `onTimeout` once `limitMs` pass without a touch(). The backends' own timeouts differ
/// (some only cover connecting, some reset on every packet), and a blocked read only wakes when
/// its stream is cancelled, so this thread does it.
class Watchdog
{
public:
    Watchdog (int limitMs, std::function<void()> onTimeout)
        : limit_ (limitMs),
          onTimeout_ (std::move (onTimeout)),
          thread_ ([this] { run(); })
    {
    }

    ~Watchdog()
    {
        {
            const std::scoped_lock l (lock_);
            done_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    void touch()
    {
        const std::scoped_lock l (lock_);
        last_ = Clock::now();
    }

    bool fired() const noexcept { return fired_.load(); }

private:
    using Clock = std::chrono::steady_clock;

    void run()
    {
        std::unique_lock l (lock_);
        while (!done_)
        {
            if (const auto due = last_ + limit_; Clock::now() < due)
            {
                wake_.wait_until (l, due);
                continue;
            }
            fired_ = true;
            l.unlock();
            onTimeout_();
            return;
        }
    }

    const std::chrono::milliseconds limit_;
    const std::function<void()> onTimeout_;
    std::mutex lock_;
    std::condition_variable wake_;
    Clock::time_point last_ = Clock::now(); // guarded by lock_
    bool done_ = false;                     // guarded by lock_
    std::atomic<bool> fired_ { false };
    std::thread thread_; // last: starts once everything above exists
};
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
    case Error::BadPackage:
        return "badPackage";
    case Error::ElevationFailed:
        return "elevationFailed";
    case Error::InstallFailed:
        return "installFailed";
    case Error::Unverified:
        return "unverified";
    case Error::Cancelled:
        return "cancelled";
    }
    return {};
}

// Cancellation ------------------------------------------------------------------------------------

void Cancellation::cancel()
{
    cancelled_ = true;
    interrupt();
}

void Cancellation::interrupt()
{
    const std::scoped_lock lock (lock_);
    if (stream_ != nullptr)
        stream_->cancel();
}

bool Cancellation::attach (juce::WebInputStream* stream)
{
    const std::scoped_lock lock (lock_);
    if (cancelled_)
        return false;
    stream_ = stream;
    return true;
}

void Cancellation::detach()
{
    const std::scoped_lock lock (lock_);
    stream_ = nullptr;
}

// Client ------------------------------------------------------------------------------------------

Client::Client (juce::String appVersion, Platform platform, juce::String apiUrl,
                juce::String releasesUrl)
    : appVersion_ (std::move (appVersion)),
      platform_ (platform),
      apiUrl_ (std::move (apiUrl)),
      releasesUrl_ (std::move (releasesUrl))
{
}

juce::String Client::userAgent() const
{
    return "SedefAmp/" + appVersion_ + " (" + platformName (platform_) + ")";
}

Client::Response Client::fetch (const juce::String& url, const char* headers, Limit limit,
                                int limitMs, const Sink& sink, Cancellation& cancel) const
{
    Response r;
    // Declared first, so it stops last: after the stream is detached and gone.
    Watchdog watchdog (limitMs, [&cancel] { cancel.interrupt(); });
    const auto fail = [&r, &cancel, &watchdog, limit, limitMs] (Error error, juce::String detail) {
        if (cancel.isCancelled())
        {
            r.error = Error::Cancelled;
        }
        else if (watchdog.fired())
        {
            r.error = Error::Network;
            const auto seconds = juce::String (limitMs / 1000) + " s";
            detail =
                limit == Limit::Total ? "timed out after " + seconds : "no data for " + seconds;
        }
        else
        {
            r.error = error;
        }
        r.detail = std::move (detail);
        return r;
    };

    std::unique_ptr<juce::WebInputStream> stream;
    const juce::ScopeGuard detach { [&cancel] { cancel.detach(); } }; // before the stream goes
    auto address = url;
    for (int redirects = 0;; ++redirects)
    {
        cancel.detach(); // the previous hop's stream is about to go
        stream = std::make_unique<juce::WebInputStream> (juce::URL (address), false);
        stream->withExtraHeaders (juce::String (headers) + "\r\nUser-Agent: " + userAgent())
            .withConnectionTimeout (kTimeoutMs)
            .withNumRedirectsToFollow (0); // each one is checked below
        // A timeout that fired before this stream was attached could not interrupt it.
        if (!cancel.attach (stream.get()) || watchdog.fired())
            return fail (Error::Cancelled, {});
        if (!stream->connect (nullptr))
            return fail (Error::Network, "could not connect to " + hostOf (address));
        r.status = stream->getStatusCode();
        if (!isRedirect (r.status))
            break;
        if (redirects == kMaxRedirects)
            return fail (Error::BadResponse, "too many redirects");
        const auto location = stream->getResponseHeaders()["Location"];
        address = redirectTarget (address, location);
        if (address.isEmpty())
            return fail (Error::BadResponse,
                         "not following a redirect to \"" + location.substring (0, 100) + "\"");
    }
    if (r.status != 200)
        return fail (errorForStatus (r.status), "HTTP " + juce::String (r.status));

    std::vector<char> buffer (1 << 16);
    for (;;)
    {
        if (cancel.isCancelled() || watchdog.fired())
            return fail (Error::Cancelled, {});
        const int n = stream->read (buffer.data(), static_cast<int> (buffer.size()));
        if (n <= 0)
            break;
        if (limit == Limit::Quiet)
            watchdog.touch();
        juce::String detail;
        if (const auto error = sink (buffer.data(), n, detail); error != Error::None)
            return fail (error, detail);
    }
    // A read that a timeout interrupted ends like the end of the body.
    if (cancel.isCancelled() || watchdog.fired())
        return fail (Error::Cancelled, {});
    return r;
}

Client::Body Client::get (const juce::String& url, const char* headers, juce::int64 maxBytes,
                          Cancellation& cancel) const
{
    Body b;
    static_cast<Response&> (b) = fetch (
        url, headers, Limit::Total, kTimeoutMs,
        [&b, maxBytes] (const char* data, int size, juce::String& detail) {
            b.data.append (data, static_cast<size_t> (size));
            if (static_cast<juce::int64> (b.data.getSize()) <= maxBytes)
                return Error::None;
            detail = "the response is too large";
            return Error::BadResponse;
        },
        cancel);
    return b;
}

CheckResult Client::fetchLatest (Cancellation& cancel) const
{
    CheckResult result;
    const auto r = get (apiUrl_, kApiHeaders, kMaxApiResponseBytes, cancel);
    if (r.status == 404 && r.error != Error::Cancelled)
        return result; // no stable release published yet
    if (r.error != Error::None)
    {
        result.error = r.error;
        result.detail = r.detail;
        return result;
    }

    auto parsed = parseRelease (juce::JSON::parse (r.data.toString()), releasesUrl_);
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
                                         Cancellation& cancel, DownloadOptions options) const
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
    juce::String expected;
    const auto finish = [&result, &asset, &release, &options, &expected] (const juce::File& file) {
        if (options.mark)
            markAsDownloaded (file, asset.url, release.pageUrl); // best effort: see DownloadMark.h
        result.file = file;
        result.sha256 = expected;
        return result;
    };

    // 0. A size to hold the download to, and one the disk can take.
    if (asset.size <= 0 || asset.size > kMaxInstallerBytes)
        return fail (Error::BadResponse,
                     asset.name + " lists a size of " + juce::String (asset.size) + " bytes");

    // 1. The expected checksum, from the same release.
    const auto* sums = release.findAsset (kChecksumsAssetName);
    if (sums == nullptr)
        return fail (Error::NoChecksum, juce::String ("the release has no ") + kChecksumsAssetName);
    report (DownloadStage::Checksums, 0);
    const auto list = get (sums->url, kBinaryHeaders, kMaxChecksumListBytes, cancel);
    if (list.error != Error::None)
        return fail (list.error, juce::String (kChecksumsAssetName) + ": " + list.detail);
    const auto table = parseChecksums (list.data.toString());
    const auto entry = table.find (asset.name);
    if (entry == table.end())
        return fail (Error::NoChecksum,
                     juce::String (kChecksumsAssetName) + " has no single line for " + asset.name);
    expected = entry->second;

    // 2. Where it goes. Never overwrite a file we did not verify.
    if (!folder.createDirectory())
        return fail (Error::Disk, "cannot create " + folder.getFullPathName());
    auto target = folder.getChildFile (asset.name);
    if (target.existsAsFile())
    {
        if (matchesChecksum (target, expected))
            return finish (target);
        if (!options.replace)
            target = numberedSibling (target);
        else if (!target.deleteFile())
            return fail (Error::Disk, "cannot replace " + target.getFullPathName());
    }
    if (folder.getBytesFreeOnVolume() < asset.size)
        return fail (Error::Disk, "not enough free space in " + folder.getFullPathName());
    const auto part = target.getSiblingFile (target.getFileName() + ".part");
    const juce::ScopeGuard removePart { [&part] { part.deleteFile(); } }; // a no-op once renamed

    // 3. Download.
    juce::int64 received = 0;
    {
        part.deleteFile();
        juce::FileOutputStream out (part);
        if (!out.openedOk())
            return fail (Error::Disk, out.getStatus().getErrorMessage());
        const auto fetched = fetch (
            asset.url, kBinaryHeaders, Limit::Quiet, stallTimeoutMs_,
            [&] (const char* data, int size, juce::String& detail) {
                if (!out.write (data, static_cast<size_t> (size)))
                {
                    detail = "could not write " + part.getFullPathName();
                    return Error::Disk;
                }
                received += size;
                if (received > asset.size)
                {
                    detail = "more data than the release lists";
                    return Error::Incomplete;
                }
                report (DownloadStage::Downloading, received);
                return Error::None;
            },
            cancel);
        if (fetched.error != Error::None)
            return fail (fetched.error, fetched.detail);
        out.flush();
        if (out.getStatus().failed())
            return fail (Error::Disk, out.getStatus().getErrorMessage());
    }
    if (received != asset.size)
        return fail (Error::Incomplete, "received " + juce::String (received) + " of " +
                                            juce::String (asset.size) + " bytes");

    // 4. Verify, then give it its real name.
    report (DownloadStage::Verifying, received);
    const auto actual = sha256Hex (part);
    if (actual != expected)
        return fail (Error::ChecksumMismatch, "expected " + expected + ", got " + actual);
    if (!part.moveFileTo (target))
        return fail (Error::Disk, "could not rename " + part.getFullPathName());
    return finish (target);
}

DownloadResult copyVerified (const juce::File& file, const juce::String& sha256,
                             const juce::File& folder, const juce::String& sourceUrl,
                             const juce::String& pageUrl)
{
    DownloadResult result;
    const auto fail = [&result] (Error error, juce::String detail) {
        result.error = error;
        result.detail = std::move (detail);
        return result;
    };
    const auto finish = [&] (const juce::File& target) {
        markAsDownloaded (target, sourceUrl, pageUrl);
        result.file = target;
        result.sha256 = sha256;
        return result;
    };
    if (!matchesChecksum (file, sha256))
        return fail (Error::ChecksumMismatch,
                     file.getFileName() + " changed after it was verified");
    if (!folder.createDirectory())
        return fail (Error::Disk, "cannot create " + folder.getFullPathName());
    auto target = folder.getChildFile (file.getFileName());
    if (target.existsAsFile())
    {
        if (matchesChecksum (target, sha256))
            return finish (target);
        target = numberedSibling (target);
    }
    if (folder.getBytesFreeOnVolume() < file.getSize())
        return fail (Error::Disk, "not enough free space in " + folder.getFullPathName());
    const auto part = target.getSiblingFile (target.getFileName() + ".part");
    const juce::ScopeGuard removePart { [&part] { part.deleteFile(); } };
    part.deleteFile();
    if (!file.copyFileTo (part))
        return fail (Error::Disk, "could not write " + part.getFullPathName());
    if (!matchesChecksum (part, sha256))
        return fail (Error::ChecksumMismatch,
                     "the copy in " + folder.getFullPathName() + " does not match");
    if (!part.moveFileTo (target))
        return fail (Error::Disk, "could not rename " + part.getFullPathName());
    return finish (target);
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
