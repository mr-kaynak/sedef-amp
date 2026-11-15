// SPDX-License-Identifier: AGPL-3.0-or-later
// The standalone app's recorder and re-amp around their real-time halves (docs/recorder.md):
// the files a take leaves, read back the way a DAW reads them, and files loaded, converted and
// dropped for re-amping. The test plays the audio thread and calls the writer's and reader's
// passes itself, so nothing depends on timing.

#include "PresetManager.h"
#include "Reamp.h"
#include "Recorder.h"
#include "sedef/record/RateConverter.h"
#include "sedef/record/WavFloatWriter.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <limits>

using namespace sedef;
using namespace sedef::plugin;
using record::TakeRecorder;

namespace
{
struct Fixture
{
    Fixture()
    {
        root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("sedef-plugin-tests")
                   .getChildFile ("run-" + juce::Uuid().toString());
        root.createDirectory();
        PresetManager::testRoot() = root;
    }
    ~Fixture()
    {
        PresetManager::testRoot() = juce::File();
        root.deleteRecursively();
    }

    juce::ScopedJuceInitialiser_GUI juce;
    juce::File root;
};

std::vector<float> ramp (int n, float scale)
{
    std::vector<float> v (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t> (i)] =
            scale * static_cast<float> ((i * 7919) % 2000 - 1000) / 1000.0f;
    return v;
}

std::vector<std::vector<float>>
readAll (const juce::File& file, juce::StringPairArray* metadata = nullptr, double* rate = nullptr)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    CHECK (reader->bitsPerSample == 32);
    CHECK (reader->usesFloatingPointData);
    if (metadata != nullptr)
        *metadata = reader->metadataValues;
    if (rate != nullptr)
        *rate = reader->sampleRate;
    const int n = static_cast<int> (reader->lengthInSamples);
    std::vector<std::vector<float>> out (reader->numChannels,
                                         std::vector<float> (static_cast<std::size_t> (n)));
    std::vector<float*> ptrs;
    for (auto& c : out)
        ptrs.push_back (c.data());
    REQUIRE (reader->read (ptrs.data(), static_cast<int> (ptrs.size()), 0, n));
    return out;
}

void writeWav (const juce::File& file, const std::vector<std::vector<float>>& channels, double rate)
{
    FileSink sink (file);
    REQUIRE (sink.openedOk());
    record::WavFloatWriter w (sink, static_cast<int> (channels.size()), rate);
    REQUIRE (w.begin());
    const int n = static_cast<int> (channels[0].size());
    std::vector<float> interleaved;
    for (int i = 0; i < n; ++i)
        for (const auto& c : channels)
            interleaved.push_back (c[static_cast<std::size_t> (i)]);
    REQUIRE (w.write (0, interleaved.data(), n));
    REQUIRE (w.finish ({}));
}

/// An 8-bit mono WAV file whose header says `rate`, holding `bytes` frames: what a crafted file
/// says about itself.
juce::File writeClaims (const juce::File& file, std::uint32_t rate, std::uint32_t claimedBytes,
                        int bytes = 4096)
{
    juce::MemoryOutputStream out;
    out.write ("RIFF", 4);
    out.writeInt (static_cast<int> (36u + claimedBytes));
    out.write ("WAVEfmt ", 8);
    out.writeInt (16);
    out.writeShort (1); // PCM
    out.writeShort (1); // mono
    out.writeInt (static_cast<int> (rate));
    out.writeInt (static_cast<int> (rate));
    out.writeShort (1);
    out.writeShort (8);
    out.write ("data", 4);
    out.writeInt (static_cast<int> (claimedBytes));
    juce::HeapBlock<char> data (static_cast<std::size_t> (bytes));
    for (int i = 0; i < bytes; ++i)
        data[i] = static_cast<char> (128 + (i % 64));
    out.write (data.get(), static_cast<std::size_t> (bytes));
    REQUIRE (file.replaceWithData (out.getData(), out.getDataSize()));
    return file;
}
} // namespace

