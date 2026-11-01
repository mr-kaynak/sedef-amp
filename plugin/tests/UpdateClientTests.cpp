// SPDX-License-Identifier: AGPL-3.0-or-later
// The update client's requests, downloads and checks against canned responses from a local HTTP
// server, so every failure path runs without GitHub. (tools/sedef_update_check.cpp runs the same
// code against the real API.)
#include "DownloadMark.h"
#include "LocalServer.h"
#include "UpdateClient.h"

#include <catch2/catch_test_macros.hpp>

using namespace sedef::update;
using sedef::update::test::LocalServer;

namespace
{
juce::MemoryBlock payload (size_t size)
{
    juce::MemoryBlock block (size);
    for (size_t i = 0; i < size; ++i)
        static_cast<char*> (block.getData())[i] = static_cast<char> ('a' + i % 26);
    return block;
}

juce::String sha256Of (const juce::MemoryBlock& block, const juce::File& scratch)
{
    const auto f = scratch.getNonexistentChildFile ("hash", ".bin");
    REQUIRE (f.replaceWithData (block.getData(), block.getSize()));
    auto hex = sha256Hex (f);
    f.deleteFile();
    return hex;
}

/// A release whose assets live on the local server.
Release localRelease (const LocalServer& server, juce::int64 installerSize)
{
    Release r;
    r.version = *Version::parse ("0.2.0");
    r.tag = "v0.2.0";
    r.assets.push_back (
        { "Sedef-Amp-macOS.dmg", installerSize, server.url ("/Sedef-Amp-macOS.dmg") });
    r.assets.push_back ({ kChecksumsAssetName, 100, server.url ("/SHA256SUMS.txt") });
    return r;
}

struct Fixture
{
    LocalServer server;
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("sedef-update-client-" + juce::Uuid().toString());
    juce::File scratch = folder.getSiblingFile (folder.getFileName() + "-scratch");
    Client client { "0.1.0", Platform::MacOS, server.url ("/repos/x/releases/latest") };
    Cancellation cancel;
    juce::MemoryBlock installer = payload (300000); // several reads, not a multiple of a block

    Fixture()
    {
        REQUIRE (scratch.createDirectory());
        server.serve ("/Sedef-Amp-macOS.dmg", 200, installer);
        server.serve ("/SHA256SUMS.txt", 200,
                      sha256Of (installer, scratch) + "  Sedef-Amp-macOS.dmg\n" +
                          juce::String::repeatedString ("0", 64) +
                          "  Sedef-Amp-Windows-Setup.exe\n");
    }

    ~Fixture()
    {
        folder.deleteRecursively();
        scratch.deleteRecursively();
    }

    DownloadResult download (const Release& release, std::vector<DownloadStage>* stages = nullptr)
    {
        return client.downloadVerified (
            release, release.assets[0], folder,
            [stages] (DownloadStage s, juce::int64, juce::int64) {
                if (stages != nullptr && (stages->empty() || stages->back() != s))
                    stages->push_back (s);
            },
            cancel);
    }

    juce::StringArray filesInFolder() const
    {
        juce::StringArray names;
        for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*"))
            names.add (f.getFileName());
        names.sort (false);
        return names;
    }
};
} // namespace

// Checking ----------------------------------------------------------------------------------------

TEST_CASE ("The check sends one GET with only the version and OS", "[update][client]")
{
    Fixture f;
    f.server.serve ("/repos/x/releases/latest", 200,
                    R"({"tag_name": "v0.2.0", "name": "Sedef Amp 0.2.0", "assets": []})");
    const auto result = f.client.fetchLatest (f.cancel);
    REQUIRE (result.error == Error::None);
    REQUIRE (result.release.has_value());
    CHECK (result.release->version.toString() == "0.2.0");

    const auto requests = f.server.requests();
    REQUIRE (requests.size() == 1);
    const auto head = requests[0];
    CHECK (head.startsWith ("GET /repos/x/releases/latest "));
    CHECK (head.containsIgnoreCase ("User-Agent: SedefAmp/0.1.0 (macOS)\r\n"));
    CHECK (head.containsIgnoreCase ("Accept: application/vnd.github+json\r\n"));
    CHECK_FALSE (head.containsIgnoreCase ("Cookie:"));
    CHECK_FALSE (head.containsIgnoreCase ("Authorization:"));
}

