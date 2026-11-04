// SPDX-License-Identifier: AGPL-3.0-or-later
// The capture library: loading, session state, preset references, the recent list and missing
// files. Every capture here is written by the test; no third-party file is involved.

#include "CaptureLibrary.h"
#include "sedef/dsp/capture/CaptureStage.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <thread>

#if !JUCE_WINDOWS
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using sedef::plugin::CaptureLibrary;

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        o->setProperty (k, v);
    return juce::var (o);
}

/// A Linear capture: an impulse response.
std::string linear (const std::vector<float>& taps, const juce::var& metadata = {})
{
    juce::Array<juce::var> w;
    for (auto t : taps)
        w.add (t);
    auto root = object (
        { { "version", "0.5.4" },
          { "architecture", "Linear" },
          { "config",
            object ({ { "receptive_field", static_cast<int> (taps.size()) }, { "bias", false } }) },
          { "weights", w },
          { "sample_rate", 48000.0 } });
    if (metadata.isObject())
        root.getDynamicObject()->setProperty ("metadata", metadata);
    return juce::JSON::toString (root, true).toStdString();
}

/// The trainer's standard WaveNet (13,802 weights), with random weights: a typical file size.
std::string standardWaveNet()
{
    juce::Array<juce::var> dilations;
    for (int d = 1; d <= 512; d *= 2)
        dilations.add (d);
    const auto array = [&dilations] (int in, int head, int channels, bool headBias) {
        return object ({ { "input_size", in },
                         { "condition_size", 1 },
                         { "head_size", head },
                         { "channels", channels },
                         { "kernel_size", 3 },
                         { "dilations", dilations },
                         { "activation", "Tanh" },
                         { "gated", false },
                         { "head_bias", headBias } });
    };
    juce::Random r (7);
    juce::Array<juce::var> w;
    for (int i = 0; i < 13802 - 1; ++i)
        w.add (0.25f * (2.0f * r.nextFloat() - 1.0f));
    w.add (0.5f);
    const auto root = object (
        { { "version", "0.5.4" },
          { "architecture", "WaveNet" },
          { "config", object ({ { "layers", juce::Array<juce::var> { array (1, 8, 16, false),
                                                                     array (16, 1, 8, true) } },
                                { "head_scale", 0.02 } }) },
          { "weights", w },
          { "sample_rate", 48000.0 },
          { "metadata", object ({ { "name", "Test Rig" }, { "gear_type", "amp_cab" } }) } });
    return juce::JSON::toString (root, true).toStdString();
}

/// A fresh folder, an engine and a library around them.
struct Rig
{
    Rig() : folder (juce::File::createTempFile ("sedef-capture-tests"))
    {
        folder.createDirectory();
        engine.prepare (48000.0, 256, 1, 2);
        library = std::make_unique<CaptureLibrary> (engine, lock, folder);
    }
    ~Rig()
    {
        library.reset();
        folder.deleteRecursively();
    }

    juce::File write (const juce::String& name, const std::string& bytes) const
    {
        auto f = folder.getChildFile ("source").getChildFile (name);
        f.getParentDirectory().createDirectory();
        f.replaceWithData (bytes.data(), bytes.size());
        return f;
    }

    void waitIdle() const
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while (library->jobsInFlight() > 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    /// Engine output for a short tone with the amp source set to `source`.
    std::vector<float> play (float source)
    {
        auto params = sedef::ParamSnapshot::defaults();
        params.values[sedef::index (sedef::ParamId::ampSource)] = source;
        std::vector<float> in (256), l (256), r (256), all;
        for (int block = 0; block < 40; ++block)
        {
            for (int i = 0; i < 256; ++i)
                in[static_cast<std::size_t> (i)] =
                    0.2f * std::sin (0.05f * static_cast<float> (block * 256 + i));
            const float* ins[] = { in.data() };
            float* outs[] = { l.data(), r.data() };
            engine.process (ins, outs, 256, params, {});
            all.insert (all.end(), l.begin(), l.end());
        }
        return all;
    }

    juce::File folder;
    sedef::Engine engine;
    std::mutex lock;
    std::unique_ptr<CaptureLibrary> library;
};

juce::String xmlOf (const juce::ValueTree& t)
{
    return t.isValid() ? t.toXmlString() : juce::String();
}
} // namespace