TEST_CASE ("Recorder: a take is a DI and an output file that a WAV reader opens, sample-exact",
           "[recorder]")
{
    Fixture fixture;
    TakeRecorder core;
    Recorder recorder (core);

    recorder.toggle (48000.0, 1, 2);
    recorder.step(); // the files are created and the take armed
    REQUIRE (core.phase() == TakeRecorder::Phase::armed);
    CHECK (recorder.status() == Recorder::Status::starting);

    // The audio thread: a DI, and an output that is the DI, louder, `latency` samples later.
    const int block = 480, blocks = 200, latency = 7;
    const auto di = ramp (block * (blocks + 2), 0.5f);
    std::vector<float> outL (di.size()), outR (di.size());
    for (std::size_t i = latency; i < di.size(); ++i)
    {
        outL[i] = 1.5f * di[i - latency]; // over full scale: nothing clips in a float file
        outR[i] = -di[i - latency];
    }
    int at = 0;
    const auto audioBlock = [&] {
        const float* in[] = { di.data() + at };
        const float* out[] = { outL.data() + at, outR.data() + at };
        core.captureInput (in, 1, block);
        core.captureOutput (out, 2, block, latency);
        at += block;
    };
    for (int b = 0; b < 100; ++b)
    {
        audioBlock();
        if (b % 10 == 0)
            recorder.step();
    }
    CHECK (recorder.status() == Recorder::Status::recording);
    recorder.toggle (48000.0, 1, 2); // Stop
    CHECK (recorder.status() == Recorder::Status::saving);
    recorder.step();
    audioBlock(); // the DI ends here; the output catches up in the same block
    REQUIRE (core.phase() == TakeRecorder::Phase::finished);
    recorder.step();
    REQUIRE (recorder.status() == Recorder::Status::idle);

    const auto state = recorder.stateAsVar();
    const auto& take = state["take"];
    CHECK (take["ended"].toString() == "stopped");
    CHECK (juce::exactlyEqual (take["seconds"].operator double(), 1.0));
    CHECK (state["folder"].toString() == Recorder::folder().getFullPathName());
    const juce::File diFile (take["diPath"].toString()), outFile (take["outputPath"].toString());
    CHECK (diFile.getParentDirectory() == Recorder::folder());
    CHECK (diFile.getFileName() == take["name"].toString() + " DI.wav");
    CHECK (outFile.getFileName() == take["name"].toString() + " Output.wav");

    juce::StringPairArray metadata;
    double rate = 0.0;
    const auto diBack = readAll (diFile, &metadata, &rate);
    CHECK (juce::exactlyEqual (rate, 48000.0));
    REQUIRE (diBack.size() == 1);
    REQUIRE (diBack[0].size() == 48000);
    CHECK (std::equal (diBack[0].begin(), diBack[0].end(), di.begin()));
    CHECK (metadata["ICMT"].contains ("DI: the dry input"));
    CHECK (metadata["ICMT"].contains (outFile.getFileName()));

    const auto outBack = readAll (outFile, &metadata);
    REQUIRE (outBack.size() == 2);
    REQUIRE (outBack[0].size() == 48000);
    CHECK (std::equal (outBack[0].begin(), outBack[0].end(), outL.begin() + latency));
    CHECK (std::equal (outBack[1].begin(), outBack[1].end(), outR.begin() + latency));
    CHECK (metadata["ICMT"].contains ("Shifted 7 samples earlier"));

    // The next take gets its own names.
    recorder.toggle (48000.0, 1, 2);
    recorder.step();
    audioBlock();
    recorder.audioStopped(); // the device stops
    recorder.step();
    CHECK (recorder.status() == Recorder::Status::idle);
    CHECK (recorder.stateAsVar()["take"]["ended"].toString() == "audioStopped");
    CHECK (Recorder::folder().findChildFiles (juce::File::findFiles, false, "*.wav").size() == 4);
}

