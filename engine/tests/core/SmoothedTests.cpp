// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/core/Smoothed.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <limits>

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

// docs/robustness.md, section 2, rule 5: nothing that once saw a NaN stays NaN.
TEST_CASE ("Smoothers keep their target when given a non-finite one", "[core][smoothing]")
{
    const float bad =
        GENERATE (std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity());
    LinearSmoothed lin;
    lin.reset (1000.0, 0.01);
    lin.setCurrentAndTarget (0.5f);
    lin.setTarget (bad);
    lin.setCurrentAndTarget (bad);
    for (int i = 0; i < 20; ++i)
        CHECK (lin.next() == 0.5f);

    ExpSmoothed ex;
    ex.reset (1000.0, 0.01);
    ex.setCurrentAndTarget (0.5f);
    ex.setTarget (bad);
    ex.setCurrentAndTarget (bad);
    for (int i = 0; i < 20; ++i)
        CHECK (ex.next() == 0.5f);
    CHECK (ex.skip (32) == 0.5f);
}

TEST_CASE ("Smoothers whose state went non-finite take the next finite target", "[core][smoothing]")
{
    // A state can only go non-finite through arithmetic on a huge value; force it with one.
    LinearSmoothed lin;
    lin.reset (1000.0, 0.01);
    lin.setCurrentAndTarget (-3.0e38f);
    lin.setTarget (3.0e38f); // step = 6e37 per sample: current overflows to inf along the way
    for (int i = 0; i < 9; ++i)
        lin.next();
    lin.setTarget (0.25f);
    CHECK (std::isfinite (lin.next()));
    for (int i = 0; i < 20; ++i)
        lin.next();
    CHECK (lin.next() == 0.25f);

    ExpSmoothed ex;
    ex.reset (1000.0, 0.01);
    ex.setCurrentAndTarget (3.0e38f);
    ex.setTarget (-3.0e38f); // current - target overflows: the state becomes non-finite
    ex.next();
    ex.setTarget (0.25f);
    CHECK (ex.next() == 0.25f);
    CHECK (std::isfinite (ex.skip (100)));
}