TEST_CASE ("Check results map to plain outcomes", "[update][client]")
{
    Fixture f;
    const auto check = [&f] (int status, const juce::String& body) {
        f.server.serve ("/repos/x/releases/latest", status, body);
        return f.client.fetchLatest (f.cancel);
    };

    auto r = check (404, R"({"message": "Not Found"})"); // nothing published yet
    CHECK (r.error == Error::None);
    CHECK_FALSE (r.release.has_value());

    r = check (200, R"({"tag_name": "v0.3.0", "draft": true})");
    CHECK (r.error == Error::None);
    CHECK_FALSE (r.release.has_value());

    CHECK (check (403, R"({"message": "API rate limit exceeded"})").error == Error::RateLimited);
    CHECK (check (429, "").error == Error::RateLimited);
    CHECK (check (503, "").error == Error::Network);
    CHECK (check (200, "<html>captive portal</html>").error == Error::BadResponse);
    CHECK (check (200, R"({"tag_name": "nightly"})").error == Error::BadResponse);
}

TEST_CASE ("A server that is not there is a network error", "[update][client]")
{
    int port = 0;
    {
        juce::StreamingSocket probe;
        REQUIRE (probe.createListener (0, "127.0.0.1"));
        port = probe.getBoundPort();
    } // closed: nothing listens there now
    Cancellation cancel;
    const Client client ("0.1.0", Platform::Linux,
                         "http://127.0.0.1:" + juce::String (port) + "/latest");
    CHECK (client.fetchLatest (cancel).error == Error::Network);
}

TEST_CASE ("Redirects are followed by hand, only where they may lead", "[update][client]")
{
    Fixture f;
    // The API moved (a renamed repository): same origin, followed.
    f.server.redirect ("/repos/x/releases/latest", 301, "/repositories/1/releases/latest");
    f.server.serve ("/repositories/1/releases/latest", 200, R"({"tag_name": "v0.2.0"})");
    const auto check = f.client.fetchLatest (f.cancel);
    CHECK (check.error == Error::None);
    CHECK (check.release.has_value());

    // An asset on another server: GitHub's CDN would be https on its own hosts.
    const auto release = localRelease (f.server, 300000);
    for (const auto& elsewhere : { f.server.url ("/cdn").replace ("127.0.0.1", "localhost"),
                                   juce::String ("https://evil.example/x"),
                                   juce::String ("http://objects.githubusercontent.com/x") })
    {
        f.server.redirect ("/Sedef-Amp-macOS.dmg", 302, elsewhere);
        const auto result = f.download (release);
        INFO (elsewhere);
        CHECK (result.error == Error::BadResponse);
        CHECK (result.detail.contains ("redirect"));
    }
    CHECK (f.server.hits ("/cdn") == 0);

    // A loop ends.
    f.server.redirect ("/loop", 307, "/loop");
    auto looping = release;
    looping.assets[0].url = f.server.url ("/loop");
    const auto loop = f.download (looping);
    CHECK (loop.error == Error::BadResponse);
    CHECK (loop.detail.contains ("too many redirects"));
    CHECK (f.server.hits ("/loop") == Client::kMaxRedirects + 1);
    CHECK (f.filesInFolder().isEmpty());

    // A path on the same server is followed, and the download still has to verify.
    f.server.redirect ("/Sedef-Amp-macOS.dmg", 302, "/cdn/Sedef-Amp-macOS.dmg");
    f.server.serve ("/cdn/Sedef-Amp-macOS.dmg", 200, f.installer);
    CHECK (f.download (release).error == Error::None);
    CHECK (f.server.hits ("/cdn/Sedef-Amp-macOS.dmg") == 1);
}

