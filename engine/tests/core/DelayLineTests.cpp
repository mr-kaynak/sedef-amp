// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/core/DelayLine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;

TEST_CASE ("DelayLine integer reads return past samples", "[core][delay]")
{
    DelayLine d;
    d.prepare (100);
    for (int i = 1; i <= 50; ++i)
        d.push (static_cast<float> (i));
    CHECK (d.read (0) == 50.0f);
    CHECK (d.read (1) == 49.0f);
    CHECK (d.read (49) == 1.0f);
}

TEST_CASE ("DelayLine interpolators are exact on a ramp", "[core][delay]")
{
    DelayLine d;
    d.prepare (64);
    for (int i = 0; i < 64; ++i)
        d.push (static_cast<float> (i));
    // Sample written `delay` samples ago on a unit ramp has value 63 - delay.
    CHECK (d.readLinear (10.25f) == Approx (63.0f - 10.25f));
    CHECK (d.readCubic (10.25f) == Approx (63.0f - 10.25f));
    CHECK (d.readCubic (3.5f) == Approx (63.0f - 3.5f));
}
