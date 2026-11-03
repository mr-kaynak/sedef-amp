// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/params/Parameters.h"

#include <array>
#include <atomic>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace sedef::plugin
{

/// Factory presets (embedded) and user presets (JSON files with the .sedefpreset extension).
///
/// A preset stores every per-preset parameter by string id. Rig-level parameters (input/output
/// gain, transpose, tuner, metronome, tempo...) are deliberately not stored, so switching presets
/// never changes your input staging or tuning. Message thread only, except hostRestored(),
/// hostRestorePending() and lockParameters().
class PresetManager
{
public:
    static constexpr const char* kExtension = ".sedefpreset";
    static constexpr int kMaxRecent = 12;

    /// Snapshot slots the editor uses: "browse" is taken when the preset browser opens (Esc goes
    /// back to it), "undo" before a load that would drop unsaved edits.
    enum class Slot
    {
        browse,
        undo
    };
    static std::optional<Slot> slotFromString (const juce::String& name);

    explicit PresetManager (juce::AudioProcessorValueTreeState& state);

    /// Re-reads the factory list and the user folder.
    void scan();

    /// Loads a preset. An audition (a click in the editor's browser) is not a Recent pick.
    bool load (const juce::String& id, bool audition = false);
    /// Records a pick for Recent without loading (the browser keeps an auditioned preset).
    void markRecent (const juce::String& id);
    /// Saves the current state as a user preset and makes it current. Returns the new id.
    /// `details` is an optional { tags, description } object; without it, an overwritten preset
    /// keeps its tags, description and author.
    juce::String save (const juce::String& name, const juce::String& folder,
                       const juce::var& details = {});
    bool remove (const juce::String& id);
    /// Puts back the user preset removed last (same file, same id).
    bool undoRemove();
    bool rename (const juce::String& id, const juce::String& newName);
    void setFavorite (const juce::String& id, bool favorite);
    void step (int delta);
    void loadInit();

    // Snapshots and A/B ---------------------------------------------------------------------------
    void snapshot (Slot slot);
    /// Restores a snapshot exactly (values, current preset, dirty reference). False when the slot
    /// is empty or the host replaced the state since it was taken.
    bool restore (Slot slot);
    /// Stores the live state in the active A/B slot and brings back the other one. The first
    /// switch copies the live state, so B starts as A.
    void compareSwitch();
    /// Copies the live state into the slot that is not being heard.
    void compareCopy();

    // Host state restores
    // ---------------------------------------------------------------------------
    /// Held while the parameters change as a whole: a preset load, a snapshot restore, or the
    /// host replacing the state (setStateInformation, from any thread). Never on the audio thread.
    [[nodiscard]] std::unique_lock<std::recursive_mutex> lockParameters()
    {
        return std::unique_lock (parametersLock_);
    }
    /// The host replaced the whole state, naming this preset. Any thread, after the values
    /// changed and while lockParameters() is held. Snapshots and A/B slots taken before no longer
    /// apply; the preset bookkeeping follows on the message thread, at the latest on its next
    /// preset call (sync()).
    void hostRestored (const juce::String& id, const juce::String& name);
    /// True while a host restore waits for sync(). Any thread.
    bool hostRestorePending() const;
    /// Applies a pending host restore and drops snapshots it outdated. Every preset call starts
    /// with it, so none of them works on the state from before the restore.
    void sync();

    bool isDirty() const;
    /// The browser's view of the library. Each call carries a higher "revision", so the editor
    /// can drop an answer that arrives after a newer broadcast.
    juce::var stateAsVar();

    juce::String currentId() const { return currentId_; }
    juce::String currentName() const { return currentName_; }

    static bool isGlobal (ParamId id) noexcept;

    /// Something a preset refers to besides its parameters (the amp capture, CaptureLibrary; the
    /// pedalboard): saved under `key` as whatever `reference (true)` returns (void: nothing),
    /// brought back on load (`select (ref, false)`), and carried exactly by snapshots and A/B
    /// (`reference (false)`, `select (ref, true)`). `differs` adds to "dirty".
    ///
    /// `resolve`, when given, turns what a preset file holds under `key` (void when the file has
    /// none) into the reference it stands for, given the file's parameters: a preset saved
    /// before the pedalboard loads the board that matches its parameters. With `selectOnInit`,
    /// Init selects what an empty preset resolves to.
    struct Attachment
    {
        juce::String key;
        std::function<juce::var (bool forPreset)> reference;
        std::function<void (const juce::var& ref, bool exact)> select;
        std::function<bool (const juce::var& ref)> differs;
        std::function<juce::var (const juce::var& stored, const juce::var& parameters)> resolve;
        bool selectOnInit = false;
    };
    void addAttachment (Attachment a);
    static juce::File userFolder();
#if SEDEF_PLUGIN_TESTS
    /// Where the test build keeps what a release keeps under the user's application data.
    static juce::File& testRoot()
    {
        static juce::File root;
        return root;
    }
#endif
    /// Serialises the current per-preset parameters (used by save and by tests).
    juce::var captureParameters() const;

private:
    struct Entry
    {
        juce::String id, name, category, folder, author, description;
        juce::StringArray tags;
        juce::File file;          ///< user presets
        juce::String factoryJson; ///< factory presets
        // What the browser shows about the sound, computed with defaults applied.
        int amp = 0, cab = 0;
        juce::StringArray blocks;
    };

    /// The preset-relevant state: every per-preset parameter, the current preset and the values
    /// "dirty" compares against.
    struct Snapshot
    {
        std::array<float, kNumParams> raw {};        ///< what the engine reads
        std::array<float, kNumParams> normalised {}; ///< the parameter's getValue()
        std::map<ParamId, float> loadedValues;
        juce::String id, name;
        bool dirty = false;
        int generation = 0;
        std::vector<juce::var> attachments, loadedAttachments; ///< one each, as attachments_
    };

    struct Removed
    {
        juce::File file;
        juce::String text, id, name;
        bool wasCurrent = false;
    };

    struct HostRestore
    {
        juce::String id, name;
    };

    const Entry* find (const juce::String& id) const;
    std::optional<juce::var> read (const Entry& e) const;
    void apply (const juce::var& parameters);
    /// The browser's summary of a preset file: amp, cab, and what it switches on.
    static void describe (Entry& e, const juce::var& preset);
    Snapshot capture() const;
    bool restore (const Snapshot& s);
    /// The current-preset bookkeeping for a session the host restored (the values are its own).
    void applyCurrent (const juce::String& id, const juce::String& name);
    /// The stored preset value of parameter `i`, cleaned up like a load would.
    static float storedValue (const juce::var& parameters, std::size_t i);
    /// Changes one parameter as a gesture, so hosts see it like a turn of the control.
    static void setWithGesture (juce::RangedAudioParameter& param, float normalised);
    /// Reads settings.json into the lists. False, leaving them as they were, when the file is
    /// there but can't be read.
    bool loadSettings();
    void storeSettings() const;
    /// Reads settings.json, applies `change` to the lists and writes them back, under a lock that
    /// other processes (another host, the standalone app) take too. Writes nothing when the file
    /// could not be read or the lock not taken; the change still applies in memory.
    void updateSettings (const std::function<void()>& change);
    static juce::File settingsFile();
    static juce::String slugify (const juce::String& s);
    static juce::String userId (const juce::File& file);
    /// A preset or folder name made safe as a file name: no characters the file system refuses,
    /// no leading dots (".." or a hidden file), no trailing dots or spaces. Empty when nothing is
    /// left.
    static juce::String safeFileName (const juce::String& name);

    juce::AudioProcessorValueTreeState& state_;
    std::vector<Entry> entries_;
    juce::StringArray favorites_, recent_;
    juce::String currentId_;
    juce::String currentName_ { "Init" };
    std::map<ParamId, float> loadedValues_;
    std::vector<Attachment> attachments_;
    /// What the current preset's attachments were when loaded or saved, one each.
    std::vector<juce::var> loadedAttachments_;
    /// What a preset file's attachment stands for (Attachment::resolve).
    juce::var resolved (const Attachment& a, const juce::var& file) const;

    std::array<std::optional<Snapshot>, 2> slots_;
    std::optional<Snapshot> compareOther_;
    bool compareIsB_ = false;
    std::optional<Removed> removed_;
    std::atomic<int> generation_ { 0 };
    int seenGeneration_ = 0;
    juce::int64 revision_ = 0;

    mutable std::recursive_mutex parametersLock_;
    mutable std::mutex hostLock_;
    std::optional<HostRestore> hostRestore_; ///< guarded by hostLock_
    juce::InterProcessLock settingsLock_ { "SedefAmpSettings" };
};

} // namespace sedef::plugin
