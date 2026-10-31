// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestUtils.h"
#include "sedef/core/Biquad.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace sedef;
using Catch::Approx;

namespace
{
double db (double g)
{
    return 20.0 * std::log10 (g);
}
} // namespace

TEST_CASE ("RBJ low-pass and high-pass are -3 dB at the corner with Butterworth Q",
           "[core][biquad]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    for (double f : { 100.0, 1000.0, 8000.0 })
    {
        CHECK (db (design::magnitude (design::lowpass (fs, f), f, fs)) ==
               Approx (-3.0103).margin (0.01));
        CHECK (db (design::magnitude (design::highpass (fs, f), f, fs)) ==
               Approx (-3.0103).margin (0.01));
        CHECK (design::magnitude (design::lowpass (fs, f), 1.0, fs) ==
               Approx (1.0).margin (5e-3)); // float coefficient quantisation
        CHECK (design::magnitude (design::highpass (fs, f), fs * 0.499, fs) ==
               Approx (1.0).margin (1e-2));
    }
}

TEST_CASE ("Peaking EQ hits its gain at the centre and is flat far away", "[core][biquad]")
{
    const double fs = 48000.0;
    for (double gain : { -12.0, -3.0, 6.0, 12.0 })
    {
        const auto c = design::peaking (fs, 1000.0, 1.41, gain);
        CHECK (db (design::magnitude (c, 1000.0, fs)) == Approx (gain).margin (0.01));
        CHECK (db (design::magnitude (c, 20.0, fs)) == Approx (0.0).margin (0.05));
    }
}

TEST_CASE ("Shelves reach their gain at the extremes", "[core][biquad]")
{
    const double fs = 48000.0;
    const auto ls = design::lowShelf (fs, 200.0, 0.7071, 9.0);
    CHECK (db (design::magnitude (ls, 10.0, fs)) == Approx (9.0).margin (0.05));
    CHECK (db (design::magnitude (ls, 20000.0, fs)) == Approx (0.0).margin (0.05));

    const auto hs = design::highShelf (fs, 4000.0, 0.7071, -6.0);
    CHECK (db (design::magnitude (hs, 23000.0, fs)) == Approx (-6.0).margin (0.1));
    CHECK (db (design::magnitude (hs, 20.0, fs)) == Approx (0.0).margin (0.05));
}

TEST_CASE ("First-order shelves are centred on their corner", "[core][biquad]")
{
    const double fs = 48000.0;
    const auto ls = design::lowShelf1 (fs, 300.0, 12.0);
    CHECK (db (design::magnitude (ls, 1.0, fs)) == Approx (12.0).margin (0.05));
    CHECK (db (design::magnitude (ls, 300.0, fs)) == Approx (6.0).margin (0.1));
    CHECK (db (design::magnitude (ls, 23000.0, fs)) == Approx (0.0).margin (0.1));

    const auto hs = design::highShelf1 (fs, 3000.0, -8.0);
    CHECK (db (design::magnitude (hs, 1.0, fs)) == Approx (0.0).margin (0.05));
    CHECK (db (design::magnitude (hs, 3000.0, fs)) == Approx (-4.0).margin (0.1));
}

TEST_CASE ("All-pass designs have unit magnitude", "[core][biquad]")
{
    const double fs = 48000.0;
    for (double f : { 50.0, 500.0, 5000.0, 15000.0 })
    {
        CHECK (design::magnitude (design::allpass (fs, 1000.0, 0.7), f, fs) ==
               Approx (1.0).margin (1e-5));
        CHECK (design::magnitude (design::allpass1 (fs, 1000.0), f, fs) ==
               Approx (1.0).margin (1e-5));
    }
}

TEST_CASE ("Biquad processing matches the analytic response", "[core][biquad]")
{
    const double fs = 48000.0;
    Biquad bq;
    bq.setCoeffs (design::peaking (fs, 2000.0, 1.0, 6.0));
    const double measured = test::measureGainDb ([&] (AudioView v) { bq.process (v); }, 2000.0, fs);
    CHECK (measured == Approx (6.0).margin (0.1));
}
