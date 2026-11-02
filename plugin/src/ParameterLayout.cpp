// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterLayout.h"

#include <cmath>
#include <map>

namespace sedef::plugin
{

namespace
{
/// value as juce::String (value, decimals) prints it (decimals > 0), with no negative zero: a
/// value that rounds to zero prints "0.0", never "-0.0". (A range test rather than r == 0, which
/// MSVC's /fp:fast may fold away along with the sign of zero.)
float printed (float value, int decimals)
{
    const float scale = std::pow (10.0f, static_cast<float> (decimals));
    const float r = std::round (value * scale) / scale;
    return std::abs (r) < 0.5f / scale ? 0.0f : r;
}

juce::String fixed (float value, int decimals)
{
    return juce::String (printed (value, decimals), decimals);
}
} // namespace

// Signs and units are chosen on the value as printed, not on the raw value, so the text parses
// back to a value that prints the same text again (hosts retype it; clap-validator checks it on
// random values). "+0.0 dB" for +0.00004 dB would read back as 0 dB, which prints "0.0 dB".
juce::String formatValue (const ParamSpec& spec, float value, int maxLength)
{
    juce::String text;
    const auto unit = spec.unit;

    if (spec.kind == ParamKind::Choice)
        text = toJuce (spec.choiceLabel (static_cast<int> (std::lround (value))));
    else if (spec.kind == ParamKind::Bool)
        text = value >= 0.5f ? "On" : "Off";
    else if (unit == "knob")
        text = fixed (value * 10.0f, 1);
    else if (unit == "dB")
        text = (printed (value, 1) > 0.0f ? "+" : "") + fixed (value, 1) + " dB";
    else if (unit == "Hz")
        text = std::round (value) >= 1000.0f ? fixed (value / 1000.0f, 2) + " kHz"
                                             : juce::String (juce::roundToInt (value)) + " Hz";
    else if (unit == "ms")
    {
        if (std::round (value) >= 1000.0f)
            text = fixed (value / 1000.0f, 2) + " s";
        else if (printed (value, 1) < 100.0f)
            text = fixed (value, 1) + " ms";
        else
            text = juce::String (juce::roundToInt (value)) + " ms";
    }
    else if (unit == "s")
        text = printed (value, 2) < 10.0f ? fixed (value, 2) + " s" : fixed (value, 1) + " s";
    else if (unit == "st")
    {
        const int semitones = juce::roundToInt (value);
        text = (semitones > 0 ? "+" : "") + juce::String (semitones) + " st";
    }
    else if (unit == "%")
        text = juce::String (juce::roundToInt (value)) + "%";
    else if (unit == "bpm")
        text = fixed (value, 1) + " BPM";
    else if (unit == "deg")
        text = juce::String (juce::roundToInt (value)) +
               juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
    else
        text = fixed (value, 2);

    return maxLength > 0 ? text.substring (0, maxLength) : text;
}

float parseValue (const ParamSpec& spec, const juce::String& text)
{
    const auto t = text.trim().toLowerCase();
    if (spec.kind == ParamKind::Choice)
    {
        for (int c = 0; c < spec.numChoices(); ++c)
            if (toJuce (spec.choiceLabel (c)).equalsIgnoreCase (t))
                return static_cast<float> (c);
        return spec.sanitise (t.getFloatValue());
    }
    if (spec.kind == ParamKind::Bool)
        return (t == "on" || t == "true" || t == "yes" || t.getFloatValue() >= 0.5f) ? 1.0f : 0.0f;

    float v = t.retainCharacters ("0123456789.-+eE").getFloatValue();
    const auto unit = spec.unit;
    if (unit == "knob")
        v /= 10.0f; // knobs read 0-10
    else if (unit == "Hz" && t.contains ("k"))
        v *= 1000.0f;
    else if (unit == "ms" && t.endsWith ("s") && !t.endsWith ("ms"))
        v *= 1000.0f;
    else if (unit == "s" && t.endsWith ("ms"))
        v /= 1000.0f;
    return spec.sanitise (v);
}

namespace
{
/// The default as the parameter can hold it. AudioParameterFloat starts from the default exactly
/// as given, but every value it takes later passes the normalised round trip, which can move a
/// value by an ULP. A default the round trip moves could never be set again exactly, so
/// "audition, then Esc" on a fresh instance would come back an ULP off. The round trip settles
/// on a value it leaves alone within a step or two; that is the default.
float heldDefault (const juce::NormalisableRange<float>& range, float value)
{
    for (int i = 0; i < 8; ++i)
    {
        const float next =
            range.snapToLegalValue (range.convertFrom0to1 (range.convertTo0to1 (value)));
        if (juce::exactlyEqual (next, value))
            return value;
        value = next;
    }
    return value;
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Groups appear in the host in schema order.
    std::vector<std::unique_ptr<juce::AudioProcessorParameterGroup>> groups;
    std::map<std::string_view, juce::AudioProcessorParameterGroup*> byName;

    for (const auto& spec : kParamSpecs)
    {
        auto& group = byName[spec.group];
        if (group == nullptr)
        {
            const auto name = toJuce (spec.group);
            groups.push_back (std::make_unique<juce::AudioProcessorParameterGroup> (
                name.removeCharacters (" ").toLowerCase(), name, " | "));
            group = groups.back().get();
        }

        const juce::ParameterID id { toJuce (spec.id), 1 };
        const auto name = toJuce (spec.name);
        // Knobs read 0-10 and angles carry their degree sign in the text.
        const auto label =
            spec.unit == "knob" || spec.unit == "deg" ? juce::String() : toJuce (spec.unit);

        switch (spec.kind)
        {
        case ParamKind::Float:
        {
            juce::NormalisableRange<float> range (spec.min, spec.max, 0.0f, spec.skew);
            group->addChild (std::make_unique<juce::AudioParameterFloat> (
                id, name, range, heldDefault (range, spec.defaultValue),
                juce::AudioParameterFloatAttributes()
                    .withLabel (label)
                    .withAutomatable (spec.automatable)
                    .withStringFromValueFunction (
                        [&spec] (float v, int len) { return formatValue (spec, v, len); })
                    .withValueFromStringFunction (
                        [&spec] (const juce::String& text) { return parseValue (spec, text); })));
            break;
        }
        case ParamKind::Int:
            group->addChild (std::make_unique<juce::AudioParameterInt> (
                id, name, static_cast<int> (spec.min), static_cast<int> (spec.max),
                static_cast<int> (spec.defaultValue),
                juce::AudioParameterIntAttributes()
                    .withLabel (label)
                    .withAutomatable (spec.automatable)
                    .withStringFromValueFunction ([&spec] (int v, int len) {
                        return formatValue (spec, static_cast<float> (v), len);
                    })
                    .withValueFromStringFunction ([&spec] (const juce::String& text) {
                        return static_cast<int> (parseValue (spec, text));
                    })));
            break;
        case ParamKind::Bool:
            group->addChild (std::make_unique<juce::AudioParameterBool> (
                id, name, spec.defaultValue >= 0.5f,
                juce::AudioParameterBoolAttributes().withAutomatable (spec.automatable)));
            break;
        case ParamKind::Choice:
        {
            juce::StringArray labels;
            for (int c = 0; c < spec.numChoices(); ++c)
                labels.add (toJuce (spec.choiceLabel (c)));
            group->addChild (std::make_unique<juce::AudioParameterChoice> (
                id, name, labels, static_cast<int> (spec.defaultValue),
                juce::AudioParameterChoiceAttributes().withAutomatable (spec.automatable)));
            break;
        }
        }
    }

    for (auto& g : groups)
        layout.add (std::move (g));
    return layout;
}

} // namespace sedef::plugin
