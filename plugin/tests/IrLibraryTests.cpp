// SPDX-License-Identifier: AGPL-3.0-or-later
// Which IR paths the plug-in is willing to open. Sessions and presets are other people's files, so
// a path from one may name a FIFO, a device or a relative path; none of those may block or be read.

#include "IrLibrary.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <juce_events/juce_events.h>
#include <mutex>

#if !JUCE_WINDOWS
#include <sys/stat.h>
#endif

using sedef::plugin::IrLibrary;

namespace
{
struct Folder
{
    Folder()
        : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("sedef-ir-tests-" + juce::Uuid().toString()))
    {
        dir.createDirectory();
    }
    ~Folder() { dir.deleteRecursively(); }
    juce::File dir;
};
} // namespace

TEST_CASE ("IR paths: only absolute paths to regular files are opened")
{
    Folder f;
    const auto ok = f.dir.getChildFile ("Room.wav");
    REQUIRE (ok.replaceWithData ("RIFF....WAVE", 12));
    CHECK (IrLibrary::checkFile (ok.getFullPathName()).isEmpty());

    CHECK (IrLibrary::checkFile ("Room.wav") == "The IR file was not found");
    CHECK (IrLibrary::checkFile ("") == "The IR file was not found");
    CHECK (IrLibrary::checkFile (f.dir.getChildFile ("Gone.wav").getFullPathName()) ==
           "The IR file was not found");
    CHECK (IrLibrary::checkFile (f.dir.getFullPathName()) == "Unsupported audio file");

    const auto empty = f.dir.getChildFile ("Empty.wav");
    REQUIRE (empty.create().wasOk());
    CHECK (IrLibrary::checkFile (empty.getFullPathName()) == "The file contains no audio");
}

#if !JUCE_WINDOWS
TEST_CASE ("IR paths: pipes, devices and links to them are refused without blocking")
{
    Folder f;
    const auto fifo = f.dir.getChildFile ("Pipe.wav");
    REQUIRE (::mkfifo (fifo.getFullPathName().toRawUTF8(), 0600) == 0);
    const auto link = f.dir.getChildFile ("Endless.wav");
    REQUIRE (juce::File ("/dev/zero").createSymbolicLink (link, true));

    const auto start = std::chrono::steady_clock::now();
    for (const auto* path : { "/dev/zero", "/dev/tty" })
        CHECK (IrLibrary::checkFile (path) == "Unsupported audio file");
    CHECK (IrLibrary::checkFile (fifo.getFullPathName()) == "Unsupported audio file");
    CHECK (IrLibrary::checkFile (link.getFullPathName()) == "Unsupported audio file");
    CHECK (std::chrono::steady_clock::now() - start < std::chrono::seconds (1));
}
#endif

namespace
{
/// A WAV file whose header says `rate` and `channels`, holding `frames` 16-bit frames (an impulse
/// on every channel). Written by hand: a header can claim what no writer would produce.
juce::File writeWav (const juce::File& file, std::uint32_t rate, int channels, int frames)
{
    juce::MemoryOutputStream out;
    const auto dataBytes = static_cast<std::uint32_t> (frames * channels * 2);
    out.write ("RIFF", 4);
    out.writeInt (static_cast<int> (36 + dataBytes));
    out.write ("WAVEfmt ", 8);
    out.writeInt (16);
    out.writeShort (1); // PCM
    out.writeShort (static_cast<short> (channels));
    out.writeInt (static_cast<int> (rate));
    out.writeInt (static_cast<int> (rate * static_cast<std::uint32_t> (channels) * 2u));
    out.writeShort (static_cast<short> (channels * 2));
    out.writeShort (16);
    out.write ("data", 4);
    out.writeInt (static_cast<int> (dataBytes));
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            out.writeShort (i == 0 ? static_cast<short> (20000) : static_cast<short> (0));
    REQUIRE (file.replaceWithData (out.getData(), out.getDataSize()));
    return file;
}

struct Rig
{
    Rig() { engine.prepare (48000.0, 256, 1, 2); }
    sedef::Engine engine;
    std::mutex engineLock;
    IrLibrary irs { engine, engineLock };
};
} // namespace

TEST_CASE ("IR files claiming a sample rate outside 8 to 384 kHz are refused with the reason")
{
    // docs/robustness.md, section 5: a claimed 4 294 967 295 Hz made the resampler run for
    // minutes, holding the engine lock.
    juce::ScopedJuceInitialiser_GUI juce;
    Folder f;
    Rig rig;
    const auto absurd = writeWav (f.dir.getChildFile ("Absurd.wav"), 4294967295u, 1, 4096);
    CHECK (rig.irs.load (0, 0, absurd) ==
           "Absurd.wav says it was recorded at 4294967295 Hz. Sedef reads IRs recorded at 8 to "
           "384 kHz.");
    CHECK (rig.irs.fileFor (0, 0) == juce::File());
    CHECK (rig.irs.load (0, 0, writeWav (f.dir.getChildFile ("Low.wav"), 7999u, 1, 4096))
               .contains ("7999 Hz"));
    CHECK (rig.irs.load (0, 0, writeWav (f.dir.getChildFile ("Wide.wav"), 48000u, 9, 64)) ==
           "Wide.wav has 9 channels. Sedef reads IRs with up to 8.");
    for (const std::uint32_t rate : { 8000u, 44100u, 384000u })
    {
        INFO (rate);
        CHECK (
            rig.irs.load (0, 1, writeWav (f.dir.getChildFile ("Ok.wav"), rate, 2, 4096)).isEmpty());
        CHECK (juce::exactlyEqual (rig.irs.installedRate (0, 1), 48000.0));
    }
}

TEST_CASE ("IR kernels are built outside the engine lock and follow a prepare made meanwhile")
{
    // docs/robustness.md, section 5: the lock is what prepareToPlay and the message thread's
    // timer wait on. While the kernel builds, it is free; a prepare made then (the host changed
    // its rate) has the slot built again for the new rate.
    juce::ScopedJuceInitialiser_GUI juce;
    Folder f;
    Rig rig;
    const auto ir = writeWav (f.dir.getChildFile ("Cab.wav"), 44100u, 1, 44100);
    int builds = 0;
    bool lockWasFree = true;
    rig.irs.whileBuilding = [&] {
        ++builds;
        lockWasFree = lockWasFree && rig.engineLock.try_lock();
        if (lockWasFree)
        {
            if (builds == 1)
                rig.engine.prepare (96000.0, 512, 1, 2); // under the lock, as prepareToPlay does
            rig.engineLock.unlock();
        }
    };
    REQUIRE (rig.irs.load (1, 1, ir).isEmpty());
    CHECK (lockWasFree);
    CHECK (builds == 2);
    CHECK (juce::exactlyEqual (rig.irs.installedRate (1, 1), 96000.0));

    // A newer load or clear while one builds wins.
    rig.irs.whileBuilding = [&] {
        rig.irs.clear (1, 1);
    };
    REQUIRE (rig.irs.load (1, 1, ir).isEmpty());
    CHECK (juce::exactlyEqual (rig.irs.installedRate (1, 1), 0.0));
    CHECK (rig.irs.fileFor (1, 1) == juce::File());
}
