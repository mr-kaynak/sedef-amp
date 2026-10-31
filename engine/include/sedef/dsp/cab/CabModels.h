// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Biquad.h"

#include <array>
#include <complex>
#include <cstdint>

namespace sedef::dsp::cab
{

/// One second-order section with its own state (transposed direct form II, mono).
struct Stage
{
    BiquadCoeffs c;
    float s1 = 0.0f, s2 = 0.0f;
    BiquadCoeffs target; ///< reached by the end of the next block when `ramp` is set
    bool ramp = false;

    void reset() noexcept { s1 = s2 = 0.0f; }

    /// Glide to new coefficients over the next block instead of jumping (no zipper noise).
    void rampTo (const BiquadCoeffs& next) noexcept
    {
        target = next;
        ramp = true;
    }
};

namespace detail
{
inline void runStage (Stage& st, float* buf, int n) noexcept
{
    float s1 = st.s1, s2 = st.s2;
    if (!st.ramp)
    {
        const BiquadCoeffs c = st.c;
        for (int i = 0; i < n; ++i)
        {
            const float x = buf[i];
            const float y = c.b0 * x + s1;
            s1 = c.b1 * x - c.a1 * y + s2;
            s2 = c.b2 * x - c.a2 * y;
            buf[i] = y;
        }
    }
    else
    {
        // Linear per-sample coefficient interpolation, landing exactly on the target.
        BiquadCoeffs c = st.c;
        const float inv = 1.0f / static_cast<float> (n);
        const BiquadCoeffs d { (st.target.b0 - c.b0) * inv, (st.target.b1 - c.b1) * inv,
                               (st.target.b2 - c.b2) * inv, (st.target.a1 - c.a1) * inv,
                               (st.target.a2 - c.a2) * inv };
        for (int i = 0; i < n; ++i)
        {
            c.b0 += d.b0;
            c.b1 += d.b1;
            c.b2 += d.b2;
            c.a1 += d.a1;
            c.a2 += d.a2;
            const float x = buf[i];
            const float y = c.b0 * x + s1;
            s1 = c.b1 * x - c.a1 * y + s2;
            s2 = c.b2 * x - c.a2 * y;
            buf[i] = y;
        }
        st.c = st.target;
        st.ramp = false;
    }
    st.s1 = s1;
    st.s2 = s2;
}

/// Four static stages in one pass. Each stage's recursion is latency-bound; interleaving four
/// lets the CPU overlap them (stage k at sample n+1 runs alongside stage k+1 at sample n).
inline void runFourStages (Stage* st, float* buf, int n) noexcept
{
    const BiquadCoeffs c0 = st[0].c, c1 = st[1].c, c2 = st[2].c, c3 = st[3].c;
    float a1 = st[0].s1, a2 = st[0].s2, b1 = st[1].s1, b2 = st[1].s2;
    float d1 = st[2].s1, d2 = st[2].s2, e1 = st[3].s1, e2 = st[3].s2;
    for (int i = 0; i < n; ++i)
    {
        const float x0 = buf[i];
        const float y0 = c0.b0 * x0 + a1;
        a1 = c0.b1 * x0 - c0.a1 * y0 + a2;
        a2 = c0.b2 * x0 - c0.a2 * y0;
        const float y1 = c1.b0 * y0 + b1;
        b1 = c1.b1 * y0 - c1.a1 * y1 + b2;
        b2 = c1.b2 * y0 - c1.a2 * y1;
        const float y2 = c2.b0 * y1 + d1;
        d1 = c2.b1 * y1 - c2.a1 * y2 + d2;
        d2 = c2.b2 * y1 - c2.a2 * y2;
        const float y3 = c3.b0 * y2 + e1;
        e1 = c3.b1 * y2 - c3.a1 * y3 + e2;
        e2 = c3.b2 * y2 - c3.a2 * y3;
        buf[i] = y3;
    }
    st[0].s1 = a1;
    st[0].s2 = a2;
    st[1].s1 = b1;
    st[1].s2 = b2;
    st[2].s1 = d1;
    st[2].s2 = d2;
    st[3].s1 = e1;
    st[3].s2 = e2;
}
} // namespace detail

/// Runs `count` cascaded stages over buf[0..n) in place. Ramping stages interpolate their
/// coefficients per sample (see Stage::rampTo).
inline void runStages (Stage* stages, int count, float* buf, int n) noexcept
{
    int s = 0;
    while (s < count)
    {
        if (s + 4 <= count &&
            !(stages[s].ramp || stages[s + 1].ramp || stages[s + 2].ramp || stages[s + 3].ramp))
        {
            detail::runFourStages (stages + s, buf, n);
            s += 4;
        }
        else
        {
            detail::runStage (stages[s], buf, n);
            ++s;
        }
    }
}

enum class FilterKind : std::uint8_t
{
    Peak,
    LowShelf,
    HighShelf,
    LowPass,
    HighPass
};

struct FilterSpec
{
    FilterKind kind = FilterKind::Peak;
    float hz = 1000.0f;
    float q = 0.7071f;
    float gainDb = 0.0f;
};

BiquadCoeffs designFilter (const FilterSpec& f, double sampleRate) noexcept;

inline constexpr int kMaxBodyStages = 10;
inline constexpr int kMaxMicStages = 4;
inline constexpr int kPositionStages = 3; // presence peak, edge roll-off, edge warmth
inline constexpr int kDistanceStages = 2; // proximity shelf, air shelf
inline constexpr int kMaxStages =
    kMaxBodyStages + kPositionStages + kMaxMicStages + kDistanceStages;

/// Speaker + cabinet: the response at the reference mic position, then what moving the mic across
/// the cone changes.
struct CabModel
{
    std::array<FilterSpec, kMaxBodyStages> body {};
    int numBody = 0;

