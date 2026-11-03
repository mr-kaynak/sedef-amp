// SPDX-License-Identifier: AGPL-3.0-or-later
// Free mic placement: where the capsule sits over the baffle (2D), how far back, and how it is
// turned. See docs/dsp/cab.md section 3.
#include "CabTestUtils.h"
#include "sedef/core/Biquad.h"
#include "sedef/dsp/cab/CabModels.h"
#include "sedef/dsp/cab/ConvolutionKernel.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <complex>
#include <string>

using namespace sedef;
using namespace sedef::test;
using namespace sedef::test::cab;
using Catch::Approx;
namespace model = sedef::dsp::cab;

namespace
{
constexpr int k1x12 = 0, k2x12 = 1, k4x12 = 2;
constexpr int kDynClassic = 0, kDynOffAxis = 1, kDynBright = 2, kCondSmall = 4, kCondLarge = 5;
constexpr int kCondPrecision = 6, kRibbonSmooth = 8, kRibbonDark = 9;

/// Third-octave band centres from 100 Hz to 8 kHz.
std::vector<double> bands (double lo = 100.0, double hi = 8000.0)
{
    std::vector<double> hz;
    for (int k = -30; k <= 30; ++k)
    {
        const double f = 1000.0 * std::exp2 (k / 3.0);
        if (f >= lo * 0.99 && f <= hi * 1.01)
            hz.push_back (f);
    }
    return hz;
}

/// Power in the third-octave band around hz (8 log-spaced points), dB.
double thirdOctaveDb (const std::vector<float>& ir, double fs, double hz)
{
    double p = 0.0;
    for (int i = 0; i < 8; ++i)
        p += std::norm (responseAt (ir, fs, hz * std::exp2 (((i + 0.5) / 8.0 - 0.5) / 3.0)));
    return 10.0 * std::log10 (p / 8.0 + 1e-30);
}

/// Largest third-octave difference between two responses over [lo, hi].
double maxBandDifference (const std::vector<float>& a, const std::vector<float>& b, double fs,
                          double lo, double hi)
{
    double worst = 0.0;
    for (double hz : bands (lo, hi))
        worst = std::max (worst, std::abs (thirdOctaveDb (a, fs, hz) - thirdOctaveDb (b, fs, hz)));
    return worst;
}

std::vector<float> response (CabSim& cab, int c, int type, float position, float azimuth,
                             float distance, float offAxis, double fs = 48000.0)
{
    return impulseResponse (cab, soloMicA (c, type, position, distance, azimuth, offAxis), fs);
}

/// Left output of `x` through the cab in fixed blocks, with settingsAt(sample) before each block.
template <typename SettingsAt>
std::vector<float> renderLeft (CabSim& cab, const std::vector<float>& x, int block,
                               SettingsAt&& settingsAt)
{
    std::vector<float> l (x.size());
    Buffer o (2, block);
    for (size_t p = 0; p < x.size(); p += static_cast<size_t> (block))
    {
        const int n = std::min (block, static_cast<int> (x.size() - p));
        cab.setSettings (settingsAt (p));
        cab.process (x.data() + p, o.view().subView (0, n));
        std::copy_n (o[0].begin(), n, l.begin() + static_cast<std::ptrdiff_t> (p));
    }
    return l;
}

float maxDifference (const std::vector<float>& a, const std::vector<float>& b, size_t from = 0)
{
    float d = 0.0f;
    for (size_t i = from; i < std::min (a.size(), b.size()); ++i)
        d = std::max (d, std::abs (a[i] - b[i]));
    return d;
}

/// A neighbour's level in the low end, where the shelf on the neighbours' sum gives back what is
/// held above (see docs/dsp/cab.md, 3.4): the plain geometry and polar pattern.
double lowEndGain (const model::Placement& pl, int k)
{
    return static_cast<double> (pl.bleed[static_cast<size_t> (k)].gain) *
           std::pow (10.0, static_cast<double> (pl.bleedShelfDb) / 20.0);
}

/// Complex response of a cascade of biquads at one frequency.
std::complex<double> chainAt (const BiquadCoeffs* c, int count, double hz, double fs)
{
    const std::complex<double> z1 = std::polar (1.0, -kTwoPiD * hz / fs), z2 = z1 * z1;
    std::complex<double> h = 1.0;
    for (int i = 0; i < count; ++i)
        h *= (static_cast<double> (c[i].b0) + static_cast<double> (c[i].b1) * z1 +
              static_cast<double> (c[i].b2) * z2) /
             (1.0 + static_cast<double> (c[i].a1) * z1 + static_cast<double> (c[i].a2) * z2);
    return h;
}

/// Group delay of a cascade of biquads at one frequency, seconds.
double groupDelaySec (const BiquadCoeffs* c, int count, double hz, double fs)
{
    const double dHz = 0.5;
    const double phase =
        std::arg (chainAt (c, count, hz + dHz, fs) / chainAt (c, count, hz - dHz, fs));
    return -phase / (kTwoPiD * 2.0 * dHz);
}

/// What only the miked cone's sound passes through (position, proximity, air), as CabSim builds it.
std::array<BiquadCoeffs, model::kNearStages>
nearCoeffs (int c, int type, const model::MicPose& pose, const model::Placement& pl, double fs)
{
    std::array<model::Stage, model::kNearStages> st {};
    model::designPositionStages (model::cabModel (c), pose.position(), fs, st.data());
    model::designProximityStage (model::micModel (type), pl, fs,
                                 st.data() + model::kPositionStages);
    model::designAirStage (pl, fs, st.data() + model::kPositionStages + 1);
    std::array<BiquadCoeffs, model::kNearStages> out {};
    for (size_t i = 0; i < st.size(); ++i)
        out[i] = st[i].c;
    return out;
}
} // namespace

