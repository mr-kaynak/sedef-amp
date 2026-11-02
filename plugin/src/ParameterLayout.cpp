// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterLayout.h"

#include <cmath>
#include <map>

namespace sedef::plugin
{

juce::String formatValue (const ParamSpec& spec, float value, int maxLength)
{
    juce::String text;
    const auto unit = spec.unit;

    if (spec.kind == ParamKind::Choice)
        text = toJuce (spec.choiceLabel (static_cast<int> (std::lround (value))));
    else if (spec.kind == ParamKind::Bool)
        text = value >= 0.5f ? "On" : "Off";
    else if (unit == "knob")
        text = juce::String (value * 10.0f, 1);
    else if (unit == "dB")
    {
        // Sign and digits from the value as shown, so a hair either side of 0 reads "0.0 dB"
        // both ways (hosts round-trip text through the normalised value).
        const float shown = std::round (value * 10.0f) / 10.0f;
        text = shown == 0.0f ? juce::String ("0.0 dB")
                             : (shown > 0.0f ? "+" : "") + juce::String (shown, 1) + " dB";
    }
    else if (unit == "Hz")
        text = value >= 1000.0f ? juce::String (value / 1000.0f, 2) + " kHz"
                                : juce::String (juce::roundToInt (value)) + " Hz";
    else if (unit == "ms")
        text = value >= 1000.0f ? juce::String (value / 1000.0f, 2) + " s"
                                : juce::String (value, value < 100.0f ? 1 : 0) + " ms";
    else if (unit == "s")
        text = juce::String (value, value < 10.0f ? 2 : 1) + " s";
    else if (unit == "st")
    {
        const int shown = juce::roundToInt (value);
        text = (shown > 0 ? "+" : "") + juce::String (shown) + " st";
    }
    else if (unit == "%")
        text = juce::String (juce::roundToInt (value)) + "%";
    else if (unit == "bpm")
        text = juce::String (value, 1) + " BPM";
    else
        text = juce::String (value, 2);

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
        const auto label = spec.unit == "knob" ? juce::String() : toJuce (spec.unit);

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
