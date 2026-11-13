// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// MIDI bindings for playing live (docs/live.md): what a controller does to the rig, and the pure
// arithmetic behind it. Plain data, so a whole map crosses to the audio thread by value.

#include "sedef/params/Parameters.h"

#include <array>
#include <cstdint>
#include <optional>

namespace sedef::live
{

/// Scenes per preset (docs/live.md §6.1).
inline constexpr int kNumScenes = 8;
/// Bindings the map holds (§7.3).
inline constexpr int kMaxBindings = 64;
/// The MIDI 1.0 split for switch controllers: 0-63 off, 64-127 on.
inline constexpr int kSwitchThreshold = 64;
/// How long learn watches a footswitch for its release (§4.4), in seconds of audio.
inline constexpr double kLearnWindowSeconds = 1.5;

/// One MIDI channel message as the live layer reads it.
struct MidiMessage
{
    std::uint8_t status = 0; ///< with the channel in the low nibble
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;

    constexpr int channel() const noexcept { return status & 0x0f; }
    constexpr bool isControlChange() const noexcept { return (status & 0xf0) == 0xb0; }
    constexpr bool isProgramChange() const noexcept { return (status & 0xf0) == 0xc0; }

    static constexpr MidiMessage controlChange (int channel, int controller, int value) noexcept
    {
        return { static_cast<std::uint8_t> (0xb0 | (channel & 0x0f)),
                 static_cast<std::uint8_t> (controller & 0x7f),
                 static_cast<std::uint8_t> (value & 0x7f) };
    }
    static constexpr MidiMessage programChange (int channel, int program) noexcept
    {
        return { static_cast<std::uint8_t> (0xc0 | (channel & 0x0f)),
                 static_cast<std::uint8_t> (program & 0x7f), 0 };
    }
};

/// What a binding drives.
enum class Action : std::uint8_t
{
    parameter,     ///< `Binding::param`
    scene,         ///< select `Binding::scene`
    sceneNext,     ///< one scene up, 8 wraps to 1
    scenePrevious, ///< one scene down, 1 wraps to 8
    presetNext,    ///< the message thread steps the preset list
    presetPrevious,
    // The looper's footswitches (§4.5), standalone app only.
    looperMain, ///< what the looper's big button does next
    looperStop,
    looperUndo, ///< undo, or redo when it was just undone
    looperClear
};

/// What a footswitch asks of the looper (§4.5), in the order presses arrived in a block.
enum class LooperPress : std::uint8_t
{
    main,
    stop,
    undo, ///< undo, or redo when the last layer (or clear) was just undone
    clear,
    doubleTap ///< the second press of a double tap on `main` (Binding::doubleTap)
};

/// A second press of the looper's main action this soon after the first stops the looper (§4.5).
inline constexpr double kDoubleTapSeconds = 0.4;
/// Looper presses kept per block (§7.3).
inline constexpr int kMaxLooperPresses = 8;

/// The double-tap window in samples: round (kDoubleTapSeconds * rate).
inline std::int64_t doubleTapSamples (double sampleRate) noexcept
{
    return static_cast<std::int64_t> (kDoubleTapSeconds * sampleRate + 0.5);
}

/// Whether a press at `now` is the second of a double tap after one at `last` (-1: none waiting):
/// the blocks start less than `window` samples apart (§4.5).
constexpr bool isDoubleTap (std::int64_t last, std::int64_t now, std::int64_t window) noexcept
{
    return last >= 0 && now - last < window;
}

/// How a switch target reads a footswitch (§4.4).
enum class SwitchMode : std::uint8_t
{
    toggle, ///< each message >= 64 flips; < 64 ignored (momentary switches)
    latch   ///< follows the message: on at >= 64 (latching switches, or hold-to-use)
};

/// One controller (a control change number on a channel) driving one target.
struct Binding
{
    std::uint8_t channel = 0;    ///< 0-15
    std::uint8_t controller = 0; ///< 0-119
    Action action = Action::parameter;
    std::uint8_t scene = 0;  ///< Action::scene: 0-7
    std::uint16_t param = 0; ///< Action::parameter: the ParamId's index
    SwitchMode mode = SwitchMode::toggle;
    float low = 0.0f;       ///< continuous targets: normalised value at controller 0
    float high = 1.0f;      ///< ... and at 127 (low > high inverts)
    bool doubleTap = false; ///< Action::looperMain: a double tap stops (§4.5)

    friend bool operator== (const Binding&, const Binding&) = default;
};

/// Everything the audio thread needs to read a controller: bindings and the program table.
struct MidiMap
{
    std::array<Binding, kMaxBindings> bindings {};
    std::uint8_t numBindings = 0;
    /// The program channel: -1 any, else 0-15.
    std::int8_t programChannel = -1;
    /// Per program: -1 load a preset (the message thread resolves which), 0-7 select that scene.
    std::array<std::int8_t, 128> programScene {};

    MidiMap() noexcept { programScene.fill (-1); }

    /// Appends a binding; false when full.
    bool add (const Binding& b) noexcept
    {
        if (numBindings >= kMaxBindings)
            return false;
        bindings[numBindings++] = b;
        return true;
    }
};

/// True for a parameter a controller switches rather than sweeps.
constexpr bool isSwitch (const ParamSpec& spec) noexcept
{
    return spec.kind == ParamKind::Bool;
}

/// The parameter value a continuous binding gives for a controller value (§4.3):
/// n = low + (high - low) * value / 127, clamped, through the parameter's skew and snapping.
float continuousValue (const ParamSpec& spec, float low, float high, int value) noexcept;

/// What a switch target does with one message (§4.4): the new state, or nothing (Toggle ignores
/// releases).
std::optional<bool> switchValue (SwitchMode mode, bool current, int value) noexcept;

/// Whether a message counts as a press for a scene or preset action: Toggle reacts to >= 64,
/// Latch to every message (each press of a latching switch sends the other value).
constexpr bool actionFires (SwitchMode mode, int value) noexcept
{
    return mode == SwitchMode::latch || value >= kSwitchThreshold;
}

/// Controllers learn takes: not channel mode messages (120-127), bank select (0, 32), data entry
/// (6, 38) or (N)RPN numbers (98-101), which are parts of other messages (§4.1).
constexpr bool isLearnable (int controller) noexcept
{
    return controller >= 0 && controller < 120 && controller != 0 && controller != 32 &&
           controller != 6 && controller != 38 && !(controller >= 98 && controller <= 101);
}

/// The learn classifier (§4.4), fed the messages of one controller in order.
class SwitchClassifier
{
public:
    /// `window` is kLearnWindowSeconds in samples at the session's rate.
    explicit SwitchClassifier (std::int64_t windowSamples) noexcept : window_ (windowSamples) {}

    /// The first message of the controller, at `time` (samples). Returns the mode when it is
    /// decided at once.
    std::optional<SwitchMode> first (int value, std::int64_t time) noexcept;
    /// A later message of the same controller. Returns the mode once decided.
    std::optional<SwitchMode> next (int value, std::int64_t time) noexcept;
    /// The audio clock moved on to `now` without a message: decides when the window is over.
    std::optional<SwitchMode> elapsed (std::int64_t now) noexcept;

    bool decided() const noexcept { return decided_.has_value(); }
    std::optional<SwitchMode> mode() const noexcept { return decided_; }

private:
    std::int64_t window_;
    std::int64_t start_ = 0;
    bool started_ = false;
    std::optional<SwitchMode> decided_;
};

} // namespace sedef::live
