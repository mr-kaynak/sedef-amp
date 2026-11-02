// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Biquad.h"

#include <algorithm>
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

/// N static stages in one pass. Each stage's recursion is latency-bound; interleaving them lets
/// the CPU overlap them (stage k at sample n+1 runs alongside stage k+1 at sample n).
template <int N>
inline void runFused (Stage* st, float* buf, int n) noexcept
{
    BiquadCoeffs c[N];
    float s1[N], s2[N];
    for (int k = 0; k < N; ++k)
    {
        c[k] = st[k].c;
        s1[k] = st[k].s1;
        s2[k] = st[k].s2;
    }
    for (int i = 0; i < n; ++i)
    {
        float x = buf[i];
        for (int k = 0; k < N; ++k)
        {
            const float y = c[k].b0 * x + s1[k];
            s1[k] = c[k].b1 * x - c[k].a1 * y + s2[k];
            s2[k] = c[k].b2 * x - c[k].a2 * y;
            x = y;
        }
        buf[i] = x;
    }
    for (int k = 0; k < N; ++k)
    {
        st[k].s1 = s1[k];
        st[k].s2 = s2[k];
    }
}
} // namespace detail

/// Runs `count` cascaded stages over buf[0..n) in place. Ramping stages interpolate their
/// coefficients per sample (see Stage::rampTo); static ones run fused in groups of up to four.
inline void runStages (Stage* stages, int count, float* buf, int n) noexcept
{
    int s = 0;
    while (s < count)
    {
        int run = 0;
        while (s + run < count && !stages[s + run].ramp)
            ++run;
        // Five static stages split 3 + 2 rather than leaving a lone one.
        const int group = run == 5 ? 3 : std::min (run, 4);
        switch (group)
        {
        case 0:
        case 1:
            detail::runStage (stages[s], buf, n);
            s += 1;
            break;
        case 2:
            detail::runFused<2> (stages + s, buf, n);
            s += 2;
            break;
        case 3:
            detail::runFused<3> (stages + s, buf, n);
            s += 3;
            break;
        default:
            detail::runFused<4> (stages + s, buf, n);
            s += 4;
            break;
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
/// Position, then proximity and air: what only the miked cone's sound gets.
inline constexpr int kNearStages = kPositionStages + 2;
inline constexpr int kOffAxisStages = 2; // presence fade, off-axis treble shelf
inline constexpr int kMaxStages = kMaxBodyStages + kNearStages + kMaxMicStages + kOffAxisStages;
inline constexpr int kMaxNeighbours = 3; // the other speakers of a 4x12

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

    /// Centres of the other speakers on the baffle, relative to the miked one, in metres (front
    /// view: x to the right, y up). The miked speaker is the left one of the 2x12 and the
    /// bottom-left one of the 4x12.
    std::array<std::array<float, 2>, kMaxNeighbours> neighbours {};
    int numNeighbours = 0;
};

struct MicModel
{
    std::array<FilterSpec, kMaxMicStages> eq {};
    int numEq = 0;
    float proximityDb = 0.0f; ///< bass boost on the grille (0 for an omni)
    float proximityHz = 160.0f;

    /// Polar pattern A + (1 - A) cos(angle): 1 omni, 0.5 cardioid, 0.37 supercardioid,
    /// 0 figure-of-eight (whose rear lobe is inverted).
    float polar = 0.5f;
    /// Off axis: the on-axis presence peak fades and a treble shelf grows. Both are given at
    /// 60 degrees and scale with sin^2 of the angle (see offAxisAmount).
    float presenceHz = 5000.0f;
    float presenceQ = 1.0f;
    float presenceDb = 0.0f;
    float offAxisHz = 4000.0f;
    float offAxisDb = 0.0f;
};

const CabModel& cabModel (int cab) noexcept;
const MicModel& micModel (int type) noexcept;

/// Where a capsule sits and how it is aimed. Front view of the miked speaker in cone radii: x to
/// the right, y up, so hypot (x, y) is the position knob (0 dust cap, 1 cone edge).
struct MicPose
{
    float x = 0.0f;
    float y = 0.0f;
    float distance = 0.0f;   ///< 0 on the grille .. 1 far
    float offAxisDeg = 0.0f; ///< capsule tilt towards the dust cap, 0..60 degrees

    float position() const noexcept;
    /// azimuthDeg runs clockwise from 12 o'clock as you face the cab.
    static MicPose polar (float position, float azimuthDeg, float distance,
                          float offAxisDeg) noexcept;
    bool operator== (const MicPose&) const = default;
};

inline constexpr float kMaxOffAxisDeg = 60.0f;
inline constexpr float kConeRadiusM = 0.12f; ///< effective piston radius; position 1 sits here

/// One of the other speakers as the capsule hears it, relative to the direct sound.
struct Bleed
{
    float delaySec = 0.0f;     ///< extra path over the direct sound
    float gain = 0.0f;         ///< level relative to the direct sound (signed: polar rear lobe)
    float lowpassHz = 1000.0f; ///< the neighbour seen off its axis beams its treble away
};

/// What the pose means physically.
struct Placement
{
    float arrivalSec = 0.0f; ///< extra time of flight relative to the grille
    float bounceSec = 0.0f;  ///< floor bounce arrival, same reference
    float bounceGain = 0.0f; ///< floor bounce level relative to the direct sound
    float levelGain = 1.0f;  ///< small level falloff with distance, times the polar gain off axis
    float proximityDb = 0.0f;
    float airDb = 0.0f;
    float presenceCutDb = 0.0f; ///< off axis: the capsule's presence peak fading
    float offAxisShelfDb = 0.0f;
    std::array<Bleed, kMaxNeighbours> bleed {};
    int numBleed = 0;
};

Placement placement (const CabModel& cab, const MicModel& mic, const MicPose& pose) noexcept;

/// 0 on axis, 1 at 60 degrees: sin^2 of the angle, like the first-order loss of a receiving
/// piston (2 J1(x) / x ~ 1 - x^2 / 8 with x proportional to sin angle).
float offAxisAmount (float offAxisDeg) noexcept;
/// Polar gain A + (1 - A) cos(angle) of a mic at that angle.
float polarGain (const MicModel& mic, float angleDeg) noexcept;

inline constexpr float kBounceLpHz = 1800.0f;
inline constexpr float kAirShelfHz = 7000.0f;

/// Stage builders. Each writes its group's coefficients (state untouched).
int designBodyStages (const CabModel& cab, double sampleRate, Stage* out) noexcept;
void designPositionStages (const CabModel& cab, float position, double sampleRate,
                           Stage* out) noexcept;
void designProximityStage (const MicModel& mic, const Placement& p, double sampleRate,
                           Stage* out) noexcept;
int designMicStages (const MicModel& mic, double sampleRate, Stage* out) noexcept;
void designOffAxisStages (const MicModel& mic, const Placement& p, double sampleRate,
                          Stage* out) noexcept;
void designAirStage (const Placement& p, double sampleRate, Stage* out) noexcept;
BiquadCoeffs designBleedFilter (const Bleed& b, double sampleRate) noexcept;

/// The capsule a custom IR was presumably recorded with: its off-axis model is what the angle
/// control applies after an IR (the IR itself already holds position and distance).
inline constexpr int kIrCapsuleType = 0;

/// One-pole low-pass coefficient: y += a (x - y).
float onePoleCoeff (double hz, double sampleRate) noexcept;

/// Complex response of a procedural voice before level normalisation. With bodyOnly, only the
/// speaker + cabinet part (what the room mic hears) is evaluated.
std::complex<double> voiceResponse (int cab, int type, const MicPose& pose, double sampleRate,
                                    double hz, bool bodyOnly = false) noexcept;

/// The pose every procedural voice is normalised at: position 0.5 at mic A's default 2 o'clock,
/// on the grille, on axis.
MicPose referencePose() noexcept;

/// Gains that bring every procedural (cab, mic) voice, measured at the reference pose, to a
/// loudness index of 0 dB (see loudnessIndexDb). Non-real-time.
struct Calibration
{
    std::array<std::array<float, 10>, 3> voice {};
    std::array<float, 3> body {};
};
Calibration calibrate (double sampleRate);

} // namespace sedef::dsp::cab