TEST_CASE ("A loaded capture plays and reports what it is")
{
    Rig rig;
    const auto file =
        rig.write ("Bright.nam", linear ({ 0.5f, 0.25f }, object ({ { "name", "Bright Amp" },
                                                                    { "modeled_by", "Sedef tests" },
                                                                    { "gear_make", "Test" },
                                                                    { "gear_model", "One" },
                                                                    { "gear_type", "amp" } })));
    REQUIRE (rig.library->loadFile (file).isEmpty());
    const auto state = rig.library->stateAsVar();
    CHECK (state["loaded"]["name"] == "Bright Amp");
    CHECK (state["loaded"]["author"] == "Sedef tests");
    CHECK (state["loaded"]["gear"] == "Test One");
    CHECK (state["loaded"]["architecture"] == "Linear");
    CHECK (state["loaded"]["kind"] == "amp");
    CHECK (state["error"].isVoid());
    CHECK (rig.library->suggestedCab() == std::optional<bool> (true));

    // The capture replaces the amp once the source says so.
    const auto model = rig.play (0.0f);
    const auto capture = rig.play (1.0f);
    CHECK (model != capture);
}

TEST_CASE ("Damaged files are refused with a message and change nothing")
{
    Rig rig;
    const auto good = rig.write ("Good.nam", linear ({ 1.0f }));
    REQUIRE (rig.library->loadFile (good).isEmpty());
    const auto hash = rig.library->currentHash();
    for (const std::string& bad :
         { std::string ("not json"), std::string ("{\"version\":\"0.5.4\"}"),
           std::string ("{\"weights\":[1,2,3]}"), std::string() })
    {
        const auto error = rig.library->loadFile (rig.write ("Bad.nam", bad));
        CAPTURE (bad, error);
        CHECK (error.isNotEmpty());
        CHECK (error.endsWithChar ('.'));
        CHECK (rig.library->currentHash() == hash);
        CHECK (rig.library->stateAsVar()["error"] == error);
    }
    CHECK (rig.library->loadFile (rig.folder.getChildFile ("nowhere.nam")).isNotEmpty());
    // Only the good file is in the recent list.
    const auto state = rig.library->stateAsVar();
    const auto* recent = state["recent"].getArray();
    REQUIRE (recent != nullptr);
    CHECK (recent->size() == 1);
}

TEST_CASE ("Session state round-trips byte for byte and carries the file itself")
{
    Rig rig;
    const auto bytes = standardWaveNet();
    const auto file = rig.write ("Standard.nam", bytes);
    REQUIRE (rig.library->loadFile (file).isEmpty());
    const auto saved = rig.library->toState();
    REQUIRE (saved.isValid());
    const auto xml = xmlOf (saved);
    // A typical capture costs this much in the session (gzip, base64).
    const auto data = saved.getProperty ("data").toString();
    WARN ("standard WaveNet: " << bytes.size() << " bytes as a file, " << data.length()
                               << " bytes in the session");
    CHECK (static_cast<std::size_t> (data.length()) < bytes.size());

    // On another machine: the path doesn't exist, the session still plays the same capture.
    file.deleteFile();
    Rig other;
    const auto started = std::chrono::steady_clock::now();
    other.library->restoreState (juce::ValueTree::fromXml (xml));
    const auto ms =
        std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - started)
            .count();
    WARN ("restore with the known level: " << ms << " ms");
    CHECK (other.library->currentHash() == CaptureLibrary::hashOf (bytes));
    CHECK (xmlOf (other.library->toState()) == xml);
    CHECK (other.library->stateAsVar()["missing"].isVoid());
    CHECK (other.play (1.0f) == rig.play (1.0f)); // bit for bit

    // A state without a capture clears it.
    other.library->restoreState ({});
    CHECK (other.library->currentHash().isEmpty());
    CHECK (!other.library->toState().isValid());
}