TEST_CASE ("The real-time chain matches the analytic model at any pose", "[cab][placement]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto cal = model::calibrate (fs);
    CabSim cab;
    Random r (7);
    for (int trial = 0; trial < 12; ++trial)
    {
        const int c = trial % 3;
        const int type = std::array { kDynClassic, kCondPrecision,
                                      kRibbonSmooth }[static_cast<size_t> (trial / 3 % 3)];
        const float position = r.unipolar();
        const float azimuth = 360.0f * r.unipolar() - 180.0f;
        const float distance = r.unipolar();
        const float offAxis = 60.0f * r.unipolar();
        const auto ir = response (cab, c, type, position, azimuth, distance, offAxis, fs);
        const auto pose = model::MicPose::polar (position, azimuth, distance, offAxis);
        const double norm = cal.voice[static_cast<size_t> (c)][static_cast<size_t> (type)];
        double worst = 0.0;
        for (double hz : bands (60.0, 4000.0))
        {
            const double measured = responseDb (ir, fs, hz);
            const double expected =
                20.0 * std::log10 (std::abs (model::voiceResponse (c, type, pose, fs, hz)) * norm);
            if (expected > -40.0) // a comb notch is too sharp to compare at one frequency
                worst = std::max (worst, std::abs (measured - expected));
        }
        INFO ("cab " << c << " type " << type << " position " << position << " azimuth " << azimuth
                     << " distance " << distance << " off axis " << offAxis << " at " << fs);
        CHECK (worst < 0.1);
    }
}

TEST_CASE ("A single speaker is round: azimuth alone barely matters on the grille",
           "[cab][placement]")
{
    const double fs = 48000.0;
    CabSim cab;
    // The 1x12 has no neighbours; only the floor bounce (-32 dB on the grille) sees the height.
    const auto right = response (cab, k1x12, kDynClassic, 0.8f, 90.0f, 0.0f, 0.0f);
    for (float azimuth : { 0.0f, 180.0f, -90.0f, 30.0f })
    {
        const auto other = response (cab, k1x12, kDynClassic, 0.8f, azimuth, 0.0f, 0.0f);
        CHECK (maxBandDifference (right, other, fs, 100.0, 8000.0) < 0.15);
    }
}

