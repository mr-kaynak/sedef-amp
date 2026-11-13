// SPDX-License-Identifier: AGPL-3.0-or-later
// The board's second part (docs/board.md): pedals after the amp, two copies of a pedal, how the
// board is spelt, and the stereo stage after the amp.
#include "board/BoardTestUtils.h"
#include "sedef/board/BoardText.h"
#include "sedef/live/SceneBank.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdio>
#include <set>
#include <string>

using namespace sedef;
using namespace sedef::test;

namespace
{
constexpr PedalRef second (PedalId id)
{
    return { id, 1 };
}

/// What 0.10 made of a board's spelling: ids it knows, each once, in order (plugin BoardText at
/// main: findPedal, Board::insert at the end, repeats skipped).
std::string asOldBuildReads (const std::string& text)
{
    std::string kept;
    std::set<std::string> seen;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const auto comma = text.find (',', start);
        const auto token =
            text.substr (start, comma == std::string::npos ? std::string::npos : comma - start);
        if (findPedal (token) && seen.insert (token).second)
            kept += (kept.empty() ? "" : ",") + token;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return kept;
}

/// Every copy of every pedal switched on.
ParamSnapshot everyCopyOn (ParamSnapshot p = cleanRig())
{
    for (std::size_t slot = 0; slot < kNumSlots; ++slot)
        p[enableOf (slotRef (slot))] = 1.0f;
    return p;
}

/// A module of the kind `id` names, prepared as the stage after the amp prepares it, given the
/// settings and played over `io` in blocks of `block`, as a PedalSlot coming out of reset plays it.
template <typename Module, typename Settings>
void playOffline (Module& m, const Settings& settings, double fs, int block,
                  std::vector<float>& left, std::vector<float>& right)
{
    m.prepare ({ fs, block, 2 });
    m.setSettings (settings);
    m.reset();
    m.setSettings (settings);
    const int frames = static_cast<int> (left.size());
    for (int s = 0; s < frames; s += block)
    {
        const int n = std::min (block, frames - s);
        std::array<float*, 2> ptrs { left.data() + s, right.data() + s };
        m.setSettings (settings);
        m.process (AudioView (ptrs.data(), 2, n));
    }
}

/// The engine's two outputs for a mono input.
std::array<std::vector<float>, 2> renderStereo (double fs, int block, const std::vector<float>& x,
                                                const ParamSnapshot& p, const Board& board)
{
    BoardRig rig (fs, block, static_cast<int> (x.size()), 1, 2);
    rig.in[0] = x;
    rig.engine.setBoard (board);
    rig.engine.reset();
    rig.run (p, 0, static_cast<int> (x.size()));
    return { rig.out[0], rig.out[1] };
}

double power (const float* d, int n)
{
    double acc = 0.0;
    for (int i = 0; i < n; ++i)
        acc += static_cast<double> (d[i]) * d[i];
    return acc / n;
}
} // namespace

// -------------------------------------------------------------------------------------------------
// The rules (docs/board.md §2)

TEST_CASE ("What may go where agrees with the modules, at every rate and in either mode", "[board]")
{
    CHECK (kCopies == 2);
    CHECK (kLatencySlots == 7);
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0);
    const auto mode = GENERATE (OversamplingMode::lowLatency, OversamplingMode::linearPhase);
    CAPTURE (fs, static_cast<int> (mode));
    PedalRack rack;
    rack.prepare ({ fs, 64, 1, mode });
    PedalRack after;
    after.prepare ({ fs, 64, 2, mode }, Side::post);
    CHECK (after.latencySamples() == 0);

    int drive = -1;
    for (std::size_t i = 0; i < kNumPedals; ++i)
    {
        const auto id = static_cast<PedalId> (i);
        INFO (pedalInfo (id).id);
        const int latency = rack.latencyOf (id);
        // Both copies alike.
        CHECK (rack.latencyOf (second (id)) == latency);
        // Only the drives and fuzzes have latency, and all the same.
        if (hasLatency (id))
        {
            if (drive < 0)
                drive = latency;
            CHECK (latency == drive);
        }
        else
            CHECK (latency == 0);
        // Where they oversample at all (up to 100 kHz) they do have it.
        if (fs <= 100000.0)
            CHECK ((latency > 0) == hasLatency (id));
        // Only modulation goes after the amp, and it has none.
        CHECK (fitsAfterAmp (id) == (pedalInfo (id).category == PedalCategory::Modulation));
        if (fitsAfterAmp (id))
            CHECK (after.latencyOf (id) == 0);
    }
    // The rack's latency is every drive and fuzz once.
    CHECK (rack.latencySamples() == drive * static_cast<int> (kLatencySlots));
}

