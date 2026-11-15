// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/Engine.h"

#include <array>
#include <cstdint>
#include <functional>
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <mutex>
#include <vector>

namespace sedef::plugin
{

/// User impulse responses for the cab mics: one slot per (mic, cab). Keeps the decoded samples so
/// kernels can be rebuilt when the sample rate changes, and remembers file paths for the state,
/// also of files that could not be read when a session was restored (docs/robustness.md,
/// section 4).
class IrLibrary
{
public:
    static constexpr int kMics = 2;
    static constexpr int kCabs = 3;
    /// Channels an IR file may have (mono, stereo and multi-mic files fold to mono).
    static constexpr int kMaxChannels = 8;

    /// Who asked for a load, which decides what a failure does.
    enum class Origin
    {
        player,  ///< Load or Locate: a failure leaves the slot as it was
        session, ///< a restored session: a failure keeps the path as a missing reference
    };

    /// `engineLock` serialises IR installation against Engine::prepare (both off the audio thread).
    IrLibrary (Engine& engine, std::mutex& engineLock);
    ~IrLibrary();

    /// Any non-real-time thread. Decodes, resamples and installs the IR; returns an error message,
    /// or an empty string on success.
    juce::String load (int mic, int cab, const juce::File& file, Origin origin = Origin::player);
    /// Message thread. Runs load() on the worker thread, then calls onDone on the message thread.
    void loadAsync (int mic, int cab, const juce::File& file,
                    std::function<void (const juce::String&)> onDone,
                    Origin origin = Origin::player);
    void clear (int mic, int cab);
    /// Any thread. Records the file a slot should hold (what the saved state reports, even while
    /// the load is still running) and invalidates older loads in flight for that slot. An empty
    /// file means "no IR". Returns the request's generation.
    std::uint64_t request (int mic, int cab, const juce::File& file);

    /// Rebuilds every loaded kernel for the engine's current sample rate (after prepare).
    void rebuildAll();
    /// Frees kernels retired by the audio thread. Message thread, periodically.
    void collectGarbage();

    juce::var stateAsVar() const;
    /// What the slot should hold: the loaded file, the one loading, or a missing one.
    juce::File fileFor (int mic, int cab) const;
    /// Why the slot's file could not be loaded when a session asked for it, or empty.
    juce::String missingReason (int mic, int cab) const;
    /// The rate the slot's kernels were last built for and handed to the engine; 0 when none.
    double installedRate (int mic, int cab) const;

    /// Any thread; never blocks. Sessions and presets are other people's files, so an IR path
    /// from one is only opened when it is absolute and names a regular file (symlinks followed)
    /// of a sane size: a FIFO, a device or a huge file is refused. Returns an error message, or
    /// an empty string when the file may be read.
    static juce::String checkFile (const juce::String& path);
    static constexpr std::int64_t kMaxFileBytes = 64 * 1024 * 1024;

    /// Tests: called on the building thread while a kernel builds, outside the engine lock.
    std::function<void()> whileBuilding;

private:
    struct Slot
    {
        juce::File file;
        std::shared_ptr<const std::vector<float>> samples;
        double sampleRate = 0.0;
    };

    /// Builds the slot's kernels for the engine as it is now, outside the engine lock, and hands
    /// them over if the slot and the engine's rate are still what they were built for.
    void install (int mic, int cab);
    juce::String loadIfCurrent (int mic, int cab, const juce::File& file, std::uint64_t generation,
                                Origin origin, const juce::File& previous);
    juce::String fail (int mic, int cab, std::uint64_t generation, Origin origin,
                       const juce::File& previous, const juce::String& error);
    /// Under slotsLock_: the slot's content changed (a load, a clear, a missing file).
    void changed (int mic, int cab);

    Engine& engine_;
    std::mutex& engineLock_;
    juce::AudioFormatManager formats_;
    mutable std::recursive_mutex slotsLock_; // slots are touched by the worker and message threads
    std::array<std::array<Slot, kCabs>, kMics> slots_;                  // guarded by slotsLock_
    std::array<std::array<juce::File, kCabs>, kMics> requested_;        // guarded by slotsLock_
    std::array<std::array<juce::String, kCabs>, kMics> missing_;        // guarded: reason
    std::array<std::array<std::uint64_t, kCabs>, kMics> generation_ {}; // guarded by slotsLock_
    std::array<std::array<std::uint64_t, kCabs>, kMics> version_ {};    // guarded: content
    std::array<std::array<double, kCabs>, kMics> installedRate_ {};     // guarded by slotsLock_
    juce::ThreadPool worker_ {
        juce::ThreadPoolOptions {}.withThreadName ("Sedef IR loader").withNumberOfThreads (1)
    };
};

} // namespace sedef::plugin
