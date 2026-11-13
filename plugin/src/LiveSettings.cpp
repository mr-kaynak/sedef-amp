// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LiveSettings.h"

#include "ParameterLayout.h"
#include "PresetManager.h"

namespace sedef::plugin
{
namespace
{
constexpr const char* kModeNames[] = { "toggle", "latch" };

struct LooperName
{
    live::Action action;
    const char* name;
};
constexpr LooperName kLooperTargets[] = {
    { live::Action::looperMain, "looper:main" },
    { live::Action::looperStop, "looper:stop" },
    { live::Action::looperUndo, "looper:undo" },
    { live::Action::looperClear, "looper:clear" },
};

int clampInt (const juce::var& v, int lo, int hi, int fallback)
{
    if (!v.isInt() && !v.isInt64() && !v.isDouble())
        return fallback;
    return juce::jlimit (lo, hi, static_cast<int> (v));
}

float clampUnit (const juce::var& v, float fallback)
{
    if (!v.isInt() && !v.isInt64() && !v.isDouble())
        return fallback;
    const auto f = static_cast<float> (static_cast<double> (v));
    return std::isfinite (f) ? juce::jlimit (0.0f, 1.0f, f) : fallback;
}
} // namespace

// LiveTarget --------------------------------------------------------------------------------------

std::optional<LiveTarget> LiveTarget::parse (const juce::String& text)
{
    LiveTarget t;
    if (text.startsWith ("param:"))
    {
        const auto id = findParam (text.fromFirstOccurrenceOf (":", false, false).toStdString());
        if (!id)
            return std::nullopt;
        // The preset switches are triggers the message thread acts on (docs/live.md §4.2).
        if (*id == ParamId::presetNext || *id == ParamId::presetPrevious)
        {
            t.action = *id == ParamId::presetNext ? live::Action::presetNext
                                                  : live::Action::presetPrevious;
            return t;
        }
        t.param = *id;
        return t;
    }
    for (const auto& looper : kLooperTargets)
        if (text == looper.name)
        {
            t.action = looper.action;
            return t;
        }
    if (text == "scene:next")
        t.action = live::Action::sceneNext;
    else if (text == "scene:previous")
        t.action = live::Action::scenePrevious;
    else if (text == "preset:next")
        t.action = live::Action::presetNext;
    else if (text == "preset:previous")
        t.action = live::Action::presetPrevious;
    else if (text.startsWith ("scene:"))
    {
        const auto digits = text.fromFirstOccurrenceOf (":", false, false);
        const int n = digits.getIntValue();
        if (!digits.containsOnly ("0123456789") || n < 1 || n > live::kNumScenes)
            return std::nullopt;
        t.action = live::Action::scene;
        t.scene = n - 1;
    }
    else
        return std::nullopt;
    return t;
}

juce::String LiveTarget::toString() const
{
    switch (action)
    {
    case live::Action::parameter:
        return "param:" + toJuce (spec (param).id);
    case live::Action::scene:
        return "scene:" + juce::String (scene + 1);
    case live::Action::sceneNext:
        return "scene:next";
    case live::Action::scenePrevious:
        return "scene:previous";
    case live::Action::presetNext:
        return "preset:next";
    case live::Action::presetPrevious:
        return "preset:previous";
    case live::Action::looperMain:
    case live::Action::looperStop:
    case live::Action::looperUndo:
    case live::Action::looperClear:
        for (const auto& looper : kLooperTargets)
            if (looper.action == action)
                return looper.name;
        break;
    }
    return {};
}

bool LiveTarget::isSwitch() const noexcept
{
    return action != live::Action::parameter || live::isSwitch (spec (param));
}

bool LiveTarget::isLooper() const noexcept
{
    return action == live::Action::looperMain || action == live::Action::looperStop ||
           action == live::Action::looperUndo || action == live::Action::looperClear;
}

// LiveSettings ------------------------------------------------------------------------------------

live::MidiMap LiveSettings::toMap() const
{
    live::MidiMap map;
    for (const auto& b : bindings)
    {
        live::Binding out;
        out.channel = static_cast<std::uint8_t> (b.channel);
        out.controller = static_cast<std::uint8_t> (b.controller);
        out.action = b.target.action;
        out.scene = static_cast<std::uint8_t> (b.target.scene);
        out.param = static_cast<std::uint16_t> (index (b.target.param));
        out.mode = b.mode;
        out.low = b.low;
        out.high = b.high;
        out.doubleTap = b.doubleTap && b.target.action == live::Action::looperMain;
        if (!map.add (out))
            break;
    }
    map.programChannel = static_cast<std::int8_t> (programChannel);
    for (const auto& [program, target] : programs)
        if (program >= 0 && program < 128 && target.scene >= 0)
            map.programScene[static_cast<std::size_t> (program)] =
                static_cast<std::int8_t> (target.scene);
    return map;
}

int LiveSettings::find (const LiveTarget& target) const
{
    for (std::size_t i = 0; i < bindings.size(); ++i)
        if (bindings[i].target == target)
            return static_cast<int> (i);
    return -1;
}

int LiveSettings::bind (const LiveTarget& target, int channel, int controller,
                        live::SwitchMode mode)
{
    int at = find (target);
    if (at < 0)
    {
        if (static_cast<int> (bindings.size()) >= live::kMaxBindings)
            return -1;
        bindings.push_back ({});
        at = static_cast<int> (bindings.size()) - 1;
    }
    auto& b = bindings[static_cast<std::size_t> (at)];
    // Learning again moves the controller; the double-tap option stays the player's.
    const bool doubleTap = b.doubleTap;
    b = LiveBinding { channel, controller, target, mode, 0.0f, 1.0f, doubleTap };
    return at;
}

juce::var LiveSettings::toVar() const
{
    juce::Array<juce::var> list;
    for (const auto& b : bindings)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("target", b.target.toString());
        o->setProperty ("channel", b.channel + 1);
        o->setProperty ("controller", b.controller);
        if (b.target.isSwitch())
            o->setProperty ("mode", kModeNames[static_cast<int> (b.mode)]);
        if (b.doubleTap)
            o->setProperty ("doubleTap", true);
        else
        {
            o->setProperty ("low", static_cast<double> (b.low));
            o->setProperty ("high", static_cast<double> (b.high));
        }
        list.add (juce::var (o));
    }
    juce::Array<juce::var> assigned;
    for (const auto& [program, target] : programs)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("program", program + 1);
        if (target.scene >= 0)
            o->setProperty ("scene", target.scene + 1);
        else
            o->setProperty ("preset", target.presetId);
        assigned.add (juce::var (o));
    }
    auto* programsObject = new juce::DynamicObject();
    programsObject->setProperty ("channel", programChannel + 1); // 0: any
    programsObject->setProperty ("assigned", assigned);

    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "sedef-midi-setup");
    root->setProperty ("version", 1);
    root->setProperty ("bindings", list);
    root->setProperty ("programs", juce::var (programsObject));
    return juce::var (root);
}

