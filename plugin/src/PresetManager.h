// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/params/Parameters.h"

#include <array>
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include <map>
#include <optional>
#include <vector>

namespace sedef::plugin
{

/// Factory presets (embedded) and user presets (JSON files with the .sedefpreset extension).
///
/// A preset stores every per-preset parameter by string id. Rig-level parameters (input/output
/// gain, transpose, tuner, metronome, tempo...) are deliberately not stored, so switching presets
/// never changes your input staging or tuning. Message thread only, except
/// invalidateSnapshots().
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
    /// The host replaced the whole state: snapshots and A/B slots taken before no longer apply.
    /// Safe to call from any thread; the message thread drops them on its next preset call.
    void invalidateSnapshots() noexcept { generation_.fetch_add (1); }

    bool isDirty() const;
    /// The browser's view of the library. Each call carries a higher "revision", so the editor
    /// can drop an answer that arrives after a newer broadcast.
    juce::var stateAsVar();

    juce::String currentId() const { return currentId_; }
    juce::String currentName() const { return currentName_; }
    /// Restores the current-preset bookkeeping from saved plugin state (values come from the host).
    void restoreCurrent (const juce::String& id, const juce::String& name);

    static bool isGlobal (ParamId id) noexcept;
    static juce::File userFolder();
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
    };

    struct Removed
    {
        juce::File file;
        juce::String text, id, name;
        bool wasCurrent = false;
    };

    const Entry* find (const juce::String& id) const;
    std::optional<juce::var> read (const Entry& e) const;
    void apply (const juce::var& parameters);
    static void describe (Entry& e, const juce::var& parameters);
    Snapshot capture() const;
    bool restore (const Snapshot& s);
    void syncGeneration();
    void loadSettings();
    void storeSettings() const;
    static juce::File settingsFile();
    static juce::String slugify (const juce::String& s);
    static juce::String userId (const juce::File& file);

    juce::AudioProcessorValueTreeState& state_;
    std::vector<Entry> entries_;
    juce::StringArray favorites_, recent_;
    juce::String currentId_;
    juce::String currentName_ { "Init" };
    std::map<ParamId, float> loadedValues_;

    std::array<std::optional<Snapshot>, 2> slots_;
    std::optional<Snapshot> compareOther_;
    bool compareIsB_ = false;
    std::optional<Removed> removed_;
    std::atomic<int> generation_ { 0 };
    int seenGeneration_ = 0;
    juce::int64 revision_ = 0;
};

} // namespace sedef::plugin