TEST_CASE ("Every pedal's second copy has parameters of its own", "[board]")
{
    // The second copies are one block of 73, mirroring the first copies (Board.h checks names,
    // groups and ranges at compile time).
    std::size_t firstCopies = 0;
    for (std::size_t i = 0; i < index (ParamId::gate2Enabled); ++i)
        for (const auto& pedal : kPedals)
            firstCopies += kParamSpecs[i].group == pedal.name ? 1 : 0;
    CHECK (firstCopies == 73);
    std::set<ParamId> seconds;
    for (std::size_t i = 0; i < kNumParams; ++i)
    {
        const auto id = static_cast<ParamId> (i);
        const auto copy = secondCopyOf (id);
        if (copy != id)
        {
            CHECK (seconds.insert (copy).second);
            CHECK (index (copy) >= index (ParamId::gate2Enabled));
            CHECK (index (copy) < index (ParamId::gate2Enabled) + 73);
            CHECK (secondCopyOf (copy) == copy);
        }
    }
    CHECK (seconds.size() == 73);
    CHECK (secondCopyOf (ParamId::kelpDrive) == ParamId::kelp2Drive);
    CHECK (secondCopyOf (ParamId::ampModel) == ParamId::ampModel);
    CHECK (findParam ("kelp2Drive") == ParamId::kelp2Drive);
    CHECK (spec (ParamId::kelp2Drive).name == "Kelp 2 Drive");
    for (const auto& pedal : kPedals)
    {
        const auto& s = spec (enableOf ({ findPedal (pedal.id).value(), 1 }));
        INFO (pedal.id);
        CHECK (s.id == std::string (pedal.id) + "2Enabled");
        CHECK (s.name == std::string (pedal.name) + " 2");
        CHECK (s.defaultValue == 0.0f); // a second copy starts off
        CHECK (s.automatable);
    }

    // The settings the second copy plays come from its own parameters, and only from them.
    auto p = cleanRig ({ ParamId::kelp2Enabled });
    p[ParamId::kelpDrive] = 0.1f;
    p[ParamId::kelp2Drive] = 0.9f;
    p[ParamId::rippleMode] = 2.0f;
    p[ParamId::ripple2Mode] = 1.0f;
    const auto s = PedalSettings::from (p);
    CHECK (s.kelp[0].drive == 0.1f);
    CHECK (s.kelp[1].drive == 0.9f);
    CHECK (s.ripple[0].mode == 2);
    CHECK (s.ripple[1].mode == 1);
    CHECK_FALSE (s.on[slotIndex (PedalId::kelp)]);
    CHECK (s.on[slotIndex (second (PedalId::kelp))]);
}

TEST_CASE ("Scenes switch every copy", "[board][live]")
{
    std::set<ParamId> switches (live::kSceneSwitches.begin(), live::kSceneSwitches.end());
    CHECK (switches.size() == live::kSceneSwitches.size());
    for (std::size_t slot = 0; slot < kNumSlots; ++slot)
        CHECK (switches.count (enableOf (slotRef (slot))) == 1);
    // The first copies' switches keep their places at the front.
    for (std::size_t i = 0; i < kNumPedals; ++i)
        CHECK (live::kSceneSwitches[i] == kPedals[i].enable);
    CHECK (live::kSceneSwitches.size() + live::kMaxSceneValues <= live::kMaxSceneParams);
}

// -------------------------------------------------------------------------------------------------
// The board (docs/board.md §3)

TEST_CASE ("Add pedal takes the next copy, next to the other", "[board]")
{
    Board b;
    CHECK (b.add (PedalId::tide) == PedalRef (PedalId::tide));
    CHECK (b.add (PedalId::kelp) == PedalRef (PedalId::kelp));
    CHECK (b.add (PedalId::gate) == PedalRef (PedalId::gate));
    CHECK (b.pre == boardOf ({ PedalId::gate, PedalId::tide, PedalId::kelp }));
    // The second copy goes right after the first, whatever its rank says.
    b.place (PedalId::kelp, Side::pre, 0);
    CHECK (b.add (PedalId::kelp) == second (PedalId::kelp));
    CHECK (b.pre ==
           boardOf ({ PedalId::kelp, second (PedalId::kelp), PedalId::gate, PedalId::tide }));
    // Two at most.
    CHECK_FALSE (b.add (PedalId::kelp).has_value());
    CHECK_FALSE (b.nextCopy (PedalId::kelp).has_value());
    // The second copy keeps its number when the first leaves; Add then brings the first back,
    // next to the second.
    CHECK (b.remove (PedalId::kelp));
    CHECK (b.nextCopy (PedalId::kelp) == PedalRef (PedalId::kelp));
    CHECK (b.add (PedalId::kelp) == PedalRef (PedalId::kelp));
    CHECK (b.pre ==
           boardOf ({ second (PedalId::kelp), PedalId::kelp, PedalId::gate, PedalId::tide }));
}

TEST_CASE ("Only modulation goes after the amp", "[board]")
{
    Board b;
    CHECK (b.addAfterAmp (PedalId::gyre) == PedalRef (PedalId::gyre));
    CHECK (b.addAfterAmp (PedalId::ripple) == PedalRef (PedalId::ripple));
    CHECK (b.post == boardOf ({ PedalId::gyre, PedalId::ripple }));
    for (std::size_t i = 0; i < kNumPedals; ++i)
    {
        const auto id = static_cast<PedalId> (i);
        INFO (pedalInfo (id).id);
        CHECK (b.canPlace (id, Side::post) == fitsAfterAmp (id));
        CHECK (b.canPlace (id, Side::pre));
    }
    CHECK_FALSE (b.addAfterAmp (PedalId::kelp).has_value());
    CHECK_FALSE (b.place (PedalId::tide, Side::post, 0));
    // A second copy of a pedal after the amp goes next to it there; one can stand on either side.
    CHECK (b.add (PedalId::gyre) == second (PedalId::gyre));
    CHECK (b.post == boardOf ({ PedalId::gyre, second (PedalId::gyre), PedalId::ripple }));
    CHECK (b.place (second (PedalId::gyre), Side::pre, 0));
    CHECK (b.pre == boardOf ({ second (PedalId::gyre) }));
    CHECK (b.find (second (PedalId::gyre))->side == Side::pre);
    CHECK (b.find (PedalId::gyre)->side == Side::post);
    // Moving across and back within the same board, the count of pedals stays.
    CHECK (b.place (PedalId::ripple, Side::pre, 0));
    CHECK (b.place (PedalId::ripple, Side::post, 9));
    CHECK (b.size() == 3);
}

