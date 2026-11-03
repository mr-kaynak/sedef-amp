// SPDX-License-Identifier: AGPL-3.0-or-later
// The plug-in's update service (worker thread, update.json, throttle across instances, the install
// and its states) against a local HTTP server standing in for the GitHub API and a scripted
// installer standing in for the OS's.
#include "LocalServer.h"
#include "UpdateService.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <juce_cryptography/juce_cryptography.h>

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

/// The OS's installer, scripted: what it answers, and what it was asked.
struct FakeInstall
{
    std::mutex lock;
    ManualReason reason = ManualReason::None;
    InstallResult::Outcome outcome = InstallResult::Outcome::Installed;
    Error error = Error::None;
    bool block = false; // waits in the permission step until cancelled
    std::atomic<int> installs { 0 }, relaunches { 0 }, cleanUps { 0 };
    std::atomic<bool> waiting { false }, sawCancel { false };
    InstallRequest last;
};

class FakeInstaller final : public Installer
{
public:
    explicit FakeInstaller (std::shared_ptr<FakeInstall> f) : f_ (std::move (f)) {}

    ManualReason manualReason() const override
    {
        const std::scoped_lock l (f_->lock);
        return f_->reason;
    }

    InstallResult install (const InstallRequest& request, const StepCallback& step,
                           Cancellation& cancel) override
    {
        ++f_->installs;
        step (InstallStep::Preparing);
        InstallResult r;
        {
            const std::scoped_lock l (f_->lock);
            f_->last = request;
            r.outcome = f_->outcome;
            r.error = f_->error;
        }
        r.detail = r.error == Error::None ? juce::String() : "scripted";
        if (!matchesChecksum (request.file, request.expectedHash))
            return { InstallResult::Outcome::Failed, Error::ChecksumMismatch, "not verified" };
        step (InstallStep::Permission);
        bool block = false;
        {
            const std::scoped_lock l (f_->lock);
            block = f_->block;
        }
        if (block)
        {
            f_->waiting = true;
            while (!cancel.isCancelled())
                juce::Thread::sleep (2);
            f_->sawCancel = true;
            return { InstallResult::Outcome::Cancelled, Error::None, {} };
        }
        step (InstallStep::Finishing);
        return r;
    }

    bool relaunchAfterExit() override
    {
        ++f_->relaunches;
        return true;
    }

    void cleanUp() override { ++f_->cleanUps; }

private:
    std::shared_ptr<FakeInstall> f_;
};

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
    juce::File cache = dir.getChildFile ("Cache");
    juce::String releases = server.url ("/releases");
    std::shared_ptr<FakeInstall> fake = std::make_shared<FakeInstall>();
    std::shared_ptr<std::atomic<int>> opened = std::make_shared<std::atomic<int>> (0);

    Fixture() { server.serve ("/latest", 200, releaseJson (releases, true)); }
    ~Fixture() { dir.deleteRecursively(); }

    std::unique_ptr<Service> make (const juce::String& version = "0.1.0") const
    {
        Service::Options o;
        o.store = store;
        o.apiUrl = server.url ("/latest");
        o.releasesUrl = releases;
        o.downloads = downloads;
        o.cache = cache;
        o.currentVersion = version;
        o.installer = std::make_unique<FakeInstaller> (fake);
        o.open = [counter = opened] (const juce::File&) {
            ++*counter; // never a real disk image or setup program in a test
            return true;
        };
        return std::make_unique<Service> (std::move (o));
    }

    int checks() const { return server.hits ("/latest"); }

    juce::StringArray downloaded() const { return names (downloads); }
    juce::StringArray cached() const { return names (cache); }

    static juce::StringArray names (const juce::File& folder)
    {
        juce::StringArray names;
        for (const auto& file : folder.findChildFiles (juce::File::findFiles, false, "*"))
            names.add (file.getFileName());
        names.sort (false);
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
        if ((p != "checking" && p != "downloading" && p != "verifying" && p != "installing") ||
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
    CHECK (state["host"].toString() == "your DAW");
    CHECK_FALSE (static_cast<bool> (state["standalone"]));
    CHECK_FALSE (static_cast<bool> (state["canRestart"]));
    CHECK (static_cast<bool> (state["autoInstall"]["available"]));
    CHECK (state["autoInstall"]["reason"].isVoid());
    CHECK (state["installedVersion"].isVoid());
    CHECK (state["install"].isVoid());
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
    s->install();
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
    s->install();
    const auto end = juce::Time::getMillisecondCounter() + kPatienceMs;
    while (static_cast<juce::int64> (s->state()["progress"]["received"]) == 0 &&
           juce::Time::getMillisecondCounter() < end)
        juce::Thread::sleep (5);
    REQUIRE (phase (*s) == "downloading");

    const auto started = juce::Time::getMillisecondCounterHiRes();
    s.reset();
    CHECK (juce::Time::getMillisecondCounterHiRes() - started < 5000.0);
    CHECK (f.downloaded().isEmpty());
    CHECK (f.cached().isEmpty()); // the partial file is gone
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
            s->install();
        const auto started = juce::Time::getMillisecondCounterHiRes();
        s.reset();
        CHECK (juce::Time::getMillisecondCounterHiRes() - started < 5000.0);
    }
    CHECK (readStore (f.store).latest.has_value());
    CHECK (f.downloaded().isEmpty());
}

