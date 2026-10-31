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
    m.speakerHeightM = 0.26f; // a bottom speaker
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

constexpr std::array<MicModel, 10> kMicModels {
    // 0 Dyn Classic: cardioid dynamic, presence peak ~5 kHz, bass roll-off that proximity offsets.
    mic ({ highPass (115.0f, 0.6f), peak (5300.0f, 1.1f, 4.5f), peak (9500.0f, 2.0f, -2.0f),
           lowPass (14000.0f, 0.7f) },
         4, 8.0f, 160.0f),
    // 1 Dyn Classic Off-Axis: same capsule at 45 degrees: less presence, darker top.
    mic ({ highPass (115.0f, 0.6f), peak (4800.0f, 1.0f, 1.5f), highShelf (3500.0f, 0.6f, -5.0f),
           lowPass (11000.0f, 0.7f) },
         4, 7.0f, 160.0f),
    // 2 Dyn Bright: supercardioid dynamic, lean low-mids, wide bright presence.
    mic ({ highPass (90.0f, 0.65f), peak (500.0f, 0.8f, -1.5f), peak (6500.0f, 0.9f, 5.5f),
           highShelf (10000.0f, 0.7f, 1.5f) },
         4, 7.5f, 150.0f),
    // 3 Dyn Broadcast: large-diaphragm dynamic built to limit proximity; flat, extended lows.
    mic ({ highPass (45.0f, 0.7f), lowShelf (150.0f, 0.7f, 1.0f), peak (3500.0f, 0.7f, 1.5f),
           lowPass (15000.0f, 0.7f) },
         4, 3.5f, 130.0f),
    // 4 Cond Small: small-diaphragm cardioid condenser, flat with a gentle top lift.
    mic ({ highPass (40.0f, 0.7f), highShelf (8000.0f, 0.7f, 2.5f), peak (12000.0f, 1.5f, 1.0f) },
         3, 4.5f, 140.0f),
    // 5 Cond Large: large-diaphragm cardioid, warmth plus a broad presence lift.
    mic ({ highPass (35.0f, 0.7f), lowShelf (220.0f, 0.7f, 1.5f), peak (6000.0f, 0.8f, 3.5f),
           highShelf (12000.0f, 0.7f, 1.0f) },
         4, 5.5f, 150.0f),
    // 6 Cond Precision: near-omni measurement-style capsule, flat.
    mic ({ highPass (25.0f, 0.7f) }, 1, 1.0f, 150.0f),
    // 7 Cond Tube: large-diaphragm valve condenser, warm low-mids, smooth highs.
    mic ({ highPass (35.0f, 0.7f), peak (180.0f, 0.8f, 2.0f), peak (3000.0f, 0.8f, 1.0f),
           highShelf (7000.0f, 0.6f, -2.5f) },
         4, 5.5f, 150.0f),
    // 8 Ribbon Smooth: figure-of-eight ribbon, rolled off from ~9 kHz, big proximity.
    mic ({ highPass (30.0f, 0.7f), peak (120.0f, 0.9f, 1.0f), highShelf (4000.0f, 0.5f, -2.0f),
           lowPass (9000.0f, 0.6f) },
         4, 9.0f, 150.0f),
    // 9 Ribbon Dark: heavier ribbon voicing, darker still.
    mic ({ highPass (30.0f, 0.7f), peak (250.0f, 0.8f, 1.5f), highShelf (2500.0f, 0.5f, -4.0f),
           lowPass (6000.0f, 0.55f) },
         4, 10.0f, 160.0f),
};

constexpr float kGrilleM = 0.025f;       // capsule-to-cone distance at distance 0
constexpr float kDistanceSpanM = 0.325f; // distance 1 is ~35 cm (~1 ms further)
constexpr float kSpeedOfSound = 343.0f;
constexpr float kFloorLoss = 0.45f; // floor absorption and the speaker's off-axis loss
constexpr float kFalloffDb = -4.5f; // level change across the distance range
constexpr float kAirDb = -2.5f;

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