TEST_CASE ("A board holds seven drives and fuzzes at most", "[board]")
{
    Board b;
    for (const auto id : { PedalId::pearl, PedalId::kelp, PedalId::coral, PedalId::breaker })
    {
        REQUIRE (b.add (id));
        if (id != PedalId::breaker)
            REQUIRE (b.add (id));
    }
    CHECK (b.latencyCount() == kLatencySlots);
    CHECK_FALSE (b.add (PedalId::remora).has_value());
    CHECK_FALSE (b.add (PedalId::breaker).has_value());
    CHECK_FALSE (b.canPlace (PedalId::urchin, Side::pre));
    // Everything else still fits, and the drives on the board still move.
    CHECK (b.add (PedalId::gate));
    CHECK (b.add (PedalId::gate));
    CHECK (b.place (second (PedalId::kelp), Side::pre, 0));
    CHECK (b.latencyCount() == kLatencySlots);
    // One off, and another goes on.
    CHECK (b.remove (PedalId::coral));
    CHECK (b.add (PedalId::remora) == PedalRef (PedalId::remora));

    // The largest board: two of each pedal without latency and seven drives and fuzzes.
    Board full;
    for (std::size_t slot = 0; slot < kNumSlots; ++slot)
        full.add (slotRef (slot).id);
    CHECK (full.size() == 25);
    CHECK (full.latencyCount() == kLatencySlots);
}

TEST_CASE ("Saved boards keep their meaning, old and new", "[board]")
{
    // Boards 0.10 could make: any first copies in any order, nothing after the amp. They are spelt
    // as 0.10 spelt them, and read back the same.
    Random r (23);
    for (int round = 0; round < 64; ++round)
    {
        PedalOrder order;
        std::string text;
        for (std::size_t i = 0; i < kNumPedals; ++i)
            if (r.unipolar() < 0.5f)
                order.insert (
                    static_cast<PedalId> (i),
                    static_cast<std::size_t> (r.unipolar() * static_cast<float> (order.size())));
        for (const auto ref : order)
            text += (text.empty() ? "" : ",") + std::string (pedalInfo (ref).id);
        CAPTURE (text);
        CHECK (boardToText (order) == text);
        CHECK (boardFromText (text) == Board (order));
    }
    CHECK (boardToText (Board {}).empty());
    CHECK (boardFromText ("").empty());

    // The new spellings round-trip.
    const auto board =
        sides ({ PedalId::gate, PedalId::kelp, second (PedalId::kelp), PedalId::coral },
               { PedalId::gyre, second (PedalId::ripple) });
    const std::string text = "gate,kelp,kelp#2,coral,amp,gyre,ripple#2";
    CHECK (boardToText (board) == text);
    CHECK (boardFromText (text) == board);
    CHECK (boardFromText (" gate , kelp,kelp#2 ,coral, amp ,gyre,ripple#2") == board);
    CHECK (tokenOf (second (PedalId::kelp)) == "kelp#2");
    CHECK (refOf ("kelp#2") == second (PedalId::kelp));
    CHECK_FALSE (refOf ("kelp#3").has_value());
    CHECK_FALSE (refOf ("kelp#1").has_value());
    CHECK_FALSE (refOf (kAmpToken).has_value());

    // What 0.10 makes of it (docs/board.md §3.3): the second copies and the amp are gone, the
    // pedals after the amp play at the end of the pedals in front of it.
    CHECK (asOldBuildReads (text) == "gate,kelp,coral,gyre");

    // Whatever a file holds, the board is valid.
    CHECK (boardFromText ("kelp,amp,kelp#2,gyre") == sides ({ PedalId::kelp }, { PedalId::gyre }));
    CHECK (boardFromText ("amp,amp,gyre,gyre,tide") == sides ({}, { PedalId::gyre }));
    CHECK (boardFromText ("kelp#2,kelp#2,nothing,kelp#3") ==
           sides ({ second (PedalId::kelp) }, {}));
    CHECK (
        boardFromText ("pearl,pearl#2,kelp,kelp#2,coral,coral#2,breaker,remora").latencyCount() ==
        kLatencySlots);
    // Nothing after the amp: no "amp" written.
    CHECK (boardToText (sides ({ PedalId::gate }, {})) == "gate");
}

TEST_CASE ("The boards for presets without one stay first copies, in front of the amp", "[board]")
{
    const auto all = Board::legacyFor ([] (ParamId) { return true; });
    CHECK (all.post.empty());
    CHECK (all.pre.size() == kNumPedals);
    for (const auto r : all.pre)
        CHECK (r.instance == 0);
    // Only the first copies are what Init and the factory presets switch on.
    const auto defaults = ParamSnapshot::defaults();
    CHECK (Board::switchedOn ([&] (ParamId id) { return defaults.flag (id); }) ==
           Board (boardOf ({ PedalId::gate })));
    // A second copy that is on lands next to its first; the drive budget holds.
    auto p = cleanRig ({ ParamId::kelpEnabled, ParamId::kelp2Enabled, ParamId::tide2Enabled });
    const auto board = Board::switchedOn ([&] (ParamId id) { return p.flag (id); });
    CHECK (board ==
           Board (boardOf ({ second (PedalId::tide), PedalId::kelp, second (PedalId::kelp) })));
    const auto every = Board::switchedOn ([] (ParamId) { return true; });
    CHECK (every.size() == 25);
    CHECK (every.latencyCount() == kLatencySlots);
}

// -------------------------------------------------------------------------------------------------
// The rack (docs/board.md §4.2)

