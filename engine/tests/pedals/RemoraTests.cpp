// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DriveTestUtils.h"
#include "sedef/core/Oversampler.h"
#include "sedef/dsp/Remora.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;
using dsp::Remora;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kGe = Remora::Germanium;
constexpr int kSi = Remora::Silicon;

void run (Remora& r, AudioView v)
{
    r.process (v);
}

/// Steady sine through a fresh pedal; the last half is analysed.
std::vector<float> sineThrough (const Remora::Settings& s, double hz, float amp, double fs = kFs)
{
    Remora r;
    r.prepare ({ fs, 512, 1 });
    r.setSettings (s);
    test::Buffer b (1, 65536);
    test::fillSine (b[0], hz, fs, amp);
    test::processInBlocks (b, 512, [&] (AudioView v) { r.process (v); });
    return std::vector<float> (b[0].begin() + 32768, b[0].end());
}

double thdOf (const Remora::Settings& s, double hz, float amp)
{
    const auto out = sineThrough (s, hz, amp);
    return test::thd (out.data(), static_cast<int> (out.size()), hz, kFs);
}

test::Harmonics harmonicsAt (const Remora::Settings& s, double hz, float amp)
{
    const auto out = sineThrough (s, hz, amp);
    return test::harmonicsOf (out.data(), static_cast<int> (out.size()), hz, kFs);
}

/// Gain (dB) of the fundamental for a sine of the given level.
double toneGainDb (const Remora::Settings& s, double hz, float amp)
{
    return test::db (harmonicsAt (s, hz, amp).h[1] / amp);
}

Remora::Settings settings (float fuzz, float bias, float guitar, int transistor)
{
    return { fuzz, 0.5f, bias, guitar, transistor };
}
} // namespace

TEST_CASE ("Remora output stays finite at every rate, block size and setting", "[pedals][remora]")
{
    test::checkFiniteEverywhere<Remora> ([] (Random& r) {
        return Remora::Settings { r.unipolar(), r.unipolar(), r.unipolar(), r.unipolar(),
                                  r.unipolar() < 0.5f ? kGe : kSi };
    });
}

TEST_CASE ("Remora passes silence as silence", "[pedals][remora]")
{
    for (int t : { kGe, kSi })
    {
        CHECK (test::silenceOutputPeak<Remora> (settings (1.0f, 0.5f, 1.0f, t), run) == 0.0f);
        CHECK (test::silenceOutputPeak<Remora> (settings (1.0f, 0.0f, 1.0f, t), run) == 0.0f);
    }
}

TEST_CASE ("Remora does not depend on how the host slices the audio", "[pedals][remora]")
{
    for (double fs : test::kDriveRates)
        for (int t : { kGe, kSi })
            CHECK (test::blockSizeDeviation<Remora> (settings (0.8f, 0.3f, 0.9f, t), fs) < 1.0e-5);
}

TEST_CASE ("Remora cleans up as the guitar's volume rolls back", "[pedals][remora]")
{
    // The fuzz's low input impedance against the guitar's pot and pickup: rolling the guitar back
    // drops the drive far more than the output, so the fuzz turns into a crunch and then nearly
    // clean, as the real circuit does. Germanium, full fuzz, noon bias, a 196 Hz note at 0.2 V.
    double previous = 1.0;
    for (float guitar : { 1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f })
    {
        const double d = thdOf (settings (1.0f, 0.5f, guitar, kGe), 196.0, 0.2f);
        CHECK (d < previous);
        previous = d;
    }
    CHECK (thdOf (settings (1.0f, 0.5f, 1.0f, kGe), 196.0, 0.2f) > 0.35); // full fuzz on 10
    CHECK (thdOf (settings (1.0f, 0.5f, 0.7f, kGe), 196.0, 0.2f) < 0.2);  // crunch on 7
    CHECK (thdOf (settings (1.0f, 0.5f, 0.5f, kGe), 196.0, 0.2f) < 0.1);  // nearly clean on 5

    // It cleans up without collapsing: on 5 the note is at most 8 dB quieter than on 10.
    const auto level = [] (float guitar) {
        const auto out = sineThrough (settings (1.0f, 0.5f, guitar, kGe), 196.0, 0.2f);
        return test::db (test::rms (out));
    };
    CHECK (level (1.0f) - level (0.5f) < 8.0);
    CHECK (level (1.0f) - level (0.5f) > 1.0);
}

TEST_CASE ("Remora gets brighter as the guitar's volume rolls back", "[pedals][remora]")
{
    // On 10 the pickup's inductance works into the fuzz's few kOhm: a low-pass near 900 Hz. Rolled
    // back, the pot's resistance isolates the coil and the corner moves up. Small signals, so the
    // pair stays linear and only the input network differs.
    const auto tilt = [] (float guitar) {
        const auto s = settings (0.0f, 0.5f, guitar, kGe);
        return toneGainDb (s, 3000.0, 1.0e-4f) - toneGainDb (s, 200.0, 1.0e-4f);
    };
    CHECK (tilt (0.6f) - tilt (1.0f) > 6.0);
    CHECK (tilt (0.8f) > tilt (1.0f));
}

