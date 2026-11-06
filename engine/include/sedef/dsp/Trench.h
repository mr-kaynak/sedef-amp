// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/analysis/MonoTracker.h"
#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/detail/PolySub.h"

#include <array>
#include <cmath>

namespace sedef::dsp
{

/// "Trench" octaver, in two modes.
///
/// - **Mono** is an analog-style divider: a note tracker (analysis::MonoTracker) finds the note
///   and isolates its fundamental, and the fundamental's phase, halved and quartered, drives two
///   square waves an octave and two octaves down, shaped by the input's envelope. Band-limited
///   edges (polyBLEP) keep them alias-free without oversampling. When there is no note to follow
///   (silence, a chord), the subs fade out instead of sputtering.
/// - **Poly** divides every partial of the input, so chords work (detail::PolySub): the subs are
///   the input's own spectrum, an octave and two octaves down.
///
/// The direct path has no latency in either mode. Switching modes crossfades over 20 ms. See
/// docs/dsp/trench.md.
class Trench
{
public:
    enum Mode
    {
        Mono = 0,
        Poly = 1,
    };

    struct Settings
    {
        float direct = 0.5f; ///< 0..1, dry level, unity at 0.5
        float sub1 = 0.5f;   ///< 0..1, one octave down, about as loud as the input at 0.5
        float sub2 = 0.0f;   ///< 0..1, two octaves down
        float tone = 0.5f;   ///< 0..1, low-pass on the sub voices, dark to bright
        int mode = Mono;
    };

    /// What the tracker on channel 0 currently believes, for tests.
    struct TrackState
    {
        float hz = 0.0f;         ///< the note it is dividing, 0 if none
        float confidence = 0.0f; ///< 0..1: how periodic the input is (the estimator's clarity)
        float subGain = 0.0f;    ///< 0..1: the gate on the sub voices (Mono)
        int partials = 0;        ///< partials being divided (Poly)
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept { return 0; }

    TrackState trackState() const noexcept;

private:
    /// One biquad section with its own coefficients and state.
    struct Section
    {
        BiquadCoeffs c;
        float s0 = 0.0f, s1 = 0.0f;

        float process (float x) noexcept
        {
            const float y = c.b0 * x + s0;
            s0 = c.b1 * x - c.a1 * y + s1;
            s1 = c.b2 * x - c.a2 * y;
            return y;
        }

        void clear() noexcept { s0 = s1 = 0.0f; }

        void flush() noexcept
        {
            s0 = std::abs (s0) < 1.0e-20f ? 0.0f : s0;
            s1 = std::abs (s1) < 1.0e-20f ? 0.0f : s1;
        }
    };

    struct Voice
    {
        float value = 1.0f;   ///< flip-flop output, +-1
        float pending = 1.0f; ///< the previous sample, with its band-limiting correction
    };

    struct Tracker
    {
        analysis::MonoTracker mono;
        detail::PolySub poly;
        // Level detector.
        Section hp, lp;
        float level = 0.0f; ///< mean |x| of the band-limited input
        // Divider: the fundamental's phase, halved, as a unit vector.
        float halfRe = 1.0f, halfIm = 0.0f;
        float previousHalf = 1.0f;
        // Output.
        Voice sub1, sub2;
        float gate = 0.0f;     ///< smoothed sub gain (Mono)
        float polyGate = 0.0f; ///< smoothed level gate (Poly)
        bool levelOpen = false;
        float pendingAmp = 0.0f;
        Section tone, dc;
    };

    void applySettings (bool snap) noexcept;
    void updateTone (float tone) noexcept;
    static void flush (Tracker& t) noexcept;
    static void resetMono (Tracker& t) noexcept;
    void updateLevel (Tracker& t, float x) noexcept;
    float monoSample (Tracker& t, float x, float g1, float g2) noexcept;

    double sampleRate_ = 48000.0;
    int numChannels_ = 0;
    Settings settings_;
    bool primed_ = false;

    std::array<Tracker, kMaxChannels> trackers_ {};

    float levelCoeff_ = 0.0f;
    float gateAttack_ = 0.0f, gateRelease_ = 0.0f;
    BiquadCoeffs toneCoeffs_;

    LinearSmoothed direct_, sub1_, sub2_;
    ExpSmoothed tone_;
    float poly_ = 0.0f;     ///< 0 = Mono .. 1 = Poly, ramps on a mode change
    float polyStep_ = 0.0f; ///< per sample
};

} // namespace sedef::dsp