TEST_CASE ("A second drive takes the place of the last drive that is off the board", "[board]")
{
    PedalRack rack;
    rack.prepare ({ 48000.0, 64, 1 });
    using S = PedalRack::Step;
    const auto stepsOf = [&] (const PedalOrder& order) {
        const auto layout = rack.layoutOf (order);
        return std::vector<S> (layout.begin(), layout.end());
    };
    const auto delay = [] (PedalId id) {
        return S { id, false };
    };
    const auto on = [] (PedalRef r) {
        return S { r, true };
    };
    // Kelp and a second Kelp: the six absent drives and fuzzes owe five delays; Breaker's, the
    // last in default order, is the one the second Kelp takes.
    const std::vector<S> expected {
        on (PedalId::gate),          delay (PedalId::remora), delay (PedalId::urchin),
        delay (PedalId::sargasso),   delay (PedalId::pearl),  on (PedalId::kelp),
        on (second (PedalId::kelp)), delay (PedalId::coral),
    };
    CHECK (stepsOf (boardOf ({ PedalId::gate, PedalId::kelp, second (PedalId::kelp) })) ==
           expected);

    // Any valid board: the delays and the pedals add up to the rack's latency.
    Random r (31);
    for (int round = 0; round < 200; ++round)
    {
        Board b;
        for (std::size_t slot = 0; slot < kNumSlots; ++slot)
            if (r.unipolar() < 0.4f)
                b.place (
                    slotRef (slot), Side::pre,
                    static_cast<std::size_t> (r.unipolar() * static_cast<float> (b.pre.size())));
        int total = 0;
        for (const auto step : rack.layoutOf (b.pre))
            total += rack.latencyOf (step.ref);
        CHECK (total == rack.latencySamples());
    }
}

TEST_CASE ("Two copies of a pedal play independently", "[board]")
{
    const double fs = 48000.0;
    constexpr int block = 64, frames = 24000;
    const auto x = testSignal (true, fs, frames);
    auto p = cleanRig ({ ParamId::kelpEnabled, ParamId::kelp2Enabled, ParamId::gyreEnabled,
                         ParamId::gyre2Enabled, ParamId::gateEnabled, ParamId::gate2Enabled });
    p[ParamId::kelpDrive] = 0.2f;
    p[ParamId::kelpLevel] = 0.4f;
    p[ParamId::kelp2Drive] = 0.85f;
    p[ParamId::kelp2Tone] = 0.7f;
    p[ParamId::gyreSpeed] = 0.7f;
    p[ParamId::gyre2Speed] = 3.0f;
    p[ParamId::gyre2Stages] = 1.0f;
    p[ParamId::gateThreshold] = -60.0f;
    p[ParamId::gate2Threshold] = -30.0f;

    const auto play = [&] (const PedalOrder& order, const ParamSnapshot& snap) {
        PedalRack rack;
        rack.prepare ({ fs, block, 1 });
        rack.rewire (order);
        rack.reset();
        auto y = x;
        const auto settings = PedalSettings::from (snap);
        const std::array<bool, kNumSlots> none {};
        for (int s = 0; s < frames; s += block)
            rack.process (y.data() + s, x.data() + s, block, settings, none);
        return y;
    };
    const auto order = boardOf ({ PedalId::gate, PedalId::kelp, second (PedalId::gyre),
                                  second (PedalId::kelp), PedalId::gyre, second (PedalId::gate) });
    const auto y = play (order, p);

    // By hand: each copy its own module with its own settings, in the board's order, with the
    // delays of the five drives and fuzzes that are off in front of it (pure delays).
    PedalRack rack;
    rack.prepare ({ fs, block, 1 });
    const auto layout = rack.layoutOf (order);
    PedalRack::Slots hand;
    const ProcessSpec mono { fs, block, 1 };
#define SEDEF_PEDAL(id, name, category, rank, enable, Module, unused)                              \
    hand.id[0].prepare (mono);                                                                     \
    hand.id[1].prepare (mono);
#include "sedef/board/PedalList.inc"
#undef SEDEF_PEDAL
    const auto settings = PedalSettings::from (p);
    DelayLine keyLine;
    keyLine.prepare (rack.latencySamples() + block + 1);
    auto z = x;
    std::vector<float> key (block);
    for (int s = 0; s < frames; s += block)
    {
        float* io = z.data() + s;
        int before = 0;
        for (int i = 0; i < block; ++i)
            keyLine.push (x[static_cast<size_t> (s + i)]);
        for (const auto step : layout)
        {
            for (int i = 0; i < block; ++i)
                key[static_cast<size_t> (i)] = keyLine.read (before + block - 1 - i);
            const auto slot = slotIndex (step.ref);
            const bool enabled = step.onBoard && settings.on[slot];
            const auto run = [&] (auto& pedal, const auto& set) {
                pedal.process (AudioView (io, block), enabled, set,
                               [&] (auto& m, const AudioView& v) {
                                   if constexpr (requires { m.process (v, key.data()); })
                                       m.process (v, key.data());
                                   else
                                       m.process (v);
                               });
            };
            switch (step.ref.id)
            {
            case PedalId::gate:
                run (hand.gate[step.ref.instance], settings.gate[step.ref.instance]);
                break;
            case PedalId::kelp:
                run (hand.kelp[step.ref.instance], settings.kelp[step.ref.instance]);
                break;
            case PedalId::gyre:
                run (hand.gyre[step.ref.instance], settings.gyre[step.ref.instance]);
                break;
            case PedalId::remora:
                run (hand.remora[0], settings.remora[0]);
                break;
            case PedalId::urchin:
                run (hand.urchin[0], settings.urchin[0]);
                break;
            case PedalId::sargasso:
                run (hand.sargasso[0], settings.sargasso[0]);
                break;
            case PedalId::pearl:
                run (hand.pearl[0], settings.pearl[0]);
                break;
            case PedalId::coral:
                run (hand.coral[0], settings.coral[0]);
                break;
            case PedalId::breaker:
                run (hand.breaker[0], settings.breaker[0]);
                break;
            default:
                FAIL ("not on this board");
            }
            before += rack.latencyOf (step.ref);
        }
    }
    CHECK (y == z);

    // A copy's knobs reach only that copy: with only the first Kelp on the board, the second's
    // drive changes nothing, and the other way round.
    auto q = p;
    q[ParamId::kelp2Drive] = 0.05f;
    CHECK (play (boardOf ({ PedalId::kelp }), p) == play (boardOf ({ PedalId::kelp }), q));
    q = p;
    q[ParamId::kelpDrive] = 0.95f;
    CHECK (play (boardOf ({ second (PedalId::kelp) }), p) ==
           play (boardOf ({ second (PedalId::kelp) }), q));
    CHECK (play (boardOf ({ second (PedalId::kelp) }), p) != play (boardOf ({ PedalId::kelp }), p));
}