TEST_CASE ("Remora silicon cleans up less than germanium", "[pedals][remora]")
{
    // More gain and a higher input impedance: rolled back to 5, silicon is still a fuzz.
    CHECK (thdOf (settings (1.0f, 0.5f, 0.5f, kSi), 196.0, 0.2f) >
           3.0 * thdOf (settings (1.0f, 0.5f, 0.5f, kGe), 196.0, 0.2f));
}

TEST_CASE ("Remora's bias moves the clipping from symmetric to lopsided", "[pedals][remora]")
{
    // Noon rests the output transistor mid-swing: odd harmonics. Either way off noon one side
    // clips first: even harmonics. A 196 Hz note at 50 mV, half fuzz.
    for (int t : { kGe, kSi })
    {
        double atNoon = 0.0;
        double offNoonMin = 1.0e9;
        for (float bias : { 0.1f, 0.3f, 0.5f, 0.7f, 0.9f })
        {
            const double r =
                harmonicsAt (settings (0.5f, bias, 1.0f, t), 196.0, 0.05f).evenOverOdd();
            if (bias == 0.5f)
                atNoon = r;
            else
                offNoonMin = std::min (offNoonMin, r);
        }
        CHECK (atNoon < 0.25);
        CHECK (offNoonMin > 2.0 * atNoon);
        CHECK (harmonicsAt (settings (0.5f, 0.1f, 1.0f, t), 196.0, 0.05f).evenOverOdd() > 1.0);
        CHECK (harmonicsAt (settings (0.5f, 0.9f, 1.0f, t), 196.0, 0.05f).evenOverOdd() > 0.9);
    }
}

TEST_CASE ("Remora starved is gated: small signals lose their gain", "[pedals][remora]")
{
    // Starving the output transistor of current takes its small-signal gain while loud notes
    // still swing it: quiet playing and decays are cut off, the gated, sputtery sound.
    const auto quiet = [] (float bias) {
        return toneGainDb (settings (1.0f, bias, 1.0f, kGe), 500.0, 5.0e-4f);
    };
    const auto loud = [] (float bias) {
        return toneGainDb (settings (1.0f, bias, 1.0f, kGe), 500.0, 0.2f);
    };
    CHECK (quiet (0.5f) - quiet (0.0f) > 15.0);
    CHECK (std::abs (loud (0.5f) - loud (0.0f)) < 3.0);
    CHECK (quiet (0.0f) < quiet (0.2f));
    CHECK (quiet (0.2f) < quiet (0.5f));
}

TEST_CASE ("Remora starved chokes the decay after a loud note", "[pedals][remora]")
{
    // The bias servo: a loud, lopsided note pulls the operating point towards cutoff, so a quiet
    // note right after it comes out weaker than the same note on its own, and recovers.
    const double fs = kFs;
    const auto render = [fs] (bool burst) {
        Remora r;
        r.prepare ({ fs, 512, 1 });
        r.setSettings (settings (1.0f, 0.15f, 1.0f, kGe));
        test::Buffer b (1, static_cast<int> (1.2 * fs));
        for (int i = 0; i < b.frames(); ++i)
        {
            const double t = i / fs;
            const float amp = burst && t < 0.4 ? 0.3f : 0.004f;
            b[0][static_cast<size_t> (i)] =
                amp * static_cast<float> (std::sin (kTwoPiD * 330.0 * t));
        }
        test::processInBlocks (b, 512, [&] (AudioView v) { r.process (v); });
        return b[0];
    };
    const auto alone = render (false);
    const auto after = render (true);
    const auto window = [fs] (const std::vector<float>& v, double from, double to) {
        const auto a = static_cast<size_t> (from * fs);
        return test::rms (v.data() + a, static_cast<int> ((to - from) * fs));
    };
    // Right after the burst: choked.
    CHECK (test::db (window (after, 0.42, 0.47) / window (alone, 0.42, 0.47)) < -3.0);
    // Half a second later: recovered.
    CHECK (test::db (window (after, 1.0, 1.15) / window (alone, 1.0, 1.15)) ==
           Approx (0.0).margin (1.0));
}

