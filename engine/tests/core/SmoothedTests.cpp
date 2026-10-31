// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/core/Smoothed.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sedef;
using Catch::Approx;

TEST_CASE ("LinearSmoothed reaches the target in exactly the ramp length", "[core][smoothing]")
{
    LinearSmoothed s;
    s.reset (1000.0, 0.01); // 10 steps
    s.setCurrentAndTarget (0.0f);
    s.setTarget (1.0f);
    float v = 0.0f;
    for (int i = 0; i < 9; ++i)
    {
        v = s.next();
        CHECK (s.isSmoothing());
    }
    CHECK (v == Approx (0.9f));
    CHECK (s.next() == 1.0f);
    CHECK_FALSE (s.isSmoothing());
}

TEST_CASE ("LinearSmoothed skip lands on the same value as stepping", "[core][smoothing]")
{
    LinearSmoothed a, b;
    a.reset (48000.0, 0.05);
    b.reset (48000.0, 0.05);
    a.setTarget (2.0f);
    b.setTarget (2.0f);
    for (int i = 0; i < 100; ++i)
        a.next();
    CHECK (b.skip (100) == Approx (a.current()));
    CHECK (b.skip (100000) == 2.0f);
}

TEST_CASE ("ExpSmoothed converges monotonically", "[core][smoothing]")
{
    ExpSmoothed s;
    s.reset (48000.0, 0.01);
    s.setCurrentAndTarget (100.0f);
    s.setTarget (1000.0f);
    float prev = 100.0f;
    for (int i = 0; i < 48000; ++i)
    {
        const float v = s.next();
        REQUIRE (v >= prev);
        prev = v;
    }
    CHECK (prev == Approx (1000.0f).epsilon (1e-4));
}
