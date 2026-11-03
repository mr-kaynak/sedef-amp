// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../analysis/PitchTestSignals.h"
#include "PedalTestUtils.h"
#include "sedef/core/Oversampler.h"
#include "sedef/dsp/Urchin.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>

using namespace sedef;
using Catch::Approx;
using dsp::Urchin;

namespace
{
constexpr double kFs = 48000.0;

void run (Urchin& u, AudioView v)
{
    u.process (v);
}

/// The two inputs the octave depends on, both sustained (no decay) so the measurement window sees
/// one spectrum. "Neck": A4 (440 Hz) fretted above the 12th fret and heard by the neck pickup,
/// which then sits near the middle of the vibrating string, where the even harmonics have their
/// nodes (weak even harmonics, nothing above 2 kHz). "Bridge": open A (110 Hz) on the bridge
/// pickup: bright, with a weak fundamental against its harmonics.
std::vector<float> neckNote (float peak = 0.3f, double fs = kFs)
{
    std::vector<float> v (static_cast<size_t> (1.5 * fs), 0.0f);
    test::Pluck p;
    p.f1 = 440.0;
    p.pluckPosition = 0.45;
    p.maxPartialHz = 2000.0;
    p.decaySeconds = 1.0e4;
    p.glideCents = 0.0;
    p.pickNoise = 0.0f;
    p.peak = peak;
    test::addPluck (v, fs, 0, p);
    return v;
}

std::vector<float> bridgeNote (float peak = 0.3f, double fs = kFs)
{
    std::vector<float> v (static_cast<size_t> (1.5 * fs), 0.0f);
    test::Pluck p;
    p.f1 = 110.0;
    p.pluckPosition = 0.1;
    p.fundamentalGain = 0.5;
    p.decaySeconds = 1.0e4;
    p.glideCents = 0.0;
    p.pickNoise = 0.0f;
    p.peak = peak;
    test::addPluck (v, fs, 0, p);
    return v;
}

std::vector<float> render (std::vector<float> in, const Urchin::Settings& s, double fs = kFs)
{
    Urchin u;
    u.prepare ({ fs, 512, 1 });
    u.setSettings (s);
    test::Buffer b (1, static_cast<int> (in.size()));
    b[0] = std::move (in);
    test::processInBlocks (b, 512, [&] (AudioView v) { u.process (v); });
    return b[0];
}

/// Level of the octave (2f) against the note (f) at the output, dB.
double octaveDb (const std::vector<float>& out, double f, double fs = kFs)
{
    const auto from = static_cast<size_t> (0.5 * fs);
    const int n = 32768;
    REQUIRE (out.size() >= from + n);
    return test::db (test::toneMagnitude (out.data() + from, n, 2.0 * f, fs) /
                     test::toneMagnitude (out.data() + from, n, f, fs));
}

/// Strongest component that is not a harmonic of f0, against the strongest harmonic (dB): with
/// the octave up, a sine's own frequency may be all but gone, so the fundamental is no reference.
double aliasingDb (const std::vector<float>& out, double f0, double fs = kFs)
{
    const float* d = out.data() + 16384;
    const int n = 32768;
    double strongest = 0.0;
    for (int k = 1; k * f0 < 0.45 * fs; ++k)
        strongest = std::max (strongest, test::toneMagnitude (d, n, k * f0, fs));
    double worst = 0.0;
    for (double hz = 40.0; hz < std::min (20000.0, 0.45 * fs); hz += 5.0)
    {
        const double k = std::round (hz / f0);
        if (std::abs (hz - k * f0) < 60.0)
            continue;
        worst = std::max (worst, test::toneMagnitude (d, n, hz, fs));
    }
    return test::db (worst / strongest);
}

double levelDb (const Urchin::Settings& s)
{
    test::Buffer b (1, 48000);
    test::fillGuitarTone (b[0], kFs);
    const float in = test::rms (b[0], 24000);
    const auto out = render (b[0], s);
    return test::db (test::rms (out, 24000) / in);
}

double sineOut (const Urchin::Settings& s, double hz, float amp, double* thd = nullptr)
{
    test::Buffer b (1, 49152);
    test::fillSine (b[0], hz, kFs, amp);
    const auto out = render (b[0], s);
    if (thd != nullptr)
        *thd = test::thd (out.data() + 16384, 32768, hz, kFs);
    return test::rms (out.data() + 16384, 32768);
}
} // namespace

TEST_CASE ("Urchin output stays finite for random settings and full-scale noise",
           "[pedals][urchin]")
{
    test::checkFiniteWithRandomSettings<Urchin> (
        [] (Random& r) {
            return Urchin::Settings { r.unipolar(), r.unipolar(), r.unipolar(), r.unipolar() };
        },
        run);
}

