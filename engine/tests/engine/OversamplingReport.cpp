// SPDX-License-Identifier: AGPL-3.0-or-later
// Hidden: sedef_engine_tests "[.report][oversampling-report]" prints the tables in
// docs/dsp/oversampling.md: the rig's latency, and per oversampled stage the worst aliasing, the
// THD and the CPU cost, in both modes. Linear phase is 0.4.0's oversampler, so it is the "before".
#include "TestUtils.h"
#include "amp/AmpTestUtils.h"
#include "pedals/PedalTestUtils.h"
#include "sedef/Engine.h"
#include "sedef/core/Denormals.h"
#include "sedef/core/Oversampler.h"
#include "sedef/dsp/Breaker.h"
#include "sedef/dsp/Coral.h"
#include "sedef/dsp/Kelp.h"
#include "sedef/dsp/Pearl.h"
#include "sedef/dsp/Remora.h"
#include "sedef/dsp/Sargasso.h"
#include "sedef/dsp/Urchin.h"
#include "sedef/dsp/amp/AmpSection.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace sedef;

namespace
{
constexpr OversamplingMode kModes[] = { OversamplingMode::linearPhase,
                                        OversamplingMode::lowLatency };

/// Strongest component that is no harmonic of f0, in dB re the strongest harmonic (an octave
/// fuzz's fundamental can be weak, so the reference is whichever harmonic leads).
double worstAliasDb (const std::vector<float>& v, double f0, double fs)
{
    const int n = 32768;
    const float* d = v.data() + v.size() - n;
    double strongest = 0.0;
    for (int k = 1; k <= 10 && k * f0 < 0.45 * fs; ++k)
        strongest = std::max (strongest, test::toneMagnitude (d, n, k * f0, fs));
    double worst = 0.0;
    for (int k = 2; k * f0 < 40.0 * fs; ++k)
    {
        // Every image of a harmonic, folded at the oversampled or the base rate, lands on
        // |k f0 mod fs| (docs/dsp/amps.md), so those are the frequencies to measure.
        double f = std::fmod (k * f0, fs);
        if (f > 0.5 * fs)
            f = fs - f;
        const double nearest = std::round (f / f0) * f0;
        if (f < 50.0 || f > 20000.0 || std::abs (f - nearest) < 40.0)
            continue;
        worst = std::max (worst, test::toneMagnitude (d, n, f, fs));
    }
    return 20.0 * std::log10 (std::max (worst, 1.0e-15) / strongest);
}

double thdPercent (const std::vector<float>& v, double f0, double fs)
{
    const int n = 32768;
    return 100.0 * test::thd (v.data() + v.size() - n, n, f0, fs);
}

/// One oversampled stage: runs a mono signal through it in place.
struct Stage
{
    std::string label;
    std::function<void (std::vector<float>&, double, OversamplingMode)> run;
    float aliasLevel; ///< sine peak for the aliasing measurement
};

template <typename Module>
Stage pedal (const char* label, typename Module::Settings s)
{
    return { label,
             [s] (std::vector<float>& v, double fs, OversamplingMode mode) {
                 Module m;
                 m.prepare ({ fs, 512, 1, mode });
                 m.setSettings (s);
                 for (size_t i = 0; i < v.size(); i += 512)
                 {
                     float* p = v.data() + i;
                     m.process (AudioView (
                         &p, 1, static_cast<int> (std::min<size_t> (512, v.size() - i))));
                 }
             },
             0.3f };
}

Stage amp (const char* label, int model)
{
    return { label,
             [model] (std::vector<float>& v, double fs, OversamplingMode mode) {
                 dsp::AmpSection a;
                 dsp::AmpSection::Settings s;
                 s.model = model;
                 s.lagoon.volume = 1.0f;
                 s.reef.gain = 1.0f;
                 s.abyss.gain = 1.0f;
                 test::prepareAmp (a, s, fs, 512, mode);
                 test::runAmp (a, v, 512);
             },
             0.1f };
}

std::vector<Stage> stages()
{
    return {
        pedal<dsp::Remora> ("Remora", { 1.0f, 0.5f, 0.5f, 1.0f, dsp::Remora::Germanium }),
        pedal<dsp::Urchin> ("Urchin", { 1.0f, 1.0f, 1.0f, 0.5f }),
        pedal<dsp::Sargasso> ("Sargasso", { 1.0f, 0.5f, 0.5f }),
        pedal<dsp::Pearl> ("Pearl", { 1.0f, 1.0f, 0.5f, 0.0f }),
        pedal<dsp::Kelp> ("Kelp", { 1.0f, 1.0f, 0.5f }),
        pedal<dsp::Coral> ("Coral", { 1.0f, 0.0f, 0.5f }),
        pedal<dsp::Breaker> ("Breaker", { 1.0f, dsp::Breaker::Treble }),
        amp ("Lagoon", 0),
        amp ("Reef", 1),
        amp ("Abyss", 2),
    };
}
} // namespace

