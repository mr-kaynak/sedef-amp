// SPDX-License-Identifier: AGPL-3.0-or-later
// Unit tests for the update checker's pure logic (plugin/src/update/UpdateLogic.h).
#include "UpdateLogic.h"

#include <catch2/catch_test_macros.hpp>

using namespace sedef::update;

namespace
{
Version v (const char* text)
{
    const auto parsed = Version::parse (text);
    REQUIRE (parsed.has_value());
    return *parsed;
}

const juce::String kDownload = "https://github.com/mr-kaynak/sedef-amp/releases/download/";

juce::var asset (const juce::String& name, const juce::String& tag, juce::int64 size = 1000)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", name);
    o->setProperty ("size", size);
    o->setProperty ("browser_download_url", kDownload + tag + "/" + name);
    return juce::var (o);
}

/// A release object as GET /repos/{owner}/{repo}/releases/latest returns it (trimmed).
juce::var releaseJson (const juce::String& tag, juce::Array<juce::var> assets)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("url", "https://api.github.com/repos/mr-kaynak/sedef-amp/releases/1");
    o->setProperty ("html_url", "https://github.com/mr-kaynak/sedef-amp/releases/tag/" + tag);
    o->setProperty ("tag_name", tag);
    o->setProperty ("name", "Sedef Amp " + tag.substring (1));
    o->setProperty ("draft", false);
    o->setProperty ("prerelease", false);
    o->setProperty ("published_at", "2026-11-20T10:00:00Z");
    o->setProperty ("body", "## What's new\n\n- **Faster** presets\n");
    o->setProperty ("assets", assets);
    return juce::var (o);
}

juce::var newRelease (const juce::String& tag = "v0.2.0")
{
    return releaseJson (tag, { asset ("Sedef-Amp-macOS.dmg", tag, 52000000),
                               asset ("Sedef-Amp-Windows-Setup.exe", tag, 41000000),
                               asset ("Sedef-Amp-Linux-x86_64.tar.gz", tag, 38000000),
                               asset ("SHA256SUMS.txt", tag, 290) });
}

Release parsed (const juce::var& json)
{
    auto p = parseRelease (json);
    REQUIRE (p.status == ParsedRelease::Status::Ok);
    return p.release;
}

juce::File scratchFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("sedef-update-tests-" + juce::Uuid().toString());
    REQUIRE (dir.createDirectory());
    return dir;
}
} // namespace

// Versions ----------------------------------------------------------------------------------------

TEST_CASE ("Versions parse with or without a v prefix", "[update][version]")
{
    CHECK (v ("v1.2.3").numbers == std::array<int, 3> { 1, 2, 3 });
    CHECK (v ("V0.10.0").numbers == std::array<int, 3> { 0, 10, 0 });
    CHECK (v ("  1.2 ").numbers == std::array<int, 3> { 1, 2, 0 });
    CHECK (v ("7").numbers == std::array<int, 3> { 7, 0, 0 });
    CHECK (v ("1.2.3-rc.1").prerelease == juce::StringArray { "rc", "1" });
    CHECK (v ("1.2.3-alpha-1").prerelease == juce::StringArray { "alpha-1" });
    CHECK (v ("1.2.3+build.5").toString() == "1.2.3");
    CHECK (v ("v1.2.3-beta.2+exp.sha.5114f85").toString() == "1.2.3-beta.2");
    CHECK_FALSE (v ("1.2.3").isPrerelease());
}

TEST_CASE ("Malformed versions are rejected", "[update][version]")
{
    for (const auto* bad : { "", "v", "1.2.3.4", "a.b.c", "1..2", "1.-2", "1.2.3-", "1.2.3-rc..1",
                             "1.2.3-r$c", "latest", "1.2.x", "1234567890.0.0" })
    {
        INFO (bad);
        CHECK_FALSE (Version::parse (bad).has_value());
    }
}

