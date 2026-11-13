// SPDX-License-Identifier: AGPL-3.0-or-later
// The arithmetic of MIDI bindings (docs/live.md §4): continuous ranges, switch modes, which
// controllers learn takes, and the learn classifier.

#include "sedef/live/LiveMap.h"
#include "sedef/live/SceneBank.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace sedef;
using namespace sedef::live;

TEST_CASE ("A continuous binding reaches both ends of its range exactly", "[live]")
{
    const auto& knob = spec (ParamId::pearlGain); // 0..1, linear
    CHECK (continuousValue (knob, 0.0f, 1.0f, 0) == 0.0f);
    CHECK (continuousValue (knob, 0.0f, 1.0f, 127) == 1.0f);
    CHECK (continuousValue (knob, 0.0f, 1.0f, 64) == Catch::Approx (64.0 / 127.0).epsilon (1e-6));

    // A part of the range: heel and toe land on low and high, not near them.
    CHECK (continuousValue (knob, 0.2f, 0.7f, 0) == 0.2f);
    CHECK (continuousValue (knob, 0.2f, 0.7f, 127) == 0.7f);
    CHECK (continuousValue (knob, 0.2f, 0.7f, 127 / 2) ==
           Catch::Approx (0.2 + 0.5 * 63.0 / 127.0).epsilon (1e-6));
}

TEST_CASE ("low above high inverts a binding", "[live]")
{
    const auto& knob = spec (ParamId::conchPosition);
    CHECK (continuousValue (knob, 1.0f, 0.0f, 0) == 1.0f);
    CHECK (continuousValue (knob, 1.0f, 0.0f, 127) == 0.0f);
    CHECK (continuousValue (knob, 0.9f, 0.3f, 0) == 0.9f);
    CHECK (continuousValue (knob, 0.9f, 0.3f, 127) == 0.3f);
    // Monotonic, downwards.
    float last = 2.0f;
    for (int v = 0; v <= 127; ++v)
    {
        const float x = continuousValue (knob, 1.0f, 0.0f, v);
        CHECK (x < last);
        last = x;
    }
}

TEST_CASE ("A continuous binding follows the parameter's skew and snapping", "[live]")
{
    // Gyre Speed: 0.1-8 Hz with skew 0.3197; the controller moves the knob's travel.
    const auto& speed = spec (ParamId::gyreSpeed);
    CHECK (continuousValue (speed, 0.0f, 1.0f, 0) == speed.min);
    CHECK (continuousValue (speed, 0.0f, 1.0f, 127) == speed.max);
    for (int v = 0; v <= 127; v += 7)
        CHECK (continuousValue (speed, 0.0f, 1.0f, v) ==
               speed.fromNormalised (static_cast<float> (v) / 127.0f));

    // A choice snaps to its nearest step: Anemone's three modes.
    const auto& mode = spec (ParamId::anemoneMode);
    CHECK (continuousValue (mode, 0.0f, 1.0f, 0) == 0.0f);
    CHECK (continuousValue (mode, 0.0f, 1.0f, 64) == 1.0f);
    CHECK (continuousValue (mode, 0.0f, 1.0f, 127) == 2.0f);

    // Out-of-range values are clamped, not wrapped.
    CHECK (continuousValue (spec (ParamId::pearlGain), -0.5f, 1.5f, 0) == 0.0f);
    CHECK (continuousValue (spec (ParamId::pearlGain), -0.5f, 1.5f, 127) == 1.0f);
}