TEST_CASE ("Remora germanium is softer and darker, silicon harder and brighter", "[pedals][remora]")
{
    // Harder knee: more high-order harmonics against the third for the same note.
    const auto ge = harmonicsAt (settings (0.5f, 0.5f, 1.0f, kGe), 196.0, 0.05f);
    const auto si = harmonicsAt (settings (0.5f, 0.5f, 1.0f, kSi), 196.0, 0.05f);
    CHECK (si.h[5] / si.h[3] > 3.0 * (ge.h[5] / ge.h[3]));

    // Softer onset: at minimum fuzz germanium distorts less than silicon.
    CHECK (thdOf (settings (0.0f, 0.5f, 1.0f, kGe), 196.0, 0.2f) <
           0.75 * thdOf (settings (0.0f, 0.5f, 1.0f, kSi), 196.0, 0.2f));

    // More gain: silicon's small-signal gain is several dB higher.
    CHECK (toneGainDb (settings (0.5f, 0.5f, 1.0f, kSi), 500.0, 5.0e-4f) -
               toneGainDb (settings (0.5f, 0.5f, 1.0f, kGe), 500.0, 5.0e-4f) >
           6.0);

    // Brighter: a larger share of the output above 3 kHz with the guitar-like tone.
    const auto highShare = [] (int t) {
        Remora r;
        r.prepare ({ kFs, 512, 1 });
        r.setSettings (settings (1.0f, 0.5f, 1.0f, t));
        test::Buffer b (1, 48000);
        test::fillGuitarTone (b[0], kFs);
        test::processInBlocks (b, 512, [&] (AudioView v) { r.process (v); });
        return test::db (test::rmsAbove (b[0], 3000.0, kFs, 24000) / test::rms (b[0], 24000));
    };
    CHECK (highShare (kSi) - highShare (kGe) > 2.0);
}

TEST_CASE ("Remora volume 0.5 is close to unity with the guitar on full", "[pedals][remora]")
{
    for (int t : { kGe, kSi })
        for (float fuzz : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            Remora r;
            r.prepare ({ kFs, 512, 1 });
            r.setSettings (settings (fuzz, 0.5f, 1.0f, t));
            test::Buffer b (1, 48000);
            test::fillGuitarTone (b[0], kFs);
            const float in = test::rms (b[0], 24000);
            test::processInBlocks (b, 512, [&] (AudioView v) { r.process (v); });
            CHECK (test::db (test::rms (b[0], 24000) / in) == Approx (0.0).margin (1.0));
        }
}

TEST_CASE ("Remora volume spans -20..+20 dB", "[pedals][remora]")
{
    const auto level = [] (float volume) {
        const auto out = sineThrough ({ 1.0f, volume, 0.5f, 1.0f, kGe }, 196.0, 0.2f);
        return test::db (test::rms (out));
    };
    CHECK (level (1.0f) - level (0.5f) == Approx (20.0).margin (0.2));
    CHECK (level (0.5f) - level (0.0f) == Approx (20.0).margin (0.2));
}

TEST_CASE ("Remora keeps aliasing low on high notes at full fuzz", "[pedals][remora]")
{
    for (double fs : { 44100.0, 48000.0 })
        for (int t : { kGe, kSi })
            for (double f0 : { 1318.5, 2637.0 })
            {
                Remora m;
                m.prepare ({ fs, 512, 1 });
                m.setSettings (settings (1.0f, 0.5f, 1.0f, t));
                test::Buffer b (1, 49152);
                test::fillSine (b[0], f0, fs, 0.3f);
                test::processInBlocks (b, 512, [&] (AudioView v) { m.process (v); });
                CHECK (test::worstInharmonicDb (b[0].data() + 16384, 32768, f0, fs) < -60.0);
            }
}

TEST_CASE ("Remora reports the oversampler's latency", "[pedals][remora]")
{
    for (double fs : test::kDriveRates)
    {
        Oversampler os;
        os.prepare (512, 1, Oversampler::factorLog2For (fs));
        Remora r;
        r.prepare ({ fs, 512, 1 });
        CHECK (r.latencySamples() == os.latencySamples());
    }
}

TEST_CASE ("Remora automation and switching are click-free", "[pedals][remora]")
{
    int which = 0;
    const auto check = [&which] (auto settingsAt) {
        const auto r = test::measureSweep<Remora> (settingsAt, run);
        INFO ("sweep " << which++);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return Remora::Settings { t, 0.5f, 0.5f, 1.0f, kGe }; });
    check ([] (float t) { return Remora::Settings { 0.7f, t, 0.5f, 1.0f, kGe }; });
    check ([] (float t) { return Remora::Settings { 0.7f, 0.5f, t, 1.0f, kGe }; });
    check ([] (float t) { return Remora::Settings { 0.7f, 0.5f, 0.5f, t, kGe }; });
    check ([] (float t) { return Remora::Settings { t, 1.0f - t, t, 1.0f - t, kSi }; });
    // The germanium/silicon switch, flipped halfway through.
    check (
        [] (float t) { return Remora::Settings { 0.7f, 0.5f, 0.5f, 1.0f, t < 0.5f ? kGe : kSi }; });
    check (
        [] (float t) { return Remora::Settings { 0.7f, 0.5f, 0.2f, 0.8f, t < 0.5f ? kSi : kGe }; });
}