TEST_CASE ("Placement over a multi-speaker baffle matters, and more so with distance",
           "[cab][placement]")
{
    const double fs = 48000.0;
    CabSim cab;
    // Mic at the cone edge, towards the other three cones of the 4x12 (up and right) or away.
    const auto spread = [&] (int c, float distance) {
        const auto toward = response (cab, c, kDynClassic, 1.0f, 45.0f, distance, 0.0f);
        const auto away = response (cab, c, kDynClassic, 1.0f, -135.0f, distance, 0.0f);
        return maxBandDifference (toward, away, fs, 100.0, 3000.0);
    };
    const double near4 = spread (k4x12, 0.0f), far4 = spread (k4x12, 1.0f);
    const double near2 = spread (k2x12, 0.0f), far2 = spread (k2x12, 1.0f);
    INFO ("4x12: " << near4 << " dB on the grille, " << far4 << " dB at distance 1; 2x12: " << near2
                   << " / " << far2 << " dB");
    CHECK (near4 > 1.0);
    CHECK (near4 < 5.0);
    CHECK (far4 > 6.0);
    CHECK (far4 > 2.0 * near4);
    CHECK (far2 > near2);

    SECTION ("each neighbour gets closer, louder and earlier as the mic moves towards it")
    {
        const auto& closed = model::cabModel (k4x12);
        const auto& dyn = model::micModel (kDynClassic);
        for (float distance : { 0.0f, 0.5f, 1.0f })
        {
            const auto toward =
                model::placement (closed, dyn, model::MicPose::polar (1.0f, 45.0f, distance, 0));
            const auto centre =
                model::placement (closed, dyn, model::MicPose::polar (0.0f, 0.0f, distance, 0));
            const auto away =
                model::placement (closed, dyn, model::MicPose::polar (1.0f, -135.0f, distance, 0));
            REQUIRE (toward.numBleed == 3);
            for (int k = 0; k < 3; ++k)
            {
                const auto kk = static_cast<size_t> (k);
                CHECK (toward.bleed[kk].delaySec < centre.bleed[kk].delaySec);
                CHECK (centre.bleed[kk].delaySec < away.bleed[kk].delaySec);
                CHECK (lowEndGain (toward, k) > lowEndGain (centre, k));
                CHECK (lowEndGain (centre, k) > lowEndGain (away, k));
            }
        }
        // Backing off, the other cones catch up with the miked one (spherical spreading).
        double previous = 0.0;
        for (float distance : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const auto pl =
                model::placement (closed, dyn, model::MicPose::polar (0.5f, 60.0f, distance, 0));
            CHECK (lowEndGain (pl, 0) > previous);
            previous = lowEndGain (pl, 0);
        }
        CHECK (previous > 0.5f);
        CHECK (model::cabModel (k1x12).numNeighbours == 0);
    }

    SECTION ("a figure-of-eight rejects the neighbours that arrive from its side")
    {
        const auto& closed = model::cabModel (k4x12);
        const auto pose = model::MicPose::polar (0.5f, 60.0f, 0.0f, 0.0f);
        const auto ribbon = model::placement (closed, model::micModel (kRibbonSmooth), pose);
        const auto omni = model::placement (closed, model::micModel (kCondPrecision), pose);
        for (size_t k = 0; k < 3; ++k)
            CHECK (std::abs (ribbon.bleed[k].gain) < 0.2f * omni.bleed[k].gain);
    }
}

TEST_CASE ("The floor bounce follows the mic's height", "[cab][placement]")
{
    const double fs = 48000.0;
    CabSim cab;
    const auto& open = model::cabModel (k1x12);
    const auto& omni = model::micModel (kCondPrecision);
    const auto top = model::placement (open, omni, model::MicPose::polar (1.0f, 0.0f, 1.0f, 0));
    const auto bottom =
        model::placement (open, omni, model::MicPose::polar (1.0f, 180.0f, 1.0f, 0));
    CHECK (top.bounceSec > bottom.bounceSec + 0.0005f);

    // Far from the cab the bounce comb moves audibly; on the grille it is 30 dB down.
    const auto far = maxBandDifference (
        response (cab, k1x12, kCondPrecision, 1.0f, 0.0f, 1.0f, 0),
        response (cab, k1x12, kCondPrecision, 1.0f, 180.0f, 1.0f, 0), fs, 150.0, 1500.0);
    const auto near = maxBandDifference (
        response (cab, k1x12, kCondPrecision, 1.0f, 0.0f, 0.0f, 0),
        response (cab, k1x12, kCondPrecision, 1.0f, 180.0f, 0.0f, 0), fs, 150.0, 1500.0);
    INFO ("top vs bottom: " << far << " dB at distance 1, " << near << " dB on the grille");
    CHECK (far > 1.0);
    CHECK (near < 0.2);
}

