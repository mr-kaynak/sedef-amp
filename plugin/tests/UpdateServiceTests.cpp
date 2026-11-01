// SPDX-License-Identifier: AGPL-3.0-or-later
// The plug-in's update service (worker thread, update.json, throttle across instances) against a
// local HTTP server standing in for the GitHub API.
#include "LocalServer.h"
#include "UpdateService.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

using namespace sedef::update;
using sedef::update::test::LocalServer;

namespace
{
/// Sets or clears an environment variable for one test and restores it afterwards.
class ScopedEnv
{
public:
    ScopedEnv (const char* name, const char* value)
        : name_ (name),
          old_ (juce::SystemStats::getEnvironmentVariable (name, {}))
    {
        set (value);
    }
    ~ScopedEnv() { set (old_.isEmpty() ? nullptr : old_.toRawUTF8()); }

private:
    void set (const char* value)
    {
#if JUCE_WINDOWS
        _putenv_s (name_, value != nullptr ? value : "");
#else
        if (value != nullptr)
            setenv (name_, value, 1);
        else
            unsetenv (name_);
#endif
    }

    const char* name_;
    juce::String old_;
};

/// v0.2.0 as the API returns it, with its assets under `releases` (the local server's).
juce::String releaseJson (const juce::String& releases, bool withInstallers,
                          juce::int64 installerSize = 1000)
{
    const auto base = releases + "/download/v0.2.0/";
    juce::String assets;
    for (const auto* name : { "Sedef-Amp-macOS.dmg", "Sedef-Amp-Windows-Setup.exe",
                              "Sedef-Amp-Linux-x86_64.tar.gz", "SHA256SUMS.txt" })
        if (withInstallers || juce::String (name) == "SHA256SUMS.txt")
            assets << (assets.isEmpty() ? "" : ", ") << R"({"name": ")" << name << R"(", "size": )"
                   << installerSize << R"(, "browser_download_url": ")" << base << name << "\"}";
    return R"({"tag_name": "v0.2.0", "name": "Sedef Amp 0.2.0", "body": "## New",)"
           R"( "html_url": ")" +
           releases + R"(/tag/v0.2.0", "assets": [)" + assets + "]}";
}

struct Fixture
{
    // The suite may run where checks are switched off (CI): these tests decide for themselves.
    ScopedEnv noDisable { "SEDEF_DISABLE_UPDATE_CHECK", nullptr };
    ScopedEnv noSmoke { "SEDEF_UI_SMOKE_TEST", nullptr };
    LocalServer server;
    juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("sedef-update-service-" + juce::Uuid().toString());
    juce::File store = dir.getChildFile ("Sedef").getChildFile ("update.json");
    juce::File downloads = dir.getChildFile ("Downloads");
    juce::String releases = server.url ("/releases");

    Fixture() { server.serve ("/latest", 200, releaseJson (releases, true)); }
    ~Fixture() { dir.deleteRecursively(); }

    std::unique_ptr<Service> make() const
    {
        return std::make_unique<Service> (store, server.url ("/latest"), releases, downloads);
    }

    int checks() const { return server.hits ("/latest"); }

    juce::StringArray downloaded() const
    {
        juce::StringArray names;
        for (const auto& file : downloads.findChildFiles (juce::File::findFiles, false, "*"))
            names.add (file.getFileName());
        return names;
    }
};

juce::String phase (Service& s)
{
    return s.state()["phase"].toString();
}

// A machine busy with other tests (ctest -j) can delay the worker by seconds.
constexpr juce::uint32 kPatienceMs = 20000;

/// Polls until the service is not busy (or kPatienceMs pass) and returns its state.
juce::var settle (Service& s)
{
    const auto end = juce::Time::getMillisecondCounter() + kPatienceMs;
    for (;;)
    {
        auto state = s.state();
        const auto p = state["phase"].toString();
        if ((p != "checking" && p != "downloading" && p != "verifying") ||
            juce::Time::getMillisecondCounter() > end)
            return state;
        juce::Thread::sleep (5);
    }
}
} // namespace

TEST_CASE ("The service reports its state in the bridge's shape", "[update][service]")
{
    Fixture f;
    auto s = f.make();
    const auto state = s->state();
    CHECK (static_cast<bool> (state["supported"]));
    CHECK (state["currentVersion"].toString() == "0.1.0");
    CHECK (state["platform"].toString() == platformName (currentPlatform()));
    CHECK (static_cast<bool> (state["autoCheck"]));
    CHECK (state["phase"].toString() == "idle");
    CHECK (state["latest"].isVoid());
    CHECK (state["lastChecked"].isVoid());
    CHECK (state["releasesUrl"].toString() == f.releases);
    CHECK (f.checks() == 0); // constructing it touches neither the network nor the disk
    CHECK_FALSE (f.store.exists());
}