// The install ------------------------------------------------------------------------------------

namespace
{
/// Serves v0.2.0 with a real installer for this OS and its checksum; returns the installer's name.
juce::String serveInstaller (Fixture& f, int size = 5000)
{
    const auto name = installerAssetName (currentPlatform(), isX64());
    juce::MemoryBlock bytes (static_cast<size_t> (size));
    for (int i = 0; i < size; ++i)
        static_cast<char*> (bytes.getData())[i] = static_cast<char> (i * 7);
    f.server.serve ("/latest", 200, releaseJson (f.releases, true, size));
    f.server.serve ("/releases/download/v0.2.0/" + name, 200, bytes);
    const auto sha = juce::SHA256 (bytes).toHexString().toLowerCase();
    f.server.serve ("/releases/download/v0.2.0/SHA256SUMS.txt", 200, sha + "  " + name + "\n");
    return name;
}

/// A service that knows about v0.2.0.
std::unique_ptr<Service> checked (Fixture& f, const juce::String& version = "0.1.0")
{
    auto s = f.make (version);
    s->checkNow();
    REQUIRE (settle (*s)["latest"]["version"].toString() == "0.2.0");
    return s;
}

int installerHits (Fixture& f, const juce::String& name)
{
    return f.server.hits ("/releases/download/v0.2.0/" + name);
}
} // namespace

TEST_CASE ("Update now installs from the private cache and asks for a restart",
           "[update][service][install]")
{
    const auto name = installerAssetName (currentPlatform(), isX64());
    if (name.isEmpty())
        SKIP ("no installer for this processor");
    Fixture f;
    serveInstaller (f);
    auto s = checked (f);
    s->install();
    CHECK (phase (*s) != "idle");
    const auto state = settle (*s);
    CHECK (state["phase"].toString() == "installed");
    CHECK (state["installedVersion"].toString() == "0.2.0");
    CHECK (state["error"].isVoid());
    CHECK_FALSE (static_cast<bool> (state["canRestart"])); // a plug-in: restart the host

    CHECK (f.fake->installs == 1);
    {
        const std::scoped_lock l (f.fake->lock);
        CHECK (f.fake->last.file.isAChildOf (f.cache));
        CHECK (f.fake->last.version == "0.2.0");
        CHECK (isSha256Hex (f.fake->last.expectedHash));
        CHECK_FALSE (f.fake->last.relaunch);
    }
    CHECK (f.cached().isEmpty());     // installed: the installer is gone
    CHECK (f.downloaded().isEmpty()); // and it never went to Downloads
    CHECK (readStore (f.store).installedVersion == "0.2.0");

    // Nothing left to install but a restart.
    s->install();
    settle (*s);
    CHECK (f.fake->installs == 1);
    CHECK (installerHits (f, name) == 1);
}

