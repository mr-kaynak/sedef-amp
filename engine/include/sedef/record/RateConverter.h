// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/record/ReampFeeder.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace sedef::record
{

/// Converts a whole source to another sample rate before re-amping it (docs/recorder.md,
/// section 6): band-limited interpolation (J. O. Smith, "Digital Audio Resampling") with a
/// Kaiser-windowed sinc whose cut-off sits at kPassband times the lower Nyquist frequency.
/// Rates are whole hertz, so output frame k sits exactly at source time k * from / to; the
/// filter is linear phase and centred there, so nothing is delayed. Frames outside the source
/// are silence. Not real time (the reader thread).
class RateConverter
{
public:
    static constexpr int kZeroCrossings = 32;
    static constexpr double kKaiserBeta = 10.0;
    static constexpr double kPassband = 0.9;

    using Write = std::function<bool (const float* const* planar, int frames)>;
    using Progress = std::function<bool (double fraction)>;

    /// Output frames for `inputLength` source frames: those whose time falls inside the source.
    static std::int64_t outputLength (std::int64_t inputLength, double fromRate, double toRate);

    /// Converts all of `source` to `toRate` and hands the frames to `write` in blocks.
    /// `progress` (0..1) may return false to cancel. False on a read or write error or a cancel.
    static bool convert (FrameSource& source, double toRate, const Write& write,
                         const Progress& progress = {});

    /// The filter's weights for an output whose source time is i0 + frac (0 <= frac < 1), for
    /// source frames i0 - taps / 2 + 1 .. i0 + taps / 2, summing to 1. (The tests check the
    /// filter's response through this.)
    static void weights (double fromRate, double toRate, double frac, std::vector<double>& out);
    /// Taps per output frame (even).
    static int taps (double fromRate, double toRate);
};

} // namespace sedef::record