Placement placement (const CabModel& cab, const MicModel& m, float distance) noexcept
{
    const float d = std::clamp (distance, 0.0f, 1.0f);
    const float r = kGrilleM + kDistanceSpanM * d;
    const float floorPath = 2.0f * cab.speakerHeightM; // image source below the floor
    const float rb = std::sqrt (r * r + floorPath * floorPath);

    Placement p;
    p.arrivalSec = (r - kGrilleM) / kSpeedOfSound;
    p.bounceSec = (rb - kGrilleM) / kSpeedOfSound;
    p.bounceGain = kFloorLoss * r / rb;
    p.levelGain = dbToGain (kFalloffDb * d);
    // Perceptual taper: most of the boost goes over the first half of the travel and a little
    // remains at the far end (a steep physical 1/r law would spend it all in the first quarter).
    p.proximityDb = m.proximityDb * (0.1f + 0.9f * std::pow (1.0f - d, 1.6f));
    p.airDb = kAirDb * d;
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

int designMicStages (const MicModel& m, double fs, Stage* out) noexcept
{
    for (int i = 0; i < m.numEq; ++i)
        out[i].c = designFilter (m.eq[static_cast<size_t> (i)], fs);
    return m.numEq;
}

void designDistanceStages (const MicModel& m, const Placement& p, double fs, Stage* out) noexcept
{
    out[0].c = design::lowShelf (fs, m.proximityHz, 0.6, p.proximityDb);
    out[1].c = design::highShelf (fs, kAirShelfHz, 0.7, p.airDb);
}

std::complex<double> voiceResponse (int cabIndex, int type, float position, float distance,
                                    double fs, double hz, bool bodyOnly) noexcept
{
    const CabModel& cab = cabModel (cabIndex);
    const MicModel& m = micModel (type);
    const double w = kTwoPiD * hz / fs;
    const std::complex<double> z1 = std::polar (1.0, -w);

    std::array<Stage, kMaxStages> stages {};
    int count = designBodyStages (cab, fs, stages.data());

    std::complex<double> h = 1.0;
    if (cab.reflectionGain != 0.0f)
    {
        const double delay = std::round (cab.reflectionMs * 0.001 * fs);
        h += static_cast<double> (cab.reflectionGain) *
             onePoleResponse (onePoleCoeff (cab.reflectionLpHz, fs), z1) *
             std::polar (1.0, -w * delay);
    }
    if (!bodyOnly)
    {
        const Placement pl = placement (cab, m, distance);
        designPositionStages (cab, position, fs, stages.data() + count);
        count += kPositionStages;
        count += designMicStages (m, fs, stages.data() + count);
        designDistanceStages (m, pl, fs, stages.data() + count);
        count += kDistanceStages;

        const std::complex<double> direct = std::polar (1.0, -w * pl.arrivalSec * fs);
        const std::complex<double> bounce = static_cast<double> (pl.bounceGain) *
                                            onePoleResponse (onePoleCoeff (kBounceLpHz, fs), z1) *
                                            std::polar (1.0, -w * pl.bounceSec * fs);
        h *= static_cast<double> (pl.levelGain) * (direct + bounce);
    }
    for (int i = 0; i < count; ++i)
        h *= stageResponse (stages[static_cast<size_t> (i)].c, z1);
    return h;
}

Calibration calibrate (double fs)
{
    Calibration cal;
    for (int c = 0; c < 3; ++c)
    {
        for (int t = 0; t < 10; ++t)
        {
            const double db = loudnessIndexDb (
                [&] (double hz) { return std::norm (voiceResponse (c, t, 0.5f, 0.0f, fs, hz)); });
            cal.voice[static_cast<size_t> (c)][static_cast<size_t> (t)] =
                static_cast<float> (std::pow (10.0, -db / 20.0));
        }
        const double bodyDb = loudnessIndexDb (
            [&] (double hz) { return std::norm (voiceResponse (c, 0, 0.5f, 0.0f, fs, hz, true)); });
        cal.body[static_cast<size_t> (c)] = static_cast<float> (std::pow (10.0, -bodyDb / 20.0));
    }
    return cal;
}

} // namespace sedef::dsp::cab
