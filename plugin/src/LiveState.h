// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The plug-in's side of playing live (docs/live.md): the master scene bank, the player's MIDI
// setup and learn, and the message thread's 50 Hz tick that writes what the audio thread's live
// layer did into the host's parameters.

#include "LiveSettings.h"
#include "sedef/live/LiveControl.h"

#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <mutex>
#include <optional>

namespace sedef::plugin
{

class SedefProcessor;

/// A preset's or a session's "scenes" object (docs/live.md §6.4) from a bank. `live` gives the
/// values of the scene in use. `full` lists every value of every other scene (sessions,
/// snapshots, the dirty reference); otherwise only the values that differ from `live` (preset
/// files). The scene in use is always {}.
juce::var scenesToVar (const live::SceneBank& bank, const std::function<float (ParamId)>& live,
                       bool full);
/// The bank a "scenes" object stands for; what it doesn't name is `base`. Void gives 8 copies of
/// `base`. Unknown ids are ignored and values sanitised.
live::SceneBank scenesFromVar (const juce::var& scenes, const std::function<float (ParamId)>& base);
/// True when only the scene in use is made and nothing is picked: a preset need not carry the
/// scenes.
bool scenesAreTrivial (const live::SceneBank& bank);

class LiveState final : private juce::Timer
{
public:
    static constexpr int kTickHz = 50;

    explicit LiveState (SedefProcessor& processor);
    ~LiveState() override;

    live::LiveControl& control() noexcept { return control_; }

    // Message thread -----------------------------------------------------------------------------

    /// The 50 Hz tick (public for the plug-in tests, which have no message loop).
    void tick();
    /// What the MIDI panel and the scene bar show (protocol.ts LiveState).
    juce::var stateAsVar();
    /// Bumped whenever stateAsVar() changes, so the editor can push it.
    std::atomic<int> revision { 0 };

    int activeScene() const;
    void selectScene (int scene);
    /// Puts the sound of the scene in use into `scene`.
    void copyScene (int scene);
    /// Picks a value for scenes, or drops it. False when it can't be (docs/live.md §6.2).
    bool rememberInScenes (ParamId id, bool remember);

    bool learn (const juce::String& target);
    void cancelLearn();
    void forget (const juce::String& target);
    void updateBinding (int index, const juce::var& changes);
    void removeBinding (int index);
    void clearBindings();
    /// `target`: { preset: id } or { scene: 0-7 }; void removes the assignment.
    void setProgram (int program, const juce::var& target);
    void setProgramChannel (int channel);
    /// The standalone app: switches one of its MIDI inputs on or off (JUCE's device manager keeps
    /// the choice with its audio settings).
    void setInput (const juce::String& identifier, bool enabled);
    const LiveSettings& settings() const noexcept { return settings_; }

    // Presets (PresetManager::Attachment "scenes") ------------------------------------------------
    juce::var reference (bool forPreset);
    void select (const juce::var& scenes, bool exact);
    bool differs (const juce::var& loaded);
    juce::var resolve (const juce::var& stored, const juce::var& parameters) const;
    bool owns (ParamId id) const;

    // Sessions: any thread ------------------------------------------------------------------------
    /// The session's "scenes" attribute; empty when the scenes are trivial.
    juce::String sessionText();
    /// Before the host's parameters are replaced: the bank the session holds (`saved` reads the
    /// session's parameter values), published first so the audio thread never takes the restored
    /// Scene parameter for a switch. Returns the active scene.
    int restoreSession (const juce::String& text, const std::function<float (ParamId)>& saved);

private:
    void timerCallback() override { tick(); }
    float liveValue (ParamId id) const;
    void write (ParamId id, float value);
    void processEvents();
    void processLearn();
    /// Under bankLock_.
    void publish (bool applySwitch);
    void settingsChanged();
    void finishLearn (std::optional<live::SwitchMode> mode);

    SedefProcessor& processor_;
    live::LiveControl control_;

    mutable std::mutex bankLock_; // the master bank: setStateInformation may run on any thread
    live::SceneBank bank_;

    LiveSettings settings_;
    juce::String settingsText_; // midi.json as last read or written
    int ticks_ = 0;

    struct Learn
    {
        LiveTarget target;
        enum class Phase
        {
            waiting,   ///< for the first control change
            listening, ///< bound; watching the controller for its release
            done
        } phase = Phase::waiting;
        int binding = -1;
        std::optional<live::SwitchClassifier> classifier;
    };
    std::optional<Learn> learn_;

    std::int64_t lastAudioTime_ = -1;
    int ticksSinceAudio_ = 1000;
    std::uint32_t pendingSwitch_ = 0; ///< an on-screen switch the audio thread may still apply
    int pendingSince_ = 0;
    bool processingEvents_ = false;
};

} // namespace sedef::plugin
