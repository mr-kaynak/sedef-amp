// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace sedef::dsp::capture
{

/// The reference DI the built-in amps are level-matched with (docs/dsp/amps.md, "Gain staging"):
/// an open E-major chord strummed once per second (alternating down and up strokes), each string a
/// sum of decaying partials shaped by the pluck position, normalised to -18 dBFS RMS; strum peaks
/// land around -6 dBFS. The same signal as test::guitarStrum, so captures are measured exactly as
/// the amps are. Non-real-time (allocates).
std::vector<float> referenceStrum (double sampleRate, double seconds, std::uint32_t seed = 7);

/// Seconds of the strum the capture level is measured on, and the start it is measured from.
inline constexpr double kReferenceSeconds = 6.0;
inline constexpr double kReferenceSkipSeconds = 0.5;

} // namespace sedef::dsp::capture