TEST_CASE ("Recorder: a take stopped before any audio leaves no files", "[recorder]")
{
    Fixture fixture;
    TakeRecorder core;
    Recorder recorder (core);
    recorder.toggle (48000.0, 1, 2);
    recorder.step();
    recorder.toggle (48000.0, 1, 2);
    recorder.step();
    const float silence[64] {};
    const float* in[] = { silence };
    const float* out[] = { silence, silence };
    core.captureInput (in, 1, 64);
    core.captureOutput (out, 2, 64, 0);
    recorder.step();
    CHECK (recorder.status() == Recorder::Status::idle);
    CHECK (recorder.stateAsVar()["take"]["ended"].toString() == "empty");
    CHECK (Recorder::folder().findChildFiles (juce::File::findFiles, false, "*.wav").isEmpty());
}

TEST_CASE ("Re-amp: files load, play exactly, convert and drop in", "[recorder]")
{
    Fixture fixture;
    record::ReampPlayer player;
    Reamp reamp (player);
    reamp.prepare (48000.0, 1, 256);
    Recorder::folder().createDirectory();

    const auto audioBlocks = [&player] (std::vector<float>& in, int from, int count) {
        for (int b = 0; b < count; ++b)
        {
            float* ch[] = { in.data() + from + b * 256 };
            player.process (ch, 1, 256);
        }
    };

    SECTION ("A take at the device's rate plays bit-exact")
    {
        const auto take = Recorder::folder().getChildFile ("2026-11-13 10-00-00 DI.wav");
        const std::vector<std::vector<float>> data { ramp (5000, 0.3f) };
        writeWav (take, data, 48000.0);
        CHECK (reamp.mayLoad (take));
        reamp.load (take);
        reamp.runOnce();
        auto state = reamp.stateAsVar();
        REQUIRE (state["status"].toString() == "ready");
        CHECK_FALSE (static_cast<bool> (state["file"]["converted"]));

        const auto files = reamp.files();
        REQUIRE (files.size() == 1);
        CHECK (files[0]["name"].toString() == "2026-11-13 10-00-00");
        CHECK (files[0]["kind"].toString() == "take");

        reamp.setLoop (false);
        reamp.play();
        reamp.runOnce(); // asks for a flush
        std::vector<float> in (256 * 40, 0.0f);
        audioBlocks (in, 0, 1); // flushed
        reamp.runOnce();        // filled, play requested
        audioBlocks (in, 256, 39);
        CHECK (std::equal (data[0].begin(), data[0].end(), in.begin() + 256));
        CHECK (std::all_of (in.begin() + 256 + 5000, in.end(), [] (float x) { return x == 0.0f; }));
    }

    SECTION ("A 44.1 kHz file is converted for a 48 kHz device")
    {
        const auto file = fixture.root.getChildFile ("elsewhere.wav");
        writeWav (file, { ramp (44100, 0.2f), ramp (44100, -0.2f) }, 44100.0);
        CHECK_FALSE (reamp.mayLoad (file)); // outside the Recordings folder: the chooser only
        reamp.load (file);
        reamp.runOnce();
        const auto state = reamp.stateAsVar();
        REQUIRE (state["status"].toString() == "ready");
        CHECK (static_cast<bool> (state["file"]["converted"]));
        CHECK (juce::exactlyEqual (state["file"]["sampleRate"].operator double(), 44100.0));
        CHECK (juce::exactlyEqual (
            reamp.live()["length"].operator double(),
            static_cast<double> (record::RateConverter::outputLength (44100, 44100.0, 48000.0)) /
                48000.0));
    }

    SECTION ("Files Re-amp cannot play are refused with a reason")
    {
        const auto text = Recorder::folder().getChildFile ("notes.wav");
        text.replaceWithText ("not audio");
        reamp.load (text);
        reamp.runOnce();
        CHECK (reamp.stateAsVar()["status"].toString() == "error");
        CHECK (reamp.stateAsVar()["error"].toString().contains ("WAV, AIFF or FLAC"));
        CHECK_FALSE (reamp.mayLoad (Recorder::folder().getChildFile ("missing.wav")));
        CHECK (reamp.importBegin ("song.mp3", 100) == 0);
        CHECK (reamp.importBegin ("huge.wav", Reamp::kMaxImportBytes + 1) == 0);
    }

    SECTION ("A file whose converted copy would pass 4 GB is refused before converting")
    {
        // docs/robustness.md, section 5: re-amp converted the whole file (minutes, and 4 GB of
        // temporary file), then blamed the disk. The length is checked first.
        CHECK (
            Reamp::conversionProblem ("Long.wav", 11'520 * 44100, 2, 44100.0, 48000.0) ==
            "Long.wav is 3 h 12 min long; converted to 48.0 kHz it would pass the 4 GB limit of a "
            "WAV file. Trim it, or set the audio device to 44.1 kHz so it plays without "
            "converting.");
        CHECK (Reamp::conversionProblem ("Ok.wav", 3'600 * 2 * 44100, 2, 44100.0, 48000.0)
                   .isEmpty()); // 2 h of stereo: 2.8 GB converted
        CHECK (Reamp::conversionProblem ("Mono.wav", 3'600 * 5 * 44100, 1, 44100.0, 48000.0)
                   .isEmpty()); // 5 h of mono: 3.5 GB
        CHECK (Reamp::conversionProblem ("Huge.wav", std::numeric_limits<juce::int64>::max(), 2,
                                         8000.0, 384000.0)
                   .isNotEmpty());

        // Through the reader: 8 kHz into a 384 kHz device is 48 times the frames, so a 22 MB
        // file is enough to pass the limit.
        reamp.prepare (384000.0, 1, 256);
        const auto file =
            writeClaims (fixture.root.getChildFile ("Long.wav"), 8000u, 22'400'000u, 22'400'000);
        reamp.load (file);
        reamp.runOnce();
        const auto state = reamp.stateAsVar();
        CHECK (state["status"].toString() == "error");
        CHECK (state["error"].toString() ==
               "Long.wav is 46 min 40 s long; converted to 384.0 kHz it would pass the 4 GB limit "
               "of a WAV file. Trim it, or set the audio device to 8.0 kHz so it plays without "
               "converting.");
    }

    SECTION ("A file claiming a rate outside 8 to 384 kHz is refused")
    {
        const auto file =
            writeClaims (fixture.root.getChildFile ("Absurd.wav"), 4294967295u, 4096u);
        reamp.load (file);
        reamp.runOnce();
        CHECK (reamp.stateAsVar()["status"].toString() == "error");
        CHECK (reamp.stateAsVar()["error"].toString() ==
               "Absurd.wav says it was recorded at 4294967295 Hz. Re-amp plays files recorded at "
               "8 to 384 kHz.");
    }

    SECTION ("A dropped file arrives in pieces, is kept under Imported and loads")
    {
        const auto source = fixture.root.getChildFile ("dropped.wav");
        writeWav (source, { ramp (3000, 0.4f) }, 48000.0);
        juce::MemoryBlock bytes;
        REQUIRE (source.loadFileAsData (bytes));
        const int token =
            reamp.importBegin ("My DI.wav", static_cast<juce::int64> (bytes.getSize()));
        REQUIRE (token != 0);
        const auto half = bytes.getSize() / 2;
        CHECK_FALSE (reamp.importChunk (token + 1, juce::Base64::toBase64 (bytes.getData(), half)));
        // (a wrong token cancels nothing it does not own)
        CHECK (reamp.importChunk (token, juce::Base64::toBase64 (bytes.getData(), half)));
        CHECK (reamp.importChunk (
            token, juce::Base64::toBase64 (static_cast<const char*> (bytes.getData()) + half,
                                           bytes.getSize() - half)));
        CHECK (reamp.importEnd (token));
        reamp.runOnce();
        const auto state = reamp.stateAsVar();
        REQUIRE (state["status"].toString() == "ready");
        const juce::File kept (state["file"]["path"].toString());
        CHECK (kept == Reamp::importFolder().getChildFile ("My DI.wav"));
        CHECK (kept.getSize() == static_cast<juce::int64> (bytes.getSize()));
        CHECK (reamp.files()[0]["kind"].toString() == "imported");
        CHECK (reamp.mayLoad (kept));
    }
}
