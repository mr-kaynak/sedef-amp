// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/record/ReampFeeder.h"
#include "sedef/record/ReampPlayer.h"

#include <atomic>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <memory>
#include <mutex>

namespace sedef::plugin
{

/// The standalone app's re-amp (docs/recorder.md, section 6): its reader thread, loading and
/// converting files, and what the panel shows. The real-time half is the deck's ReampPlayer.
///
///   prepare()          while the audio callback does not run, after the engine's prepare
///   everything else    message thread
class Reamp final : private juce::Thread
{
public:
    /// The longest file a drop may copy in (the page sends it in pieces).
    static constexpr juce::int64 kMaxImportBytes = juce::int64 (2) << 30;
    /// Recorded DIs the panel lists.
    static constexpr int kMaxTakes = 30;

    explicit Reamp (record::ReampPlayer& player);
    ~Reamp() override;

    /// Recordings/Imported: where dropped files are copied.
    static juce::File importFolder();

    void prepare (double sampleRate, int channels, int maxBlock);

    /// Loads (and converts, if its rate is not the device's) on the reader thread.
    void load (const juce::File& file);
    void play();
    void stop();
    void setLoop (bool loop);
    void setMix (bool mix);

    /// A file the page may ask for by path: a listed take or an imported file.
    bool mayLoad (const juce::File& file) const;

    // Drops: the page sends the file's bytes in pieces (base64), they are kept in importFolder().
    /// Returns a token, or 0 when the name or size is refused (error set).
    int importBegin (const juce::String& fileName, juce::int64 size);
    bool importChunk (int token, const juce::String& base64);
    /// Closes the copy and loads it.
    bool importEnd (int token);
    void importCancel (int token);

    juce::var stateAsVar() const;
    /// What changes while it plays: playing, the position and the length in seconds.
    juce::var live() const;
    /// Recorded DIs and imported files, newest first: { name, path, seconds, kind }.
    juce::var files();
    std::atomic<int> revision { 0 };

    /// One pass of the reader thread: commands, loading, topping up the FIFO. (The plug-in's
    /// tests call it by hand instead of starting the thread.)
    void runOnce();

private:
    void run() override;
    void ensureThread();
    void openFile (const juce::File& file);
    void unload();
    void setError (const juce::String& message);
    void bump() { ++revision; }

    record::ReampPlayer& player_;
    record::ReampFeeder feeder_;
    juce::AudioFormatManager formats_;     // the reader thread's
    juce::AudioFormatManager listFormats_; // the message thread's (files())
    std::atomic<bool> playing_ { false };

    /// Held by the reader thread while it feeds, and by prepare() while it rebuilds the player.
    std::mutex feedLock_;
    std::unique_ptr<record::FrameSource> source_; // the file at the device's rate (feedLock_)
    juce::File converted_;                        // the temporary converted file, if any

    // Commands for the reader thread, and what the panel shows (stateLock_).
    mutable std::mutex stateLock_;
    juce::File wantLoad_;
    bool loadWanted_ = false, playWanted_ = false, stopWanted_ = false;
    bool loop_ = true, mix_ = false;
    bool loopChanged_ = false;

    enum class Status
    {
        empty,
        loading,
        converting,
        ready,
        error
    };
    Status status_ = Status::empty;
    juce::File file_;
    double fileRate_ = 0.0, deviceRate_ = 48000.0, progress_ = 0.0;
    int fileChannels_ = 0;
    juce::int64 fileFrames_ = 0, playFrames_ = 0;
    juce::String error_;
    std::atomic<int> cancelLoad_ { 0 };

    // Imports (message thread).
    std::unique_ptr<juce::FileOutputStream> import_;
    juce::File importFile_;
    int importToken_ = 0;
    juce::int64 importExpected_ = 0;
};

} // namespace sedef::plugin