TEST_CASE ("Urchin passes silence as silence", "[pedals][urchin]")
{
    CHECK (test::silenceOutputPeak<Urchin> (Urchin::Settings {}, run) == 0.0f);
    CHECK (test::silenceOutputPeak<Urchin> (Urchin::Settings { 1.0f, 1.0f, 1.0f, 1.0f }, run) ==
           0.0f);
}

TEST_CASE ("Urchin's octave grows with the Octave knob", "[pedals][urchin]")
{
    // 2f against f at the output, for the neck note and a pure sine: at Octave 0 only the plain
    // fuzz's own second harmonic (an asymmetric stage), then more octave with every step.
    test::Buffer sine (1, 72000);
    test::fillSine (sine[0], 440.0, kFs, 0.2f);
    double previousNeck = -1.0e9, previousSine = -1.0e9;
    for (float octave : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        INFO ("octave " << octave);
        const Urchin::Settings s { 0.5f, octave, 0.5f, 0.5f };
        const double neck = octaveDb (render (neckNote(), s), 440.0);
        const double pure = octaveDb (render (sine[0], s), 440.0);
        CHECK (neck > previousNeck);
        CHECK (pure > previousSine);
        previousNeck = neck;
        previousSine = pure;
    }
    CHECK (octaveDb (render (neckNote(), { 0.5f, 0.0f, 0.5f, 0.5f }), 440.0) < -10.0);
}

TEST_CASE ("Urchin's octave is strongest on clean notes high on the neck", "[pedals][urchin]")
{
    // Full-wave rectification folds a sine into its octave: a near-sinusoidal neck note comes out
    // an octave up, the note itself far below it. A bright bridge note's harmonics intermodulate
    // in the rectifier and keep the note audible: its octave stands much less clear.
    const Urchin::Settings s { 0.5f, 1.0f, 0.5f, 0.5f };
    const double neck = octaveDb (render (neckNote(), s), 440.0);
    const double bridge = octaveDb (render (bridgeNote(), s), 110.0);
    INFO ("neck " << neck << " dB, bridge " << bridge << " dB");
    CHECK (neck > 12.0);
    CHECK (bridge < neck - 10.0);

    // A pure sine is the limit: no energy at f at all, only its octave and the octave's harmonics.
    test::Buffer b (1, 72000);
    test::fillSine (b[0], 440.0, kFs, 0.2f);
    CHECK (octaveDb (render (b[0], s), 440.0) > 40.0);
}

TEST_CASE ("Urchin saturates", "[pedals][urchin]")
{
    // THD rises with the fuzz; at full fuzz the output barely follows the input (20 dB more in,
    // under 3 dB more out) and its peak stays bounded.
    double previous = 0.0;
    for (float fuzz : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        INFO ("fuzz " << fuzz);
        double thd = 0.0;
        sineOut ({ fuzz, 0.0f, 1.0f, 0.5f }, 220.0, 0.05f, &thd);
        CHECK (thd > previous);
        previous = thd;
    }
    const Urchin::Settings full { 1.0f, 1.0f, 0.5f, 0.5f };
    const double quiet = sineOut (full, 330.0, 0.02f);
    const double loud = sineOut (full, 330.0, 0.2f);
    CHECK (test::db (loud / quiet) < 3.0);
    for (float amp : { 0.1f, 1.0f, 10.0f })
    {
        test::Buffer b (1, 24000);
        test::fillNoise (b[0], amp, 5);
        CHECK (test::peak (render (b[0], { 1.0f, 1.0f, 1.0f, 0.5f })) < 1.0f);
    }
}

TEST_CASE ("Urchin's tone darkens counter-clockwise", "[pedals][urchin]")
{
    const auto highs = [] (float tone) {
        const auto out = render (neckNote(), { 0.5f, 1.0f, tone, 0.5f });
        return test::rmsAbove (out, 4000.0, kFs, 24000) / test::rms (out, 24000);
    };
    CHECK (highs (0.0f) < 0.5f * highs (0.5f));
    CHECK (highs (0.5f) < 0.7f * highs (1.0f));
}