TEST_CASE ("One check a day, shared by every instance", "[update][service]")
{
    Fixture f;
    auto first = f.make();
    first->pageOpened();
    const auto state = settle (*first);
    REQUIRE (state["phase"].toString() == "idle");
    CHECK (f.checks() == 1);
    CHECK (state["latest"]["version"].toString() == "0.2.0");
    CHECK (static_cast<bool> (state["updateAvailable"]));
    CHECK (static_cast<juce::int64> (state["lastChecked"]) > 0);
    if (const auto name = installerAssetName (currentPlatform(), isX64()); name.isNotEmpty())
        CHECK (state["latest"]["installer"]["name"].toString() == name);

    const auto stored = readStore (f.store);
    CHECK (stored.lastAttemptMs > 0);
    CHECK (stored.lastSuccessMs >= stored.lastAttemptMs);
    REQUIRE (stored.latest.has_value());
    CHECK (stored.latest->tag == "v0.2.0");

    // The same window again, and another instance (or process) with the same update.json.
    first->pageOpened();
    auto second = f.make();
    second->pageOpened();
    CHECK (settle (*second)["latest"]["version"].toString() == "0.2.0");
    settle (*first);
    CHECK (f.checks() == 1);

    // "Check now" ignores the daily limit.
    second->checkNow();
    CHECK (phase (*second) != "idle");
    settle (*second);
    CHECK (f.checks() == 2);
}

TEST_CASE ("The switch and the environment turn automatic checks off", "[update][service]")
{
    Fixture f;
    {
        auto s = f.make();
        s->setAutoCheck (false);
        CHECK_FALSE (readStore (f.store).autoCheck);
    }
    auto s = f.make();
    s->pageOpened();
    CHECK_FALSE (static_cast<bool> (settle (*s)["autoCheck"]));
    CHECK (f.checks() == 0);

    s->setAutoCheck (true); // on again, and due: checks now
    settle (*s);
    CHECK (f.checks() == 1);
    CHECK (readStore (f.store).autoCheck);

    f.store.deleteFile();
    {
        const ScopedEnv off ("SEDEF_DISABLE_UPDATE_CHECK", "1");
        auto quiet = f.make();
        quiet->pageOpened();
        CHECK_FALSE (static_cast<bool> (settle (*quiet)["autoCheckAllowed"]));
        CHECK (f.checks() == 1);
        quiet->checkNow(); // asking still works
        settle (*quiet);
        CHECK (f.checks() == 2);
    }
    f.store.deleteFile();
    {
        const ScopedEnv smoke ("SEDEF_UI_SMOKE_TEST", "/tmp/result.json");
        auto quiet = f.make();
        quiet->pageOpened();
        settle (*quiet);
        CHECK (f.checks() == 2);
    }
}

TEST_CASE ("A skipped version is remembered", "[update][service]")
{
    Fixture f;
    auto s = f.make();
    s->skip ("v0.2.0");
    CHECK (readStore (f.store).skippedVersion == "0.2.0");
    auto other = f.make();
    other->pageOpened();
    CHECK (settle (*other)["skippedVersion"].toString() == "0.2.0");
    s->skip ("not a version");
    CHECK (readStore (f.store).skippedVersion.isEmpty());
}

TEST_CASE ("A failed check is reported and retried after an hour", "[update][service]")
{
    Fixture f;
    f.server.serve ("/latest", 503, juce::String());
    auto s = f.make();
    s->pageOpened();
    auto state = settle (*s);
    CHECK (state["phase"].toString() == "error");
    CHECK (state["error"]["code"].toString() == "network");
    CHECK (state["lastChecked"].isVoid());
    CHECK (f.checks() == 1);

    s->pageOpened(); // within the hour: no retry
    settle (*s);
    CHECK (f.checks() == 1);

    auto stored = readStore (f.store);
    stored.lastAttemptMs -= 2 * 60 * 60 * 1000;
    REQUIRE (writeStore (f.store, stored));
    f.server.serve ("/latest", 403, R"({"message": "API rate limit exceeded"})");
    s->pageOpened();
    state = settle (*s);
    CHECK (f.checks() == 2);
    CHECK (state["error"]["code"].toString() == "rateLimited");
}

