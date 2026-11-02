// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/cab/CabModels.h"

#include "sedef/core/Math.h"
#include "sedef/dsp/cab/ConvolutionKernel.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp::cab
{
namespace
{
constexpr FilterSpec peak (float hz, float q, float db)
{
    return { FilterKind::Peak, hz, q, db };
}
constexpr FilterSpec lowShelf (float hz, float q, float db)
{
    return { FilterKind::LowShelf, hz, q, db };
}
constexpr FilterSpec highShelf (float hz, float q, float db)
{
    return { FilterKind::HighShelf, hz, q, db };
}
constexpr FilterSpec lowPass (float hz, float q)
{
    return { FilterKind::LowPass, hz, q, 0.0f };
}
constexpr FilterSpec highPass (float hz, float q)
{
    return { FilterKind::HighPass, hz, q, 0.0f };
}

// --- Cabinets ----------------------------------------------------------------------------------
// Values are our own voicing targets, shaped after the general behaviour described in loudspeaker
// texts (closed-box Q, dipole cancellation of open baffles, cone break-up above ~2 kHz).

constexpr CabModel makeOpen1x12()
{
    CabModel m;
    m.body = {
        highPass (70.0f, 0.75f),     // speaker low-frequency limit
        peak (96.0f, 2.0f, 5.0f),    // loose, lightly damped resonance
        peak (400.0f, 1.0f, -2.0f),  // lean low-mids of a small open box
        peak (2000.0f, 0.9f, 3.5f),  // cone presence region
        peak (3600.0f, 3.0f, 3.0f),  // break-up peak
        peak (4400.0f, 4.5f, -5.0f), // break-up dip
        lowPass (6800.0f, 0.8f),     // gentle top: "airy"
        lowPass (8500.0f, 0.6f),
    };
    m.numBody = 8;
    m.reflectionMs = 0.85f; // rear wave round a small baffle
    m.reflectionGain = -0.6f;
    m.reflectionLpHz = 220.0f;
    m.presenceHz = 4800.0f;
    m.presenceQ = 1.0f;
    m.presenceDb = 4.0f;
    m.edgeLpHz = 3400.0f;
    m.edgeWarmthHz = 320.0f;
    m.edgeWarmthDb = 1.5f;
    m.speakerHeightM = 0.42f;
    return m;
}

constexpr CabModel makeOpen2x12()
{
    CabModel m;
    m.body = {
        highPass (62.0f, 0.8f),      peak (86.0f, 2.2f, 5.0f), // resonance
        peak (220.0f, 0.8f, 1.0f),                             // second cone adds body
        peak (650.0f, 1.2f, -1.5f),  peak (2400.0f, 1.3f, 4.0f),
        peak (3200.0f, 3.5f, 3.0f),  // break-up peak
        peak (4000.0f, 5.0f, -6.0f), // break-up dip
        lowPass (6300.0f, 0.85f),    lowPass (7800.0f, 0.6f),
    };
    m.numBody = 9;
    m.reflectionMs = 1.2f; // wider baffle: longer path, less cancellation
    m.reflectionGain = -0.4f;
    m.reflectionLpHz = 240.0f;
    m.presenceHz = 4500.0f;
    m.presenceQ = 1.1f;
    m.presenceDb = 4.5f;
    m.edgeLpHz = 3200.0f;
    m.edgeWarmthHz = 280.0f;
    m.edgeWarmthDb = 1.5f;
    m.speakerHeightM = 0.36f;
    m.neighbours = { { { 0.34f, 0.0f } } }; // the right-hand speaker
    m.numNeighbours = 1;
    return m;
}

constexpr CabModel makeClosed4x12()
{
    CabModel m;
    m.body = {
        highPass (70.0f, 1.0f),      // closed-box alignment, Qtc ~1
        peak (84.0f, 2.0f, 2.5f),    // tight resonance
        peak (150.0f, 1.0f, 1.5f),   // thump
        peak (500.0f, 1.0f, -2.5f),  // low-mid scoop
        peak (2500.0f, 2.0f, 6.0f),  // break-up peak
        peak (3300.0f, 6.0f, -9.0f), // break-up notch
        peak (4100.0f, 4.0f, 4.5f),  // second break-up peak
        lowPass (5200.0f, 0.9f),     // steep cone roll-off (36 dB/oct)
        lowPass (5800.0f, 0.6f),     lowPass (9000.0f, 0.7f),
    };
    m.numBody = 10;
    m.reflectionMs = 1.6f; // internal reflection off the sealed back
    m.reflectionGain = 0.12f;
    m.reflectionLpHz = 900.0f;
    m.presenceHz = 4200.0f;
    m.presenceQ = 1.3f;
    m.presenceDb = 4.5f;
    m.edgeLpHz = 3000.0f;
    m.edgeWarmthHz = 300.0f;
    m.edgeWarmthDb = 2.0f;
    m.speakerHeightM = 0.26f; // the bottom-left speaker
    m.neighbours = { { { 0.33f, 0.0f }, { 0.0f, 0.32f }, { 0.33f, 0.32f } } };
    m.numNeighbours = 3;
    return m;
}

constexpr std::array<CabModel, 3> kCabModels { makeOpen1x12(), makeOpen2x12(), makeClosed4x12() };

// --- Microphones -------------------------------------------------------------------------------
// Generic descriptions of common studio microphone families (directional dynamic, condenser,
// ribbon). Proximity depends on directivity: none for an omni, most for a figure-of-eight.

constexpr MicModel mic (std::array<FilterSpec, kMaxMicStages> eq, int numEq, float proxDb,
                        float proxHz)
{
    MicModel m;
    m.eq = eq;
    m.numEq = numEq;
    m.proximityDb = proxDb;
    m.proximityHz = proxHz;
    return m;
}

/// Off-axis behaviour: polar pattern, the presence peak that fades (Hz, Q, dB on axis) and the
/// treble shelf reached at 60 degrees. Larger diaphragms beam more, so they lose more treble.
constexpr MicModel offAxis (MicModel m, float polar, FilterSpec presence, float shelfHz,
                            float shelfDb)
{
    m.polar = polar;
    m.presenceHz = presence.hz;
    m.presenceQ = presence.q;
    m.presenceDb = presence.gainDb;
    m.offAxisHz = shelfHz;
    m.offAxisDb = shelfDb;
    return m;
}

constexpr std::array<MicModel, 10> kMicModels {
    // 0 Dyn Classic: cardioid dynamic, presence peak ~5 kHz, bass roll-off that proximity offsets.
    // At 45 degrees its off-axis model lands on voicing 1 (presence 4.5 -> 1.5 dB, -5 dB shelf).
    offAxis (mic ({ highPass (115.0f, 0.6f), peak (5300.0f, 1.1f, 4.5f),
                    peak (9500.0f, 2.0f, -2.0f), lowPass (14000.0f, 0.7f) },
                  4, 8.0f, 160.0f),
             0.5f, peak (5300.0f, 1.1f, 4.5f), 3500.0f, -7.5f),
    // 1 Dyn Classic Off-Axis: same capsule at 45 degrees: less presence, darker top. Turning it
    // further fades the rest of its presence.
    offAxis (mic ({ highPass (115.0f, 0.6f), peak (4800.0f, 1.0f, 1.5f),
                    highShelf (3500.0f, 0.6f, -5.0f), lowPass (11000.0f, 0.7f) },
                  4, 7.0f, 160.0f),
             0.5f, peak (4800.0f, 1.0f, 1.5f), 3500.0f, -5.0f),
    // 2 Dyn Bright: supercardioid dynamic, lean low-mids, wide bright presence.
    offAxis (mic ({ highPass (90.0f, 0.65f), peak (500.0f, 0.8f, -1.5f), peak (6500.0f, 0.9f, 5.5f),
                    highShelf (10000.0f, 0.7f, 1.5f) },
                  4, 7.5f, 150.0f),
             0.37f, peak (6500.0f, 0.9f, 5.5f), 4000.0f, -7.0f),
    // 3 Dyn Broadcast: large-diaphragm dynamic built to limit proximity; flat, extended lows.
    offAxis (mic ({ highPass (45.0f, 0.7f), lowShelf (150.0f, 0.7f, 1.0f),
                    peak (3500.0f, 0.7f, 1.5f), lowPass (15000.0f, 0.7f) },
                  4, 3.5f, 130.0f),
             0.5f, peak (3500.0f, 0.7f, 1.5f), 2500.0f, -8.0f),
    // 4 Cond Small: small-diaphragm cardioid condenser, flat with a gentle top lift; the small
    // capsule keeps its polar pattern furthest up the band.
    offAxis (mic ({ highPass (40.0f, 0.7f), highShelf (8000.0f, 0.7f, 2.5f),
                    peak (12000.0f, 1.5f, 1.0f) },
                  3, 4.5f, 140.0f),
             0.5f, peak (12000.0f, 1.5f, 1.0f), 8000.0f, -3.0f),
    // 5 Cond Large: large-diaphragm cardioid, warmth plus a broad presence lift.
    offAxis (mic ({ highPass (35.0f, 0.7f), lowShelf (220.0f, 0.7f, 1.5f),
                    peak (6000.0f, 0.8f, 3.5f), highShelf (12000.0f, 0.7f, 1.0f) },
                  4, 5.5f, 150.0f),
             0.5f, peak (6000.0f, 0.8f, 3.5f), 4500.0f, -6.0f),
    // 6 Cond Precision: near-omni measurement-style capsule, flat. An omni turns directional only
    // in the top octave.
    offAxis (mic ({ highPass (25.0f, 0.7f) }, 1, 1.0f, 150.0f), 0.9f, peak (8000.0f, 1.0f, 0.0f),
             10000.0f, -4.0f),
    // 7 Cond Tube: large-diaphragm valve condenser, warm low-mids, smooth highs.
    offAxis (mic ({ highPass (35.0f, 0.7f), peak (180.0f, 0.8f, 2.0f), peak (3000.0f, 0.8f, 1.0f),
                    highShelf (7000.0f, 0.6f, -2.5f) },
                  4, 5.5f, 150.0f),
             0.5f, peak (3000.0f, 0.8f, 1.0f), 4500.0f, -6.0f),
    // 8 Ribbon Smooth: figure-of-eight ribbon, rolled off from ~9 kHz, big proximity. Its pattern
    // is cos(angle) at every frequency, with a little extra treble loss off axis.
    offAxis (mic ({ highPass (30.0f, 0.7f), peak (120.0f, 0.9f, 1.0f),
                    highShelf (4000.0f, 0.5f, -2.0f), lowPass (9000.0f, 0.6f) },
                  4, 9.0f, 150.0f),
             0.0f, peak (5000.0f, 1.0f, 0.0f), 5000.0f, -3.0f),
    // 9 Ribbon Dark: heavier ribbon voicing, darker still.
    offAxis (mic ({ highPass (30.0f, 0.7f), peak (250.0f, 0.8f, 1.5f),
                    highShelf (2500.0f, 0.5f, -4.0f), lowPass (6000.0f, 0.55f) },
                  4, 10.0f, 160.0f),
             0.0f, peak (4000.0f, 1.0f, 0.0f), 4000.0f, -3.0f),
};

constexpr float kGrilleM = 0.025f;       // capsule-to-cone distance at distance 0
constexpr float kDistanceSpanM = 0.325f; // distance 1 is ~35 cm (~1 ms further)
constexpr float kSpeedOfSound = 343.0f;
constexpr float kFloorLoss = 0.45f; // floor absorption and the speaker's off-axis loss
constexpr float kFalloffDb = -4.5f; // level change across the distance range
constexpr float kAirDb = -2.5f;
constexpr float kPistonCorner = 1.62f; // k a sin(phi) where 2 J1(x) / x is 3 dB down
// The directivity 2 J1(x) / x is real: it scales a neighbour's sound without delaying it. A
// second-order Bessel low-pass (Q = 1/sqrt 3, maximally flat delay) has the closest magnitude
// with a nearly constant group delay, which the path delay gives back.
constexpr float kBleedLpQ = 0.57735027f;
constexpr float kBesselCorner = 0.78615138f;  // its -3 dB point relative to the design frequency
constexpr float kBesselDelay = 0.21666156f;   // its group delay (s) times the -3 dB frequency (Hz)
constexpr float kMinBleedDelaySec = 25.0e-6f; // at least one sample at 44.1 kHz (Hermite reads)
constexpr float kCapRadiusM = 0.03f;          // inside the dust cap the capsule's lean fades out
constexpr float kDegToRad = kPi / 180.0f;

std::complex<double> stageResponse (const BiquadCoeffs& c, std::complex<double> z1) noexcept
{
    const std::complex<double> z2 = z1 * z1;
    return (static_cast<double> (c.b0) + static_cast<double> (c.b1) * z1 +
            static_cast<double> (c.b2) * z2) /
           (1.0 + static_cast<double> (c.a1) * z1 + static_cast<double> (c.a2) * z2);
}

std::complex<double> onePoleResponse (float a, std::complex<double> z1) noexcept
{
    return static_cast<double> (a) / (1.0 - (1.0 - static_cast<double> (a)) * z1);
}
} // namespace

const CabModel& cabModel (int cab) noexcept
{
    return kCabModels[static_cast<size_t> (std::clamp (cab, 0, 2))];
}

const MicModel& micModel (int type) noexcept
{
    return kMicModels[static_cast<size_t> (std::clamp (type, 0, 9))];
}

BiquadCoeffs designFilter (const FilterSpec& f, double fs) noexcept
{
    switch (f.kind)
    {
    case FilterKind::Peak:
        return design::peaking (fs, f.hz, f.q, f.gainDb);
    case FilterKind::LowShelf:
        return design::lowShelf (fs, f.hz, f.q, f.gainDb);
    case FilterKind::HighShelf:
        return design::highShelf (fs, f.hz, f.q, f.gainDb);
    case FilterKind::LowPass:
        return design::lowpass (fs, f.hz, f.q);
    case FilterKind::HighPass:
        return design::highpass (fs, f.hz, f.q);
    }
    return {};
}

float MicPose::position() const noexcept
{
    return std::min (1.0f, std::hypot (x, y));
}

MicPose MicPose::polar (float position, float azimuthDeg, float distance, float offAxisDeg) noexcept
{
    const float p = std::clamp (position, 0.0f, 1.0f);
    const float a = azimuthDeg * kDegToRad;
    MicPose m;
    m.x = p * std::sin (a);
    m.y = p * std::cos (a);
    m.distance = std::clamp (distance, 0.0f, 1.0f);
    m.offAxisDeg = std::clamp (offAxisDeg, 0.0f, kMaxOffAxisDeg);
    return m;
}

MicPose referencePose() noexcept
{
    return MicPose::polar (0.5f, 60.0f, 0.0f, 0.0f);
}

float offAxisAmount (float offAxisDeg) noexcept
{
    const float s = std::sin (std::clamp (offAxisDeg, 0.0f, kMaxOffAxisDeg) * kDegToRad);
    return s * s * (4.0f / 3.0f); // sin^2 (60 degrees) = 3/4
}

float polarGain (const MicModel& m, float angleDeg) noexcept
{
    return m.polar + (1.0f - m.polar) * std::cos (angleDeg * kDegToRad);
}

Placement placement (const CabModel& cab, const MicModel& m, const MicPose& pose) noexcept
{
    const float d = std::clamp (pose.distance, 0.0f, 1.0f);
    const float angle = std::clamp (pose.offAxisDeg, 0.0f, kMaxOffAxisDeg);
    const float z = kGrilleM + kDistanceSpanM * d; // capsule in front of the cone
    // The capsule over the baffle, in metres from the miked speaker's centre.
    const float radius = std::hypot (pose.x, pose.y);
    const float scale = kConeRadiusM / std::max (1.0f, radius);
    const float mx = pose.x * scale, my = pose.y * scale;

    Placement p;
    p.arrivalSec = (z - kGrilleM) / kSpeedOfSound;
    // Floor bounce: the image source sits as far below the floor as the speaker is above it, so
    // the reflected path climbs 2h + (the mic's height above the speaker centre).
    const float rise = 2.0f * cab.speakerHeightM + my;
    const float rb = std::sqrt (z * z + rise * rise);
    p.bounceSec = (rb - kGrilleM) / kSpeedOfSound;
    p.bounceGain = kFloorLoss * z / rb;

    // Off axis the capsule hears the cone through its polar pattern: a broadband loss, less
    // proximity bass (only the pressure-gradient part has it), and the treble model.
    const float polar = polarGain (m, angle);
    p.levelGain = dbToGain (kFalloffDb * d) * polar;
    // Perceptual taper: most of the boost goes over the first half of the travel and a little
    // remains at the far end (a steep physical 1/r law would spend it all in the first quarter).
    const float onAxisProximity = m.proximityDb * (0.1f + 0.9f * std::pow (1.0f - d, 1.6f));
    if (angle > 0.0f)
    {
        // Response A + (1 - A) cos(angle) G at low frequencies (G: on-axis proximity gain)
        // against A + (1 - A) cos(angle) in the mids.
        const float g = dbToGain (onAxisProximity);
        const float lows = m.polar + std::cos (angle * kDegToRad) * (g - m.polar);
        p.proximityDb = gainToDb (lows / polar);
    }
    else
    {
        p.proximityDb = onAxisProximity;
    }
    p.airDb = kAirDb * d;
    const float amount = offAxisAmount (angle);
    p.presenceCutDb = -m.presenceDb * amount;
    p.offAxisShelfDb = m.offAxisDb * amount;

    // The other speakers. Each is a baffled piston of radius a seen from the capsule: at low
    // frequencies its pressure is k a^2 / (2 R) against k (sqrt (z^2 + a^2) - z) on the axis of
    // the miked cone, it beams its treble away (2 J1(x) / x, x = k a sin(phi)), and the capsule
    // weighs its arrival with the polar pattern. The axis leans towards the dust cap by the
    // off-axis angle; directly over the cap the lean fades out.
    constexpr float a = kConeRadiusM;
    const float nearLf = std::sqrt (z * z + a * a) - z;
    const float lean = std::sin (angle * kDegToRad) / std::max (std::hypot (mx, my), kCapRadiusM);
    const float hx = -mx * lean, hy = -my * lean;
    const float hz = std::sqrt (std::max (0.0f, 1.0f - hx * hx - hy * hy));
    p.numBleed = cab.numNeighbours;
    for (int k = 0; k < cab.numNeighbours; ++k)
    {
        const auto& n = cab.neighbours[static_cast<size_t> (k)];
        const float dx = n[0] - mx, dy = n[1] - my;
        const float rho = std::hypot (dx, dy);
        const float r = std::sqrt (rho * rho + z * z);
        const float cosArrival = (hx * dx + hy * dy + hz * z) / r;
        Bleed& b = p.bleed[static_cast<size_t> (k)];
        b.gain = a * a / (2.0f * r * nearLf) * (m.polar + (1.0f - m.polar) * cosArrival) / polar;
        b.lowpassHz = kPistonCorner * kSpeedOfSound / (kTwoPi * a * std::max (rho / r, 0.05f));
        b.delaySec =
            std::max (kMinBleedDelaySec, (r - z) / kSpeedOfSound - kBesselDelay / b.lowpassHz);
    }
    return p;
}

float onePoleCoeff (double hz, double fs) noexcept
{
    return static_cast<float> (1.0 - std::exp (-kTwoPiD * clampFrequency (hz, fs) / fs));
}

int designBodyStages (const CabModel& cab, double fs, Stage* out) noexcept
{
    for (int i = 0; i < cab.numBody; ++i)
        out[i].c = designFilter (cab.body[static_cast<size_t> (i)], fs);
    return cab.numBody;
}

void designPositionStages (const CabModel& cab, float position, double fs, Stage* out) noexcept
{
    const float p = std::clamp (position, 0.0f, 1.0f);
    // Centre: dust-cap presence and no extra roll-off. Edge: the outer cone radiates less treble,
    // so the roll-off corner glides down exponentially and the low-mids fill in a little.
    const double top = std::min (18000.0, 0.45 * fs);
    const double corner = top * std::pow (cab.edgeLpHz / top, static_cast<double> (p));
    out[0].c = design::peaking (fs, cab.presenceHz, cab.presenceQ, cab.presenceDb * (1.0f - p));
    out[1].c = design::lowpass (fs, corner, cab.edgeLpQ);
    out[2].c = design::peaking (fs, cab.edgeWarmthHz, 0.8, cab.edgeWarmthDb * p);
}

void designProximityStage (const MicModel& m, const Placement& p, double fs, Stage* out) noexcept
{
    out[0].c = design::lowShelf (fs, m.proximityHz, 0.6, p.proximityDb);
}

int designMicStages (const MicModel& m, double fs, Stage* out) noexcept
{
    for (int i = 0; i < m.numEq; ++i)
        out[i].c = designFilter (m.eq[static_cast<size_t> (i)], fs);
    return m.numEq;
}

void designOffAxisStages (const MicModel& m, const Placement& p, double fs, Stage* out) noexcept
{
    out[0].c = design::peaking (fs, m.presenceHz, m.presenceQ, p.presenceCutDb);
    out[1].c = design::highShelf (fs, m.offAxisHz, 0.6, p.offAxisShelfDb);
}

void designAirStage (const Placement& p, double fs, Stage* out) noexcept
{
    out[0].c = design::highShelf (fs, kAirShelfHz, 0.7, p.airDb);
}

BiquadCoeffs designBleedFilter (const Bleed& b, double fs) noexcept
{
    return design::lowpass (
        fs, std::min (static_cast<double> (b.lowpassHz / kBesselCorner), 0.45 * fs), kBleedLpQ);
}

std::complex<double> voiceResponse (int cabIndex, int type, const MicPose& pose, double fs,
                                    double hz, bool bodyOnly) noexcept
{
    const CabModel& cab = cabModel (cabIndex);
    const MicModel& m = micModel (type);
    const double w = kTwoPiD * hz / fs;
    const std::complex<double> z1 = std::polar (1.0, -w);
    const auto product = [z1] (const Stage* st, int count) {
        std::complex<double> h = 1.0;
        for (int i = 0; i < count; ++i)
            h *= stageResponse (st[i].c, z1);
        return h;
    };

    std::array<Stage, kMaxStages> stages {};
    const int numBody = designBodyStages (cab, fs, stages.data());
    std::complex<double> h = 1.0;
    if (cab.reflectionGain != 0.0f)
    {
        const double delay = std::round (cab.reflectionMs * 0.001 * fs);
        h += static_cast<double> (cab.reflectionGain) *
             onePoleResponse (onePoleCoeff (cab.reflectionLpHz, fs), z1) *
             std::polar (1.0, -w * delay);
    }
    h *= product (stages.data(), numBody);
    if (bodyOnly)
        return h;

    // The miked cone (position, proximity, air) plus the other speakers, then the capsule.
    const Placement pl = placement (cab, m, pose);
    Stage* near = stages.data() + numBody;
    designPositionStages (cab, pose.position(), fs, near);
    designProximityStage (m, pl, fs, near + kPositionStages);
    designAirStage (pl, fs, near + kPositionStages + 1);
    std::complex<double> heard = product (near, kNearStages);
    for (int k = 0; k < pl.numBleed; ++k)
    {
        const Bleed& b = pl.bleed[static_cast<size_t> (k)];
        heard += static_cast<double> (b.gain) * stageResponse (designBleedFilter (b, fs), z1) *
                 std::polar (1.0, -w * static_cast<double> (b.delaySec) * fs);
    }
    Stage* capsule = near + kNearStages;
    int count = designMicStages (m, fs, capsule);
    designOffAxisStages (m, pl, fs, capsule + count);
    count += kOffAxisStages;
    heard *= product (capsule, count);

    const std::complex<double> direct = std::polar (1.0, -w * pl.arrivalSec * fs);
    const std::complex<double> bounce = static_cast<double> (pl.bounceGain) *
                                        onePoleResponse (onePoleCoeff (kBounceLpHz, fs), z1) *
                                        std::polar (1.0, -w * pl.bounceSec * fs);
    return h * heard * static_cast<double> (pl.levelGain) * (direct + bounce);
}

Calibration calibrate (double fs)
{
    Calibration cal;
    const MicPose reference = referencePose();
    for (int c = 0; c < 3; ++c)
    {
        for (int t = 0; t < 10; ++t)
        {
            const double db = loudnessIndexDb (
                [&] (double hz) { return std::norm (voiceResponse (c, t, reference, fs, hz)); });
            cal.voice[static_cast<size_t> (c)][static_cast<size_t> (t)] =
                static_cast<float> (std::pow (10.0, -db / 20.0));
        }
        const double bodyDb = loudnessIndexDb (
            [&] (double hz) { return std::norm (voiceResponse (c, 0, reference, fs, hz, true)); });
        cal.body[static_cast<size_t> (c)] = static_cast<float> (std::pow (10.0, -bodyDb / 20.0));
    }
    return cal;
}

} // namespace sedef::dsp::cab