TEST_CASE ("Build metadata is dropped, but must be well-formed", "[update][version]")
{
    CHECK (v ("1.2.3+build.5-x").toString() == "1.2.3");
    for (const auto* bad : { "1.2.3+", "1.2.3+a..b", "1.2.3+.a", "1.2.3+a/b", "1.2.3+a?b",
                             "1.2.3+a#b", "1.2.3+%2e%2e", "1.2.3+a b", "1.2.3+a+b" })
    {
        INFO (bad);
        CHECK_FALSE (Version::parse (bad).has_value());
    }
    // The tag goes into download URLs: one that could leave the release's folder is no release.
    CHECK (parseRelease (releaseJson ("v0.2.0+x/../../../other/repo", {})).status ==
           ParsedRelease::Status::Invalid);
}

TEST_CASE ("Version precedence follows semver", "[update][version]")
{
    // The ordering example from semver.org, section 11.
    const char* ordered[] = { "1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                              "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0",
                              "1.0.1",        "1.1.0",         "1.10.0",           "2.0.0" };
    for (size_t i = 0; i + 1 < std::size (ordered); ++i)
    {
        INFO (ordered[i] << " < " << ordered[i + 1]);
        CHECK (compare (v (ordered[i]), v (ordered[i + 1])) < 0);
        CHECK (compare (v (ordered[i + 1]), v (ordered[i])) > 0);
    }
    CHECK (compare (v ("v0.2.0"), v ("0.2.0")) == 0);
    CHECK (compare (v ("1.2"), v ("1.2.0")) == 0);
    CHECK (compare (v ("1.0.0+a"), v ("1.0.0+b")) == 0); // build metadata does not count
    CHECK (compare (v ("0.9.0"), v ("0.10.0")) < 0);     // numbers, not text
    CHECK (compare (v ("0.1.0-dev"), v ("0.1.0")) < 0);  // a dev build is older than its release
}

// Releases ----------------------------------------------------------------------------------------

TEST_CASE ("A GitHub release becomes the release model", "[update][release]")
{
    const auto r = parsed (newRelease());
    CHECK (r.version.toString() == "0.2.0");
    CHECK (r.tag == "v0.2.0");
    CHECK (r.name == "Sedef Amp 0.2.0");
    CHECK (r.notes.contains ("**Faster**"));
    CHECK (r.pageUrl == "https://github.com/mr-kaynak/sedef-amp/releases/tag/v0.2.0");
    CHECK (r.publishedAt == "2026-11-20T10:00:00Z");
    REQUIRE (r.assets.size() == 4);
    REQUIRE (r.findAsset ("Sedef-Amp-macOS.dmg") != nullptr);
    CHECK (r.findAsset ("Sedef-Amp-macOS.dmg")->size == 52000000);
    CHECK (r.findAsset ("Sedef-Amp-macOS.dmg")->url == kDownload + "v0.2.0/Sedef-Amp-macOS.dmg");
    CHECK (r.findAsset ("nope") == nullptr);
}

TEST_CASE ("Drafts and pre-releases are never offered", "[update][release]")
{
    auto draft = newRelease();
    draft.getDynamicObject()->setProperty ("draft", true);
    CHECK (parseRelease (draft).status == ParsedRelease::Status::Ignored);

    auto flagged = newRelease();
    flagged.getDynamicObject()->setProperty ("prerelease", true);
    CHECK (parseRelease (flagged).status == ParsedRelease::Status::Ignored);

    // A pre-release tag published without GitHub's flag is still a pre-release.
    CHECK (parseRelease (newRelease ("v0.3.0-rc.1")).status == ParsedRelease::Status::Ignored);
}

TEST_CASE ("Responses that are not releases are invalid", "[update][release]")
{
    CHECK (parseRelease (juce::var()).status == ParsedRelease::Status::Invalid);
    CHECK (parseRelease (juce::JSON::parse ("[1, 2]")).status == ParsedRelease::Status::Invalid);
    CHECK (parseRelease (juce::JSON::parse (R"({"message": "Not Found"})")).status ==
           ParsedRelease::Status::Invalid);
    CHECK (parseRelease (newRelease ("nightly")).status == ParsedRelease::Status::Invalid);
}