TEST_CASE ("A session whose capture can't be found says so and keeps the reference")
{
    Rig rig;
    const auto bytes = linear ({ 0.7f });
    juce::ValueTree tree ("Capture");
    tree.setProperty ("name", "Gone", nullptr);
    tree.setProperty ("fileName", "Gone.nam", nullptr);
    tree.setProperty ("path", rig.folder.getChildFile ("Gone.nam").getFullPathName(), nullptr);
    tree.setProperty ("sha256", CaptureLibrary::hashOf (bytes), nullptr);
    tree.setProperty ("data", "corrupted", nullptr);
    rig.library->restoreState (tree);
    rig.waitIdle(); // the path is looked at on the worker
    const auto state = rig.library->stateAsVar();
    CHECK (state["loaded"].isVoid());
    CHECK (state["missing"]["name"] == "Gone");
    // Saving again keeps what the session asked for.
    CHECK (xmlOf (rig.library->toState()) == xmlOf (tree));

    // Once the file is back where it was, the reference finds it.
    rig.folder.getChildFile ("Gone.nam").replaceWithData (bytes.data(), bytes.size());
    rig.library->restoreState (tree);
    rig.waitIdle(); // files are read on the worker
    CHECK (rig.library->currentHash() == CaptureLibrary::hashOf (bytes));
    CHECK (rig.library->stateAsVar()["missing"].isVoid());
}

TEST_CASE ("Dropped files are kept under Captures without duplicates")
{
    Rig rig;
    const auto a = linear ({ 0.5f });
    const auto b = linear ({ 0.25f });
    REQUIRE (rig.library->loadDropped ("../../Drop.nam", a).isEmpty());
    REQUIRE (rig.library->loadDropped ("Drop.nam", a).isEmpty());
    REQUIRE (rig.library->loadDropped ("Drop.nam", b).isEmpty());
    const auto kept =
        rig.library->droppedFolder().findChildFiles (juce::File::findFiles, false, "*.nam");
    CHECK (kept.size() == 2);
    CHECK (rig.library->droppedFolder().getChildFile ("Drop.nam").existsAsFile());
    CHECK (rig.library->droppedFolder().getChildFile ("Drop (2).nam").existsAsFile());
    CHECK (rig.library->reference()["path"] ==
           rig.library->droppedFolder().getChildFile ("Drop (2).nam").getFullPathName());
    // Not a capture: nothing is kept.
    CHECK (rig.library->loadDropped ("Text.nam", "hello").isNotEmpty());
    CHECK (!rig.library->droppedFolder().getChildFile ("Text.nam").exists());
}

TEST_CASE ("The recent list keeps the last twelve captures, newest first")
{
    Rig rig;
    for (int i = 0; i < 14; ++i)
        REQUIRE (rig.library
                     ->loadFile (rig.write ("C" + juce::String (i) + ".nam",
                                            linear ({ 0.1f + 0.01f * static_cast<float> (i) })))
                     .isEmpty());
    // Loading one again moves it to the top instead of adding it twice.
    REQUIRE (rig.library->loadFile (rig.folder.getChildFile ("source/C5.nam")).isEmpty());
    const auto state = rig.library->stateAsVar();
    const auto* recent = state["recent"].getArray();
    REQUIRE (recent != nullptr);
    CHECK (recent->size() == CaptureLibrary::kMaxRecent);
    CHECK ((*recent)[0]["fileName"] == "C5.nam");
    CHECK (static_cast<bool> ((*recent)[0]["current"]));
    CHECK ((*recent)[1]["fileName"] == "C13.nam");
    // Another instance reads the same list.
    CaptureLibrary second (rig.engine, rig.lock, rig.folder);
    const auto secondState = second.stateAsVar();
    CHECK (secondState["recent"].getArray()->size() == CaptureLibrary::kMaxRecent);
}

