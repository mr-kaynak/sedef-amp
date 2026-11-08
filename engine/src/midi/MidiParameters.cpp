// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/midi/MidiParameters.h"

#include <cmath>

namespace sedef::midi
{

std::optional<ParamId> findParam (std::string_view id) noexcept
{
    for (std::size_t i = 0; i < kNumParams; ++i)
        if (kParamSpecs[i].id == id)
            return static_cast<ParamId> (i);
    return std::nullopt;
}

Values defaultValues() noexcept
{
    Values v {};
    for (std::size_t i = 0; i < kNumParams; ++i)
        v[i] = kParamSpecs[i].defaultValue;
    return v;
}

namespace
{
float value (const Values& v, ParamId id) noexcept
{
    const float x = v[index (id)];
    return spec (id).sanitise (std::isfinite (x) ? x : spec (id).defaultValue);
}

int whole (const Values& v, ParamId id) noexcept
{
    return static_cast<int> (std::lround (value (v, id)));
}
} // namespace

Settings settingsFrom (const Values& v) noexcept
{
    Settings s;
    s.octave = whole (v, ParamId::octave) - 2; // choices -2, -1, 0, +1
    s.mode = whole (v, ParamId::mode) == 1 ? Mode::notesAndBend : Mode::notes;
    s.bendRange = whole (v, ParamId::bendRange);
    s.timing = whole (v, ParamId::timing) == 1 ? Timing::steady : Timing::fastest;
    s.channel = whole (v, ParamId::channel);
    s.gateDb = value (v, ParamId::gate);
    s.dynamics = value (v, ParamId::dynamics) / 100.0f;
    const int curve = whole (v, ParamId::curve);
    s.curve = curve == 0 ? Curve::soft : (curve == 2 ? Curve::hard : Curve::linear);
    return s;
}

bool audioThrough (const Values& v) noexcept
{
    return whole (v, ParamId::audio) == 0;
}

} // namespace sedef::midi