TEST_CASE ("Second copies never change the latency", "[board]")
{
    // Every pedal switched off: any board is the same pure delay, second copies and pedals after
    // the amp included, through the whole engine, at every rate and in either mode.
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0, 192000.0);
    const auto mode = GENERATE (OversamplingMode::lowLatency, OversamplingMode::linearPhase);
    CAPTURE (fs, static_cast<int> (mode));
    const Board boards[] = {
        Board {},
        Board::legacy(),
        sides ({ PedalId::kelp, second (PedalId::kelp), PedalId::pearl, second (PedalId::pearl),
                 PedalId::coral, second (PedalId::coral), PedalId::breaker },
               { PedalId::gyre, second (PedalId::jetty) }),
        sides ({ second (PedalId::breaker), PedalId::gate, second (PedalId::gate) },
               { PedalId::ripple, PedalId::beacon, second (PedalId::ripple) }),
    };
    std::vector<float> first;
    int latency = -1;
    for (const auto& board : boards)
    {
        BoardRig rig (fs, 128, 8192, 1, 2, mode);
        rig.engine.setBoard (board);
        rig.engine.reset();
        if (latency < 0)
            latency = rig.engine.latencySamples();
        CHECK (rig.engine.latencySamples() == latency);
        rig.in[0][1000] = 0.5f;
        rig.run (cleanRig(), 0, 8192);
        if (first.empty())
            first = rig.out[0];
        else
            CHECK (rig.out[0] == first);
    }
}

// -------------------------------------------------------------------------------------------------
// After the amp (docs/board.md §4.1, §6)

TEST_CASE ("Until a board comes, the engine plays the fixed chain and nothing after the amp",
           "[board]")
{
    // A host that never sends a board (and the tests that drive the engine directly) hears what
    // 0.10 played: the legacy board in front of the amp, nothing after it. Every modulation pedal
    // on shows a pedal doubled after the amp at once.
    constexpr double fs = 48000.0;
    std::vector<float> x (static_cast<std::size_t> (0.3 * fs));
    fillSine (x, 220.0, fs, 0.2f);
    auto p = cleanRig();
    for (const auto r : PedalOrder::everyPedal())
        if (fitsAfterAmp (r.id))
            p[enableOf (r)] = 1.0f;
    const auto played = [&] (const Board* board) {
        BoardRig rig (fs, 64, static_cast<int> (x.size()), 1, 2);
        rig.in[0] = x;
        if (board != nullptr)
            rig.engine.setBoard (*board);
        rig.engine.reset();
        rig.run (p, 0, static_cast<int> (x.size()));
        return std::array { rig.out[0], rig.out[1] };
    };
    const Board legacy = Board::legacy();
    const auto fresh = played (nullptr);
    const auto sent = played (&legacy);
    CHECK (fresh[0] == sent[0]);
    CHECK (fresh[1] == sent[1]);
}

TEST_CASE ("A pedal after the amp is the same pedal applied to the amp's output", "[board]")
{
    // The stage after the amp sits after the EQ and in front of everything else, which is off
    // here: the rig's output with nothing after the amp is exactly what reaches it.
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    const auto ref =
        GENERATE (PedalRef (PedalId::gyre), PedalRef (PedalId::jetty), PedalRef (PedalId::ripple),
                  PedalRef (PedalId::beacon), second (PedalId::gyre), second (PedalId::ripple));
    const int block = GENERATE (64, 333);
    CAPTURE (fs, pedalInfo (ref).id, ref.instance, block);
    const auto x = testSignal (true, fs, static_cast<int> (0.5 * fs));
    auto p = cleanRig ({ enableOf (ref) });
    p[ParamId::gyreFeedback] = 0.6f;
    p[ParamId::gyre2Speed] = 2.5f;
    p[ParamId::jettyFeedback] = 70.0f;
    p[ParamId::rippleMode] = 1.0f; // chorus: the mode that spreads
    p[ParamId::ripple2Mode] = 1.0f;
    p[ParamId::ripple2Speed] = 1.5f;

    const auto wet = renderStereo (fs, block, x, p, sides ({}, { ref }));
    auto dry = renderStereo (fs, block, x, p, Board {});
    CHECK (wet[0] != dry[0]);

    const auto settings = PedalSettings::from (p);
    const auto c = ref.instance;
    switch (ref.id)
    {
    case PedalId::gyre:
    {
        dsp::Gyre m;
        playOffline (m, settings.gyre[c], fs, block, dry[0], dry[1]);
        break;
    }
    case PedalId::jetty:
    {
        dsp::Jetty m;
        playOffline (m, settings.jetty[c], fs, block, dry[0], dry[1]);
        break;
    }
    case PedalId::ripple:
    {
        dsp::Ripple m;
        playOffline (m, settings.ripple[c], fs, block, dry[0], dry[1]);
        break;
    }
    case PedalId::beacon:
    {
        dsp::Beacon m;
        playOffline (m, settings.beacon[c], fs, block, dry[0], dry[1]);
        break;
    }
    default:
        FAIL ("not a pedal for after the amp");
    }
    CHECK (wet[0] == dry[0]);
    CHECK (wet[1] == dry[1]);
}