TEST_CASE ("Turning the capsule off axis", "[cab][placement]")
{
    const double fs = 48000.0;
    CabSim cab;
    const auto hf = [&] (int type, float angle) {
        return relativeBandDb (response (cab, k1x12, type, 0.5f, 60.0f, 0.0f, angle), fs, 4000.0,
                               8000.0);
    };
    // Below the off-axis shelves and above the proximity shelf.
    const auto mids = [&] (int type, float angle) {
        return bandDb (response (cab, k1x12, type, 0.5f, 60.0f, 0.0f, angle), fs, 400.0, 700.0);
    };

    SECTION ("treble falls steadily with the angle, most for large diaphragms")
    {
        for (int type = 0; type < 10; ++type)
        {
            double previous = 1e9;
            for (float angle : { 0.0f, 15.0f, 30.0f, 45.0f, 60.0f })
            {
                const double v = hf (type, angle);
                INFO ("type " << type << " at " << angle << " degrees");
                CHECK (v < previous);
                previous = v;
            }
        }
        const auto loss = [&] (int type) {
            return hf (type, 0.0f) - hf (type, 60.0f);
        };
        CHECK (loss (kDynClassic) > 6.0);
        CHECK (loss (kCondLarge) > loss (kCondSmall) + 2.0);
        CHECK (loss (kCondPrecision) < loss (kCondSmall));
    }

    SECTION ("the mids drop by the polar pattern")
    {
        // Cardioid 0.5 + 0.5 cos 60 = 0.75; figure-of-eight cos 60 = 0.5; near-omni 0.95.
        CHECK (mids (kDynClassic, 60.0f) - mids (kDynClassic, 0.0f) ==
               Approx (20.0 * std::log10 (0.75)).margin (0.4));
        CHECK (mids (kRibbonSmooth, 60.0f) - mids (kRibbonSmooth, 0.0f) ==
               Approx (20.0 * std::log10 (0.5)).margin (0.4));
        CHECK (mids (kCondPrecision, 60.0f) - mids (kCondPrecision, 0.0f) > -1.0);
    }

    SECTION ("a cardioid loses proximity off axis, a figure-of-eight keeps it")
    {
        // Only the pressure-gradient part of a directional mic has proximity effect, and it
        // follows cos(angle) while the pressure part does not.
        const auto lows = [&] (int type, float angle) {
            const auto ir = response (cab, k1x12, type, 0.5f, 60.0f, 0.0f, angle);
            return bandDb (ir, fs, 40.0, 100.0) - bandDb (ir, fs, 400.0, 700.0);
        };
        CHECK (lows (kDynClassic, 60.0f) < lows (kDynClassic, 0.0f) - 1.0);
        CHECK (lows (kRibbonSmooth, 60.0f) == Approx (lows (kRibbonSmooth, 0.0f)).margin (0.3));
    }

    SECTION ("Dyn Classic at 45 degrees has the shape of the Dyn Classic Off-Axis voicing")
    {
        // On the 1x12: on the bigger cabs the turned capsule also hears the other cones
        // differently, which the fixed voicing cannot.
        const auto turned = response (cab, k1x12, kDynClassic, 0.5f, 60.0f, 0.0f, 45.0f);
        const auto voicing = response (cab, k1x12, kDynOffAxis, 0.5f, 60.0f, 0.0f, 0.0f);
        const double offset =
            thirdOctaveDb (turned, fs, 1000.0) - thirdOctaveDb (voicing, fs, 1000.0);
        double worst = 0.0;
        for (double hz : bands (100.0, 8000.0))
            worst = std::max (worst, std::abs (thirdOctaveDb (turned, fs, hz) -
                                               thirdOctaveDb (voicing, fs, hz) - offset));
        INFO ("shape within " << worst << " dB, level " << offset << " dB");
        CHECK (worst < 0.5);
        CHECK (offset < -1.0); // a real capsule turned away is quieter
    }
}

