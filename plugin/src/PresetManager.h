// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/params/Parameters.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>
#include <vector>

namespace sedef::plugin
{

/// Factory presets (embedded) and user presets (JSON files with the .sedefpreset extension).
///
/// A preset stores every per-preset parameter by string id. Rig-level parameters (input/output
/// gain, transpose, tuner, metronome, tempo...) are deliberately not stored, so switching presets
/// never changes your input staging or tuning. Message thread only.
class PresetManager
{
public:
    static constexpr const char* kExtension = ".sedefpreset";

    explicit PresetManager (juce::AudioProcessorValueTreeState& state);

    /// Re-reads the factory list and the user folder.
    void scan();

    bool load (const juce::String& id);
    /// Saves the current state as a user preset and makes it current. Returns the new id.
    juce::String save (const juce::String& name, const juce::String& folder);
    bool remove (const juce::String& id);
    bool rename (const juce::String& id, const juce::String& newName);
    void setFavorite (const juce::String& id, bool favorite);
    void step (int delta);
    void loadInit();

    bool isDirty() const;
    juce::var stateAsVar() const;

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
        juce::String id, name, category, folder, author;
        juce::StringArray tags;
        juce::File file;          ///< user presets
        juce::String factoryJson; ///< factory presets
    };

    const Entry* find (const juce::String& id) const;
    std::optional<juce::var> read (const Entry& e) const;
    void apply (const juce::var& parameters);
    void loadFavorites();
    void storeFavorites() const;
    static juce::File settingsFile();
    static juce::String slugify (const juce::String& s);

    juce::AudioProcessorValueTreeState& state_;
    std::vector<Entry> entries_;
    juce::StringArray favorites_;
    juce::String currentId_;
    juce::String currentName_ { "Init" };
    std::map<ParamId, float> loadedValues_;
};

} // namespace sedef::plugin