TEST_CASE ("A cancelled check does not run", "[update][client]")
{
    Fixture f;
    f.cancel.cancel();
    CHECK (f.client.fetchLatest (f.cancel).error == Error::Cancelled);
    CHECK (f.server.requests().isEmpty());
}

// Downloading -------------------------------------------------------------------------------------

TEST_CASE ("Downloads are verified before they get their name", "[update][client]")
{
    Fixture f;
    std::vector<DownloadStage> stages;
    const auto result = f.download (localRelease (f.server, 300000), &stages);
    REQUIRE (result.error == Error::None);
    CHECK (result.file == f.folder.getChildFile ("Sedef-Amp-macOS.dmg"));
    juce::MemoryBlock written;
    REQUIRE (result.file.loadFileAsData (written));
    CHECK (written == f.installer);
    CHECK (f.filesInFolder() == juce::StringArray { "Sedef-Amp-macOS.dmg" }); // no .part left
    CHECK (stages == std::vector<DownloadStage> { DownloadStage::Checksums,
                                                  DownloadStage::Downloading,
                                                  DownloadStage::Verifying });
    for (const auto& head : f.server.requests())
        CHECK (head.containsIgnoreCase ("User-Agent: SedefAmp/0.1.0 (macOS)\r\n"));
}

TEST_CASE ("A download that does not match its checksum is deleted", "[update][client]")
{
    Fixture f;
    auto tampered = f.installer;
    static_cast<char*> (tampered.getData())[1234] ^= 1;
    f.server.serve ("/Sedef-Amp-macOS.dmg", 200, tampered);
    const auto result = f.download (localRelease (f.server, 300000));
    CHECK (result.error == Error::ChecksumMismatch);
    CHECK (result.detail.contains ("expected"));
    CHECK (result.file == juce::File());
    CHECK (f.filesInFolder().isEmpty());
}

TEST_CASE ("No checksum, no download", "[update][client]")
{
    Fixture f;
    auto release = localRelease (f.server, 300000);
    release.assets.pop_back(); // no SHA256SUMS.txt in the release
    CHECK (f.download (release).error == Error::NoChecksum);

    f.server.serve ("/SHA256SUMS.txt", 200,
                    juce::String::repeatedString ("0", 64) + "  Some-Other-File.zip\n");
    CHECK (f.download (localRelease (f.server, 300000)).error == Error::NoChecksum);
    CHECK (f.server.hits ("/Sedef-Amp-macOS.dmg") == 0);
    CHECK (f.filesInFolder().isEmpty());
}

TEST_CASE ("A short or oversized download is incomplete", "[update][client]")
{
    Fixture f;
    CHECK (f.download (localRelease (f.server, 300001)).error == Error::Incomplete);
    CHECK (f.download (localRelease (f.server, 299999)).error == Error::Incomplete);
    CHECK (f.filesInFolder().isEmpty());
}

TEST_CASE ("Download HTTP errors are reported", "[update][client]")
{
    Fixture f;
    f.server.serve ("/Sedef-Amp-macOS.dmg", 404, juce::String());
    CHECK (f.download (localRelease (f.server, 300000)).error == Error::BadResponse);
    f.server.serve ("/Sedef-Amp-macOS.dmg", 502, juce::String());
    CHECK (f.download (localRelease (f.server, 300000)).error == Error::Network);
    f.server.serve ("/SHA256SUMS.txt", 403, juce::String());
    CHECK (f.download (localRelease (f.server, 300000)).error == Error::RateLimited);
    CHECK (f.filesInFolder().isEmpty());
}

