// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Numbers from someone else's file, a host or the page (docs/robustness.md, section 2):
// present but null, or of the wrong type, is absent; a number must be finite before it is
// clamped or converted, because jlimit and std::clamp let NaN through and converting NaN or a
// value outside int's range to int is undefined behaviour.

#include <cmath>
#include <juce_core/juce_core.h>
#include <limits>
#include <optional>

namespace sedef::plugin
{

/// A JSON or ValueTree number (int, int64, double or bool), finite; nothing for anything else.
/// Strings count only when `allowText`: XML attributes (sessions) are always text.
inline std::optional<double> finiteNumber (const juce::var& v, bool allowText = false)
{
    double d = 0.0;
    if (v.isInt() || v.isInt64() || v.isDouble() || v.isBool())
        d = static_cast<double> (v);
    else if (allowText && v.isString())
    {
        const auto text = v.toString().trim();
        if (text.isEmpty() || !text.containsOnly ("0123456789+-.eE"))
            return std::nullopt;
        d = text.getDoubleValue();
    }
    else
        return std::nullopt;
    if (!std::isfinite (d))
        return std::nullopt;
    return d;
}

/// finiteNumber() as a float: values beyond float's range become its largest (converting them
/// would be undefined behaviour).
inline std::optional<float> finiteFloat (const juce::var& v, bool allowText = false)
{
    const auto d = finiteNumber (v, allowText);
    if (!d)
        return std::nullopt;
    constexpr double big = static_cast<double> (std::numeric_limits<float>::max());
    return static_cast<float> (juce::jlimit (-big, big, *d));
}

/// A whole number clamped to [lo, hi] (fractions rounded), or `fallback` when it isn't one.
inline int toIntIn (const juce::var& v, int lo, int hi, int fallback, bool allowText = false)
{
    const auto d = finiteNumber (v, allowText);
    if (!d)
        return fallback;
    const double r = std::round (*d);
    if (r <= static_cast<double> (lo))
        return lo;
    if (r >= static_cast<double> (hi))
        return hi;
    return static_cast<int> (r);
}

/// A finite number clamped to [lo, hi], or `fallback`.
inline double toDoubleIn (const juce::var& v, double lo, double hi, double fallback,
                          bool allowText = false)
{
    const auto d = finiteNumber (v, allowText);
    return d ? juce::jlimit (lo, hi, *d) : fallback;
}

/// A finite number inside int64's range (a byte count, a time), or `fallback`.
inline juce::int64 toInt64 (const juce::var& v, juce::int64 fallback)
{
    if (v.isInt() || v.isInt64())
        return static_cast<juce::int64> (v);
    const auto d = finiteNumber (v);
    // 2^63 is the first double outside int64.
    if (!d || *d >= 9223372036854775808.0 || *d < -9223372036854775808.0)
        return fallback;
    return static_cast<juce::int64> (*d);
}

} // namespace sedef::plugin