TEST_CASE ("Toggle flips on presses and ignores releases; Latch follows", "[live]")
{
    // MIDI 1.0 switch split: 0-63 off, 64-127 on.
    CHECK (switchValue (SwitchMode::toggle, false, 127) == true);
    CHECK (switchValue (SwitchMode::toggle, true, 127) == false);
    CHECK (switchValue (SwitchMode::toggle, true, 64) == false);
    CHECK_FALSE (switchValue (SwitchMode::toggle, true, 63).has_value());
    CHECK_FALSE (switchValue (SwitchMode::toggle, false, 0).has_value());

    CHECK (switchValue (SwitchMode::latch, false, 127) == true);
    CHECK (switchValue (SwitchMode::latch, true, 0) == false);
    CHECK (switchValue (SwitchMode::latch, false, 64) == true);
    CHECK (switchValue (SwitchMode::latch, true, 63) == false);
    CHECK (switchValue (SwitchMode::latch, true, 127) == true); // already on: stays

    // Scene and preset actions: Toggle on presses, Latch on every message.
    CHECK (actionFires (SwitchMode::toggle, 127));
    CHECK_FALSE (actionFires (SwitchMode::toggle, 0));
    CHECK (actionFires (SwitchMode::latch, 0));
    CHECK (actionFires (SwitchMode::latch, 127));
}

TEST_CASE ("A footswitch drives a pedal the same way through a whole set of presses", "[live]")
{
    struct Case
    {
        const char* name;
        SwitchMode mode;
        std::vector<int> values;  ///< what the controller sends for four presses
        std::vector<bool> states; ///< the pedal after each message, starting off
    };
    const Case cases[] = {
        // Momentary: 127 on press, 0 on release; Toggle flips once per press.
        { "momentary, toggle",
          SwitchMode::toggle,
          { 127, 0, 127, 0, 127, 0, 127, 0 },
          { true, true, false, false, true, true, false, false } },
        // Latching: 127 and 0 on alternate presses; Latch follows each press.
        { "latching, latch", SwitchMode::latch, { 127, 0, 127, 0 }, { true, false, true, false } },
        // A switch that sends 127 on every press: Toggle flips each time.
        { "repeat-127, toggle",
          SwitchMode::toggle,
          { 127, 127, 127, 127 },
          { true, false, true, false } },
        // A momentary switch on Latch is on while held.
        { "momentary, latch (hold)",
          SwitchMode::latch,
          { 127, 0, 127, 0 },
          { true, false, true, false } },
    };
    for (const auto& c : cases)
    {
        CAPTURE (c.name);
        bool on = false;
        for (std::size_t i = 0; i < c.values.size(); ++i)
        {
            if (const auto next = switchValue (c.mode, on, c.values[i]))
                on = *next;
            CHECK (on == c.states[i]);
        }
    }
}

TEST_CASE ("Learn takes controls, not parts of other messages", "[live]")
{
    for (const int skipped : { 0, 6, 32, 38, 98, 99, 100, 101, 120, 121, 123, 127 })
    {
        CAPTURE (skipped);
        CHECK_FALSE (isLearnable (skipped));
    }
    for (const int taken : { 1, 4, 7, 11, 20, 31, 33, 64, 80, 97, 102, 119 })
    {
        CAPTURE (taken);
        CHECK (isLearnable (taken));
    }
    int count = 0;
    for (int c = 0; c < 128; ++c)
        count += isLearnable (c) ? 1 : 0;
    CHECK (count == 120 - 8); // 0-119 without 0, 6, 32, 38 and 98-101
}