TEST_CASE ("Preset references select captures by hash, path or recent list")
{
    Rig rig;
    const auto a = rig.write ("A.nam", linear ({ 0.5f }));
    const auto b = rig.write ("B.nam", linear ({ 0.3f }));
    REQUIRE (rig.library->loadFile (a).isEmpty());
    const auto refA = rig.library->reference();
    REQUIRE (rig.library->loadFile (b).isEmpty());
    const auto refB = rig.library->reference();
    CHECK (rig.library->differs (refA));
    CHECK (!rig.library->differs (refB));
    CHECK (!rig.library->differs ({})); // a preset without a capture doesn't care

    // Built this session: at once.
    rig.library->select (refA, false);
    CHECK (rig.library->currentHash() == refA["sha256"].toString());
    // A preset without a capture leaves it alone; a snapshot without one clears it.
    rig.library->select ({}, false);
    CHECK (rig.library->currentHash() == refA["sha256"].toString());
    rig.library->select ({}, true);
    CHECK (rig.library->currentHash().isEmpty());

    // In a new session: from the path, on the worker.
    Rig other;
    other.library->select (refB, false);
    other.waitIdle();
    CHECK (other.library->currentHash() == refB["sha256"].toString());

    // Moved: found through the recent list by hash.
    const auto moved = rig.folder.getChildFile ("moved/A.nam");
    moved.getParentDirectory().createDirectory();
    REQUIRE (a.moveFileTo (moved));
    REQUIRE (other.library->loadFile (moved).isEmpty()); // now in the recent list
    other.library->clear();
    auto stale = refA.clone();
    stale.getDynamicObject()->setProperty ("path",
                                           rig.folder.getChildFile ("A.nam").getFullPathName());
    other.library->select (stale, false);
    other.waitIdle();
    CHECK (other.library->currentHash() == refA["sha256"].toString());

    // Nowhere: reported missing, and a missing capture doesn't count as an edit.
    auto lost = object ({ { "name", "Lost" },
                          { "fileName", "Lost.nam" },
                          { "path", rig.folder.getChildFile ("Lost.nam").getFullPathName() },
                          { "sha256", juce::String::repeatedString ("0", 64) } });
    other.library->select (lost, false);
    other.waitIdle();
    CHECK (other.library->currentHash().isEmpty());
    CHECK (other.library->stateAsVar()["missing"]["name"] == "Lost");
    CHECK (!other.library->differs (lost));
    // The session and a preset saved now keep the reference while it is missing.
    CHECK (other.library->toState().getProperty ("sha256") == lost["sha256"]);
    CHECK (other.library->reference()["sha256"] == lost["sha256"]);
}

TEST_CASE ("A newer request wins over a load still running")
{
    Rig rig;
    const auto slow = rig.write ("Slow.nam", standardWaveNet());
    const auto quick = rig.write ("Quick.nam", linear ({ 0.4f }));
    rig.library->loadFileAsync (slow, {});
    CHECK (rig.library->isLoading());
    REQUIRE (rig.library->loadFile (quick).isEmpty());
    rig.waitIdle(); // the slow one finishes building, and is dropped
    CHECK (rig.library->stateAsVar()["loaded"]["fileName"] == "Quick.nam");
    CHECK (!rig.library->isLoading());
}

TEST_CASE ("Rebuilding after a rate change keeps the capture playing")
{
    Rig rig;
    REQUIRE (rig.library->loadFile (rig.write ("R.nam", linear ({ 0.5f, 0.2f }))).isEmpty());
    {
        const std::scoped_lock lock (rig.lock);
        rig.engine.prepare (96000.0, 128, 1, 2);
    }
    rig.library->rebuild();
    CHECK (rig.library->stateAsVar()["error"].isVoid());
    const auto model = rig.play (0.0f);
    const auto capture = rig.play (1.0f);
    CHECK (model != capture);
}

TEST_CASE ("Rebuilding after an oversampling switch keeps the capture playing")
{
    // Settings > Latency prepares the engine again in the other mode: the capture is rebuilt for
    // the new rate conversion and the stage's new latency.
    Rig rig;
    REQUIRE (rig.library->loadFile (rig.write ("O.nam", linear ({ 0.5f, 0.2f }))).isEmpty());
    for (const auto mode :
         { sedef::OversamplingMode::linearPhase, sedef::OversamplingMode::lowLatency })
    {
        {
            const std::scoped_lock lock (rig.lock);
            rig.engine.setOversampling (mode);
            rig.engine.prepare (44100.0, 128, 1, 2);
        }
        rig.library->rebuild();
        CHECK (rig.library->stateAsVar()["error"].isVoid());
        CHECK (rig.engine.capture (0).modelSpec().oversampling == mode);
        CHECK (rig.engine.latencySamples() ==
               (mode == sedef::OversamplingMode::lowLatency ? 40 : 296));
        const auto model = rig.play (0.0f);
        const auto capture = rig.play (1.0f);
        CHECK (model != capture);
        CHECK (rig.engine.capture (0).captureAudible());
    }
}

// Untrusted paths and files --------------------------------------------------------------------
// Sessions, presets and the recent list all name files; none of them is trusted.

