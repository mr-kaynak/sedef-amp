// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Math.h"

#include <cmath>

/// Memoryless nonlinearities shared by drives and amps. Always run them oversampled.
namespace sedef::shape
{

/// Symmetric soft clip.
inline float tanhClip (float x) noexcept
{
    return std::tanh (x);
}

/// Asymmetric soft clip with zero output at zero input. A positive bias pushes the positive half
/// into saturation first, producing even harmonics like a single-ended triode stage.
inline float asymmetric (float x, float bias) noexcept
{
    return std::tanh (x + bias) - std::tanh (bias);
}

/// Smooth diode-pair style clipper: linear near zero, logarithmic-ish compression above.
inline float diodePair (float x) noexcept
{
    return std::asinh (x) * 0.6f;
}

/// Hard clip at +-1 with a quadratic soft knee of half-width `knee` (C1 continuous).
inline float softKneeClip (float x, float knee = 0.2f) noexcept
{
    const float ax = std::abs (x);
    if (ax <= 1.0f - knee)
        return x;
    if (ax >= 1.0f + knee)
        return std::copysign (1.0f, x);
    const float d = ax - (1.0f - knee);
    return std::copysign (ax - d * d / (4.0f * knee), x);
}

/// Triode-like transfer with unity slope at zero: soft, wide positive swing (grid conduction
/// compresses gently) and an earlier, firmer negative limit (cutoff). Produces even harmonics.
inline float triode (float x) noexcept
{
    if (x >= 0.0f)
        return 0.92f * x / (1.0f + 0.35f * x) + 0.08f * std::tanh (x);
    return std::tanh (1.2f * x) / 1.2f;
}

} // namespace sedef::shape
