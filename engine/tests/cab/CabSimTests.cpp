// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CabTestUtils.h"
#include "sedef/core/Biquad.h"
#include "sedef/dsp/cab/ConvolutionKernel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace sedef;
using namespace sedef::test;
using namespace sedef::test::cab;
using Catch::Approx;

namespace
{
constexpr int kDynClassic = 0;
constexpr int kDynOffAxis = 1;
constexpr int kCondPrecision = 6;
constexpr int kRibbonSmooth = 8;
constexpr int kRibbonDark = 9;

CabSim::Settings randomSettings (Random& r)
{
    CabSim::Settings s;
    s.cab = static_cast<int> (r.nextU32() % 3);
    for (auto& m : s.mics)
    {
        m.enabled = r.unipolar() < 0.85f;
        m.type = static_cast<int> (r.nextU32() % CabSim::kNumMicTypes);
        m.position = r.unipolar();
        m.distance = r.unipolar();
        m.levelDb = -40.0f + 46.0f * r.unipolar();
        m.pan = r.bipolar();
        m.invertPolarity = r.unipolar() < 0.3f;
        m.roomEnabled = r.unipolar() < 0.5f;
        m.roomLevelDb = -40.0f + 46.0f * r.unipolar();
    }
    return s;
}

/// Runs `seconds` of a mono input through the cab in fixed blocks; returns L and R.
template <typename Automate>
std::pair<std::vector<float>, std::vector<float>> run (CabSim& cab, const std::vector<float>& x,
                                                       int block, Automate&& automate)
{
    std::vector<float> l (x.size()), r (x.size());
    Buffer b (2, block);
    for (size_t pos = 0; pos < x.size(); pos += static_cast<size_t> (block))
    {
        const int n = std::min (block, static_cast<int> (x.size() - pos));
        automate (static_cast<int> (pos));
        auto view = b.view().subView (0, n);
        cab.process (x.data() + pos, view);
        std::copy_n (b[0].begin(), n, l.begin() + static_cast<std::ptrdiff_t> (pos));
        std::copy_n (b[1].begin(), n, r.begin() + static_cast<std::ptrdiff_t> (pos));
    }
    return { l, r };
}

/// Energy in [lo, hi] Hz relative to the 800-1250 Hz band, dB.
double relativeBandDb (const std::vector<float>& ir, double fs, double lo, double hi)
{
    return bandDb (ir, fs, lo, hi) - bandDb (ir, fs, 800.0, 1250.0);
}

/// Reverberation time from the Schroeder decay of the squared IR (T20 extrapolated to 60 dB).
double reverbTime (const std::vector<float>& ir, double fs)
{
    std::vector<double> edc (ir.size() + 1, 0.0);
    for (size_t i = ir.size(); i-- > 0;)
        edc[i] = edc[i + 1] + static_cast<double> (ir[i]) * ir[i];
    const auto timeAt = [&] (double db) {
        for (size_t i = 0; i < ir.size(); ++i)
            if (10.0 * std::log10 (edc[i] / edc[0] + 1e-30) <= db)
                return static_cast<double> (i) / fs;
        return static_cast<double> (ir.size()) / fs;
    };
    return 3.0 * (timeAt (-25.0) - timeAt (-5.0));
}
} // namespace

TEST_CASE ("CabSim output is finite for random settings and full-scale noise", "[cab]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const int block = GENERATE (1, 17, 64, 512);

    CabSim cab;
    cab.prepare ({ fs, 512, 2 });
    // Custom IRs in some slots, one deliberately for the wrong rate.
    std::vector<float> ir (2000);
    fillNoise (ir, 1.0f, 3);
    for (int i = 0; i < 2000; ++i)
        ir[static_cast<size_t> (i)] *= std::exp (-static_cast<float> (i) / 300.0f);
    cab.setCustomIr (0, 0, CabSim::makeKernel (ir.data(), 2000, 44100.0, fs, 512));
    cab.setCustomIr (1, 2, CabSim::makeKernel (ir.data(), 2000, 48000.0, fs, 512));
    cab.setCustomIr (1, 1, CabSim::makeKernel (ir.data(), 2000, 48000.0, fs * 0.5, 512));

    std::vector<float> x (static_cast<size_t> (0.25 * fs));
    fillNoise (x, 1.0f, 17);
    Random r (static_cast<std::uint32_t> (fs) + static_cast<std::uint32_t> (block));
    int nextChange = 0;
    const auto [l, rOut] = run (cab, x, block, [&] (int pos) {
        if (pos >= nextChange)
        {
            cab.setSettings (randomSettings (r));
            nextChange =
                pos + 1 + static_cast<int> (r.unipolar() * 0.02f * static_cast<float> (fs));
        }
    });
    CHECK (allFinite (l));
    CHECK (allFinite (rOut));
    CHECK (peak (l) < 100.0f);
    CHECK (peak (rOut) < 100.0f);
    cab.collectGarbage();
}