TEST_CASE ("Pedals on both sides and second copies play the same in any block size", "[board]")
{
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE (fs);
    const auto x = testSignal (true, fs, static_cast<int> (0.4 * fs));
    auto p = everyCopyOn();
    p[ParamId::rippleMode] = 1.0f;
    p[ParamId::gyre2Feedback] = 0.7f;
    p[ParamId::jetty2Speed] = 1.3f;
    const auto board = sides (
        { PedalId::gate, PedalId::tide, PedalId::kelp, second (PedalId::kelp),
          second (PedalId::tide), PedalId::gyre },
        { second (PedalId::gyre), PedalId::jetty, second (PedalId::jetty), PedalId::ripple });
    const auto play = [&] (int block) {
        SidesHarness h (fs, std::min (block, 512), board);
        std::vector<float> l, r;
        h.run (x, l, r, block, p);
        return std::array<std::vector<float>, 2> { l, r };
    };
    const auto reference = play (64);
    CHECK (reference[0] != reference[1]); // the stage after the amp is stereo
    // As every module's own block-size test holds it (AmpSectionTests): states flushed to zero
    // below 1e-15 at the end of a block leave differences far under the signal.
    for (const int block : { 1, 7, 37, 333, 512, 1000, 4096 })
    {
        CAPTURE (block);
        const auto y = play (block);
        float diff = 0.0f;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < x.size(); ++i)
                diff = std::max (diff, std::abs (y[c][i] - reference[c][i]));
        CHECK (diff < 1.0e-6f);
    }
}

// -------------------------------------------------------------------------------------------------
// Changing the board while it plays (docs/board.md §4.3)

TEST_CASE ("A pedal crosses the amp through its fades, without a duck", "[board]")
{
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    const int block = GENERATE (1, 64, 512);
    CAPTURE (fs, block);
    auto p = cleanRig ({ ParamId::tideEnabled, ParamId::gyreEnabled, ParamId::jettyEnabled });
    const auto x = testSignal (false, fs, static_cast<int> (0.8 * fs));
    const auto before = sides ({ PedalId::tide, PedalId::gyre }, { PedalId::jetty });
    const auto after = sides ({ PedalId::tide }, { PedalId::gyre, PedalId::jetty });
    const int at = (static_cast<int> (0.4 * fs) / block) * block;

    SidesHarness h (fs, std::min (block, 512), before);
    std::vector<float> l, r;
    bool ducked = false;
    int leftAt = -1, joinedAt = -1;
    h.run (x, l, r, block, p, [&] (int s) {
        if (s == at)
            h.setTarget (after);
        ducked = ducked || !h.preSwitch.duck().idle() || !h.postSwitch.duck().idle();
        if (leftAt < 0 && s > at && !h.pre.order().contains (PedalId::gyre))
            leftAt = s;
        if (joinedAt < 0 && s > at && h.post.order().contains (PedalId::gyre))
            joinedAt = s;
    });
    CHECK_FALSE (ducked);
    // Out in front within its 10 ms fade (and the block it ends in); in after the amp at once.
    const int fade = static_cast<int> (std::floor (0.01 * fs));
    REQUIRE (leftAt >= 0);
    REQUIRE (joinedAt >= 0);
    CHECK (leftAt - at >= fade);
    CHECK (leftAt - at <= fade + 2 * block);
    CHECK (joinedAt - at <= block);
    CHECK (h.pre.order() == after.pre);
    CHECK (h.post.order() == after.post);

    // No step, against the board before and after it.
    const auto ref = [&] (const Board& b) {
        SidesHarness g (fs, std::min (block, 512), b);
        std::vector<float> rl, rr;
        g.run (x, rl, rr, block, p);
        return rl;
    };
    const auto t = compareTransient (l, ref (before), ref (after), fs, at);
    CHECK (t.excessDb < -50.0f);
    CHECK (t.dipDb > -6.5f); // a phaser crossfading with its dry signal, twice
}

TEST_CASE ("A change ducks only the side it reorders", "[board]")
{
    const double fs = 48000.0;
    const int block = GENERATE (1, 64, 512);
    CAPTURE (block);
    auto p = cleanRig ({ ParamId::tideEnabled, ParamId::pearlEnabled, ParamId::gyreEnabled,
                         ParamId::jettyEnabled });
    p[ParamId::pearlGain] = 0.3f;
    const auto x = testSignal (true, fs, 38400);
    const int at = (19200 / block) * block;
    const auto play = [&] (const Board& from, const Board& to, bool& preDucked, bool& postDucked) {
        SidesHarness h (fs, std::min (block, 512), from);
        std::vector<float> l, r;
        h.run (x, l, r, block, p, [&] (int s) {
            if (s == at)
                h.setTarget (to);
            preDucked = preDucked || !h.preSwitch.duck().idle();
            postDucked = postDucked || !h.postSwitch.duck().idle();
        });
        CHECK (h.pre.order() == to.pre);
        CHECK (h.post.order() == to.post);
        return l;
    };
    const auto start = sides ({ PedalId::tide, PedalId::pearl }, { PedalId::gyre, PedalId::jetty });

    // Two pedals after the amp swap: that side ducks, the front doesn't.
    bool preDucked = false, postDucked = false;
    play (start, sides ({ PedalId::tide, PedalId::pearl }, { PedalId::jetty, PedalId::gyre }),
          preDucked, postDucked);
    CHECK_FALSE (preDucked);
    CHECK (postDucked);

    // A pedal crosses the amp while the front is reordered: the front ducks, the back fades.
    preDucked = postDucked = false;
    play (start, sides ({ PedalId::pearl, PedalId::tide, PedalId::jetty }, { PedalId::gyre }),
          preDucked, postDucked);
    CHECK (preDucked);
    CHECK_FALSE (postDucked);

    // Both at once: each ducks on its own.
    preDucked = postDucked = false;
    play (start, sides ({ PedalId::pearl, PedalId::tide }, { PedalId::jetty, PedalId::gyre }),
          preDucked, postDucked);
    CHECK (preDucked);
    CHECK (postDucked);
}

