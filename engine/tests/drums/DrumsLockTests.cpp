// SPDX-License-Identifier: AGPL-3.0-or-later
// The drums and the looper (docs/drums.md, section 3.6; docs/looper.md, "Lock"): a take in bars
// starts on a drum bar line, the count-in is a groove bar, the drums stay on the loop's wraps pass
// after pass, and drums started mid-loop snap to it with a whole bar.
#include "DrumsTestUtils.h"
#include "sedef/dsp/Looper.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <memory>
#include <vector>

using namespace sedef;
using namespace sedef::test::drums;
using sedef::dsp::Looper;
using sedef::dsp::LoopMemory;
using Kind = Looper::Event::Kind;
using Command = Looper::Command;

namespace
{
/// A metronome playing drums beside a looper, wired as Engine::process wires them: the looper
/// hears the metronome's downbeats and its presses at the block start, then every wrap re-aligns
/// the metronome, then the metronome plays. The input is the sample's own index, so a loop's
/// samples say where its take started.
struct LockRig
{
    LockRig (double rate, const Metronome::Settings& s)
        : fs (rate),
          settings (s),
          memory (std::make_unique<LoopMemory>())
    {
        metronome.prepare ({ fs, 512, 2 });
        metronome.setSettings (settings);
        looper.prepare ({ fs, 512, 2 });
        looper.attach (memory.get());
    }

    /// Processes up to sample `until` in blocks of 512, 97 and 300 in turn (shorter where
    /// `until` falls), a press acting at the first block's start.
    void runTo (long long until, bool press = false)
    {
        while (t < until)
        {
            const int size = blockSizes[static_cast<size_t> (numBlocks++ % blockSizes.size())];
            const int n = static_cast<int> (std::min<long long> (size, until - t));
            block (n, press);
            press = false;
        }
    }

    void block (int n, bool press)
    {
        metronome.setSettings (settings);
        std::array<int, 8> db {};
        const int nd = metronome.downbeats (n, db.data(), static_cast<int> (db.size()));
        std::vector<Looper::Event> ev;
        for (int k = 0; k < nd; ++k)
            ev.push_back ({ db[static_cast<size_t> (k)], Kind::downbeat, Command::main });
        if (press)
            ev.push_back ({ 0, Kind::command, Command::main });
        Looper::Settings ls;
        ls.countIn = countIn;
        ls.clickRunning = metronome.isRunning();
        ls.clickBpm = settings.bpm;
        ls.clickSignature = settings.signature;
        ls.barSamples = metronome.barSamples();
        looper.setSettings (ls);

        auto view = buffer.view().subView (0, n);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                view.channel (c)[i] = static_cast<float> (t + i);
        looper.process (view, ev.data(), static_cast<int> (ev.size()));
        for (int k = 0; k < looper.numWraps(); ++k)
        {
            metronome.alignDownbeat (looper.wrap (k));
            wraps.push_back (t + looper.wrap (k));
        }
        metronome.process (view);
        const Metronome::FiredEvent* fired = nullptr;
        const int count = metronome.firedEvents (&fired);
        for (int k = 0; k < count; ++k)
            events.push_back ({ t + fired[k].offset, fired[k].instrument, fired[k].velocity });
        // A block is far shorter than a bar: a wrap's bar is the only one that starts in it.
        for (int k = 0; k < looper.numWraps(); ++k)
            barAtWrap.push_back (metronome.groovePlayer().bar());
        t += n;
    }

    /// Where the take started: its samples are the input's indices.
    long long takeStart() const
    {
        const auto loop = looper.loopSamples (0);
        return static_cast<long long> (loop.at (1000)) - 1000;
    }

    bool kickAt (long long sample) const
    {
        return std::any_of (events.begin(), events.end(), [sample] (const Fired& e) {
            return e.sample == sample && e.instrument == Instrument::kick;
        });
    }

    std::vector<Fired> eventsIn (long long from, long long to) const
    {
        std::vector<Fired> out;
        for (const auto& e : events)
            if (e.sample >= from && e.sample < to)
                out.push_back (e);
        return out;
    }