    /// Short reflection added to the speaker output: y = x + gain * lowpass (x delayed).
    /// Open backs use a negative gain (the rear wave diffracting round the baffle cancels lows);
    /// the closed back uses a small positive one (internal box reflection through the cone).
    float reflectionMs = 0.0f;
    float reflectionGain = 0.0f;
    float reflectionLpHz = 1000.0f;

    float presenceHz = 4500.0f; ///< dust-cap presence peak, full at the centre, gone at the edge
    float presenceQ = 1.0f;
    float presenceDb = 4.0f;
    float edgeLpHz = 3000.0f; ///< second-order roll-off corner reached at the cone edge
    float edgeLpQ = 0.6f;
    float edgeWarmthHz = 300.0f; ///< low-mid lift that grows towards the edge
    float edgeWarmthDb = 1.5f;

    float speakerHeightM = 0.3f; ///< speaker centre above the floor (floor bounce geometry)
};

struct MicModel
{
    std::array<FilterSpec, kMaxMicStages> eq {};
    int numEq = 0;
    float proximityDb = 0.0f; ///< bass boost on the grille (0 for an omni)
    float proximityHz = 160.0f;
};

const CabModel& cabModel (int cab) noexcept;
const MicModel& micModel (int type) noexcept;

/// What the distance knob means physically.
struct Placement
{
    float arrivalSec = 0.0f; ///< extra time of flight relative to the grille
    float bounceSec = 0.0f;  ///< floor bounce arrival, same reference
    float bounceGain = 0.0f; ///< floor bounce level relative to the direct sound
    float levelGain = 1.0f;  ///< small level falloff
    float proximityDb = 0.0f;
    float airDb = 0.0f;
};

Placement placement (const CabModel& cab, const MicModel& mic, float distance) noexcept;

inline constexpr float kBounceLpHz = 1800.0f;
inline constexpr float kAirShelfHz = 7000.0f;

/// Stage builders. Each writes its group's coefficients (state untouched).
int designBodyStages (const CabModel& cab, double sampleRate, Stage* out) noexcept;
void designPositionStages (const CabModel& cab, float position, double sampleRate,
                           Stage* out) noexcept;
int designMicStages (const MicModel& mic, double sampleRate, Stage* out) noexcept;
void designDistanceStages (const MicModel& mic, const Placement& p, double sampleRate,
                           Stage* out) noexcept;

/// One-pole low-pass coefficient: y += a (x - y).
float onePoleCoeff (double hz, double sampleRate) noexcept;

/// Complex response of a procedural voice before level normalisation. With bodyOnly, only the
/// speaker + cabinet part (what the room mic hears) is evaluated.
std::complex<double> voiceResponse (int cab, int type, float position, float distance,
                                    double sampleRate, double hz, bool bodyOnly = false) noexcept;

/// Gains that bring every procedural (cab, mic) voice, measured at position 0.5 and distance 0, to
/// a loudness index of 0 dB (see loudnessIndexDb). Non-real-time.
struct Calibration
{
    std::array<std::array<float, 10>, 3> voice {};
    std::array<float, 3> body {};
};
Calibration calibrate (double sampleRate);

} // namespace sedef::dsp::cab