TEST_CASE ("Oversampling report: rig latency", "[.report][oversampling-report]")
{
    std::printf ("\n| Rate | Low latency | Linear phase |\n|---|---|---|\n");
    for (const double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        int latency[2] {};
        for (const auto mode : kModes)
        {
            Engine e;
            e.setOversampling (mode);
            e.prepare (fs, 256, 1, 2);
            latency[static_cast<int> (mode)] = e.latencySamples();
        }
        std::printf ("| %.1f kHz | %d samples, %.2f ms | %d samples, %.2f ms |\n", fs / 1000.0,
                     latency[0], latency[0] * 1000.0 / fs, latency[1], latency[1] * 1000.0 / fs);
    }
}

TEST_CASE ("Oversampling report: aliasing and THD per stage", "[.report][oversampling-report]")
{
    ScopedNoDenormals noDenormals;
    std::printf ("\nWorst aliasing (dB re the strongest harmonic), full drive, notes E6 and E7 "
                 "at 44.1 and 48 kHz; THD at A3 (220 Hz) at 48 kHz\n");
    std::printf ("| Stage | Aliasing, linear phase | Aliasing, low latency | THD, linear phase | "
                 "THD, low latency |\n|---|---|---|---|---|\n");
    for (const auto& st : stages())
    {
        double worst[2] { -300.0, -300.0 }, thd[2] {};
        for (const auto mode : kModes)
        {
            const int m = static_cast<int> (mode);
            for (const double fs : { 44100.0, 48000.0 })
                for (const double f0 : { 1318.5, 2637.0 })
                {
                    std::vector<float> v (65536);
                    test::fillSine (v, f0, fs, st.aliasLevel);
                    st.run (v, fs, mode);
                    worst[m] = std::max (worst[m], worstAliasDb (v, f0, fs));
                }
            std::vector<float> v (65536);
            test::fillSine (v, 220.0, 48000.0, 0.1f);
            st.run (v, 48000.0, mode);
            thd[m] = thdPercent (v, 220.0, 48000.0);
        }
        if (st.label == "Urchin") // the note leaves an octave up: no THD of the fundamental
            std::printf ("| %s | %.1f | %.1f | n/a | n/a |\n", st.label.c_str(), worst[1],
                         worst[0]);
        else
            std::printf ("| %s | %.1f | %.1f | %.2f %% | %.2f %% |\n", st.label.c_str(), worst[1],
                         worst[0], thd[1], thd[0]);
    }
}

TEST_CASE ("Oversampling report: CPU per stage", "[.report][oversampling-report]")
{
    ScopedNoDenormals noDenormals;
    std::printf ("\nCPU, ns per sample at 48 kHz mono, 64-sample blocks (a guitar strum)\n");
    std::printf ("| Stage | Linear phase | Low latency |\n|---|---|---|\n");
    const auto strum = test::guitarStrum (48000.0, 4.0);
    for (const auto& st : stages())
    {
        double ns[2] {};
        for (const auto mode : kModes)
        {
            // The stage's own loop, timed over 40 s of audio in 64-sample blocks.
            std::vector<float> v (strum.size());
            double best = 1.0e30;
            for (int round = 0; round < 3; ++round)
            {
                std::copy (strum.begin(), strum.end(), v.begin());
                const auto t0 = std::chrono::steady_clock::now();
                for (int rep = 0; rep < 10; ++rep)
                    st.run (v, 48000.0, mode);
                const auto t1 = std::chrono::steady_clock::now();
                best = std::min (best, std::chrono::duration<double, std::nano> (t1 - t0).count() /
                                           (10.0 * static_cast<double> (v.size())));
            }
            ns[static_cast<int> (mode)] = best;
        }
        std::printf ("| %s | %.0f | %.0f |\n", st.label.c_str(), ns[1], ns[0]);
    }
    // The oversampler alone: up and down with nothing in between.
    for (const int f : { 2, 1 })
    {
        double ns[2] {};
        for (const auto mode : kModes)
        {
            Oversampler os;
            os.prepare (64, 1, f, mode);
            ns[static_cast<int> (mode)] = test::nsPerSample ([&os] (AudioView v) {
                os.upsample (v);
                os.downsample (v);
            });
        }
        std::printf ("| Oversampler alone, %dx | %.0f | %.0f |\n", 1 << f, ns[1], ns[0]);
    }
}

