// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/record/TakeFiles.h"
#include "sedef/record/TakeRecorder.h"

#include <atomic>
#include <juce_core/juce_core.h>
#include <memory>
#include <mutex>

namespace sedef::plugin
{

/// A ByteSink on a file: the recorder's takes and the re-amp's converted files.
class FileSink final : public record::ByteSink
{
public:
    explicit FileSink (const juce::File& file);
    bool openedOk() const { return stream_ != nullptr && stream_->openedOk(); }
    bool writeAt (std::uint64_t offset, const void* data, std::size_t bytes) override;
    bool flush() override;
    void close() { stream_.reset(); }

private:
    std::unique_ptr<juce::FileOutputStream> stream_;
};

/// The standalone app's recorder (docs/recorder.md): its writer thread, the files, and what the
/// panel shows. The real-time half is the deck's TakeRecorder.
///
///   toggle(), stateAsVar(), live()   message thread
///   audioStopped()                   while the audio callback does not run
class Recorder final : private juce::Thread
{
public:
    explicit Recorder (record::TakeRecorder& core);
    ~Recorder() override;

    /// Music/Sedef Amp/Recordings (plugin tests: under PresetManager::testRoot()).
    static juce::File folder();

    /// Starts a take in the engine's current format, or stops the one running.
    void toggle (double sampleRate, int diChannels, int outChannels);
    /// The audio device stopped or restarts (prepareToPlay, releaseResources): ends a take here.
    void audioStopped();
    /// Finishes a take and waits for its files (shutdown, tests).
    void finishNow();

    enum class Status
    {
        idle,
        starting, ///< files open, waiting for the first audio block
        recording,
        saving ///< stopped; the output catches up and the files are completed
    };
    Status status() const noexcept { return status_.load(); }

    /// The panel's state: status, folder, the current or last take, the error.
    juce::var stateAsVar() const;
    /// What changes every frame: elapsed seconds, the files' levels (dB) and dropouts.
    juce::var live();
    /// Bumped whenever stateAsVar() changes.
    std::atomic<int> revision { 0 };

    /// One pass of the writer thread. (The plug-in's tests call it by hand instead of starting
    /// the thread.)
    void step();

private:
    void run() override;
    void begin (const record::TakeRecorder::Config& config);
    void complete();
    void setError (const juce::String& message);

    record::TakeRecorder& core_;
    std::atomic<Status> status_ { Status::idle };

    std::mutex commandLock_;
    bool startWanted_ = false, stopWanted_ = false; // guarded by commandLock_
    record::TakeRecorder::Config wanted_;           // guarded by commandLock_

    // Writer thread.
    std::unique_ptr<FileSink> diSink_, outSink_;
    std::unique_ptr<record::TakeFiles> files_;
    bool writeFailed_ = false;
    std::int64_t sinceHeader_ = 0;

    mutable std::mutex stateLock_;
    struct Take
    {
        juce::String name;
        juce::File di, output;
        double seconds = 0.0;
        std::int64_t dropouts = 0;
        double droppedSeconds = 0.0;
        juce::String ended; // "", "stopped", "sizeLimit", "audioStopped", "writeError", "empty"
    };
    Take take_;            // guarded by stateLock_: the current take, or the last one
    bool hasTake_ = false; // guarded
    juce::String error_;   // guarded
    double rate_ = 48000.0;
};

} // namespace sedef::plugin