TEST_CASE ("Cancelling the password dialog returns to ready, and the next try reuses the download",
           "[update][service][install]")
{
    const auto name = installerAssetName (currentPlatform(), isX64());
    if (name.isEmpty())
        SKIP ("no installer for this processor");
    Fixture f;
    serveInstaller (f);
    f.fake->outcome = InstallResult::Outcome::Declined;
    auto s = checked (f);
    s->install();
    auto state = settle (*s);
    CHECK (state["phase"].toString() == "idle");
    CHECK (state["error"].isVoid()); // not an error
    CHECK (static_cast<bool> (state["updateAvailable"]));
    CHECK (state["installedVersion"].isVoid());
    CHECK (f.cached() == juce::StringArray { name });
    CHECK (readStore (f.store).installedVersion.isEmpty());

    f.fake->outcome = InstallResult::Outcome::Installed;
    s->install();
    state = settle (*s);
    CHECK (state["phase"].toString() == "installed");
    CHECK (f.fake->installs == 2);
    CHECK (installerHits (f, name) == 1); // verified again, not downloaded again
}

TEST_CASE ("A failed install offers Install manually, which uses the verified download",
           "[update][service][install]")
{
    const auto name = installerAssetName (currentPlatform(), isX64());
    if (name.isEmpty())
        SKIP ("no installer for this processor");
    Fixture f;
    serveInstaller (f);
    f.fake->outcome = InstallResult::Outcome::Failed;
    f.fake->error = Error::InstallFailed;
    auto s = checked (f);
    s->install();
    auto state = settle (*s);
    CHECK (state["phase"].toString() == "error");
    CHECK (state["error"]["code"].toString() == "installFailed");
    CHECK (readStore (f.store).installedVersion.isEmpty());

    s->installManually();
    state = settle (*s);
    CHECK (state["phase"].toString() == "done");
    CHECK (state["error"].isVoid());
    CHECK (state["download"]["fileName"].toString() == name);
    CHECK (f.downloaded() == juce::StringArray { name });
    CHECK (sha256Hex (f.downloads.getChildFile (name)) ==
           sha256Hex (f.cache.getChildFile (name))); // the same verified bytes
    CHECK (installerHits (f, name) == 1);
    CHECK (f.fake->installs == 1);
}

TEST_CASE ("Where the automatic route can't run, Update now installs manually",
           "[update][service][install]")
{
    const auto name = installerAssetName (currentPlatform(), isX64());
    if (name.isEmpty())
        SKIP ("no installer for this processor");
    Fixture f;
    serveInstaller (f);
    f.fake->reason = ManualReason::Elsewhere;
    auto s = checked (f);
    auto state = s->state();
    CHECK_FALSE (static_cast<bool> (state["autoInstall"]["available"]));
    CHECK (state["autoInstall"]["reason"].toString() == "elsewhere");
    s->install();
    state = settle (*s);
    CHECK (state["phase"].toString() == "done");
    CHECK (f.downloaded() == juce::StringArray { name });
    CHECK (f.fake->installs == 0);
}

TEST_CASE ("Closing during the install does not wait, and stops what it waits on",
           "[update][service][install]")
{
    const auto name = installerAssetName (currentPlatform(), isX64());
    if (name.isEmpty())
        SKIP ("no installer for this processor");
    Fixture f;
    serveInstaller (f);
    f.fake->block = true;
    auto s = checked (f);
    s->install();
    const auto end = juce::Time::getMillisecondCounter() + kPatienceMs;
    while (!f.fake->waiting && juce::Time::getMillisecondCounter() < end)
        juce::Thread::sleep (5);
    REQUIRE (f.fake->waiting);
    const auto state = s->state();
    CHECK (state["phase"].toString() == "installing");
    CHECK (state["install"]["step"].toString() == "permission");
    s->cancel(); // no Cancel once the OS's installer runs: its prompt is the place
    CHECK (phase (*s) == "installing");

    const auto started = juce::Time::getMillisecondCounterHiRes();
    s.reset();
    CHECK (juce::Time::getMillisecondCounterHiRes() - started < 5000.0);
    CHECK (f.fake->sawCancel);
    CHECK (readStore (f.store).installedVersion.isEmpty());
}