TEST_CASE ("Custom IRs keep their placement; the off-axis angle still applies", "[cab][placement]")
{
    const double fs = 48000.0;
    const int block = 64;
    std::vector<float> unit (64, 0.0f);
    unit[0] = 1.0f;
    std::vector<float> x (static_cast<size_t> (0.6 * fs));
    fillNoise (x, 0.5f, 41);

    const auto withIr = [&] (CabSim& cab) {
        cab.prepare ({ fs, block, 2 });
        cab.setCustomIr (0, 0, CabSim::makeKernel (unit.data(), 64, fs, fs, block));
    };
    auto s = soloMicA (k1x12, CabSim::kCustomIrType);

    SECTION ("position, azimuth and distance do not touch an IR")
    {
        CabSim a, b;
        withIr (a);
        withIr (b);
        auto moved = s;
        moved.mics[0].position = 0.9f;
        moved.mics[0].azimuthDeg = -100.0f;
        moved.mics[0].distance = 0.8f;
        const auto la = renderLeft (a, x, block, [&] (size_t) { return s; });
        const auto lb = renderLeft (b, x, block, [&] (size_t p) { return p < 2048 ? s : moved; });
        CHECK (maxDifference (la, lb) == 0.0f);
        // An impulse IR on axis passes the input through untouched (centre pan: x / sqrt 2).
        for (size_t i = 0; i < x.size(); ++i)
            REQUIRE (la[i] == Approx (x[i] * 0.70710678f).margin (1e-6));
    }

    SECTION ("turning the capsule filters the IR with the dynamic capsule's off-axis model")
    {
        CabSim cab;
        withIr (cab);
        auto turned = s;
        turned.mics[0].offAxisDeg = 45.0f;
        cab.setSettings (turned);
        cab.reset();
        std::vector<float> in (16384, 0.0f), ir (in.size());
        in[0] = 1.0f;
        Buffer o (2, block);
        for (size_t p = 0; p < in.size(); p += block)
        {
            cab.process (in.data() + p, o.view());
            for (int i = 0; i < block; ++i)
                ir[p + static_cast<size_t> (i)] =
                    (o[0][static_cast<size_t> (i)] + o[1][static_cast<size_t> (i)]) * 0.70710678f;
        }
        const auto& capsule = model::micModel (model::kIrCapsuleType);
        model::Placement pl;
        pl.presenceCutDb = -capsule.presenceDb * model::offAxisAmount (45.0f);
        pl.offAxisShelfDb = capsule.offAxisDb * model::offAxisAmount (45.0f);
        std::array<model::Stage, model::kOffAxisStages> st {};
        model::designOffAxisStages (capsule, pl, fs, st.data());
        for (double hz : { 200.0, 1000.0, 3000.0, 5000.0, 8000.0 })
        {
            const double expected = 20.0 * std::log10 (model::polarGain (capsule, 45.0f) *
                                                       design::magnitude (st[0].c, hz, fs) *
                                                       design::magnitude (st[1].c, hz, fs));
            CHECK (responseDb (ir, fs, hz) == Approx (expected).margin (0.02));
        }
        CHECK (responseDb (ir, fs, 5000.0) < responseDb (ir, fs, 500.0) - 4.0);
    }

    SECTION ("back on axis the IR plays untouched again")
    {
        CabSim cab;
        withIr (cab);
        auto turned = s;
        turned.mics[0].offAxisDeg = 30.0f;
        std::vector<float> y (static_cast<size_t> (1.0 * fs));
        fillNoise (y, 0.5f, 42);
        const auto l = renderLeft (cab, y, block,
                                   [&] (size_t p) { return p >= 4096 && p < 12000 ? turned : s; });
        float during = 0.0f;
        for (size_t i = 8000; i < 12000; ++i)
            during = std::max (during, std::abs (l[i] - y[i] * 0.70710678f));
        CHECK (during > 1.0e-3f);
        // Once the angle has glided home the filter stops: exactly the input again.
        for (size_t i = static_cast<size_t> (0.75 * fs); i < y.size(); ++i)
            REQUIRE (l[i] == y[i] * 0.70710678f);
    }
}

TEST_CASE ("Azimuth wraps the short way round", "[cab][placement]")
{
    const double fs = 48000.0;
    const int block = 64;
    std::vector<float> x (static_cast<size_t> (0.3 * fs));
    fillNoise (x, 0.5f, 43);
    const auto render = [&] (float before, float after) {
        CabSim cab;
        cab.prepare ({ fs, block, 2 });
        return renderLeft (cab, x, block, [&] (size_t p) {
            return soloMicA (k4x12, kDynClassic, 1.0f, 1.0f, p < x.size() / 2 ? before : after);
        });
    };
    const auto steady = render (179.0f, 179.0f);
    const auto wrapped = render (179.0f, -179.0f); // 2 degrees clockwise across 6 o'clock
    const auto opposite = render (179.0f, 0.0f);   // straight across the cone
    const auto from = x.size() / 2;
    CHECK (maxDifference (steady, wrapped, from) < 0.05f * maxDifference (steady, opposite, from));
}