LiveSettings LiveSettings::fromVar (const juce::var& v)
{
    LiveSettings s;
    if (const auto* list = v["bindings"].getArray())
        for (const auto& item : *list)
        {
            const auto target = LiveTarget::parse (item["target"].toString());
            const int controller = clampInt (item["controller"], -1, 127, -1);
            if (!target || !live::isLearnable (controller) ||
                static_cast<int> (s.bindings.size()) >= live::kMaxBindings || s.find (*target) >= 0)
                continue; // unknown, unusable or repeated: a file from a newer version, or edited
            LiveBinding b;
            b.target = *target;
            b.channel = clampInt (item["channel"], 1, 16, 1) - 1;
            b.controller = controller;
            b.mode = item["mode"].toString() == "latch" ? live::SwitchMode::latch
                                                        : live::SwitchMode::toggle;
            b.low = clampUnit (item["low"], 0.0f);
            b.high = clampUnit (item["high"], 1.0f);
            b.doubleTap = b.target.action == live::Action::looperMain &&
                          static_cast<bool> (item["doubleTap"]);
            s.bindings.push_back (b);
        }
    const auto& programs = v["programs"];
    s.programChannel = clampInt (programs["channel"], 0, 16, 0) - 1;
    if (const auto* list = programs["assigned"].getArray())
        for (const auto& item : *list)
        {
            const int program = clampInt (item["program"], 0, 128, 0) - 1;
            if (program < 0)
                continue;
            ProgramTarget t;
            if (item.hasProperty ("scene"))
            {
                t.scene = clampInt (item["scene"], 0, live::kNumScenes, 0) - 1;
                if (t.scene < 0)
                    continue;
            }
            else
            {
                t.presetId = item["preset"].toString();
                if (t.presetId.isEmpty())
                    continue;
            }
            s.programs[program] = t;
        }
    return s;
}

juce::File LiveSettings::file()
{
    return PresetManager::userFolder().getSiblingFile ("midi.json");
}

bool LiveSettings::load()
{
    const auto f = file();
    if (!f.existsAsFile())
    {
        *this = {};
        return true;
    }
    const auto parsed = juce::JSON::parse (f.loadFileAsString());
    if (!parsed.isObject())
        return false; // never read a broken file as "no bindings": a save would wipe them
    *this = fromVar (parsed);
    return true;
}

bool LiveSettings::save() const
{
    const auto f = file();
    f.getParentDirectory().createDirectory();
    return f.replaceWithText (juce::JSON::toString (toVar()));
}

} // namespace sedef::plugin