TEST_CASE ("Urchin level 0.5 is close to unity across the fuzz range", "[pedals][urchin]")
{
    for (float fuzz : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        for (float octave : { 0.0f, 1.0f })
        {
            INFO ("fuzz " << fuzz << ", octave " << octave);
            CHECK (levelDb ({ fuzz, octave, 0.5f, 0.5f }) == Approx (0.0).margin (2.0));
        }
}

TEST_CASE ("Urchin keeps aliasing low on high notes", "[pedals][urchin]")
{
    // Two nonlinear stages (rectifier and fuzz), both oversampled and anti-aliased. Every product
    // of a note is a harmonic of it, so anything else is aliasing.
    for (double f0 : { 1318.5, 2637.0 })
        for (float octave : { 0.0f, 1.0f })
        {
            INFO ("note " << f0 << ", octave " << octave);
            test::Buffer b (1, 49152);
            test::fillSine (b[0], f0, kFs, 0.3f);
            const auto out = render (b[0], { 1.0f, octave, 1.0f, 0.5f });
            CHECK (aliasingDb (out, f0) < -60.0);
        }
}

TEST_CASE ("Urchin reports the oversampler's latency", "[pedals][urchin]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        Oversampler os;
        os.prepare (512, 1, Oversampler::factorLog2For (fs));
        Urchin u;
        u.prepare ({ fs, 512, 1 });
        CHECK (u.latencySamples() == os.latencySamples());
    }
}

TEST_CASE ("Urchin automation is click-free", "[pedals][urchin]")
{
    const auto check = [] (auto settingsAt) {
        const auto r = test::measureSweep<Urchin> (settingsAt, run);
        CHECK (r.finite);
        CHECK (r.sweep <= 1.5f * r.reference);
    };
    check ([] (float t) { return Urchin::Settings { t, 1.0f, 0.5f, 0.5f }; });
    check ([] (float t) { return Urchin::Settings { 0.5f, t, 0.5f, 0.5f }; });
    check ([] (float t) { return Urchin::Settings { 0.5f, 1.0f, t, 0.5f }; });
    check ([] (float t) { return Urchin::Settings { 0.5f, 1.0f, 0.5f, t }; });
    check ([] (float t) { return Urchin::Settings { t, 1.0f - t, t, 1.0f - t }; });
}

TEST_CASE ("Urchin measurements", "[.report]")
{
    std::printf (
        "Urchin: octave (2f) against the note (f) at the output, dB; fuzz, tone, level noon\n");
    std::printf ("octave   neck A4   bridge A2   sine 440\n");
    for (float octave : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const Urchin::Settings s { 0.5f, octave, 0.5f, 0.5f };
        test::Buffer b (1, 72000);
        test::fillSine (b[0], 440.0, kFs, 0.2f);
        std::printf ("%4.2f   %+8.1f   %+9.1f   %+8.1f\n", static_cast<double> (octave),
                     octaveDb (render (neckNote(), s), 440.0),
                     octaveDb (render (bridgeNote(), s), 110.0),
                     octaveDb (render (b[0], s), 440.0));
    }
    std::printf ("neck note, octave 1, by picking strength:\n");
    for (float peak : { 0.01f, 0.03f, 0.1f, 0.3f })
        std::printf ("  peak %.2f: %+6.1f dB\n", static_cast<double> (peak),
                     octaveDb (render (neckNote (peak), { 0.5f, 1.0f, 0.5f, 0.5f }), 440.0));
    std::printf ("fuzz   THD (220 Hz, -26 dBFS, octave 0)   out(-14 dBFS) - out(-34 dBFS)\n");
    for (float fuzz : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        double thd = 0.0;
        sineOut ({ fuzz, 0.0f, 1.0f, 0.5f }, 220.0, 0.05f, &thd);
        const Urchin::Settings s { fuzz, 1.0f, 0.5f, 0.5f };
        std::printf ("%4.2f   %6.1f %%                           %+5.1f dB\n",
                     static_cast<double> (fuzz), 100.0 * thd,
                     test::db (sineOut (s, 330.0, 0.2f) / sineOut (s, 330.0, 0.02f)));
    }
    std::printf ("level 0.5 vs input (reference tone)\n");
    for (float fuzz : { 0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f })
        std::printf ("fuzz %.3f: octave 0 %+5.2f dB, octave 0.5 %+5.2f dB, octave 1 %+5.2f dB\n",
                     static_cast<double> (fuzz), levelDb ({ fuzz, 0.0f, 0.5f, 0.5f }),
                     levelDb ({ fuzz, 0.5f, 0.5f, 0.5f }), levelDb ({ fuzz, 1.0f, 0.5f, 0.5f }));
    for (double f0 : { 1318.5, 2637.0 })
        for (float octave : { 0.0f, 1.0f })
        {
            test::Buffer b (1, 49152);
            test::fillSine (b[0], f0, kFs, 0.3f);
            const auto out = render (b[0], { 1.0f, octave, 1.0f, 0.5f });
            std::printf ("aliasing, full fuzz, octave %.0f, %.1f Hz: %.1f dB\n",
                         static_cast<double> (octave), f0, aliasingDb (out, f0));
        }
}