TEST_CASE ("Without an installer for this OS nothing is downloaded", "[update][service]")
{
    Fixture f;
    f.server.serve ("/latest", 200, releaseJson (f.releases, false));
    auto s = f.make();
    s->checkNow();
    REQUIRE (settle (*s)["latest"]["installer"].isVoid());
    const auto requests = f.server.requests().size();
    s->download();
    const auto state = settle (*s);
    CHECK (state["phase"].toString() == "error");
    CHECK (state["error"]["code"].toString() == "noInstaller");
    CHECK (f.server.requests().size() == requests);
}

TEST_CASE ("Closing during a slow request does not wait for it", "[update][service]")
{
    Fixture f;
    f.server.delay (15000); // longer than the client's own 10 s timeout
    auto s = f.make();
    s->pageOpened();
    const auto end = juce::Time::getMillisecondCounter() + kPatienceMs;
    while (f.checks() == 0 && juce::Time::getMillisecondCounter() < end)
        juce::Thread::sleep (5);
    REQUIRE (f.checks() == 1);
    CHECK (phase (*s) == "checking");

    const auto started = juce::Time::getMillisecondCounterHiRes();
    s.reset();
    CHECK (juce::Time::getMillisecondCounterHiRes() - started < 5000.0);
    CHECK (readStore (f.store).lastSuccessMs == 0); // cancelled, not a success
}

TEST_CASE ("Settings apply at once and outlive the window", "[update][service]")
{
    Fixture f;
    {
        auto s = f.make();
        s->setAutoCheck (false);
        s->skip ("v0.2.0");
        const auto state = s->state();
        CHECK_FALSE (static_cast<bool> (state["autoCheck"]));
        CHECK (state["skippedVersion"].toString() == "0.2.0");
    } // closed at once
    const auto stored = readStore (f.store);
    CHECK_FALSE (stored.autoCheck);
    CHECK (stored.skippedVersion == "0.2.0");
    CHECK (f.checks() == 0);
}

TEST_CASE ("Closing during a download does not wait for it", "[update][service]")
{
    const auto installer = installerAssetName (currentPlatform(), isX64());
    if (installer.isEmpty())
        SKIP ("no installer for this processor");
    Fixture f;
    constexpr int kSize = 300000;
    f.server.serve ("/latest", 200, releaseJson (f.releases, true, kSize));
    f.server.serve ("/releases/download/v0.2.0/" + installer, 200,
                    juce::MemoryBlock (static_cast<size_t> (kSize), true));
    f.server.serve ("/releases/download/v0.2.0/SHA256SUMS.txt", 200,
                    juce::String::repeatedString ("0", 64) + "  " + installer + "\n");
    f.server.pauseAfter (100000, 15000); // the installer stops a third of the way in

    auto s = f.make();
    s->checkNow();
    REQUIRE (settle (*s)["latest"]["installer"]["name"].toString() == installer);
    s->download();
    const auto end = juce::Time::getMillisecondCounter() + kPatienceMs;
    while (static_cast<juce::int64> (s->state()["progress"]["received"]) == 0 &&
           juce::Time::getMillisecondCounter() < end)
        juce::Thread::sleep (5);
    REQUIRE (phase (*s) == "downloading");

    const auto started = juce::Time::getMillisecondCounterHiRes();
    s.reset();
    CHECK (juce::Time::getMillisecondCounterHiRes() - started < 5000.0);
    CHECK (f.downloaded().isEmpty()); // the partial file is gone
}

TEST_CASE ("Closing right after asking does not wait either", "[update][service]")
{
    // The worker may or may not have taken the job when the service closes: either way it must
    // not start a request the destructor cannot cancel.
    Fixture f;
    auto first = f.make();
    first->checkNow();
    REQUIRE (settle (*first)["phase"].toString() == "idle");
    first.reset();
    f.server.delay (15000); // longer than the client's own 10 s timeout

    for (int i = 0; i < 10; ++i)
    {
        auto s = f.make();
        s->pageOpened(); // the release from the first check, and no check due
        if (i % 2 == 0)
            s->checkNow();
        else
            s->download();
        const auto started = juce::Time::getMillisecondCounterHiRes();
        s.reset();
        CHECK (juce::Time::getMillisecondCounterHiRes() - started < 5000.0);
    }
    CHECK (readStore (f.store).latest.has_value());
    CHECK (f.downloaded().isEmpty());
}