TEST_CASE ("Oversampling report: small-signal response, low latency against linear phase",
           "[.report][oversampling-report]")
{
    ScopedNoDenormals noDenormals;
    std::printf (
        "\nLevel difference (dB) low latency minus linear phase, sine at -40 dBFS, 48 kHz\n");
    std::printf ("| Stage |");
    const double freqs[] = { 100, 1000, 3000, 5000, 8000, 10000, 12000, 15000, 18000 };
    for (double f : freqs)
        std::printf (" %.0f Hz |", f);
    std::printf ("\n");
    for (const auto& st : stages())
    {
        if (st.label == "Urchin")
            continue; // its rectifier leaves no small-signal response at the input's frequency
        std::printf ("| %s |", st.label.c_str());
        for (double f : freqs)
        {
            double level[2] {};
            for (const auto mode : kModes)
            {
                std::vector<float> v (32768);
                test::fillSine (v, f, 48000.0, 0.01f);
                st.run (v, 48000.0, mode);
                level[static_cast<int> (mode)] =
                    test::toneMagnitude (v.data() + 16384, 16384, f, 48000.0);
            }
            std::printf (" %+.2f |", 20.0 * std::log10 (level[0] / level[1]));
        }
        std::printf ("\n");
    }
}

TEST_CASE ("Oversampling report: harmonic spectrum, low latency against linear phase",
           "[.report][oversampling-report]")
{
    // The tone of each stage driven like a guitar: a sine at -20 dBFS, the first ten harmonics
    // (those within 60 dB of the strongest) compared between the modes.
    ScopedNoDenormals noDenormals;
    std::printf (
        "\nLargest harmonic level difference (dB), -20 dBFS sine at 48 kHz, harmonics within 60 dB "
        "of the strongest\n| Stage | 110 Hz | 440 Hz | 1320 Hz |\n|---|---|---|---|\n");
    for (const auto& st : stages())
    {
        std::printf ("| %s |", st.label.c_str());
        for (const double f0 : { 110.0, 440.0, 1320.0 })
        {
            std::array<std::array<double, 10>, 2> h {};
            for (const auto mode : kModes)
            {
                std::vector<float> v (65536);
                test::fillSine (v, f0, 48000.0, 0.1f);
                st.run (v, 48000.0, mode);
                for (int k = 1; k <= 10; ++k)
                    h[static_cast<size_t> (mode)][static_cast<size_t> (k - 1)] =
                        k * f0 < 20000.0
                            ? test::toneMagnitude (v.data() + 32768, 32768, k * f0, 48000.0)
                            : 0.0;
            }
            const double top = *std::max_element (h[1].begin(), h[1].end());
            double worst = 0.0;
            for (size_t k = 0; k < 10; ++k)
                if (h[1][k] > top * 1.0e-3)
                    worst = std::max (worst, std::abs (20.0 * std::log10 (h[0][k] / h[1][k])));
            std::printf (" %.3f |", worst);
        }
        std::printf ("\n");
    }
}