TEST_CASE ("A release without a usable installer size downloads nothing", "[update][client]")
{
    Fixture f;
    for (const auto size : { juce::int64 { 0 }, kMaxInstallerBytes + 1 })
    {
        const auto result = f.download (localRelease (f.server, size));
        CHECK (result.error == Error::BadResponse);
        CHECK (result.detail.contains ("size"));
    }
    CHECK (f.server.requests().isEmpty());
    CHECK (f.filesInFolder().isEmpty());
}

TEST_CASE ("A stalled download fails instead of hanging", "[update][client]")
{
    // The server stalls for less than the backends' own 10 s timeouts, which would also end it.
    // One fixture each: the test server serves one connection at a time, stall included.
    const auto stalled = [] (auto&& stall) {
        Fixture f;
        f.client.setStallTimeout (1000);
        stall (f.server);
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto result = f.download (localRelease (f.server, 300000));
        CHECK (juce::Time::getMillisecondCounterHiRes() - started < 6000.0);
        CHECK (f.filesInFolder().isEmpty());
        return result;
    };

    const auto midway = stalled ([] (LocalServer& s) { s.pauseAfter (100000, 8000); });
    CHECK (midway.error == Error::Network);
    CHECK (midway.detail.contains ("no data"));

    const auto silent =
        stalled ([] (LocalServer& s) { s.delay ("/Sedef-Amp-macOS.dmg", 8000); }); // no headers
    CHECK (silent.error == Error::Network);
}

TEST_CASE ("Downloads carry the OS's downloaded-file mark", "[update][client]")
{
    Fixture f;
    const bool marks = currentPlatform() != Platform::Linux;
    const auto release = localRelease (f.server, 300000);
    const auto result = f.download (release);
    REQUIRE (result.error == Error::None);
    CHECK (hasDownloadMark (result.file) == marks);

    // A verified copy that came without one (copied in by hand) gets it before it is opened.
    REQUIRE (result.file.deleteFile());
    REQUIRE (result.file.replaceWithData (f.installer.getData(), f.installer.getSize()));
    REQUIRE_FALSE (hasDownloadMark (result.file));
    REQUIRE (f.download (release).error == Error::None);
    CHECK (f.server.hits ("/Sedef-Amp-macOS.dmg") == 1); // reused, not fetched again
    CHECK (hasDownloadMark (result.file) == marks);
    CHECK (f.filesInFolder() == juce::StringArray { "Sedef-Amp-macOS.dmg" }); // no stray stream
}

TEST_CASE ("Cancelling mid-download removes the partial file", "[update][client]")
{
    Fixture f;
    const auto release = localRelease (f.server, 300000);
    const auto result = f.client.downloadVerified (
        release, release.assets[0], f.folder,
        [&f] (DownloadStage stage, juce::int64, juce::int64) {
            if (stage == DownloadStage::Downloading)
                f.cancel.cancel();
        },
        f.cancel);
    CHECK (result.error == Error::Cancelled);
    CHECK (f.filesInFolder().isEmpty());
}

TEST_CASE ("A verified copy is reused, a stranger's file is left alone", "[update][client]")
{
    Fixture f;
    const auto release = localRelease (f.server, 300000);
    REQUIRE (f.download (release).error == Error::None);
    REQUIRE (f.server.hits ("/Sedef-Amp-macOS.dmg") == 1);

    // Same release again: the file in Downloads already matches, nothing is fetched.
    REQUIRE (f.download (release).error == Error::None);
    CHECK (f.server.hits ("/Sedef-Amp-macOS.dmg") == 1);

    // A different file under that name is the user's: download next to it.
    const auto mine = f.folder.getChildFile ("Sedef-Amp-macOS.dmg");
    REQUIRE (mine.replaceWithText ("my own file"));
    const auto result = f.download (release);
    REQUIRE (result.error == Error::None);
    CHECK (result.file.getFileName() == "Sedef-Amp-macOS (2).dmg");
    CHECK (mine.loadFileAsString() == "my own file");
    CHECK (f.filesInFolder() ==
           juce::StringArray { "Sedef-Amp-macOS (2).dmg", "Sedef-Amp-macOS.dmg" });
}
