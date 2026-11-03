// SPDX-License-Identifier: AGPL-3.0-or-later
// Measurement report for the mods pedals: levels, responses and sweeps that the docs quote and
// the trims were fitted from. Hidden; run with
//   build/engine/engine/sedef_engine_tests "[.mods-report]"
#include "ModsTestUtils.h"
#include "sedef/dsp/Anemone.h"
#include "sedef/dsp/Beacon.h"
#include "sedef/dsp/Conch.h"
#include "sedef/dsp/Gyre.h"
#include "sedef/dsp/Jetty.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>

using namespace sedef;

TEST_CASE ("Mods pedals measurement report", "[.mods-report]")
{
    const double fs = 48000.0;
    const auto freqs = test::logFrequencies (40.0, 20000.0, 48);

    std::printf ("\n== Conch (wah): peak and Q against Position (Q setting 5)\n");
    for (float p : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const auto h =
            test::impulseResponse<dsp::Conch> ({ dsp::Conch::Manual, p, 5.0f, 0.5f }, fs, 1 << 15);
        const auto r = test::responseDb (h, freqs, fs);
        const auto peak = test::findPeak (r, freqs);
        std::printf (
            "  pos %.2f: peak %7.1f Hz (%+.1f dB), Q %.2f, 100 Hz %+.1f dB, guitar %+.2f dB\n", p,
            peak.hz, peak.db, test::peakQ (r, freqs), test::responseDb (h, { 100.0 }, fs)[0],
            test::guitarGainDb<dsp::Conch> ({ dsp::Conch::Manual, p, 5.0f, 0.5f }));
    }

    std::printf ("\n== Anemone: guitar level against resonance, per mode (sens 0.5, range 0.6)\n");
    for (int mode : { 0, 1, 2 })
        for (float res : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            std::printf ("  mode %d res %.2f: steady %+.2f dB, plucks %+.2f dB (down %+.2f dB)\n",
                         mode, res, test::guitarGainDb<dsp::Anemone> ({ 0.5f, 0.6f, res, mode, 0 }),
                         test::pluckGainDb<dsp::Anemone> ({ 0.5f, 0.6f, res, mode, 0 }),
                         test::pluckGainDb<dsp::Anemone> ({ 0.5f, 0.6f, res, mode, 1 }));

    std::printf ("\n== Gyre: guitar level against feedback (speed 0.5, depth 0.8)\n");
    for (int stages : { 0, 1 })
        for (float fb : { 0.0f, 0.5f, 1.0f })
            std::printf ("  stages %d fb %.1f: %+.2f dB\n", stages, fb,
                         test::guitarGainDb<dsp::Gyre> ({ 0.5f, 0.8f, fb, stages }, fs, 4.0));

    std::printf ("\n== Jetty: guitar level against regen (rate 0.3, depth 0.6, manual 2 ms)\n");
    for (float regen : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
        std::printf ("  regen %+.1f: %+.2f dB\n", regen,
                     test::guitarGainDb<dsp::Jetty> ({ 0.3f, 0.6f, 2.0f, regen }, fs, 4.0));

    std::printf ("\n== Beacon: guitar level per mode, intensity; light swing per speed\n");
    for (int mode : { 0, 1 })
        for (float in : { 0.0f, 0.7f, 1.0f })
            std::printf ("  mode %d intensity %.1f: %+.2f dB\n", mode, in,
                         test::guitarGainDb<dsp::Beacon> ({ 2.0f, in, mode }, fs, 4.0));
    for (float speed : { 0.3f, 1.0f, 2.0f, 4.0f, 8.0f })
    {
        dsp::Beacon b;
        b.prepare ({ fs, 512, 1 });
        b.setSettings ({ speed, 1.0f, 0 });
        test::Buffer buf (1, static_cast<int> (6.0 * fs));
        float lo = 1.0f, hi = 0.0f;
        int risingSamples = 0, fallingSamples = 0;
        float prev = 0.0f;
        for (int i = 0; i < buf.frames(); ++i)
        {
            auto v = buf.view().subView (i, 1);
            b.process (v);
            if (i > static_cast<int> (3.0 * fs))
            {
                lo = std::min (lo, b.light());
                hi = std::max (hi, b.light());
                if (b.light() > prev)
                    ++risingSamples;
                else
                    ++fallingSamples;
            }
            prev = b.light();
        }
        std::printf (
            "  speed %.1f Hz: light %.3f..%.3f, stage-1 %.0f..%.0f Hz, rising %.2f of the time\n",
            speed, lo, hi, dsp::Beacon::stageHz (0, lo), dsp::Beacon::stageHz (0, hi),
            static_cast<double> (risingSamples) / (risingSamples + fallingSamples));
    }
}