    double fs;
    Metronome::Settings settings;
    bool countIn = false;
    std::unique_ptr<LoopMemory> memory;
    Metronome metronome;
    Looper looper;
    test::Buffer buffer { 2, 512 };
    std::array<int, 3> blockSizes { 512, 97, 300 };
    int numBlocks = 0;
    long long t = 0;
    std::vector<long long> wraps;
    std::vector<Fired> events;
    std::vector<int> barAtWrap; ///< the groove's bar count when each wrap's bar started
};

/// Straight rock in 4/4 at 117 bpm: a bar of 98 461.54 samples at 48 kHz, so the grid's bars
/// alternate in length and the lock has something to hold.
GrooveSetup rock117()
{
    GrooveSetup g;
    g.bpm = 117.0f;
    g.groove = 0;
    return g;
}

long long downbeat (double bar, int k)
{
    return static_cast<long long> (std::ceil (static_cast<double> (k) * bar));
}
} // namespace

TEST_CASE ("Drums and the looper: a take in bars starts on a drum bar line, the count-in is a "
           "groove bar",
           "[drums][looper][lock]")
{
    const auto g = rock117();
    const bool countIn = GENERATE (false, true);
    CAPTURE (countIn);
    LockRig rig (g.fs, settingsFor (g));
    rig.countIn = countIn;
    const double bar = rig.metronome.barSamples();
    // Record pressed in the middle of bar 0; closed past half of the take's second bar, so it
    // rounds up to two bars.
    rig.runTo (30000);
    rig.runTo (30001, true);
    const int startBar = countIn ? 2 : 1;
    const long long s = downbeat (bar, startBar);
    rig.runTo (s + static_cast<long long> (1.7 * bar));
    rig.runTo (s + static_cast<long long> (1.7 * bar) + 1, true);
    rig.runTo (s + static_cast<long long> (6.5 * bar));

    REQUIRE (rig.looper.state() == Looper::State::playing);
    CHECK (rig.looper.bars() == 2);
    CHECK (rig.looper.length() == downbeat (bar, startBar + 2) - s);
    // The take starts on the drums' "1": the kick of the groove's downbeat sounds on its sample.
    CHECK (rig.takeStart() == s);
    CHECK (rig.kickAt (s));

    // The count-in is the bar before it, and the drums play it as any bar of the groove: the
    // same events as a metronome alone, the downbeat's kick on its first sample.
    const long long before = downbeat (bar, startBar - 1);
    const auto alone = renderGroove (g, static_cast<int> (s), 512);
    std::vector<Fired> expected;
    for (const auto& e : alone.events)
        if (e.sample >= before && e.sample < s)
            expected.push_back (e);
    CHECK (rig.eventsIn (before, s) == expected);
    CHECK (rig.kickAt (before));
    CHECK (expected.size() >= 8);
}

TEST_CASE ("Drums and the looper: every wrap is a drum downbeat, pass after pass",
           "[drums][looper][lock]")
{
    const auto g = rock117();
    LockRig rig (g.fs, settingsFor (g));
    const double bar = rig.metronome.barSamples();
    // A three-bar take from bar 1, then ten minutes of it.
    rig.runTo (30000);
    rig.runTo (30001, true);
    const long long s = downbeat (bar, 1);
    rig.runTo (s + static_cast<long long> (2.7 * bar));
    rig.runTo (s + static_cast<long long> (2.7 * bar) + 1, true);
    rig.runTo (static_cast<long long> (g.fs * 600.0));

    REQUIRE (rig.looper.synced());
    CHECK (rig.looper.bars() == 3);
    REQUIRE (rig.wraps.size() >= 95); // a pass is 6.15 s
    REQUIRE (rig.barAtWrap.size() == rig.wraps.size());
    int onTheOne = 0;
    for (const auto w : rig.wraps)
        onTheOne += rig.kickAt (w) ? 1 : 0;
    CHECK (onTheOne == static_cast<int> (rig.wraps.size()));
    // Every pass is three bars of the groove, none doubled and none skipped, each bar line a
    // step 0 of the grid the wrap re-anchored, and the passes as long as the loop.
    const double interval = bar / 4.0;
    int wholePasses = 0;
    for (size_t k = 0; k + 1 < rig.wraps.size(); ++k)
    {
        const long long w = rig.wraps[k];
        INFO ("pass " << k);
        CHECK (rig.wraps[k + 1] - w == rig.looper.length());
        bool lines = true;
        for (int b = 1; b < 3; ++b)
            lines = lines && rig.kickAt (static_cast<long long> (
                                 std::ceil (static_cast<double> (w) + 4.0 * b * interval)));
        CHECK (lines);
        wholePasses += rig.barAtWrap[k + 1] - rig.barAtWrap[k] == 3 ? 1 : 0;
    }
    CHECK (wholePasses == static_cast<int> (rig.wraps.size()) - 1);
}

