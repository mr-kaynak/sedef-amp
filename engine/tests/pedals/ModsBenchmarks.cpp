// SPDX-License-Identifier: AGPL-3.0-or-later
// CPU cost of the mods pedals. Hidden; run with
//   build/engine/engine/sedef_engine_tests "[.bench][mods-bench]"
#include "PedalTestUtils.h"
#include "sedef/core/Denormals.h"
#include "sedef/dsp/Anemone.h"
#include "sedef/dsp/Beacon.h"
#include "sedef/dsp/Conch.h"
#include "sedef/dsp/Gyre.h"
#include "sedef/dsp/Jetty.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>

using namespace sedef;

namespace
{
template <typename Module>
void bench (const char* name, const typename Module::Settings& s)
{
    Module m;
    m.prepare ({ 48000.0, 64, 1 });
    m.setSettings (s);
    const double ns = test::nsPerSample ([&] (AudioView v) { m.process (v); });
    std::printf ("%-18s %7.1f ns/sample  %5.2f %% of a core (48 kHz mono)\n", name, ns,
                 100.0 * ns / (1.0e9 / 48000.0));
}
} // namespace

TEST_CASE ("Mods pedals CPU cost", "[.bench][mods-bench]")
{
    ScopedNoDenormals noDenormals;
    bench<dsp::Conch> ("Conch manual", { dsp::Conch::Manual, 0.5f, 0.5f, 0.5f });
    bench<dsp::Conch> ("Conch auto", { dsp::Conch::Auto, 0.2f, 0.5f, 0.6f });
    bench<dsp::Anemone> ("Anemone", { 0.5f, 0.6f, 0.5f, dsp::Anemone::LowPass, 0 });
    bench<dsp::Gyre> ("Gyre 4-stage", { 0.5f, 0.8f, 0.3f, dsp::Gyre::Four });
    bench<dsp::Gyre> ("Gyre 6-stage", { 0.5f, 0.8f, 0.3f, dsp::Gyre::Six });
    bench<dsp::Jetty> ("Jetty", { 0.3f, 0.6f, 2.0f, 0.4f });
    bench<dsp::Beacon> ("Beacon", { 2.0f, 0.7f, dsp::Beacon::Chorus });
}