namespace
{
/// Through the whole rig: Gyre crossing the amp, and Gyre and Jetty swapping after it.
struct RigTransients
{
    Transient cross, swap;
};

RigTransients rigTransients (double fs, bool guitar)
{
    // The phaser and flanger hold still (depth 0), so the renders that play either board from
    // the start are a fair reference: a sweep restarted by the change would differ from theirs
    // by where it is, not by a click.
    auto p = cleanRig ({ ParamId::tideEnabled, ParamId::gyreEnabled, ParamId::jettyEnabled });
    p[ParamId::gyreDepth] = 0.0f;
    p[ParamId::jettyDepth] = 0.0f;
    const auto x = testSignal (guitar, fs, static_cast<int> (1.2 * fs));
    const int at = (static_cast<int> (0.6 * fs) / 64) * 64;
    const auto transient = [&] (const Board& a, const Board& b) {
        return compareTransient (renderEngine (fs, 64, x, p, a, &b, at),
                                 renderEngine (fs, 64, x, p, a, nullptr, 0),
                                 renderEngine (fs, 64, x, p, b, nullptr, 0), fs, at);
    };
    return {
        transient (sides ({ PedalId::tide, PedalId::gyre }, { PedalId::jetty }),
                   sides ({ PedalId::tide }, { PedalId::gyre, PedalId::jetty })),
        transient (sides ({ PedalId::tide }, { PedalId::gyre, PedalId::jetty }),
                   sides ({ PedalId::tide }, { PedalId::jetty, PedalId::gyre })),
    };
}
} // namespace

TEST_CASE ("Through the whole rig, crossing the amp and swapping after it stay click-free",
           "[board]")
{
    const double fs = GENERATE (48000.0, 96000.0);
    const bool guitar = GENERATE (false, true);
    CAPTURE (fs, guitar);
    const auto [cross, swap] = rigTransients (fs, guitar);
    // Gyre crosses the amp: two bypass fades, no step.
    CHECK (cross.excessDb < -50.0f);
    CHECK (cross.dipDb > -12.0f);
    // The two after the amp swap: that side's duck. Nothing behind it fills the gap, so the rig
    // dips to silence for about two fades, without a click.
    CHECK (swap.dipDb < -40.0f);
    CHECK (swap.dipMs <= 2.0 * 1000.0 * BoardSwitch::kDuckSeconds + 2.0);
    CHECK (swap.excessDb < -50.0f);
    // The repo's click bound for automation sweeps (PedalTestUtils.h).
    CHECK (swap.slopeRatio <= (guitar ? 1.2f : 1.6f));
}

TEST_CASE ("Crossing the amp and swapping after it (measurement)", "[.board-measure]")
{
    for (const double fs : { 48000.0, 96000.0 })
        for (const bool guitar : { false, true })
        {
            const auto [cross, swap] = rigTransients (fs, guitar);
            for (const auto& [what, t] :
                 { std::pair { "cross the amp", cross }, std::pair { "swap after", swap } })
                std::printf ("%-13s %-6s %6.0f Hz: dip %6.1f dB, %4.1f ms below -1 dB, swell "
                             "%+.1f dB, slope x%.2f (excess %.0f dB)\n",
                             what, guitar ? "guitar" : "sine", fs, static_cast<double> (t.dipDb),
                             static_cast<double> (t.dipMs), static_cast<double> (t.swellDb),
                             static_cast<double> (t.slopeRatio), static_cast<double> (t.excessDb));
        }
}

// -------------------------------------------------------------------------------------------------
// Stereo after the amp (docs/board.md §6)

