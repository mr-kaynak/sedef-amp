// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The player's MIDI setup (docs/live.md §3-5): bindings and program assignments, kept per user in
// midi.json next to the presets, shared by every instance and the standalone app.

#include "sedef/live/LiveMap.h"
#include "sedef/live/SceneBank.h"

#include <juce_core/juce_core.h>
#include <map>
#include <optional>
#include <vector>

namespace sedef::plugin
{

/// What a binding drives, as the UI and midi.json name it: "param:<id>", "scene:<1-8>",
/// "scene:next", "scene:previous", "preset:next", "preset:previous", or the looper's
/// "looper:main", "looper:stop", "looper:undo" and "looper:clear" (docs/live.md §4.5).
struct LiveTarget
{
    live::Action action = live::Action::parameter;
    ParamId param {};
    int scene = 0; ///< 0-7

    static std::optional<LiveTarget> parse (const juce::String& text);
    juce::String toString() const;
    /// Switch-like: a footswitch's mode matters (an on/off parameter, a scene or a preset step).
    bool isSwitch() const noexcept;
    /// One of the looper's actions (the standalone app's).
    bool isLooper() const noexcept;

    friend bool operator== (const LiveTarget& a, const LiveTarget& b) noexcept
    {
        return a.action == b.action && a.param == b.param && a.scene == b.scene;
    }
};

struct LiveBinding
{
    int channel = 0;    ///< 0-15
    int controller = 0; ///< 0-119
    LiveTarget target;
    live::SwitchMode mode = live::SwitchMode::toggle;
    float low = 0.0f, high = 1.0f; ///< normalised range, continuous targets
    bool doubleTap = false;        ///< "looper:main": a double tap stops (docs/live.md §4.5)

    friend bool operator== (const LiveBinding& a, const LiveBinding& b) noexcept
    {
        return a.channel == b.channel && a.controller == b.controller && a.target == b.target &&
               a.mode == b.mode && juce::exactlyEqual (a.low, b.low) &&
               juce::exactlyEqual (a.high, b.high) && a.doubleTap == b.doubleTap;
    }
};

/// What a program change does when the player assigned it: a preset, or a scene.
struct ProgramTarget
{
    juce::String presetId; ///< when scene < 0
    int scene = -1;        ///< 0-7

    friend bool operator== (const ProgramTarget&, const ProgramTarget&) = default;
};

class LiveSettings
{
public:
    std::vector<LiveBinding> bindings;
    int programChannel = -1;               ///< -1 any, else 0-15
    std::map<int, ProgramTarget> programs; ///< by program 0-127

    /// What the audio thread reads. Bindings past live::kMaxBindings are dropped (bind() refuses
    /// them first).
    live::MidiMap toMap() const;

    /// Adds a binding of `target` to the controller, or moves the target's binding there (one
    /// source per target). Returns its index, or -1 when the map is full.
    int bind (const LiveTarget& target, int channel, int controller, live::SwitchMode mode);
    int find (const LiveTarget& target) const;

    /// midi.json, with channels 1-16 and programs 1-128 as players read them.
    juce::var toVar() const;
    static LiveSettings fromVar (const juce::var& v);

    friend bool operator== (const LiveSettings&, const LiveSettings&) = default;

    static juce::File file();
    /// Reads midi.json; a missing file is an empty setup, an unreadable one leaves this as is.
    bool load();
    bool save() const;
};

} // namespace sedef::plugin
