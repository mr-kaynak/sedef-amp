// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/params/Parameters.h"

#include <algorithm>
#include <cmath>

namespace sedef
{

float ParamSpec::toNormalised (float value) const noexcept
{
    const float proportion = std::clamp ((sanitise (value) - min) / (max - min), 0.0f, 1.0f);
    return skew == 1.0f ? proportion : std::pow (proportion, skew);
}

float ParamSpec::fromNormalised (float norm) const noexcept
{
    norm = std::clamp (norm, 0.0f, 1.0f);
    const float proportion =
        (skew == 1.0f || norm <= 0.0f) ? norm : std::exp (std::log (norm) / skew);
    return sanitise (min + (max - min) * proportion);
}

float ParamSpec::sanitise (float value) const noexcept
{
    if (!std::isfinite (value))
        return defaultValue;
    value = std::clamp (value, min, max);
    switch (kind)
    {
    case ParamKind::Float:
        return value;
    case ParamKind::Int:
    case ParamKind::Choice:
        return std::round (value);
    case ParamKind::Bool:
        return value >= 0.5f ? 1.0f : 0.0f;
    }
    return value;
}

int ParamSpec::numChoices() const noexcept
{
    if (kind != ParamKind::Choice || choices.empty())
        return 0;
    return static_cast<int> (std::count (choices.begin(), choices.end(), '|')) + 1;
}

std::string_view ParamSpec::choiceLabel (int idx) const noexcept
{
    std::string_view rest = choices;
    for (int i = 0; i < idx; ++i)
    {
        const auto bar = rest.find ('|');
        if (bar == std::string_view::npos)
            return {};
        rest.remove_prefix (bar + 1);
    }
    return rest.substr (0, rest.find ('|'));
}

std::optional<ParamId> findParam (std::string_view id) noexcept
{
    for (std::size_t i = 0; i < kNumParams; ++i)
        if (kParamSpecs[i].id == id)
            return static_cast<ParamId> (i);
    return std::nullopt;
}

} // namespace sedef
