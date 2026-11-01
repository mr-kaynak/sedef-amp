// SPDX-License-Identifier: AGPL-3.0-or-later
// The update client's requests, downloads and checks against canned responses from a local HTTP
// server, so every failure path runs without GitHub. (tools/sedef_update_check.cpp runs the same
// code against the real API.)
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