TEST_CASE ("An installed update is remembered across processes until one runs it",
           "[update][service][install]")
{
    const auto name = installerAssetName (currentPlatform(), isX64());
    if (name.isEmpty())
        SKIP ("no installer for this processor");
    Fixture f;
    serveInstaller (f);
    {
        auto first = checked (f);
        first->install();
        REQUIRE (settle (*first)["phase"].toString() == "installed");
    }
    // Another DAW, still on 0.1.0: restart, don't download again.
    auto other = f.make ("0.1.0");
    other->pageOpened();
    auto state = settle (*other);
    CHECK (state["phase"].toString() == "installed");
    CHECK (state["installedVersion"].toString() == "0.2.0");
    other->install();
    settle (*other);
    CHECK (f.fake->installs == 1);

    // A process that runs 0.2.0 clears the record...
    auto updated = f.make ("0.2.0");
    updated->pageOpened();
    state = settle (*updated);
    const auto end = juce::Time::getMillisecondCounter() + kPatienceMs;
    while (readStore (f.store).installedVersion.isNotEmpty() &&
           juce::Time::getMillisecondCounter() < end)
        juce::Thread::sleep (5);
    CHECK (readStore (f.store).installedVersion.isEmpty());
    CHECK (state["phase"].toString() == "idle");
    CHECK_FALSE (static_cast<bool> (updated->state()["updateAvailable"]));
    CHECK (f.fake->cleanUps >= 1); // and detaches what an earlier process may have left

    // ...so an old copy that opens later offers the update again.
    auto old = f.make ("0.1.0");
    old->pageOpened();
    state = settle (*old);
    CHECK (state["phase"].toString() == "idle");
    CHECK (static_cast<bool> (state["updateAvailable"]));
}

TEST_CASE ("Setup that waits for the host is scheduled, not installed",
           "[update][service][install]")
{
    const auto name = installerAssetName (currentPlatform(), isX64());
    if (name.isEmpty())
        SKIP ("no installer for this processor");
    Fixture f;
    serveInstaller (f);
    f.fake->outcome = InstallResult::Outcome::Scheduled;
    auto s = checked (f);
    s->setHost ("Test Host", false);
    s->install();
    auto state = settle (*s);
    CHECK (state["phase"].toString() == "scheduled");
    CHECK (state["installedVersion"].toString() == "0.2.0");
    CHECK (state["host"].toString() == "Test Host");
    CHECK_FALSE (static_cast<bool> (state["canRestart"]));
    CHECK (f.cached() == juce::StringArray { name });       // Setup still needs it
    CHECK (readStore (f.store).installedVersion.isEmpty()); // nothing is installed yet

    s->install(); // already waiting
    settle (*s);
    CHECK (f.fake->installs == 1);
    s->checkNow(); // a check keeps it scheduled
    CHECK (settle (*s)["phase"].toString() == "scheduled");
    CHECK_FALSE (s->prepareRestart()); // a plug-in can't restart its host
}

TEST_CASE ("The standalone app restarts itself into the new version", "[update][service][install]")
{
    const auto name = installerAssetName (currentPlatform(), isX64());
    if (name.isEmpty())
        SKIP ("no installer for this processor");
    Fixture f;
    serveInstaller (f);
    auto s = checked (f);
    s->setHost ("Sedef", true);
    CHECK_FALSE (s->prepareRestart()); // nothing installed yet
    s->install();
    const auto state = settle (*s);
    CHECK (state["phase"].toString() == "installed");
    CHECK (state["host"].toString() == "Sedef");
    CHECK (static_cast<bool> (state["standalone"]));
    CHECK (static_cast<bool> (state["canRestart"]));
    {
        const std::scoped_lock l (f.fake->lock);
        // On Windows Setup reopens the app (/RELAUNCH); elsewhere Sedef's own helper does.
        CHECK (f.fake->last.relaunch == (currentPlatform() == Platform::Windows));
    }
    CHECK (s->prepareRestart());
    CHECK (f.fake->relaunches == 1);
}
