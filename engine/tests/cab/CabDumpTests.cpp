// SPDX-License-Identifier: AGPL-3.0-or-later
// Hidden helpers for voicing work: print measured responses as tables.
//   sedef_engine_tests "[.cabdump]"
#include "CabTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>

using namespace sedef;
using namespace sedef::test::cab;

namespace
{
void printTable (const char* title, const std::vector<std::vector<float>>& irs,
                 const std::vector<const char*>& names, double fs)
{
    std::printf ("\n%s\n%8s", title, "Hz");
    for (const char* n : names)
        std::printf ("%9s", n);
    std::printf ("\n");
    for (int k = -15; k <= 12; ++k)
    {
        const double hz = 1000.0 * std::exp2 (k / 3.0);
        std::printf ("%8.0f", hz);
        for (const auto& ir : irs)
            std::printf ("%9.1f", responseDb (ir, fs, hz));
        std::printf ("\n");
    }
}
} // namespace

TEST_CASE ("Cab voicing dump", "[.cabdump]")
{
    const double fs = 48000.0;
    CabSim cab;

    std::vector<std::vector<float>> irs;
    for (int c = 0; c < 3; ++c)
        irs.push_back (impulseResponse (cab, soloMicA (c, 6), fs));
    printTable ("Cabs, Cond Precision, pos 0.5, dist 0", irs, { "1x12", "2x12", "4x12" }, fs);

    irs.clear();
    for (int c = 0; c < 3; ++c)
        irs.push_back (impulseResponse (cab, soloMicA (c, 0), fs));
    printTable ("Cabs, Dyn Classic, pos 0.5, dist 0", irs, { "1x12", "2x12", "4x12" }, fs);

    for (int c = 0; c < 3; ++c)
    {
        irs.clear();
        for (int t = 0; t < 10; ++t)
            irs.push_back (impulseResponse (cab, soloMicA (c, t), fs));
        char title[64];
        std::snprintf (title, sizeof (title), "Mics on cab %d, pos 0.5, dist 0", c);
        printTable (title, irs,
                    { "DynCl", "DynOff", "DynBr", "DynBc", "CSmall", "CLarge", "CPrec", "CTube",
                      "RibSm", "RibDk" },
                    fs);
    }

    irs.clear();
    for (float p : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        irs.push_back (impulseResponse (cab, soloMicA (2, 0, p), fs));
    printTable ("4x12 Dyn Classic, position 0 .. 1", irs, { "p0", "p.25", "p.5", "p.75", "p1" },
                fs);

    irs.clear();
    for (float d : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        irs.push_back (impulseResponse (cab, soloMicA (2, 0, 0.5f, d), fs));
    printTable ("4x12 Dyn Classic, distance 0 .. 1", irs, { "d0", "d.25", "d.5", "d.75", "d1" },
                fs);

    irs.clear();
    for (float d : { 0.0f, 0.5f, 1.0f })
        irs.push_back (impulseResponse (cab, soloMicA (1, 8, 0.5f, d), fs));
    printTable ("2x12 Ribbon Smooth, distance 0 / 0.5 / 1", irs, { "d0", "d.5", "d1" }, fs);
}
