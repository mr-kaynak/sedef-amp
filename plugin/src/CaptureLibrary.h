// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/Engine.h"
#include "sedef/dsp/capture/CaptureFile.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sedef::plugin
{

/// The .nam capture the amp stage plays (docs/dsp/nam.md): loading off the audio thread, the
/// session state (the file's path and the file itself), the reference user presets keep, the
/// recent list and what to show while a capture is missing.
///
/// Every call is safe from any non-real-time thread (hosts save and restore state wherever they
/// like); the async calls answer on the message thread.
class CaptureLibrary
{
public:
    static constexpr int kMaxRecent = 12;
    /// Captures kept built for this session, so presets, snapshots and A/B switch at once.
    static constexpr int kCacheSize = 6;

    /// `folder` holds captures.json (the recent list) and Captures/ (dropped files are kept
    /// there, so presets and the recent list can find them again).
    CaptureLibrary (Engine& engine, std::mutex& engineLock, juce::File folder);
    ~CaptureLibrary();

    /// Reads, validates, builds and installs; returns an error message, or an empty string on
    /// success. A successful load goes to the top of the recent list.
    juce::String loadFile (const juce::File& file);
    /// A file dropped on the editor: no path, so it is first kept under Captures/.
    juce::String loadDropped (const juce::String& fileName, const std::string& bytes);
    /// The same on the worker thread; `done` gets the error on the message thread.
    void loadFileAsync (const juce::File& file, std::function<void (const juce::String&)> done);
    void loadDroppedAsync (const juce::String& fileName, std::string bytes,
                           std::function<void (const juce::String&)> done);
    void clear();

    /// After Engine::prepare: builds the capture again for the new rate and block size.
    void rebuild();
    /// Frees models the audio thread has retired. Periodically.
    void collectGarbage();

    // Session state ----------------------------------------------------------------------------
    /// <Capture> with the path, the hash, the measured level and the file itself (gzip, base64).
    /// Invalid when no capture is loaded or wanted. Byte for byte what restoreState() was given.
    juce::ValueTree toState() const;
    /// With the file embedded (so a session sounds the same on any machine): synchronous, so a
    /// restored session plays its capture from the first block; the stored level makes that
    /// quick and is measured again on the worker. Without it, the file at its path (or a recent
    /// or kept file with its hash) is read on the worker, never on the calling thread; toState()
    /// returns the tree meanwhile. An invalid tree clears.
    void restoreState (const juce::ValueTree& tree);

    // Presets: a reference ({ name, fileName, path, sha256 }), never the model itself ----------
    /// The loaded capture's reference, else the missing one's (saving keeps asking for it), or
    /// void.
    juce::var reference() const;
    /// Makes `ref` the capture: the current one if it matches, else one built earlier this
    /// session, else the file at its path, else a recent or kept file with the same hash (those
    /// load on the worker). A void `ref` clears when `exact` (snapshots, A/B) and is ignored
    /// otherwise (presets without a capture). Records the capture as missing when nothing
    /// matches.
    void select (const juce::var& ref, bool exact);
    /// Whether the capture differs from what `ref` asks for (a preset's dirty state). A void
    /// `ref` never differs, nor does a capture that is missing or still loading.
    bool differs (const juce::var& ref) const;

    /// What a fresh load should set "Capture Through Cab" to, from the metadata: off for a full
    /// rig, on for an amp, nothing when the file doesn't say.
    std::optional<bool> suggestedCab() const;

    /// For the bridge (ui/src/bridge/protocol.ts, CaptureState).
    juce::var stateAsVar() const;
    /// Bumped on every change, so the editor knows to send the state.
    std::atomic<int> revision { 0 };

    juce::String currentHash() const;
    /// A load the player is waiting for is running.
    bool isLoading() const;
    /// Loads queued or running on the worker, including superseded ones (tests).
    int jobsInFlight() const;
    juce::File droppedFolder() const;
    juce::File recentFile() const;

    static juce::String hashOf (const std::string& bytes);
    /// Instrumentation for tests: files opened for reading so far, by every instance.
    static int fileReads() noexcept;

private:
    struct Entry
    {
        std::shared_ptr<const dsp::CaptureFile> file;
        juce::String fileName, sha256;
        juce::File path;
        std::shared_ptr<const std::string> bytes; ///< the file as loaded
    };

    struct Recent
    {
        juce::String name, fileName, path, sha256;
    };

    struct Found
    {
        juce::File file;
        std::string bytes; ///< as read and hashed
    };

    /// Builds `bytes` and, when `generation` is still the newest request, installs it.
    juce::String loadBytes (const juce::String& fileName, std::shared_ptr<const std::string> bytes,
                            const juce::File& path, std::optional<float> knownNormalisationDb,
                            bool remember, std::uint64_t generation,
                            const juce::ValueTree& restoredState = {});
    juce::String loadPath (const juce::File& file, bool remember, std::uint64_t generation);
    juce::String loadDropped (const juce::String& fileName, const std::string& bytes,
                              std::uint64_t generation);
    juce::String install (const std::shared_ptr<const dsp::CaptureFile>& file);
    void makeCurrent (Entry entry, const juce::ValueTree& restoredState);
    std::uint64_t request (const juce::var& pendingRef = {});
    bool isNewest (std::uint64_t generation) const;
    void finish (std::uint64_t generation, const juce::String& error);
    void markMissing (const juce::var& ref, const juce::ValueTree& state);
    void changed();
    std::optional<Entry> cached (const juce::String& sha256) const;
    void remember (const Entry& e);
    void loadAsync (std::function<juce::String()> job,
                    std::function<void (const juce::String&)> done);
    std::optional<Found> findByHash (const juce::String& sha256,
                                     const juce::String& fileName) const;
    void checkLevel (std::shared_ptr<const std::string> bytes, const juce::String& sha256,
                     float stored, std::uint64_t generation);
    static juce::String displayName (const dsp::CaptureInfo& info, const juce::String& fileName);
    static juce::ValueTree makeState (const Entry& e);

    std::vector<Recent> readRecent() const;
    void writeRecent (const std::function<void (std::vector<Recent>&)>& change);

    Engine& engine_;
    std::mutex& engineLock_;
    juce::File folder_;

    mutable std::recursive_mutex lock_;
    std::optional<Entry> current_; // guarded by lock_
    juce::ValueTree currentState_; // guarded: what toState() returns while current_ is set
    juce::var missing_;            // guarded: the reference that couldn't be found, or void
    juce::ValueTree missingState_; // guarded: kept so saving again doesn't lose the reference
    juce::String error_;           // guarded: the last failed load's message
    juce::var pending_;            // guarded: the reference a load in flight is for
    int inFlight_ = 0;             // guarded
    std::uint64_t generation_ = 0; // guarded: the newest request
    std::vector<Entry> cache_;     // guarded: most recent first

    juce::InterProcessLock recentLock_ { "SedefAmpCaptures" };
    juce::ThreadPool worker_ {
        juce::ThreadPoolOptions {}.withThreadName ("Sedef capture loader").withNumberOfThreads (1)
    };
};

} // namespace sedef::plugin