namespace
{
juce::ValueTree sessionFor (const std::string& bytes, const juce::String& path,
                            const juce::String& name = "Disk")
{
    juce::ValueTree tree ("Capture");
    tree.setProperty ("name", name, nullptr);
    tree.setProperty ("fileName", name + ".nam", nullptr);
    tree.setProperty ("path", path, nullptr);
    tree.setProperty ("sha256", CaptureLibrary::hashOf (bytes), nullptr);
    return tree; // no data: only the path can supply it
}

#if !JUCE_WINDOWS
/// Runs `job` on a thread and says whether it finished within two seconds. If it didn't, it is
/// stuck opening `fifo`, so the pipe is opened for writing and closed to let it go.
bool finishesQuickly (const std::function<void()>& job, const juce::File& fifo)
{
    std::atomic<bool> done { false };
    std::thread t ([&] {
        job();
        done = true;
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (2);
    while (!done && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    const bool quick = done.load();
    while (!done)
    {
        if (const int fd = ::open (fifo.getFullPathName().toRawUTF8(), O_WRONLY | O_NONBLOCK);
            fd >= 0)
            ::close (fd);
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    t.join();
    return quick;
}
#endif
} // namespace

#if !JUCE_WINDOWS
TEST_CASE ("A pipe named as a capture is refused without waiting for it")
{
    Rig rig;
    const auto fifo = rig.folder.getChildFile ("Pipe.nam");
    REQUIRE (::mkfifo (fifo.getFullPathName().toRawUTF8(), 0600) == 0);
    juce::String error;
    CHECK (finishesQuickly ([&] { error = rig.library->loadFile (fifo); }, fifo));
    CAPTURE (error);
    CHECK (error.contains ("isn't a file"));

    // From a session and from a preset: the loader thread doesn't get stuck either.
    const auto bytes = linear ({ 0.4f });
    CHECK (finishesQuickly (
        [&] {
            rig.library->restoreState (sessionFor (bytes, fifo.getFullPathName()));
            rig.waitIdle();
        },
        fifo));
    CHECK (rig.library->stateAsVar()["missing"]["name"] == "Disk");
    CHECK (finishesQuickly (
        [&] {
            rig.library->select (object ({ { "name", "Pipe" },
                                           { "fileName", "Pipe.nam" },
                                           { "path", fifo.getFullPathName() },
                                           { "sha256", CaptureLibrary::hashOf (bytes) } }),
                                 false);
            rig.waitIdle();
        },
        fifo));
}

TEST_CASE ("Devices and links to them are refused")
{
    Rig rig;
    const juce::File zero ("/dev/zero");
    const auto link = rig.folder.getChildFile ("Endless.nam");
    REQUIRE (zero.createSymbolicLink (link, true));
    for (const auto& f : { zero, link })
    {
        const auto error = rig.library->loadFile (f);
        CAPTURE (f.getFullPathName(), error);
        CHECK (error.contains ("isn't a file"));
    }
}
#endif

TEST_CASE ("An empty file is refused as empty")
{
    Rig rig;
    const auto empty = rig.folder.getChildFile ("Empty.nam");
    REQUIRE (empty.create().wasOk());
    const auto error = rig.library->loadFile (empty);
    CAPTURE (error);
    CHECK (error.contains ("is empty"));
}

TEST_CASE ("Relative paths from presets and sessions count as missing")
{
    Rig rig;
    const auto bytes = linear ({ 0.6f });
    const auto dir = rig.folder.getChildFile ("cwd");
    dir.createDirectory();
    dir.getChildFile ("Rel.nam").replaceWithData (bytes.data(), bytes.size());
    const auto previous = juce::File::getCurrentWorkingDirectory();
    dir.setAsCurrentWorkingDirectory();

    rig.library->select (object ({ { "name", "Rel" },
                                   { "fileName", "Rel.nam" },
                                   { "path", "Rel.nam" },
                                   { "sha256", CaptureLibrary::hashOf (bytes) } }),
                         false);
    rig.waitIdle();
    const bool fromPreset = rig.library->currentHash().isEmpty();
    rig.library->restoreState (sessionFor (bytes, "Rel.nam", "Rel"));
    rig.waitIdle();
    const bool fromSession = rig.library->currentHash().isEmpty();
    previous.setAsCurrentWorkingDirectory();

    CHECK (fromPreset);
    CHECK (fromSession);
    CHECK (rig.library->stateAsVar()["missing"]["name"] == "Rel");
}

TEST_CASE ("A capture found on disk loads from the bytes that were checked")
{
    // Read once: a file that changes between checking its hash and loading it can't slip in.
    Rig rig;
    const auto bytes = linear ({ 0.45f });
    const auto file = rig.write ("Once.nam", bytes);
    int before = CaptureLibrary::fileReads();
    rig.library->restoreState (sessionFor (bytes, file.getFullPathName(), "Once"));
    rig.waitIdle();
    CHECK (rig.library->currentHash() == CaptureLibrary::hashOf (bytes));
    CHECK (CaptureLibrary::fileReads() - before == 1);

    // A dropped copy kept under Captures/, found by its name for a preset.
    const auto kept = linear ({ 0.55f });
    REQUIRE (rig.library->loadDropped ("Kept.nam", kept).isEmpty());
    rig.library->recentFile().deleteFile(); // not in the recent list
    CaptureLibrary fresh (rig.engine, rig.lock, rig.folder);
    before = CaptureLibrary::fileReads();
    fresh.select (object ({ { "name", "Kept" },
                            { "fileName", "Kept.nam" },
                            { "path", rig.folder.getChildFile ("gone/Kept.nam").getFullPathName() },
                            { "sha256", CaptureLibrary::hashOf (kept) } }),
                  false);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (fresh.jobsInFlight() > 0 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    CHECK (fresh.currentHash() == CaptureLibrary::hashOf (kept));
    CHECK (CaptureLibrary::fileReads() - before == 1);
}

TEST_CASE ("Dropped files never take a name Windows keeps for a device")
{
    Rig rig;
    const std::pair<const char*, const char*> names[] = {
        { "CON.nam", "_CON.nam" },       { "nul.txt.nam", "_nul.txt.nam" },
        { "Com1.nam", "_Com1.nam" },     { "lpt9 .nam", "_lpt9.nam" },
        { "aux .x.nam", "_aux .x.nam" }, { "Console.nam", "Console.nam" },
        { "COM10.nam", "COM10.nam" },    { "Prn-Lead.nam", "Prn-Lead.nam" },
    };
    float tap = 0.1f;
    for (const auto& [given, kept] : names)
    {
        REQUIRE (rig.library->loadDropped (given, linear ({ tap += 0.01f })).isEmpty());
        CAPTURE (given);
        CHECK (juce::File (rig.library->reference()["path"].toString()).getFileName() == kept);
    }
}

TEST_CASE ("Restoring a session never reads a file on the calling thread")
{
    Rig rig;
    const auto bytes = linear ({ 0.35f });
    const auto file = rig.write ("Disk.nam", bytes);
    const auto tree = sessionFor (bytes, file.getFullPathName());
    rig.library->restoreState (tree);
    // Not yet: the worker reads it. Meanwhile saving keeps what the session asked for.
    CHECK (rig.library->currentHash().isEmpty());
    CHECK (xmlOf (rig.library->toState()) == xmlOf (tree));
    CHECK (rig.library->reference()["sha256"] == tree.getProperty ("sha256"));
    rig.waitIdle();
    CHECK (rig.library->currentHash() == CaptureLibrary::hashOf (bytes));
    CHECK (rig.library->stateAsVar()["missing"].isVoid());
}

TEST_CASE ("A session's stored level is checked against the capture itself")
{
    Rig rig;
    REQUIRE (rig.library->loadFile (rig.write ("Level.nam", linear ({ 0.5f, 0.2f }))).isEmpty());
    const auto saved = rig.library->toState();
    const double measured = saved.getProperty ("normalisationDb").toString().getDoubleValue();

    // As saved: restored at once, with its level, and kept byte for byte.
    Rig honest;
    honest.library->restoreState (saved);
    CHECK (honest.library->currentHash() == rig.library->currentHash());
    honest.waitIdle();
    CHECK (xmlOf (honest.library->toState()) == xmlOf (saved));

    // 30 dB louder than the capture measures: measured again, and played at its real level.
    auto loud = saved.createCopy();
    loud.setProperty ("normalisationDb", juce::String (measured + 30.0, 9), nullptr);
    Rig tampered;
    tampered.library->restoreState (loud);
    tampered.waitIdle();
    const double level =
        static_cast<double> (tampered.library->stateAsVar()["loaded"]["normalisationDb"]);
    CAPTURE (measured, level);
    CHECK (std::abs (level - measured) < 0.01);
    CHECK (tampered.play (1.0f) == rig.play (1.0f));
}