TEST_CASE ("Missing fields fall back, foreign links and odd assets are dropped",
           "[update][release]")
{
    auto json =
        juce::JSON::parse (R"({"tag_name": "v0.4.0", "html_url": "https://example.com/x"})");
    auto r = parsed (json);
    CHECK (r.name == "v0.4.0");
    CHECK (r.notes.isEmpty());
    CHECK (r.assets.empty());
    CHECK (r.pageUrl == releasesPageUrl()); // never link outside the repository's releases

    auto* foreign = new juce::DynamicObject();
    foreign->setProperty ("name", "Sedef-Amp-macOS.dmg");
    foreign->setProperty ("size", 10);
    foreign->setProperty ("browser_download_url", "https://example.com/Sedef-Amp-macOS.dmg");
    auto otherTag = asset ("Sedef-Amp-Windows-Setup.exe", "v0.1.0"); // another release's file
    auto traversal = asset ("x", "v0.4.0");
    traversal.getDynamicObject()->setProperty ("name", "../../Library/evil.dmg");
    auto hidden = asset (".profile", "v0.4.0");
    r = parsed (releaseJson ("v0.4.0", { juce::var (foreign), otherTag, traversal, hidden,
                                         asset ("SHA256SUMS.txt", "v0.4.0") }));
    REQUIRE (r.assets.size() == 1);
    CHECK (r.assets[0].name == "SHA256SUMS.txt");
}

TEST_CASE ("The cached copy reads back as the same release", "[update][release]")
{
    const auto a = parsed (newRelease());
    const auto b = parsed (toVar (a));
    CHECK (compare (a.version, b.version) == 0);
    CHECK (a.tag == b.tag);
    CHECK (a.name == b.name);
    CHECK (a.notes == b.notes);
    CHECK (a.pageUrl == b.pageUrl);
    CHECK (a.publishedAt == b.publishedAt);
    REQUIRE (a.assets.size() == b.assets.size());
    for (size_t i = 0; i < a.assets.size(); ++i)
    {
        CHECK (a.assets[i].name == b.assets[i].name);
        CHECK (a.assets[i].size == b.assets[i].size);
        CHECK (a.assets[i].url == b.assets[i].url);
    }
}

TEST_CASE ("Only newer, unskipped releases are offered", "[update][release]")
{
    const auto r = parsed (newRelease ("v0.2.0"));
    CHECK (isNewer (r, v ("0.1.0")));
    CHECK (isNewer (r, v ("0.2.0-rc.2")));
    CHECK_FALSE (isNewer (r, v ("0.2.0")));
    CHECK_FALSE (isNewer (r, v ("0.3.0")));

    CHECK (shouldNotify (r, v ("0.1.0"), ""));
    CHECK (shouldNotify (r, v ("0.1.0"), "0.1.5"));
    CHECK_FALSE (shouldNotify (r, v ("0.1.0"), "0.2.0"));
    CHECK_FALSE (shouldNotify (r, v ("0.1.0"), "v0.2.0"));
    CHECK (shouldNotify (parsed (newRelease ("v0.2.1")), v ("0.1.0"), "0.2.0"));
    CHECK_FALSE (shouldNotify (r, v ("0.2.0"), ""));
}

TEST_CASE ("Redirects lead to GitHub's download hosts or stay where they are", "[update][release]")
{
    const auto from = kDownload + "v0.2.0/Sedef-Amp-macOS.dmg";
    const juce::String cdn = "https://release-assets.githubusercontent.com/github-production-"
                             "release-asset/1?sp=r&sig=a%2Bb";
    CHECK (redirectTarget (from, cdn) == cdn);
    CHECK (redirectTarget (from, "https://objects.githubusercontent.com/x") ==
           "https://objects.githubusercontent.com/x");
    CHECK (redirectTarget (from, " HTTPS://GitHub.com/x ") == "HTTPS://GitHub.com/x");
    CHECK (redirectTarget (from, "//objects.githubusercontent.com/x") ==
           "https://objects.githubusercontent.com/x");

    // The same origin: a renamed repository on the API, or a path on the same (test) server.
    const juce::String api = "https://api.github.com/repos/a/b/releases/latest";
    CHECK (redirectTarget (api, "/repositories/1/releases/latest") ==
           "https://api.github.com/repositories/1/releases/latest");
    CHECK (redirectTarget (api, "https://api.github.com/repositories/1/releases/latest") ==
           "https://api.github.com/repositories/1/releases/latest");
    CHECK (redirectTarget ("http://127.0.0.1:8080/a", "/b") == "http://127.0.0.1:8080/b");

    for (const auto* bad :
         { "", "http://objects.githubusercontent.com/x", "http://github.com/x",
           "https://evil.example/x", "https://github.com.evil.example/x",
           "https://evil.example/github.com", "https://github.com@evil.example/x",
           "https://user:pw@github.com/x", "https://github.com:8443/x", "relative/path",
           "file:///etc/passwd", "javascript:alert(1)", "ftp://github.com/x",
           "https://github.com/a b", "https://github.com\\@evil.example/", "https:///x" })
    {
        INFO (bad);
        CHECK (redirectTarget (from, bad).isEmpty());
    }
    CHECK (redirectTarget ("http://127.0.0.1:8080/a", "http://127.0.0.1:9090/b").isEmpty());
    CHECK (redirectTarget ("not a url", "/b").isEmpty());
}

// Installers --------------------------------------------------------------------------------------

TEST_CASE ("Each OS gets its own installer", "[update][assets]")
{
    const auto r = parsed (newRelease());
    REQUIRE (selectInstaller (r, Platform::MacOS, false) != nullptr);
    CHECK (selectInstaller (r, Platform::MacOS, false)->name == "Sedef-Amp-macOS.dmg");
    CHECK (selectInstaller (r, Platform::MacOS, true)->name == "Sedef-Amp-macOS.dmg");
    REQUIRE (selectInstaller (r, Platform::Windows, true) != nullptr);
    CHECK (selectInstaller (r, Platform::Windows, true)->name == "Sedef-Amp-Windows-Setup.exe");
    REQUIRE (selectInstaller (r, Platform::Linux, true) != nullptr);
    CHECK (selectInstaller (r, Platform::Linux, true)->name == "Sedef-Amp-Linux-x86_64.tar.gz");

    // No ARM builds for Windows or Linux: the UI offers the releases page instead.
    CHECK (selectInstaller (r, Platform::Windows, false) == nullptr);
    CHECK (selectInstaller (r, Platform::Linux, false) == nullptr);
    CHECK (installerAssetName (Platform::Linux, false).isEmpty());
}

TEST_CASE ("A release with the old zip names has no installer", "[update][assets]")
{
    // v0.1.0 shipped zips per OS, before the installers existed.
    const auto r = parsed (releaseJson ("v0.1.0", { asset ("SedefAmp-v0.1.0-macOS.zip", "v0.1.0"),
                                                    asset ("SedefAmp-v0.1.0-Windows.zip", "v0.1.0"),
                                                    asset ("SedefAmp-v0.1.0-Linux.zip", "v0.1.0"),
                                                    asset ("SHA256SUMS.txt", "v0.1.0") }));
    CHECK (selectInstaller (r, Platform::MacOS, true) == nullptr);
    CHECK (selectInstaller (r, Platform::Windows, true) == nullptr);
    CHECK (selectInstaller (r, Platform::Linux, true) == nullptr);
    CHECK (selectInstaller (parsed (releaseJson ("v0.2.0", {})), Platform::MacOS, true) == nullptr);
}

TEST_CASE ("An installer without a size to check is no installer", "[update][assets]")
{
    const auto withDmg = [] (const juce::var& dmg) {
        return parsed (releaseJson ("v0.2.0", { dmg, asset ("SHA256SUMS.txt", "v0.2.0") }));
    };
    const auto dmg = [] (juce::int64 size) {
        return asset ("Sedef-Amp-macOS.dmg", "v0.2.0", size);
    };
    CHECK (selectInstaller (withDmg (dmg (1)), Platform::MacOS, true) != nullptr);
    CHECK (selectInstaller (withDmg (dmg (kMaxInstallerBytes)), Platform::MacOS, true) != nullptr);
    CHECK (selectInstaller (withDmg (dmg (0)), Platform::MacOS, true) == nullptr);
    CHECK (selectInstaller (withDmg (dmg (kMaxInstallerBytes + 1)), Platform::MacOS, true) ==
           nullptr);
    auto noSize = dmg (1);
    noSize.getDynamicObject()->removeProperty ("size");
    CHECK (selectInstaller (withDmg (noSize), Platform::MacOS, true) == nullptr);
}

TEST_CASE ("Platform names match the User-Agent and the UI", "[update][assets]")
{
    CHECK (platformName (Platform::MacOS) == "macOS");
    CHECK (platformName (Platform::Windows) == "Windows");
    CHECK (platformName (Platform::Linux) == "Linux");
}

// Checksums ---------------------------------------------------------------------------------------

TEST_CASE ("SHA256SUMS.txt is read in sha256sum format", "[update][checksums]")
{
    const juce::String a (juce::String::repeatedString ("a", 64));
    const juce::String b (juce::String::repeatedString ("B", 64));
    const juce::String c (juce::String::repeatedString ("0", 63) + "1");
    const auto text = a + "  Sedef-Amp-macOS.dmg\r\n" +       // text mode, CRLF
                      b + " *Sedef-Amp-Windows-Setup.exe\n" + // binary mode
                      "# comment\n\n" +                       // ignored
                      "deadbeef  short-hash.zip\n" +          // not a SHA-256
                      a + "Sedef-Amp-no-separator\n" +        // malformed
                      c + "  Sedef Amp with spaces.tar.gz\n";
    const auto sums = parseChecksums (text);
    CHECK (sums.size() == 3);
    CHECK (sums.at ("Sedef-Amp-macOS.dmg") == a);
    CHECK (sums.at ("Sedef-Amp-Windows-Setup.exe") == b.toLowerCase());
    CHECK (sums.at ("Sedef Amp with spaces.tar.gz") == c);
    CHECK (sums.count ("short-hash.zip") == 0);

    CHECK (parseChecksums ({}).empty());
    CHECK (isSha256Hex (a));
    CHECK_FALSE (isSha256Hex (a.dropLastCharacters (1)));
    CHECK_FALSE (isSha256Hex (juce::String::repeatedString ("g", 64)));
}

TEST_CASE ("A name listed twice with different hashes has no checksum", "[update][checksums]")
{
    const juce::String a (juce::String::repeatedString ("a", 64));
    const juce::String b (juce::String::repeatedString ("b", 64));
    const auto sums = parseChecksums (a + "  Sedef-Amp-macOS.dmg\n" +         //
                                      b + "  Sedef-Amp-macOS.dmg\n" +         // conflicts
                                      a + "  Sedef-Amp-Windows-Setup.exe\n" + //
                                      a.toUpperCase() + " *Sedef-Amp-Windows-Setup.exe\n" + // same
                                      " \t" + b + "  Sedef-Amp-Linux-x86_64.tar.gz \n");
    CHECK (sums.count ("Sedef-Amp-macOS.dmg") == 0);
    CHECK (sums.at ("Sedef-Amp-Windows-Setup.exe") == a);
    CHECK (sums.at ("Sedef-Amp-Linux-x86_64.tar.gz") == b); // indented lines count
}

TEST_CASE ("Files are verified against their SHA-256", "[update][checksums]")
{
    const auto dir = scratchFolder();
    const auto file = dir.getChildFile ("abc.bin");
    REQUIRE (file.replaceWithText ("abc"));
    // FIPS 180-2, appendix B.1.
    const juce::String abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK (sha256Hex (file) == abc);
    CHECK (matchesChecksum (file, abc));
    CHECK (matchesChecksum (file, abc.toUpperCase()));
    CHECK_FALSE (matchesChecksum (file, juce::String::repeatedString ("0", 64)));
    CHECK_FALSE (matchesChecksum (file, "not a checksum"));

    const auto empty = dir.getChildFile ("empty.bin");
    REQUIRE (empty.create().wasOk());
    CHECK (sha256Hex (empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // Larger than the hashing buffer, and not a multiple of the 64-byte block.
    juce::MemoryBlock big (200001);
    big.fillWith ('x');
    const auto large = dir.getChildFile ("large.bin");
    REQUIRE (large.replaceWithData (big.getData(), big.getSize()));
    CHECK (sha256Hex (large) == "d2e1d7d6c34b8e6e5573bbbc5b47217788a15b882bf71a3b213a153e70aae115");

    CHECK (sha256Hex (dir.getChildFile ("missing.bin")).isEmpty());
    CHECK_FALSE (matchesChecksum (dir.getChildFile ("missing.bin"), abc));
    dir.deleteRecursively();
}

// Throttle ----------------------------------------------------------------------------------------

TEST_CASE ("Automatic checks run at most once a day", "[update][throttle]")
{
    constexpr juce::int64 hour = 60 * 60 * 1000;
    const juce::int64 t = 1'800'000'000'000; // some time in 2027

    CHECK (isCheckDue (t, 0, 0)); // never checked

    // After a success: a day.
    CHECK_FALSE (isCheckDue (t + hour, t, t + 500));
    CHECK_FALSE (isCheckDue (t + 24 * hour - 1, t, t + 500));
    CHECK (isCheckDue (t + 24 * hour, t, t + 500));
    CHECK_FALSE (isCheckDue (t + 1, t, t)); // success in the same millisecond

    // After a failure, or while another process's check is still running: an hour.
    CHECK_FALSE (isCheckDue (t + hour / 2, t, t - 5 * hour));
    CHECK (isCheckDue (t + hour, t, t - 5 * hour));
    CHECK (isCheckDue (t + hour, t, 0));

    // A clock that went backwards does not suppress checks until it catches up.
    CHECK (isCheckDue (t - 10 * hour, t, t));
}

// Persisted state ---------------------------------------------------------------------------------

TEST_CASE ("update.json round-trips and tolerates junk", "[update][store]")
{
    Store s;
    s.autoCheck = false;
    s.skippedVersion = "0.2.0";
    s.lastAttemptMs = 1'800'000'000'000;
    s.lastSuccessMs = 1'800'000'000'500;
    s.latest = parsed (newRelease());

    const auto back = storeFromVar (juce::JSON::parse (juce::JSON::toString (toVar (s))));
    CHECK_FALSE (back.autoCheck);
    CHECK (back.skippedVersion == "0.2.0");
    CHECK (back.lastAttemptMs == s.lastAttemptMs);
    CHECK (back.lastSuccessMs == s.lastSuccessMs);
    REQUIRE (back.latest.has_value());
    CHECK (back.latest->tag == "v0.2.0");
    CHECK (back.latest->assets.size() == 4);

    const auto defaults = storeFromVar (juce::JSON::parse ("not json"));
    CHECK (defaults.autoCheck);
    CHECK (defaults.skippedVersion.isEmpty());
    CHECK (defaults.lastAttemptMs == 0);
    CHECK_FALSE (defaults.latest.has_value());

    const auto odd = storeFromVar (juce::JSON::parse (
        R"({"autoCheck": "no", "lastAttempt": -5, "lastSuccess": "x", "latest": {"tag_name": "beta"}})"));
    CHECK (odd.autoCheck); // only a real boolean turns it off
    CHECK (odd.lastAttemptMs == 0);
    CHECK (odd.lastSuccessMs == 0);
    CHECK_FALSE (odd.latest.has_value());
}

TEST_CASE ("update.json is replaced atomically", "[update][store]")
{
    const auto dir = scratchFolder();
    const auto file = dir.getChildFile ("Sedef").getChildFile ("update.json");

    CHECK (readStore (file).autoCheck); // missing file: defaults

    Store s;
    s.autoCheck = false;
    s.lastAttemptMs = 42;
    REQUIRE (writeStore (file, s)); // creates the folder
    CHECK_FALSE (readStore (file).autoCheck);
    CHECK (readStore (file).lastAttemptMs == 42);

    s.autoCheck = true;
    REQUIRE (writeStore (file, s));
    CHECK (readStore (file).autoCheck);
    // Only the file itself is left: the temporary copy was renamed over it.
    CHECK (file.getParentDirectory().getNumberOfChildFiles (juce::File::findFilesAndDirectories |
                                                            juce::File::ignoreHiddenFiles) == 1);
    CHECK (file.getParentDirectory().getNumberOfChildFiles (juce::File::findFiles) == 1);
    dir.deleteRecursively();
}