namespace
{
/// The mono sum of a stereo modulation on white noise (docs/board.md §6.5), in 4096-frame
/// windows (85 ms: a few per cent of the sweeps below, and a power estimate within about 0.1 dB
/// on noise).
struct MonoSum
{
    std::string name;
    bool firstSideUnchanged = false; ///< the first side is the pedal as it was, in mono
    double worstLoss = 1.0;          ///< the sum's power against the two sides' mean, lowest
    double below = 0.0;              ///< the pedal's quietest mono window against the sum's
    double above = 0.0;              ///< the sum against the pedal's loudest mono window
};

std::vector<MonoSum> measureMonoSums()
{
    const double fs = 48000.0;
    constexpr int window = 4096;
    const int frames = static_cast<int> (8.0 * fs); // two cycles of the slowest sweep below
    std::vector<float> noise (static_cast<std::size_t> (frames));
    fillNoise (noise, 0.25f, 7);

    struct Case
    {
        std::string name;
        std::function<void (const AudioView&, int channels)> run;
    };
    // Each pedal at full depth, slow enough to sweep through every window size of interest,
    // feedback off and at its most.
    const auto make = [&] (auto settings, auto tag) {
        using Module = typename decltype (tag)::type;
        return [settings] (const AudioView& io, int channels) {
            Module m;
            m.prepare ({ 48000.0, 64, channels });
            m.setSettings (settings);
            m.reset();
            for (int s = 0; s < io.numFrames(); s += 64)
            {
                m.setSettings (settings);
                m.process (io.subView (s, std::min (64, io.numFrames() - s)));
            }
        };
    };
    std::vector<Case> cases;
    struct GyreTag
    {
        using type = dsp::Gyre;
    };
    struct JettyTag
    {
        using type = dsp::Jetty;
    };
    struct RippleTag
    {
        using type = dsp::Ripple;
    };
    for (const float fb : { 0.0f, 1.0f })
    {
        const std::string feedback = fb > 0.0f ? ", feedback" : "";
        cases.push_back (
            { "gyre" + feedback,
              make (dsp::Gyre::Settings { 0.5f, 1.0f, fb, dsp::Gyre::Six }, GyreTag {}) });
        cases.push_back ({ "jetty 10 ms" + feedback,
                           make (dsp::Jetty::Settings { 0.25f, 1.0f, 10.0f, fb }, JettyTag {}) });
        cases.push_back ({ "jetty 0.5 ms" + feedback,
                           make (dsp::Jetty::Settings { 0.25f, 1.0f, 0.5f, -fb }, JettyTag {}) });
    }
    cases.push_back (
        { "ripple chorus", make (dsp::Ripple::Settings { 1, 0.69f, 1.0f }, RippleTag {}) });

    std::vector<MonoSum> results;
    for (const auto& c : cases)
    {
        MonoSum m;
        m.name = c.name;
        // Stereo, the same noise on both sides.
        auto l = noise, r = noise;
        std::array<float*, 2> ptrs { l.data(), r.data() };
        c.run (AudioView (ptrs.data(), 2, frames), 2);
        // The same pedal without the spread: one channel.
        auto mono = noise;
        c.run (AudioView (mono.data(), frames), 1);
        m.firstSideUnchanged = l == mono;

        double lowest = 1e9, highest = 0.0;
        for (int s = 0; s + window <= frames; s += window)
        {
            const double pm = power (mono.data() + s, window);
            lowest = std::min (lowest, pm);
            highest = std::max (highest, pm);
        }
        std::vector<float> sum (window);
        for (int s = static_cast<int> (0.1 * fs); s + window <= frames; s += window)
        {
            for (int i = 0; i < window; ++i)
                sum[static_cast<size_t> (i)] =
                    0.5f * (l[static_cast<size_t> (s + i)] + r[static_cast<size_t> (s + i)]);
            const double pm = power (sum.data(), window);
            const double sidesMean =
                0.5 * (power (l.data() + s, window) + power (r.data() + s, window));
            m.worstLoss = std::min (m.worstLoss, pm / sidesMean);
            m.below = std::max (m.below, lowest / pm);
            m.above = std::max (m.above, pm / highest);
        }
        results.push_back (m);
    }
    return results;
}

double toDb (double powerRatio)
{
    return 10.0 * std::log10 (powerRatio);
}
} // namespace

TEST_CASE ("The stereo modulation's mono sum stays within bounds", "[board]")
{
    for (const auto& m : measureMonoSums())
    {
        CAPTURE (m.name);
        CHECK (m.firstSideUnchanged);
        // No cancellation: never more than 3 dB under the two sides' mean (measured: at most
        // 2.9 dB, the phaser at full feedback; docs/board.md §6.6).
        CHECK (m.worstLoss >= 0.5);
        // Never louder than the pedal itself gets (measured: 0.2 dB under at the closest)...
        CHECK (m.above <= 1.0);
        // ...and at most 2.5 dB under its quietest moment (measured: 2.0 dB).
        CHECK (m.below <= std::pow (10.0, 0.25));
    }
}

TEST_CASE ("The stereo modulation's mono sum (measurement)", "[.board-measure]")
{
    std::printf ("mono sum on noise, 4096-frame windows: worst loss against the sides' mean; "
                 "loudest against the pedal's loudest; quietest against the pedal's quietest\n");
    for (const auto& m : measureMonoSums())
        std::printf ("  %-24s loss %5.2f dB, above %+5.2f dB, below %5.2f dB\n", m.name.c_str(),
                     -toDb (m.worstLoss), toDb (m.above), toDb (m.below));
}

TEST_CASE ("Tremolo, vibrato and the photocell vibe stay mono-identical after the amp", "[board]")
{
    const double fs = 48000.0;
    const int frames = 48000;
    std::vector<float> noise (static_cast<std::size_t> (frames));
    fillNoise (noise, 0.25f, 9);
    const auto same = [&] (auto& m, const auto& settings) {
        m.prepare ({ fs, 64, 2 });
        m.setSettings (settings);
        m.reset();
        auto l = noise, r = noise;
        for (int s = 0; s < frames; s += 64)
        {
            std::array<float*, 2> ptrs { l.data() + s, r.data() + s };
            m.setSettings (settings);
            m.process (AudioView (ptrs.data(), 2, 64));
        }
        return l == r && l != noise;
    };
    dsp::Ripple tremolo, vibrato;
    dsp::Beacon chorus, vibe;
    CHECK (same (tremolo, dsp::Ripple::Settings { dsp::Ripple::Tremolo, 5.0f, 1.0f }));
    CHECK (same (vibrato, dsp::Ripple::Settings { dsp::Ripple::Vibrato, 5.0f, 1.0f }));
    CHECK (same (chorus, dsp::Beacon::Settings { 3.0f, 1.0f, dsp::Beacon::Chorus }));
    CHECK (same (vibe, dsp::Beacon::Settings { 3.0f, 1.0f, dsp::Beacon::Vibrato }));
}