TEST_CASE ("The learn classifier tells momentary from latching switches", "[live]")
{
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE (fs);
    const auto window = static_cast<std::int64_t> (kLearnWindowSeconds * fs);
    const auto at = [fs] (double seconds) {
        return static_cast<std::int64_t> (seconds * fs);
    };
    const std::int64_t t0 = 12345;

    SECTION ("a pedal or a knob is Latch at once")
    {
        SwitchClassifier c (window);
        CHECK (c.first (64, t0) == SwitchMode::latch);
        CHECK (c.decided());
    }
    SECTION ("a press that sends 0 is a latching switch")
    {
        SwitchClassifier c (window);
        CHECK (c.first (0, t0) == SwitchMode::latch);
    }
    SECTION ("127 then a release within the window is a momentary switch")
    {
        SwitchClassifier c (window);
        CHECK_FALSE (c.first (127, t0).has_value());
        CHECK_FALSE (c.elapsed (t0 + at (0.2)).has_value());
        CHECK (c.next (0, t0 + at (0.3)) == SwitchMode::toggle);
        // Decided: later messages don't change it.
        CHECK (c.next (127, t0 + at (0.6)) == SwitchMode::toggle);
    }
    SECTION ("127 and nothing else for the whole window is a latching switch")
    {
        SwitchClassifier c (window);
        CHECK_FALSE (c.first (127, t0).has_value());
        CHECK_FALSE (c.elapsed (t0 + window).has_value()); // the window's last sample still counts
        CHECK (c.elapsed (t0 + window + 1) == SwitchMode::latch);
    }
    SECTION ("a release after the window is the next press of a latching switch")
    {
        SwitchClassifier c (window);
        CHECK_FALSE (c.first (127, t0).has_value());
        CHECK (c.next (0, t0 + window + 1) == SwitchMode::latch);
    }
    SECTION ("a second 127 within the window decides nothing yet")
    {
        SwitchClassifier c (window);
        CHECK_FALSE (c.first (127, t0).has_value());
        CHECK_FALSE (c.next (127, t0 + at (0.5)).has_value());
        CHECK (c.elapsed (t0 + window + 1) == SwitchMode::latch);
    }
}

TEST_CASE ("Scenes hold every pedal's switch and the post blocks", "[live]")
{
    // Both copies of every pedal (docs/board.md §7), the EQ banks, Doubler, Drift and Fathom.
    CHECK (kSceneSwitches.size() == kNumSlots + 6);
    for (std::size_t slot = 0; slot < kNumSlots; ++slot)
    {
        bool found = false;
        for (const auto id : kSceneSwitches)
            found = found || id == enableOf (slotRef (slot));
        CAPTURE (slot);
        CHECK (found);
    }
    for (const auto id : kSceneSwitches)
        CHECK (spec (id).kind == ParamKind::Bool);

    auto values = ParamSnapshot::defaults();
    values[ParamId::pearlEnabled] = 1.0f;
    auto bank = SceneBank::withSwitches (values);
    CHECK (bank.numParams == kSceneSwitches.size());
    CHECK (bank.numValues() == 0);
    const int pearl = bank.find (ParamId::pearlEnabled);
    REQUIRE (pearl >= 0);
    for (const auto& scene : bank.values)
        CHECK (scene[static_cast<std::size_t> (pearl)] == 1.0f);

    // Picked values: continuous only, at most 16, never twice, removable; switches stay.
    CHECK_FALSE (bank.addValue (ParamId::anemoneMode, 0.0f)); // a choice
    CHECK_FALSE (bank.addValue (ParamId::abyssBright, 0.0f)); // a switch outside the set
    // Transpose: an int, and a scene must never change the latency (docs/live.md §6.2).
    CHECK_FALSE (bank.addValue (ParamId::transpose, -2.0f));
    CHECK (bank.addValue (ParamId::driftMix, 0.3f));
    CHECK_FALSE (bank.addValue (ParamId::driftMix, 0.3f));
    CHECK (bank.numValues() == 1);
    CHECK (bank.values[5][static_cast<std::size_t> (bank.find (ParamId::driftMix))] == 0.3f);
    CHECK_FALSE (bank.removeValue (ParamId::pearlEnabled));
    CHECK (bank.removeValue (ParamId::driftMix));
    CHECK (bank.find (ParamId::driftMix) == -1);

    int added = 0;
    for (std::size_t i = 0; i < kNumParams && added < kMaxSceneValues + 3; ++i)
        if (bank.addValue (static_cast<ParamId> (i), 0.5f))
            ++added;
    CHECK (added == kMaxSceneValues);
    CHECK (bank.numParams <= kMaxSceneParams);
}