TEST_CASE ("CabSim handles empty blocks, mono outputs and in-place processing", "[cab]")
{
    CabSim cab;
    cab.prepare ({ 48000.0, 64, 2 });
    cab.setSettings (soloMicA (2, 0));
    cab.reset();

    Buffer b (2, 64);
    fillNoise (b[0], 0.5f, 1);
    cab.process (b[0].data(), b.view().subView (0, 0));

    // In place: the input is also the left output.
    std::vector<float> x = b[0];
    CabSim ref;
    ref.prepare ({ 48000.0, 64, 2 });
    ref.setSettings (soloMicA (2, 0));
    ref.reset();
    Buffer o (2, 64);
    ref.process (x.data(), o.view());
    cab.process (b[0].data(), b.view());
    CHECK (b[0] == o[0]);
    CHECK (b[1] == o[1]);

    // One output channel: the constant-power fold.
    Buffer mono (1, 64);
    cab.reset();
    cab.process (x.data(), mono.view());
    for (int i = 0; i < 64; ++i)
        CHECK (
            mono[0][static_cast<size_t> (i)] ==
            Approx ((o[0][static_cast<size_t> (i)] + o[1][static_cast<size_t> (i)]) * 0.70710678f)
                .margin (1e-6));
}

TEST_CASE ("Procedural cabinets have plausible responses", "[cab]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    CabSim cab;

    SECTION ("the closed 4x12 rolls off steeply above 5-6 kHz")
    {
        for (int t = 0; t < 10; ++t)
        {
            const auto ir = impulseResponse (cab, soloMicA (2, t, 0.0f), fs);
            CHECK (bandDb (ir, fs, 9000.0, 11000.0) < bandDb (ir, fs, 900.0, 1100.0) - 12.0);
        }
        const auto ir = impulseResponse (cab, soloMicA (2, kDynClassic), fs);
        CHECK (bandDb (ir, fs, 9000.0, 11000.0) < bandDb (ir, fs, 900.0, 1100.0) - 25.0);
    }

    SECTION ("resonances sit where each cabinet puts them")
    {
        struct Expect
        {
            int cab;
            double lo, hi;
        };
        for (const auto& e :
             { Expect { 0, 88.0, 110.0 }, Expect { 1, 78.0, 100.0 }, Expect { 2, 72.0, 95.0 } })
        {
            const auto ir = impulseResponse (cab, soloMicA (e.cab, kCondPrecision), fs);
            double best = 0.0, bestDb = -1e9;
            for (double hz = 40.0; hz <= 250.0; hz += 1.0)
            {
                const double db = responseDb (ir, fs, hz);
                if (db > bestDb)
                {
                    bestDb = db;
                    best = hz;
                }
            }
            INFO ("cab " << e.cab << " peak at " << best << " Hz");
            CHECK (best >= e.lo);
            CHECK (best <= e.hi);
            // A real peak, not a shelf edge.
            CHECK (bestDb > responseDb (ir, fs, best * 0.6) + 2.0);
            CHECK (bestDb > responseDb (ir, fs, best * 2.5) + 0.5);
        }
    }

    SECTION ("open backs cancel the deep lows a closed back keeps")
    {
        const auto open = impulseResponse (cab, soloMicA (0, kCondPrecision), fs);
        const auto closed = impulseResponse (cab, soloMicA (2, kCondPrecision), fs);
        CHECK (relativeBandDb (open, fs, 40.0, 70.0) <
               relativeBandDb (closed, fs, 40.0, 70.0) - 6.0);
    }

    SECTION ("moving from the cone centre to the edge darkens monotonically")
    {
        for (int c = 0; c < 3; ++c)
            for (int t : { kDynClassic, kCondPrecision, kRibbonSmooth })
            {
                double previous = 1e9;
                for (float p : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                {
                    const double hf =
                        bandDb (impulseResponse (cab, soloMicA (c, t, p), fs), fs, 4000.0, 8000.0);
                    INFO ("cab " << c << " mic " << t << " position " << p);
                    CHECK (hf < previous - 0.5);
                    previous = hf;
                }
            }
    }

    SECTION ("distance trades proximity bass for a small level drop")
    {
        const auto proximityLoss = [&] (int c, int t) {
            const auto near = impulseResponse (cab, soloMicA (c, t, 0.5f, 0.0f), fs);
            const auto far = impulseResponse (cab, soloMicA (c, t, 0.5f, 1.0f), fs);
            return relativeBandDb (near, fs, 40.0, 150.0) - relativeBandDb (far, fs, 40.0, 150.0);
        };
        for (int c = 0; c < 3; ++c)
        {
            const double dyn = proximityLoss (c, kDynClassic);
            const double ribbon = proximityLoss (c, kRibbonSmooth);
            const double omni = proximityLoss (c, kCondPrecision);
            INFO ("cab " << c << ": dynamic " << dyn << " dB, ribbon " << ribbon << " dB, omni "
                         << omni << " dB");
            CHECK (dyn > 3.0);
            CHECK (ribbon > dyn);
            CHECK (omni < dyn);
        }

        // Monotonic in distance, and the overall level falls only a little.
        double previous = 1e9;
        for (float d : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const auto ir = impulseResponse (cab, soloMicA (2, kDynClassic, 0.5f, d), fs);
            const double lows = bandDb (ir, fs, 40.0, 150.0);
            CHECK (lows < previous);
            previous = lows;
        }
        const auto near = impulseResponse (cab, soloMicA (2, kDynClassic, 0.5f, 0.0f), fs);
        const auto far = impulseResponse (cab, soloMicA (2, kDynClassic, 0.5f, 1.0f), fs);
        const double drop = bandDb (near, fs, 1500.0, 4000.0) - bandDb (far, fs, 1500.0, 4000.0);
        CHECK (drop > 1.0);
        CHECK (drop < 9.0);
    }

    SECTION ("mic voicings order as described")
    {
        const auto hf = [&] (int t) {
            return relativeBandDb (impulseResponse (cab, soloMicA (1, t), fs), fs, 4000.0, 7000.0);
        };
        CHECK (hf (kDynOffAxis) < hf (kDynClassic) - 2.0);
        CHECK (hf (kRibbonSmooth) < hf (kDynClassic) - 3.0);
        CHECK (hf (kRibbonDark) < hf (kRibbonSmooth) - 2.0);
        CHECK (hf (2) > hf (kDynClassic)); // Dyn Bright
    }
}

TEST_CASE ("Distance moves the arrival time by up to about a millisecond", "[cab]")
{
    const double fs = 96000.0;
    CabSim cab;
    // Lag of the cross-correlation peak between the near and far impulse responses.
    const auto near = impulseResponse (cab, soloMicA (0, kCondPrecision, 0.5f, 0.0f), fs, 4096);
    const auto far = impulseResponse (cab, soloMicA (0, kCondPrecision, 0.5f, 1.0f), fs, 4096);
    int bestLag = 0;
    double best = -1e9;
    for (int lag = 0; lag < 300; ++lag)
    {
        double acc = 0.0;
        for (size_t i = 0; i + static_cast<size_t> (lag) < far.size(); ++i)
            acc += static_cast<double> (near[i]) * far[i + static_cast<size_t> (lag)];
        if (acc > best)
        {
            best = acc;
            bestLag = lag;
        }
    }
    const double extra = bestLag / fs;
    INFO ("extra arrival " << extra * 1000.0 << " ms");
    CHECK (extra > 0.0008);
    CHECK (extra < 0.0011);
}

TEST_CASE ("Mono fold of two mics comb-filters with distance and cancels when inverted", "[cab]")
{
    const double fs = 48000.0;
    CabSim cab;

    auto both = soloMicA (2, kDynClassic, 0.5f, 0.0f);
    both.mics[1] = both.mics[0];
    both.mics[1].enabled = true;

    SECTION ("different distances: deep notches in the fold")
    {
        both.mics[1].distance = 0.6f;
        auto onlyA = both;
        onlyA.mics[1].enabled = false;
        auto onlyB = both;
        onlyB.mics[0].enabled = false;
        const auto a = impulseResponse (cab, onlyA, fs);
        const auto b = impulseResponse (cab, onlyB, fs);
        const auto fold = impulseResponse (cab, both, fs);

        double deepest = 0.0;
        for (double hz = 300.0; hz <= 3000.0; hz += 2.0)
        {
            const double coherent =
                std::abs (responseAt (a, fs, hz)) + std::abs (responseAt (b, fs, hz));
            const double db = 20.0 * std::log10 (std::abs (responseAt (fold, fs, hz)) / coherent);
            deepest = std::min (deepest, db);
        }
        CHECK (deepest < -10.0);
    }

    SECTION ("identical mics with B inverted cancel")
    {
        both.mics[1].invertPolarity = true;
        const auto fold = impulseResponse (cab, both, fs);
        auto onlyA = both;
        onlyA.mics[1].enabled = false;
        const auto a = impulseResponse (cab, onlyA, fs);
        CHECK (rms (fold) < rms (a) * 1.0e-6f);

        // Also with a running signal (not bit-exact: fused multiply-adds round differently).
        std::vector<float> x (8192);
        fillNoise (x, 0.5f, 2);
        cab.prepare ({ fs, 64, 2 });
        cab.setSettings (onlyA);
        cab.reset();
        const auto [al, ar] = run (cab, x, 64, [] (int) {});
        cab.setSettings (both);
        cab.reset();
        const auto [l, r] = run (cab, x, 64, [] (int) {});
        std::vector<float> sum (x.size());
        for (size_t i = 0; i < x.size(); ++i)
            sum[i] = l[i] + r[i];
        CHECK (rms (sum) < rms (al) * 1.0e-6f);
    }
}

TEST_CASE ("Panning is constant-power and per mic", "[cab]")
{
    const double fs = 48000.0;
    CabSim cab;
    cab.prepare ({ fs, 256, 2 });
    auto s = soloMicA (1, kDynClassic);
    std::vector<float> x (8192);
    fillNoise (x, 0.5f, 9);

    s.mics[0].pan = -1.0f;
    cab.setSettings (s);
    cab.reset();
    auto [l, r] = run (cab, x, 256, [] (int) {});
    CHECK (rms (r) < rms (l) * 1.0e-6f);

    s.mics[0].pan = 0.0f;
    cab.setSettings (s);
    cab.reset();
    auto [lc, rc] = run (cab, x, 256, [] (int) {});
    CHECK (rms (lc) == Approx (rms (l) * 0.70710678f).epsilon (1e-3));
    CHECK (rms (rc) == Approx (rms (lc)).epsilon (1e-4));
}

TEST_CASE ("Room ambience adds a small-room tail that rings out after it is switched off", "[cab]")
{
    const double fs = 48000.0;
    CabSim cab;

    SECTION ("decay time")
    {
        auto withRoom = soloMicA (0, kDynClassic);
        withRoom.mics[0].enabled = false; // the room alone
        withRoom.mics[0].roomEnabled = true;
        withRoom.mics[0].roomLevelDb = 0.0f;
        const auto ir = impulseResponse (cab, withRoom, fs, static_cast<int> (1.5 * fs));
        const double rt = reverbTime (ir, fs);
        INFO ("RT " << rt << " s");
        CHECK (rt > 0.25);
        CHECK (rt < 0.6);
        // The room starts with early reflections, not at time zero.
        float early = 0.0f;
        for (int i = 0; i < static_cast<int> (0.002 * fs); ++i)
            early = std::max (early, std::abs (ir[static_cast<size_t> (i)]));
        CHECK (early < 1.0e-6f);
    }

    SECTION ("spill-over, then true silence")
    {
        auto s = soloMicA (2, kDynClassic);
        s.mics[0].enabled = false;
        s.mics[0].roomEnabled = true;
        cab.prepare ({ fs, 128, 2 });
        cab.setSettings (s);
        cab.reset();
        std::vector<float> x (static_cast<size_t> (2.5 * fs), 0.0f);
        std::vector<float> burst (static_cast<size_t> (0.3 * fs));
        fillNoise (burst, 0.5f, 4);
        std::copy (burst.begin(), burst.end(), x.begin());
        const int off = static_cast<int> (0.3 * fs);
        const auto [l, r] = run (cab, x, 128, [&] (int pos) {
            if (pos >= off && s.mics[0].roomEnabled)
            {
                s.mics[0].roomEnabled = false;
                cab.setSettings (s);
            }
        });
        const auto window = [&] (double t0, double t1) {
            return rms (l.data() + static_cast<size_t> (t0 * fs),
                        static_cast<int> ((t1 - t0) * fs));
        };
        CHECK (window (0.32, 0.4) > 1.0e-3f);                   // the tail keeps ringing
        CHECK (window (0.32, 0.4) > window (0.6, 0.7) * 10.0f); // and decays
        CHECK (window (2.0, 2.5) == 0.0f);                      // then the room is switched off
    }
}

TEST_CASE ("Disabled mics are silent and the cab costs nothing when everything is off", "[cab]")
{
    CabSim cab;
    cab.prepare ({ 48000.0, 256, 2 });
    CabSim::Settings s;
    s.mics[0].enabled = false;
    s.mics[1].enabled = false;
    cab.setSettings (s);
    cab.reset();
    std::vector<float> x (4096);
    fillNoise (x, 1.0f, 3);
    const auto [l, r] = run (cab, x, 256, [] (int) {});
    CHECK (peak (l) == 0.0f);
    CHECK (peak (r) == 0.0f);
}

TEST_CASE ("Automating every control is click-free", "[cab]")
{
    const double fs = 48000.0;
    const int block = 64;
    CabSim cab;
    cab.prepare ({ fs, block, 2 });

    // Custom IR on mic A / 2x12 so type changes also cross into and out of convolution.
    std::vector<float> ir (3000);
    fillNoise (ir, 1.0f, 8);
    for (int i = 0; i < 3000; ++i)
        ir[static_cast<size_t> (i)] *= std::exp (-static_cast<float> (i) / 200.0f);
    cab.setCustomIr (0, 1, CabSim::makeKernel (ir.data(), 3000, fs, fs, block));

    CabSim::Settings s;
    s.mics[1].type = kRibbonSmooth;
    s.mics[1].pan = 0.3f;
    cab.setSettings (s);
    cab.reset();

    const int total = static_cast<int> (6.0 * fs);
    std::vector<float> x (static_cast<size_t> (total));
    fillSine (x, 110.0, fs, 0.5f);

    const auto automate = [&] (int pos) {
        const double t = pos / fs;
        auto& a = s.mics[0];
        auto& b = s.mics[1];
        a.position = static_cast<float> (0.5 + 0.5 * std::sin (kTwoPiD * 0.31 * t));
        a.distance = static_cast<float> (0.5 + 0.5 * std::sin (kTwoPiD * 0.23 * t));
        b.position = static_cast<float> (0.5 + 0.5 * std::sin (kTwoPiD * 0.47 * t + 1.0));
        b.distance = static_cast<float> (0.5 + 0.5 * std::sin (kTwoPiD * 0.53 * t + 2.0));
        a.levelDb = static_cast<float> (-10.0 + 10.0 * std::sin (kTwoPiD * 0.7 * t));
        b.pan = static_cast<float> (std::sin (kTwoPiD * 0.9 * t));
        b.roomLevelDb = static_cast<float> (-20.0 + 14.0 * std::sin (kTwoPiD * 0.4 * t));
        s.cab = static_cast<int> (t / 0.8) % 3;
        a.type = static_cast<int> (t / 0.35) % CabSim::kNumMicTypes;
        b.type = (static_cast<int> (t / 0.5) * 3) % 10;
        b.invertPolarity = static_cast<int> (t / 1.3) % 2 == 1;
        a.enabled = static_cast<int> (t / 1.7) % 3 != 2;
        b.roomEnabled = static_cast<int> (t / 0.9) % 2 == 1;
        a.roomEnabled = static_cast<int> (t / 1.1) % 2 == 0;
        cab.setSettings (s);
    };
    const auto [l, r] = run (cab, x, block, automate);
    REQUIRE (allFinite (l));

    // The output of a 110 Hz tone has (almost) nothing above 2.5 kHz unless something clicks.
    const auto clickiness = [&] (std::vector<float> y) {
        std::array<Biquad, 4> hp;
        for (auto& f : hp)
            f.setCoeffs (design::highpass (fs, 2500.0, 0.7071));
        for (auto& f : hp)
            f.process (AudioView (y.data(), static_cast<int> (y.size())));
        const size_t skip = static_cast<size_t> (0.2 * fs);
        float hfPeak = 0.0f;
        for (size_t i = skip; i < y.size(); ++i)
            hfPeak = std::max (hfPeak, std::abs (y[i]));
        return hfPeak;
    };
    const float level = std::max (rms (l), rms (r));
    const float clicksL = clickiness (l), clicksR = clickiness (r);
    INFO ("level " << level << ", HF peaks " << clicksL << " / " << clicksR);
    // -80 dB: a hard parameter jump or a 32-sample coefficient staircase measures -55..-65 dB.
    CHECK (clicksL < level * 1.0e-4f);
    CHECK (clicksR < level * 1.0e-4f);
    cab.collectGarbage();
}
