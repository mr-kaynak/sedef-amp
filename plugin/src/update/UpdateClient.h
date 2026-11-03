// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "UpdateLogic.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

/// The update checker's network side: one GET to the GitHub releases API, and the verified
/// download of a release asset. Everything blocks, so callers run it on a background thread; a
/// Cancellation aborts it from another thread. Redirects are followed by hand, only where
/// redirectTarget allows. Shared by the plug-in (UpdateService) and the sedef-update-check tool.
namespace sedef::update
{

enum class Error
{
    None,
    Network,          // could not connect, timed out, stalled, or the server failed (5xx)
    RateLimited,      // GitHub answered 403/429
    BadResponse,      // unexpected status, a redirect we do not follow, or an unreadable body
    NoInstaller,      // the release has no installer for this OS
    NoChecksum,       // no SHA256SUMS.txt, or no line for the installer in it
    ChecksumMismatch, // the download does not match its published SHA-256 (it was deleted)
    Incomplete,       // the download ended early or has the wrong size
    Disk,             // could not write to the download folder
    OpenFailed,       // verified, but the OS would not open it
    BadPackage,       // verified, but its contents are not what Sedef's installer looks like
    ElevationFailed,  // the OS could not ask for permission (a sandbox, no polkit agent)
    InstallFailed,    // the OS's installer ran and reported a failure
    Unverified,       // installed, but the installed files don't have the release's version
    Cancelled
};

/// The code the UI maps to a message: "network", "rateLimited", ...
juce::String toString (Error error);

/// Lets another thread abort a blocking request (the user pressed Cancel, or the plug-in unloads).
class Cancellation
{
public:
    void cancel();
    bool isCancelled() const noexcept { return cancelled_.load(); }

private:
    friend class Client;
    /// The stream a cancel() aborts until detach(), which must come before the stream is
    /// destroyed. False, and nothing attached, once cancelled.
    bool attach (juce::WebInputStream* stream);
    void detach();
    /// Aborts the request in flight without cancelling the operation: how a timeout wakes a
    /// blocked read.
    void interrupt();

    std::mutex lock_;
    juce::WebInputStream* stream_ = nullptr; // guarded by lock_
    std::atomic<bool> cancelled_ { false };
};

struct CheckResult
{
    Error error = Error::None;
    juce::String detail; // technical detail for logs and the CLI, not shown as the message
    /// The latest stable release; empty when the repository has published none.
    std::optional<Release> release;
};

struct DownloadResult
{
    Error error = Error::None;
    juce::String detail;
    juce::File file;     // the verified file, on success
    juce::String sha256; // its checksum from SHA256SUMS.txt, on success
};

struct DownloadOptions
{
    /// Give the file the OS's downloaded-file mark (markAsDownloaded). Off for the private cache:
    /// Sedef hands that copy to the OS's installer itself.
    bool mark = true;
    /// A different file with the same name is replaced instead of left alone (the cache is
    /// Sedef's own).
    bool replace = false;
};

enum class DownloadStage
{
    Checksums,
    Downloading,
    Verifying
};
using DownloadProgress =
    std::function<void (DownloadStage stage, juce::int64 received, juce::int64 total)>;

class Client
{
public:
    /// The API request and the checksum list, from start to finish.
    static constexpr int kTimeoutMs = 10000;
    /// A download fails after this long without a byte.
    static constexpr int kStallTimeoutMs = 60000;
    static constexpr int kMaxRedirects = 5;

    /// Only tests pass other URLs.
    Client (juce::String appVersion, Platform platform, juce::String apiUrl = latestReleaseApiUrl(),
            juce::String releasesUrl = releasesPageUrl());

    /// Tests only: a shorter kStallTimeoutMs.
    void setStallTimeout (int ms) noexcept { stallTimeoutMs_ = ms; }

    /// "SedefAmp/<version> (<OS>)": all that identifies a request. No ids, no OS version.
    juce::String userAgent() const;

    /// GET /repos/<repository>/releases/latest, at most kTimeoutMs from start to finish. A 404
    /// (nothing published) and a draft or pre-release (never returned there) both mean "no
    /// release".
    CheckResult fetchLatest (Cancellation& cancel) const;

    /// Downloads `asset` of `release` into `folder` and verifies it against the release's
    /// SHA256SUMS.txt before it gets its final name: the bytes go to "<name>.part", which is
    /// deleted on any failure. The asset must list its size (at most kMaxInstallerBytes), and the
    /// download must match it. A verified copy already in the folder is reused; a different file
    /// with the same name is left alone and the download gets a new name. The result carries the
    /// OS's downloaded-file mark (markAsDownloaded), so the OS checks it before it runs.
    DownloadResult downloadVerified (const Release& release, const Asset& asset,
                                     const juce::File& folder, const DownloadProgress& progress,
                                     Cancellation& cancel, DownloadOptions options = {}) const;

private:
    struct Response
    {
        Error error = Error::None;
        juce::String detail;
        int status = 0; // of the last response, also on failure
    };
    struct Body : Response
    {
        juce::MemoryBlock data;
    };
    /// Takes each chunk of a response body; anything but Error::None stops the transfer.
    using Sink = std::function<Error (const char* data, int size, juce::String& detail)>;
    enum class Limit
    {
        Total, // limitMs from start to finish
        Quiet  // limitMs without a byte
    };

    /// GETs `url`, follows allowed redirects and feeds the body of the final 200 to `sink`.
    Response fetch (const juce::String& url, const char* headers, Limit limit, int limitMs,
                    const Sink& sink, Cancellation& cancel) const;
    /// A small response held in memory, kTimeoutMs from start to finish.
    Body get (const juce::String& url, const char* headers, juce::int64 maxBytes,
              Cancellation& cancel) const;

    juce::String appVersion_;
    Platform platform_;
    juce::String apiUrl_, releasesUrl_;
    int stallTimeoutMs_ = kStallTimeoutMs;
};

/// The user's Downloads folder (XDG_DOWNLOAD_DIR on Linux), or the home folder without one.
juce::File downloadsFolder();

/// Copies a verified file into `folder` for the user to run ("Install manually"): checked again
/// against `sha256`, written as "<name>.part" and renamed, with the OS's downloaded-file mark. A
/// verified copy already there is reused; a different file with the same name is left alone and
/// the copy is numbered, as downloadVerified does.
DownloadResult copyVerified (const juce::File& file, const juce::String& sha256,
                             const juce::File& folder, const juce::String& sourceUrl,
                             const juce::String& pageUrl);

} // namespace sedef::update