TEST_CASE ("Drums started in the middle of a loop snap to its next wrap with a whole bar",
           "[drums][looper][lock]")
{
    const auto g = rock117();
    LockRig rig (g.fs, settingsFor (g));
    const double bar = rig.metronome.barSamples();
    const double step = bar / 4.0;
    rig.runTo (30000);
    rig.runTo (30001, true);
    const long long s = downbeat (bar, 1);
    rig.runTo (s + static_cast<long long> (1.7 * bar));
    rig.runTo (s + static_cast<long long> (1.7 * bar) + 1, true);
    rig.runTo (s + static_cast<long long> (2.5 * bar)); // playing, past its first wrap
    REQUIRE (rig.looper.synced());
    const long long L = rig.looper.length();
    const long long w1 = s + 2 * L; // the second wrap

    // Stopped in the first pass, started again 3.25 steps before the wrap: steps 0 to 3 sound,
    // step 3 a quarter of a step before the wrap. A click would count that step as the "1"; the
    // drums can't, since a bar's downbeat plays only on its step 0, so the bar starts again on
    // the wrap.
    rig.settings.enabled = false;
    rig.runTo (s + L + L / 3);
    const long long restart = w1 - static_cast<long long> (std::llround (3.25 * step));
    rig.runTo (restart);
    rig.settings.enabled = true;
    rig.runTo (w1 + static_cast<long long> (step / 2));
    const auto lastStep =
        static_cast<long long> (std::ceil (static_cast<double> (restart) + 3.0 * step));
    REQUIRE (lastStep < w1);
    REQUIRE (static_cast<double> (w1 - lastStep) < 0.5 * step);
    CHECK (rig.kickAt (restart));
    CHECK (rig.kickAt (w1));
    CHECK (std::find (rig.wraps.begin(), rig.wraps.end(), w1) != rig.wraps.end());
    CHECK (rig.metronome.groovePlayer().bar() == 1); // the restart's bar 0, then bar 1 on the wrap

    // And from there it stays locked.
    rig.runTo (w1 + 5 * L + 100);
    for (int k = 0; k <= 5; ++k)
    {
        INFO ("pass " << k);
        CHECK (rig.kickAt (w1 + k * L));
    }
}

TEST_CASE ("Drums switched on in the block of a wrap start on the wrap, as a fresh run",
           "[drums][looper][lock]")
{
    const auto g = rock117();
    LockRig rig (g.fs, settingsFor (g));
    const double bar = rig.metronome.barSamples();
    rig.runTo (30000);
    rig.runTo (30001, true);
    const long long s = downbeat (bar, 1);
    rig.runTo (s + static_cast<long long> (1.7 * bar));
    rig.runTo (s + static_cast<long long> (1.7 * bar) + 1, true);
    rig.runTo (s + static_cast<long long> (2.5 * bar));
    REQUIRE (rig.looper.synced());
    const long long L = rig.looper.length();
    const long long w = s + 3 * L; // the third wrap

    // Off for a while, then on in a block that holds the wrap 100 samples in: the run starts on
    // the wrap itself, its first bar the groove's bar 0, exactly as a metronome started there.
    rig.settings.enabled = false;
    rig.runTo (w - 100);
    rig.settings.enabled = true;
    rig.block (512, false);
    rig.runTo (w + static_cast<long long> (bar) + 10);
    REQUIRE (std::find (rig.wraps.begin(), rig.wraps.end(), w) != rig.wraps.end());
    CHECK (rig.eventsIn (s + 2 * L, w).empty());

    const auto fresh = renderGroove (g, static_cast<int> (bar) + 10, 512);
    std::vector<Fired> expected;
    for (auto e : fresh.events)
    {
        e.sample += w;
        expected.push_back (e);
    }
    CHECK (rig.eventsIn (w, w + static_cast<long long> (bar) + 10) == expected);
    CHECK (rig.metronome.groovePlayer().bar() == 1); // bar 0 from the wrap, bar 1 just begun
}