TEST_CASE ("No other speaker outweighs the miked one and their combs stay finite",
           "[cab][placement]")
{
    // A grid of poses on the 2x12 and the 4x12, for every capsule family, on axis and turned.
    // No neighbour, and above the low end not even all of them together, outweighs the miked
    // cone, so their comb with it (the miked cone plus the others, relative to the miked cone
    // alone, 150 Hz - 3 kHz) never notches below the floor the room's diffuse field sets at that
    // distance (see docs/dsp/cab.md, 3.4). In the low end, where they cannot oppose it, the others
    // still add up: a backed-off 4x12 gains low end.
    const double fs = 48000.0;
    constexpr double grille = 0.025, span = 0.325, roomQ = 4.0, sabins = 0.161 * 100.0 / 0.5;
    for (int c : { k2x12, k4x12 })
    {
        double single = 0.0, together = 0.0, lows = 0.0, notch = 0.0, belowFloor = 0.0;
        std::string where;
        for (int type :
             { kDynClassic, kDynBright, kCondLarge, kCondPrecision, kRibbonSmooth, kRibbonDark })
            for (float offAxis : { 0.0f, 30.0f, 60.0f })
                for (float distance : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                    for (float position : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                        for (float azimuth = -180.0f; azimuth < 180.0f; azimuth += 15.0f)
                        {
                            const auto pose =
                                model::MicPose::polar (position, azimuth, distance, offAxis);
                            const auto pl = model::placement (model::cabModel (c),
                                                              model::micModel (type), pose);
                            const auto shelf = model::designBleedShelf (pl, fs);
                            double sum = 0.0;
                            std::array<BiquadCoeffs, model::kMaxNeighbours> lp {};
                            for (size_t k = 0; k < static_cast<size_t> (pl.numBleed); ++k)
                            {
                                const double g = std::abs (pl.bleed[k].gain);
                                single = std::max (single, g);
                                sum += g;
                                lp[k] = model::designBleedFilter (pl.bleed[k], fs);
                            }
                            together = std::max (together, sum);
                            lows = std::max (lows, sum * std::abs (chainAt (&shelf, 1, 20.0, fs)));
                            const double z = grille + span * distance;
                            const double floorDb =
                                10.0 * std::log10 (16.0 * kPiD * z * z / (roomQ * sabins));
                            const auto near = nearCoeffs (c, type, pose, pl, fs);
                            for (double hz = 150.0; hz <= 3000.0; hz *= std::exp2 (1.0 / 36.0))
                            {
                                std::complex<double> others = 0.0;
                                for (size_t k = 0; k < static_cast<size_t> (pl.numBleed); ++k)
                                    others +=
                                        static_cast<double> (pl.bleed[k].gain) *
                                        chainAt (&lp[k], 1, hz, fs) *
                                        std::polar (1.0,
                                                    -kTwoPiD * hz *
                                                        static_cast<double> (pl.bleed[k].delaySec));
                                others *= chainAt (&shelf, 1, hz, fs);
                                const double db =
                                    20.0 *
                                    std::log10 (std::abs (
                                        1.0 + others / chainAt (near.data(), model::kNearStages, hz,
                                                                fs)));
                                belowFloor = std::max (belowFloor, floorDb - db);
                                if (db < notch)
                                {
                                    notch = db;
                                    where = "type " + std::to_string (type) + " off axis " +
                                            std::to_string (offAxis) + " distance " +
                                            std::to_string (distance) + " position " +
                                            std::to_string (position) + " azimuth " +
                                            std::to_string (azimuth) + " at " +
                                            std::to_string (hz) + " Hz";
                                }
                            }
                        }
        INFO ("cab " << c << ": loudest neighbour " << single << ", all together " << together
                     << " (low end " << lows << "), deepest notch " << notch << " dB (" << where
                     << "), most below the room floor " << belowFloor << " dB");
        CHECK (single < 1.0);
        CHECK (together < 1.0);
        CHECK (lows < 2.5);
        CHECK (notch > -25.0);
        CHECK (belowFloor < 1.0);
        if (c == k4x12)
            CHECK (lows > 2.0); // three coherent cones at 35 cm: about 0.7 of the miked one each
    }
}

TEST_CASE ("The other speakers arrive when their path says", "[cab][placement]")
{
    // Relative to the miked cone, a neighbour's sound lags by its extra path (R - z) / c. Both
    // paths run through filters that delay them (the neighbour's beaming low-pass and the shelf on
    // the neighbours' sum; the miked cone's position, proximity and air stages), so the bleed
    // delay makes up the difference. Checked as group delay at the neighbour's beaming corner,
    // where its comb with the miked cone forms.
    const double fs = GENERATE (44100.0, 48000.0);
    constexpr double c = 343.0, grille = 0.025, span = 0.325; // as in CabModels.cpp
    for (int cab : { k2x12, k4x12 })
    {
        double worst = 0.0;
        std::string where;
        for (int type : { kDynClassic, kCondPrecision, kRibbonSmooth })
            for (float distance : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                for (float position : { 0.0f, 0.5f, 1.0f })
                    for (float azimuth = -180.0f; azimuth < 180.0f; azimuth += 30.0f)
                    {
                        const auto pose = model::MicPose::polar (position, azimuth, distance, 0);
                        const auto& geometry = model::cabModel (cab);
                        const auto pl = model::placement (geometry, model::micModel (type), pose);
                        const auto near = nearCoeffs (cab, type, pose, pl, fs);
                        const double z = grille + span * distance;
                        for (size_t k = 0; k < static_cast<size_t> (pl.numBleed); ++k)
                        {
                            const double dx =
                                geometry.neighbours[k][0] - pose.x * model::kConeRadiusM;
                            const double dy =
                                geometry.neighbours[k][1] - pose.y * model::kConeRadiusM;
                            const double path = (std::sqrt (dx * dx + dy * dy + z * z) - z) / c;
                            const double hz = pl.bleed[k].lowpassHz;
                            const std::array<BiquadCoeffs, 2> others {
                                model::designBleedFilter (pl.bleed[k], fs),
                                model::designBleedShelf (pl, fs)
                            };
                            const double arrival =
                                pl.bleed[k].delaySec + groupDelaySec (others.data(), 2, hz, fs) -
                                groupDelaySec (near.data(), model::kNearStages, hz, fs);
                            if (std::abs (arrival - path) > std::abs (worst))
                            {
                                worst = arrival - path;
                                where = "type " + std::to_string (type) + " distance " +
                                        std::to_string (distance) + " position " +
                                        std::to_string (position) + " azimuth " +
                                        std::to_string (azimuth) + " neighbour " +
                                        std::to_string (k);
                            }
                        }
                    }
        INFO ("cab " << cab << " at " << fs << ": worst arrival error " << worst * 1e6 << " us ("
                     << where << ")");
        CHECK (std::abs (worst) < 20.0e-6);
    }
}

TEST_CASE ("After the input stops the cab settles to exact silence", "[cab][placement]")
{
    // Engine::process runs with flush-to-zero, but CabSim does not count on it: filter states are
    // flushed at the end of each block, and the off-axis stages stop once back on axis and quiet.
    // Here, without flush-to-zero, nothing may decay through subnormal values after the music
    // stops, also when the capsule is turned back while it is silent.
    const double fs = GENERATE (48000.0, 192000.0);
    const int block = 32;
    std::vector<float> unit (64, 0.0f);
    unit[0] = 1.0f;
    for (int type : { kDynClassic, kRibbonSmooth, CabSim::kCustomIrType })
        for (int c : { k1x12, k4x12 })
        {
            CabSim cab;
            cab.prepare ({ fs, block, 2 });
            cab.setCustomIr (0, c, CabSim::makeKernel (unit.data(), 64, fs, fs, block));
            const auto playing = soloMicA (c, type, 0.7f, 0.6f, 60.0f, 30.0f);
            auto quiet = playing;
            quiet.mics[0].offAxisDeg = 0.0f;
            std::vector<float> x (static_cast<size_t> (2.0 * fs), 0.0f);
            const auto heard = static_cast<size_t> (0.2 * fs);
            std::vector<float> head (heard);
            fillNoise (head, 0.5f, 46);
            std::copy (head.begin(), head.end(), x.begin());
            const auto l = renderLeft (cab, x, block, [&] (size_t p) {
                return p < heard + static_cast<size_t> (0.05 * fs) ? playing : quiet;
            });
            size_t subnormal = 0;
            for (float v : l)
                subnormal += std::fpclassify (v) == FP_SUBNORMAL ? 1 : 0;
            INFO ("cab " << c << " type " << type << " at " << fs);
            CHECK (subnormal == 0);
            bool silent = true;
            for (size_t i = l.size() - static_cast<size_t> (0.5 * fs); i < l.size(); ++i)
                silent = silent && l[i] == 0.0f;
            CHECK (silent);
        }
}
