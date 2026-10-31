// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PedalTestUtils.h"
#include "sedef/core/Denormals.h"
#include "sedef/dsp/Coral.h"
#include "sedef/dsp/NoiseGate.h"
#include "sedef/dsp/Pearl.h"
#include "sedef/dsp/Ripple.h"
#include "sedef/dsp/Tide.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>

using namespace sedef;

namespace
{
void report (const char* name, double ns)
{
    // One core at 48 kHz has 20833 ns per sample.
    std::printf ("%-18s %7.1f ns/sample  %5.2f %% of a core (48 kHz mono)\n", name, ns,
                 100.0 * ns / (1.0e9 / 48000.0));
}

template <typename Module>
double bench (const typename Module::Settings& s)
{
    Module m;
    m.prepare ({48000.0, 64, 1});
    m.setSettings (s);
    return test::nsPerSample ([&] (AudioView v) { m.process (v); });
}
} // namespace

TEST_CASE ("Pedal CPU cost", "[.bench]")
{
    ScopedNoDenormals noDenormals;

    {
        dsp::NoiseGate gate;
        gate.prepare ({48000.0, 64, 1});
        gate.setSettings ({-60.0f});
        report ("Gate", test::nsPerSample ([&] (AudioView v) { gate.process (v, v.channel (0)); }));
    }
    report ("Tide", bench<dsp::Tide> ({0.7f, 0.6f, 0.5f, 0.7f}));
    report ("Pearl", bench<dsp::Pearl> ({0.6f, 0.5f, 0.5f, 0.5f}));
    report ("Coral", bench<dsp::Coral> ({0.8f, 0.5f, 0.5f}));
    report ("Ripple tremolo", bench<dsp::Ripple> ({dsp::Ripple::Tremolo, 5.0f, 0.7f}));
    report ("Ripple chorus", bench<dsp::Ripple> ({dsp::Ripple::Chorus, 1.0f, 0.7f}));
    report ("Ripple vibrato", bench<dsp::Ripple> ({dsp::Ripple::Vibrato, 5.0f, 0.7f}));
}
