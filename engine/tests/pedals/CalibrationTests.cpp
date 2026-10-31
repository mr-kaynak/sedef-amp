// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PedalTestUtils.h"
#include "sedef/dsp/Coral.h"
#include "sedef/dsp/Pearl.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>

using namespace sedef;

/// Prints the output level of the drives at level 0.5 against the reference tone, per gain
/// setting. Re-run after changing a drive's voicing and refit its trimDb().
TEST_CASE ("Calibrate drive output levels", "[.calibrate]")
{
    const double fs = 48000.0;
    const int frames = 48000;
    for (int which = 0; which < 2; ++which)
    {
        for (float gain : { 0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f })
        {
            test::Buffer b (1, frames);
            test::fillGuitarTone (b[0], fs);
            const float in = test::rms (b[0], frames / 2);
            if (which == 0)
            {
                dsp::Pearl p;
                p.prepare ({ fs, 512, 1 });
                p.setSettings ({ gain, 0.5f, 0.5f, 0.0f });
                test::processInBlocks (b, 512, [&] (AudioView v) { p.process (v); });
            }
            else
            {
                dsp::Coral c;
                c.prepare ({ fs, 512, 1 });
                c.setSettings ({ gain, 0.5f, 0.5f });
                test::processInBlocks (b, 512, [&] (AudioView v) { c.process (v); });
            }
            const float out = test::rms (b[0], frames / 2);
            std::printf ("%s gain %.3f: %+.2f dB  peak %.3f\n", which == 0 ? "Pearl" : "Coral",
                         gain, test::db (out / in), test::peak (b[0]));
        }
    }
}
