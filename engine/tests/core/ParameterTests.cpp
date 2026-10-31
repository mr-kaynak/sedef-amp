// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/params/Parameters.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>

using namespace sedef;
using Catch::Approx;

TEST_CASE ("Parameter ids are unique and defaults are in range", "[params]")
{
    std::set<std::string_view> ids;
    for (const auto& s : kParamSpecs)
    {
        INFO (std::string (s.id));
        CHECK (ids.insert (s.id).second);
        CHECK (s.min < s.max);
        CHECK (s.defaultValue >= s.min);
        CHECK (s.defaultValue <= s.max);
        CHECK (s.skew > 0.0f);
        CHECK_FALSE (s.name.empty());
        if (s.kind == ParamKind::Choice)
            CHECK (s.numChoices() == static_cast<int> (s.max) + 1);
    }
}

TEST_CASE ("Normalisation round-trips for every parameter", "[params]")
{
    for (const auto& s : kParamSpecs)
    {
        INFO (std::string (s.id));
        for (float n : { 0.0f, 0.1f, 0.33f, 0.5f, 0.9f, 1.0f })
        {
            const float v = s.fromNormalised (n);
            CHECK (v >= s.min);
            CHECK (v <= s.max);
            if (s.kind == ParamKind::Float)
                CHECK (s.toNormalised (v) == Approx (n).margin (1e-4));
        }
        CHECK (s.fromNormalised (s.toNormalised (s.defaultValue)) ==
               Approx (s.defaultValue).margin (1e-3 * (s.max - s.min)));
    }
}

TEST_CASE ("Skewed ranges put the configured centre at the middle", "[params]")
{
    const auto& level = spec (ParamId::micALevel);
    CHECK (level.fromNormalised (0.5f) == Approx (0.0f).margin (0.01f));
    CHECK (spec (ParamId::fathomDecay).fromNormalised (0.0f) == Approx (1.0f));
}

TEST_CASE ("Lookups and choice labels", "[params]")
{
    REQUIRE (findParam ("ampModel").has_value());
    CHECK (*findParam ("ampModel") == ParamId::ampModel);
    CHECK_FALSE (findParam ("doesNotExist").has_value());
    CHECK (spec (ParamId::ampModel).choiceLabel (2) == "Abyss");
    CHECK (spec (ParamId::driftNote).choiceLabel (13) == "1/4");
    CHECK (spec (ParamId::micACab1Type).choiceLabel (10) == "Custom IR");

    auto snap = ParamSnapshot::defaults();
    CHECK (snap.choice (ParamId::ampModel) == 1);
    CHECK (snap.flag (ParamId::gateEnabled));
}