TEST_CASE ("Oversampling report: what folds into the audible band",
           "[.report][oversampling-report]")
{
    // The oversampler alone: a tone written into the oversampled stream (what a waveshaper's
    // harmonics are) that would fold to 20 kHz or below on the way down. The worst one, per mode.
    std::printf ("\nWorst fold-down into 20 Hz - 20 kHz (dB), the oversampler alone\n"
                 "| Rate | Linear phase | Low latency |\n|---|---|---|\n");
    for (const double fs : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        const int f = Oversampler::factorLog2For (fs);
        const double fsHigh = fs * (1 << f);
        double worst[2] { -300.0, -300.0 };
        for (const auto mode : kModes)
            for (double hf = fs - 20000.0; hf < fsHigh / 2.0; hf += 251.0)
            {
                double folded = std::fmod (hf, fs);
                if (folded > fs / 2.0)
                    folded = fs - folded;
                if (folded > 20000.0 || folded < 20.0)
                    continue;
                Oversampler os;
                const int block = 256;
                os.prepare (block, 1, f, mode);
                const int frames = block * 64;
                std::vector<float> out (static_cast<size_t> (frames)), dummy (block, 0.0f);
                long t = 0;
                for (int s = 0; s < frames; s += block)
                {
                    float* d = dummy.data();
                    const auto up = os.upsample (AudioView (&d, 1, block));
                    for (int i = 0; i < up.numFrames(); ++i, ++t)
                        up.channel (0)[i] =
                            0.5f * static_cast<float> (std::sin (kTwoPiD * hf * t / fsHigh));
                    float* o = out.data() + s;
                    os.downsample (AudioView (&o, 1, block));
                }
                const double a = test::toneMagnitude (out.data() + frames - 8192, 8192, folded, fs);
                worst[static_cast<int> (mode)] = std::max (
                    worst[static_cast<int> (mode)], 20.0 * std::log10 (std::max (a, 1e-12) / 0.5));
            }
        std::printf ("| %.1f kHz | %.1f | %.1f |\n", fs / 1000.0, worst[1], worst[0]);
    }
}

TEST_CASE ("Oversampling report: footswitch crossfades at the rig's output",
           "[.report][oversampling-report]")
{
    // Each oversampled pedal switched on under a strum through the default rig, against the
    // ideal 10 ms crossfade of the steady off and on renders (best alignment): the error's level
    // re. the signal over the fade. Mostly the pedal's restart from a clean state, in both modes.
    ScopedNoDenormals noDenormals;
    constexpr int kSwitch = 48000, kFade = 480;
    const auto strum = test::guitarStrum (48000.0, 1.5);
    std::printf ("\nCrossfade error at the rig's output (dB re. the signal over the fade), 48 kHz\n"
                 "| Pedal | Linear phase | Low latency |\n|---|---|---|\n");
    for (const auto& [label, enable] :
         { std::pair { "Remora", ParamId::remoraEnabled },
           std::pair { "Urchin", ParamId::urchinEnabled },
           std::pair { "Sargasso", ParamId::sargassoEnabled },
           std::pair { "Pearl", ParamId::pearlEnabled }, std::pair { "Kelp", ParamId::kelpEnabled },
           std::pair { "Coral", ParamId::coralEnabled },
           std::pair { "Breaker", ParamId::breakerEnabled } })
    {
        double err[2] {};
        for (const auto mode : kModes)
        {
            const auto render = [&, mode, enable] (int which) {
                Engine e;
                e.setOversampling (mode);
                e.prepare (48000.0, 64, 1, 2);
                auto p = ParamSnapshot::defaults();
                p[ParamId::gateEnabled] = 0.0f;
                p[enable] = which == 1 ? 1.0f : 0.0f;
                std::vector<float> l (strum.size()), r (strum.size());
                Transport t;
                for (size_t s = 0; s + 64 <= strum.size(); s += 64)
                {
                    if (which == 2 && s == kSwitch)
                        p[enable] = 1.0f;
                    const float* in[] = { strum.data() + s };
                    float* out[] = { l.data() + s, r.data() + s };
                    e.process (in, out, 64, p, t);
                }
                return l;
            };
            const auto off = render (0), on = render (1), sw = render (2);
            double best = 1.0e30, sig = 1.0;
            for (int start = 0; start <= 400; ++start)
            {
                double e2 = 0.0, s2 = 0.0;
                for (int i = kSwitch - 64; i < kSwitch + 400 + 2 * kFade; ++i)
                {
                    const auto k = static_cast<size_t> (i);
                    const float a =
                        std::clamp (static_cast<float> (i - kSwitch - start) / kFade, 0.0f, 1.0f);
                    const float c = off[k] + a * (on[k] - off[k]);
                    e2 += static_cast<double> (sw[k] - c) * (sw[k] - c);
                    s2 += static_cast<double> (c) * c;
                }
                if (e2 < best)
                {
                    best = e2;
                    sig = s2;
                }
            }
            err[static_cast<int> (mode)] = 10.0 * std::log10 (best / sig);
        }
        std::printf ("| %s | %.1f | %.1f |\n", label, err[1], err[0]);
    }
}
