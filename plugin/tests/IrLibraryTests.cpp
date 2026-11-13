// SPDX-License-Identifier: AGPL-3.0-or-later
// Which IR paths the plug-in is willing to open. Sessions and presets are other people's files, so
// a path from one may name a FIFO, a device or a relative path; none of those may block or be read.

#include "IrLibrary.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>

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
