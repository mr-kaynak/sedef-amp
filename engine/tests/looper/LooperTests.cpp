// SPDX-License-Identifier: AGPL-3.0-or-later
// The looper (docs/looper.md): sample-exact timing, crossfade bounds, bit-exact undo, invariance.
#include "TestUtils.h"
#include "sedef/dsp/Looper.h"
#include "sedef/dsp/Metronome.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <memory>
#include <vector>

using namespace sedef;
using sedef::dsp::Looper;
using sedef::dsp::LoopMemory;
using Command = Looper::Command;
using State = Looper::State;
using Kind = Looper::Event::Kind;

namespace
{
/// One pool for every test (92 MB): loopers here run one at a time, and a looper never reads a
/// chunk it hasn't written in its own run.
LoopMemory& memory()
{
    static auto m = std::make_unique<LoopMemory>();
    return *m;
}

struct TimedEvent
{
    long long at = 0;
    Kind kind = Kind::command;
    Command command = Command::main;
};

TimedEvent press (long long at, Command c = Command::main)
{
    return { at, Kind::command, c };
}

/// Downbeats of a grid like the metronome's: ceil(anchor + k * bar).
std::vector<TimedEvent> downbeats (double bar, long long until, double anchor = 0.0)
{
    std::vector<TimedEvent> out;
    for (long long k = 0;; ++k)
    {
        const auto t = static_cast<long long> (std::ceil (anchor + static_cast<double> (k) * bar));
        if (t >= until)
            break;
        out.push_back ({ t, Kind::downbeat, Command::main });
    }
    return out;
}

std::vector<TimedEvent> merged (std::vector<TimedEvent> a, const std::vector<TimedEvent>& b)
{
    a.insert (a.end(), b.begin(), b.end());
    std::stable_sort (a.begin(), a.end(),
                      [] (const TimedEvent& x, const TimedEvent& y) { return x.at < y.at; });
    return a;
}

/// Deterministic stereo input: noise in both channels, different seeds.
test::Buffer noiseInput (int frames, float amp = 0.25f)
{
    test::Buffer in (2, frames);
    test::fillNoise (in[0], amp, 11);
    test::fillNoise (in[1], amp, 23);
    return in;
}

struct Run
{
    std::unique_ptr<Looper> looper = std::make_unique<Looper>();
    test::Buffer out { 2, 1 };
    std::vector<State> states; ///< after each block
};

/// Processes `in` through a fresh looper in blocks of `block`, delivering each event on its
/// sample. `perBlock` may change settings at block starts.
template <typename PerBlock>
Run render (const test::Buffer& in, int block, double fs, const Looper::Settings& settings,
            const std::vector<TimedEvent>& events, PerBlock&& perBlock, int maxFrames = 0)
{
    Run r;
    r.out = in;
    auto& lp = *r.looper;
    lp.prepare ({ fs, block, 2 });
    lp.attach (&memory());
    if (maxFrames > 0)
        lp.setMaxFramesForTest (maxFrames);
    lp.setSettings (settings);
    std::vector<Looper::Event> blockEvents;
    std::size_t next = 0;
    for (int start = 0; start < r.out.frames(); start += block)
    {
        const int n = std::min (block, r.out.frames() - start);
        auto s = settings;
        perBlock (s, start);
        lp.setSettings (s);
        blockEvents.clear();
        while (next < events.size() && events[next].at < start + n)
        {
            const auto& e = events[next++];
            if (e.at >= start)
                blockEvents.push_back ({ static_cast<int> (e.at - start), e.kind, e.command });
        }
        lp.process (r.out.view().subView (start, n), blockEvents.data(),
                    static_cast<int> (blockEvents.size()));
        r.states.push_back (lp.state());
    }
    return r;
}

Run render (const test::Buffer& in, int block, double fs, const Looper::Settings& settings,
            const std::vector<TimedEvent>& events, int maxFrames = 0)
{
    return render (in, block, fs, settings, events, [] (Looper::Settings&, int) {}, maxFrames);
}

int fadeLen (double fs)
{
    return static_cast<int> (std::lround (Looper::kFadeSeconds * fs));
}

float rc (int k, int len)
{
    if (k <= 0)
        return 0.0f;
    if (k >= len)
        return 1.0f;
    return 0.5f * (1.0f - std::cos (kPi * static_cast<float> (k) / static_cast<float> (len)));
}

Looper::Settings clickSettings (double bar)
{
    Looper::Settings s;
    s.clickRunning = true;
    s.barSamples = bar;
    return s;
}
} // namespace

// -------------------------------------------------------------------------------------------------
// Free length

TEST_CASE ("Looper: a free take starts on the press and is as long as the presses are apart",
           "[looper]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const auto in = noiseInput (static_cast<int> (fs * 3));
    const long long p = 7001, L = static_cast<long long> (fs * 0.9) + 13;
    const auto r = render (in, 64, fs, {}, { press (p), press (p + L) });
    const auto& lp = *r.looper;
    REQUIRE (lp.state() == State::playing);
    REQUIRE (lp.length() == L);
    const int X = fadeLen (fs);

    for (int c = 0; c < 2; ++c)
    {
        const auto loop = lp.loopSamples (c);
        auto& x = const_cast<test::Buffer&> (in)[c];
        // Loop sample i is the input at p + i, sample for sample, up to the seam...
        bool same = true;
        for (long long i = 0; i < L - X; ++i)
            same = same && loop[static_cast<size_t> (i)] == x[static_cast<size_t> (p + i)];
        CHECK (same);
        // ...where its end fades into the X samples before p (they run into loop sample 0).
        for (int j = 0; j < X; ++j)
        {
            const float end = x[static_cast<size_t> (p + L - X + j)];
            const float pre = x[static_cast<size_t> (p - X + j)];
            const float w = rc (j + 1, X);
            CHECK (loop[static_cast<size_t> (L - X + j)] == end + w * (pre - end));
        }
        // Loop sample 0 plays on the closing press, fading in over X, then at the loop's level
        // (0 dB: exactly 1); the second pass runs on through the seam.
        auto& y = const_cast<Run&> (r).out[c];
        for (long long i = 0; i < 2 * L && p + L + i < static_cast<long long> (y.size()); ++i)
        {
            const auto t = static_cast<size_t> (p + L + i);
            const float g = i < X ? rc (static_cast<int> (i) + 1, X) * 1.0f : 1.0f;
            const float part = g * loop[static_cast<size_t> (i % L)];
            const float expected = part != 0.0f ? x[t] + part : x[t];
            if (y[t] != expected)
            {
                FAIL ("playback differs at pass sample " << i);
            }
        }
        // Before the close only the live signal is heard.
        for (long long t = 0; t < p + L; ++t)
            if (y[static_cast<size_t> (t)] != x[static_cast<size_t> (t)])
                FAIL ("the take changed the live signal at " << t);
    }
}

TEST_CASE ("Looper: latency compensation writes the input that much earlier", "[looper]")
{
    const double fs = 48000.0;
    const auto in = noiseInput (static_cast<int> (fs * 3));
    Looper::Settings s;
    s.latencySamples = 317;
    const long long p = 5000, L = 40000;
    const auto r = render (in, 128, fs, s, { press (p), press (p + L) });
    const auto loop = r.looper->loopSamples (0);
    REQUIRE (loop.size() == static_cast<size_t> (L));
    auto& x = const_cast<test::Buffer&> (in)[0];
    // Played R before it reached the looper: loop sample i is the input at p + R + i.
    for (long long i = 0; i < L - fadeLen (fs); ++i)
        if (loop[static_cast<size_t> (i)] != x[static_cast<size_t> (p + 317 + i)])
            FAIL ("loop sample " << i);
}

TEST_CASE ("Looper: a take shorter than the minimum keeps recording", "[looper]")
{
    const double fs = 48000.0;
    const auto in = noiseInput (48000);
    const long long p = 1000;
    const auto r = render (in, 64, fs, {}, { press (p), press (p + 500) });
    CHECK (r.looper->length() == static_cast<int> (std::lround (Looper::kMinLoopSeconds * fs)));
    CHECK (r.looper->state() == State::playing);
}

TEST_CASE ("Looper: the longest take closes by itself and plays", "[looper]")
{
    const double fs = 48000.0;
    const auto in = noiseInput (100000);
    const auto r = render (in, 512, fs, {}, { press (1000) }, 30000);
    CHECK (r.looper->length() == 30000);
    CHECK (r.looper->state() == State::playing);
}

TEST_CASE ("Looper: Stop during a take closes it and stays stopped; Play fades in from 0",
           "[looper]")
{
    const double fs = 48000.0;
    const auto in = noiseInput (120000);
    const long long p = 1000, q = 31000, playAt = 50000;
    const auto r = render (in, 64, fs, {}, { press (p), press (q, Command::stop), press (playAt) });
    const auto& lp = *r.looper;
    CHECK (lp.length() == q - p);
    CHECK (lp.state() == State::playing);
    // Silent between the stop and the play; then loop sample 0 at playAt, faded in.
    auto& x = const_cast<test::Buffer&> (in)[0];
    auto& y = const_cast<Run&> (r).out[0];
    for (long long t = q; t < playAt; ++t)
        REQUIRE (y[static_cast<size_t> (t)] == x[static_cast<size_t> (t)]);
    const auto loop = lp.loopSamples (0);
    const int X = fadeLen (fs);
    CHECK (y[static_cast<size_t> (playAt)] ==
           x[static_cast<size_t> (playAt)] + (rc (1, X) * 1.0f) * loop[0]);
}

// -------------------------------------------------------------------------------------------------
// Bars

TEST_CASE ("Looper: in bars a take starts exactly on the next downbeat", "[looper][bars]")
{
    const double fs = 48000.0;
    const double bar = 4.0 * fs * 60.0 / 117.0; // 4/4 at 117 bpm: 98 461.53... samples
    const auto in = noiseInput (static_cast<int> (bar * 7));
    const auto grid = downbeats (bar, in.frames());
    const auto at = [&] (int k) {
        return grid[static_cast<size_t> (k)].at;
    };
    const bool countIn = GENERATE (false, true);
    auto s = clickSettings (bar);
    s.countIn = countIn;
    // Pressed mid-bar 1: starts on downbeat 2 (or 3 with the count-in bar).
    const long long p = at (1) + 30000;
    const int startBar = countIn ? 3 : 2;
    const auto r =
        render (in, 64, fs, s, merged (grid, { press (p), press (at (startBar + 2) - 9000) }));
    const auto& lp = *r.looper;
    REQUIRE (lp.state() == State::playing);
    CHECK (lp.bars() == 2);
    // L is the grid's own distance between the two downbeats, to the sample.
    CHECK (lp.length() == at (startBar + 2) - at (startBar));
    const auto loop = lp.loopSamples (0);
    auto& x = const_cast<test::Buffer&> (in)[0];
    CHECK (loop[0] == x[static_cast<size_t> (at (startBar))]);
    CHECK (loop[1000] == x[static_cast<size_t> (at (startBar) + 1000)]);
}

TEST_CASE ("Looper: a press just after a downbeat still starts on it (the grace)", "[looper][bars]")
{
    const double fs = 48000.0;
    const double bar = 2.0 * fs * 60.0 / 90.0;
    const auto in = noiseInput (static_cast<int> (bar * 6));
    const auto grid = downbeats (bar, in.frames());
    const auto at = [&] (int k) {
        return grid[static_cast<size_t> (k)].at;
    };
    const int G = static_cast<int> (std::lround (Looper::kGraceSeconds * fs));
    const long long late = GENERATE_COPY (0LL, 1LL, static_cast<long long> (G));
    const auto r = render (in, 97, fs, clickSettings (bar),
                           merged (grid, { press (at (1) + late), press (at (3) + 100) }));
    const auto& lp = *r.looper;
    REQUIRE (lp.length() == at (3) - at (1));
    const auto loop = lp.loopSamples (1);
    auto& x = const_cast<test::Buffer&> (in)[1];
    bool same = true;
    for (long long i = 0; i < 20000; ++i)
        same = same && loop[static_cast<size_t> (i)] == x[static_cast<size_t> (at (1) + i)];
    CHECK (same);

    // One sample past the grace waits for the next downbeat.
    const auto r2 = render (in, 97, fs, clickSettings (bar),
                            merged (grid, { press (at (1) + G + 1), press (at (3) + 100) }));
    CHECK (r2.looper->length() == at (3) - at (2));
}

TEST_CASE ("Looper: the closing press rounds to the nearest bar line", "[looper][bars]")
{
    const double fs = 44100.0;
    const double bar = 3.0 * fs * 60.0 / 133.0;
    const auto in = noiseInput (static_cast<int> (bar * 9));
    const auto grid = downbeats (bar, in.frames());
    const auto at = [&] (int k) {
        return grid[static_cast<size_t> (k)].at;
    };
    const auto s = clickSettings (bar);

    SECTION ("early: keeps recording to the bar line, then plays from 0 there")
    {
        const long long q = at (4) - static_cast<long long> (bar * 0.4);
        const auto r = render (in, 32, fs, s, merged (grid, { press (at (1) - 10), press (q) }),
                               [] (Looper::Settings&, int) {});
        const auto& lp = *r.looper;
        CHECK (lp.length() == at (4) - at (1));
        CHECK (lp.bars() == 3);
        auto& x = const_cast<test::Buffer&> (in)[0];
        auto& y = const_cast<Run&> (r).out[0];
        // Still only live until the bar line; loop sample 0 at the bar line.
        CHECK (y[static_cast<size_t> (at (4) - 1)] == x[static_cast<size_t> (at (4) - 1)]);
        const auto loop = lp.loopSamples (0);
        CHECK (y[static_cast<size_t> (at (4))] ==
               x[static_cast<size_t> (at (4))] + (rc (1, fadeLen (fs)) * 1.0f) * loop[0]);
    }
    SECTION ("late: closes on the line just past, already playing from where the press is")
    {
        const long long q = at (4) + static_cast<long long> (bar * 0.3);
        const auto r = render (in, 32, fs, s, merged (grid, { press (at (1) - 10), press (q) }));
        const auto& lp = *r.looper;
        CHECK (lp.length() == at (4) - at (1));
        CHECK (lp.bars() == 3);
        auto& x = const_cast<test::Buffer&> (in)[0];
        auto& y = const_cast<Run&> (r).out[0];
        const auto loop = lp.loopSamples (0);
        // Joined mid-pass: playback fades in from loop position q - at(4).
        const int X = fadeLen (fs);
        const auto pos = static_cast<size_t> (q - at (4));
        CHECK (y[static_cast<size_t> (q)] ==
               x[static_cast<size_t> (q)] + (rc (1, X) * 1.0f) * loop[pos]);
        CHECK (y[static_cast<size_t> (q + X)] ==
               x[static_cast<size_t> (q + X)] + loop[pos + static_cast<size_t> (X)]);
    }
    SECTION ("in the first bar: always one whole bar")
    {
        const auto r =
            render (in, 32, fs, s, merged (grid, { press (at (1) - 10), press (at (1) + 200) }));
        CHECK (r.looper->length() == at (2) - at (1));
        CHECK (r.looper->bars() == 1);
    }
}

TEST_CASE ("Looper: the main button cancels a wait, Stop cancels too", "[looper][bars]")
{
    const double fs = 48000.0;
    const double bar = 96000.0;
    const auto in = noiseInput (300000);
    const auto grid = downbeats (bar, in.frames());
    const auto cmd = GENERATE (Command::main, Command::stop, Command::clear);
    const auto r = render (in, 64, fs, clickSettings (bar),
                           merged (grid, { press (10000), press (20000, cmd) }));
    CHECK (r.looper->state() == State::empty);
    CHECK (r.looper->length() == 0);
}

TEST_CASE ("Looper: a click that stops ends the wait and closes a take free", "[looper][bars]")
{
    const double fs = 48000.0;
    const double bar = 96000.0;
    const auto in = noiseInput (400000);
    const auto grid = downbeats (bar, 150000);
    // Closing waits for the downbeat at 192000; the click stops at 150016 (a block start).
    const auto r =
        render (in, 64, fs, clickSettings (bar), merged (grid, { press (90000), press (130000) }),
                [] (Looper::Settings& s, int start) {
                    if (start >= 150016)
                        s.clickRunning = false;
                });
    CHECK (r.looper->length() == 150016 - 96000);
    CHECK (!r.looper->synced());
}

// -------------------------------------------------------------------------------------------------
// Crossfades

namespace
{
/// The loop's contribution to the output: output minus input.
std::vector<float> loopPart (const Run& r, const test::Buffer& in, int c)
{
    auto& y = const_cast<Run&> (r).out[c];
    auto& x = const_cast<test::Buffer&> (in)[c];
    std::vector<float> d (y.size());
    for (size_t i = 0; i < y.size(); ++i)
        d[i] = y[i] - x[i];
    return d;
}

float maxStep (const std::vector<float>& v, size_t from, size_t to)
{
    float m = 0.0f;
    for (size_t i = from + 1; i < to && i < v.size(); ++i)
        m = std::max (m, std::abs (v[i] - v[i - 1]));
    return m;
}
} // namespace

TEST_CASE ("Looper: every boundary stays within the crossfade bound", "[looper][xfade]")
{
    const double fs = GENERATE (44100.0, 96000.0);
    const double f = 220.0;
    const float A = 0.5f;
    const int frames = static_cast<int> (fs * 6);
    test::Buffer in (2, frames);
    test::fillSine (in[0], f, fs, A);
    test::fillSine (in[1], f, fs, A, 1.0);
    const int X = fadeLen (fs);
    // L is no whole number of periods, so a hard seam would jump.
    const long long p = static_cast<long long> (fs * 0.2),
                    L = static_cast<long long> (fs * 0.61) + 7;
    const long long q = p + L;
    std::vector<TimedEvent> ev { press (p), press (q) };
    // Overdub over part of pass 2 (the same sine at another phase: punch-in and out land
    // mid-wave), then undo and redo during pass 4, stop, play.
    ev.push_back (press (q + L + L / 5));
    ev.push_back (press (q + L + L / 2));
    ev.push_back (press (q + 3 * L + L / 3, Command::undo));
    ev.push_back (press (q + 3 * L + L / 2, Command::redo));
    ev.push_back (press (q + 4 * L + 101, Command::stop));
    ev.push_back (press (q + 4 * L + 9000));
    Looper::Settings s;
    s.feedback = 0.6f;
    const auto r = render (in, 64, fs, s, ev);
    REQUIRE (r.looper->state() == State::playing);

    // The loop is at most the old take (A) plus a dub (A): its own step is at most twice a sine's.
    const float own = 2.0f * static_cast<float> (kTwoPiD * f / fs) * A;
    const float bound = own + kPi * A / static_cast<float> (X) + 1.0e-5f;
    for (int c = 0; c < 2; ++c)
    {
        const auto part = loopPart (r, in, c);
        const float measured = maxStep (part, static_cast<size_t> (q), part.size());
        INFO ("fs " << fs << " channel " << c << " measured " << measured << " bound " << bound);
        CHECK (measured <= bound);
    }
    // A hard seam (the take's last sample, then its first) would jump far more.
    auto& x = in[0];
    const float hard = std::abs (x[static_cast<size_t> (p)] - x[static_cast<size_t> (q - 1)]);
    CHECK (hard > 10.0f * bound);
}

// -------------------------------------------------------------------------------------------------
// Overdub, feedback, undo

TEST_CASE ("Looper: an overdub mixes the input over the old take at the feedback", "[looper]")
{
    const double fs = 48000.0;
    const float fb = GENERATE (1.0f, 0.5f, 0.0f);
    const auto in = noiseInput (200000);
    const long long p = 1000, L = 40000, q = p + L;
    Looper::Settings s;
    s.feedback = fb;
    // Overdub the whole of pass 2 (punch in at its start, out at its end).
    const auto before = render (in, 64, fs, s, { press (p), press (q) });
    const auto take = before.looper->loopSamples (0);
    const auto r =
        render (in, 64, fs, s, { press (p), press (q), press (q + L), press (q + 2 * L) });
    const auto loop = r.looper->loopSamples (0);
    auto& x = const_cast<test::Buffer&> (in)[0];
    const int X = fadeLen (fs);
    // Past the punch-in ramp and before the punch-out: old * fb + input, computed as the looper
    // does.
    for (long long i = X; i < L - 1; ++i)
    {
        const auto t = static_cast<size_t> (q + L + i);
        const float expected =
            take[static_cast<size_t> (i)] * (1.0f - 1.0f * (1.0f - fb)) + x[t] * 1.0f;
        if (loop[static_cast<size_t> (i)] != expected)
            FAIL ("sample " << i);
    }
}

TEST_CASE ("Looper: undo and redo restore the loop bit for bit", "[looper][undo]")
{
    const double fs = 48000.0;
    const auto in = noiseInput (600000);
    const long long p = 1000, L = 40000, q = p + L;
    // Take, layer 1 over pass 2, layer 2 over half of pass 4.
    const std::vector<TimedEvent> base { press (p), press (q) };
    auto layer1 = base;
    layer1.push_back (press (q + L + 300));
    layer1.push_back (press (q + 2 * L - 700));
    auto layer2 = layer1;
    layer2.push_back (press (q + 3 * L + 5000));
    layer2.push_back (press (q + 3 * L + 25000));

    const auto loopsOf = [&] (const std::vector<TimedEvent>& ev) {
        const auto r = render (in, 128, fs, {}, ev);
        return std::array<std::vector<float>, 2> { r.looper->loopSamples (0),
                                                   r.looper->loopSamples (1) };
    };
    const auto takeOnly = loopsOf (base);
    const auto after1 = loopsOf (layer1);
    const auto after2 = loopsOf (layer2);
    REQUIRE (after1 != takeOnly);
    REQUIRE (after2 != after1);

    SECTION ("undo the last layer, redo it")
    {
        auto ev = layer2;
        ev.push_back (press (q + 5 * L, Command::undo));
        CHECK (loopsOf (ev) == after1);
        ev.push_back (press (q + 6 * L, Command::redo));
        CHECK (loopsOf (ev) == after2);
    }
    SECTION ("undo while overdubbing drops the layer at once")
    {
        auto ev = layer1;
        ev.push_back (press (q + 3 * L + 5000));
        ev.push_back (press (q + 3 * L + 15000, Command::undo));
        const auto r = render (in, 128, fs, {}, ev);
        CHECK (r.looper->state() == State::playing);
        CHECK (r.looper->loopSamples (0) == after1[0]);
        CHECK (r.looper->loopSamples (1) == after1[1]);
    }
    SECTION ("undo, then a new layer, then undo: back to before both")
    {
        auto ev = layer2;
        ev.push_back (press (q + 5 * L, Command::undo)); // -> after1
        ev.push_back (press (q + 6 * L + 100));          // a new layer over after1
        ev.push_back (press (q + 7 * L));
        const auto r = render (in, 128, fs, {}, ev);
        REQUIRE (r.looper->loopSamples (0) != after1[0]);
        ev.push_back (press (q + 8 * L, Command::undo));
        CHECK (loopsOf (ev) == after1);
    }
    SECTION ("clear, undo the clear, redo it")
    {
        auto ev = layer2;
        ev.push_back (press (q + 5 * L, Command::clear));
        {
            const auto r = render (in, 128, fs, {}, ev);
            CHECK (r.looper->state() == State::empty);
            CHECK (r.looper->length() == 0);
            CHECK (r.looper->canUndo());
            CHECK (r.looper->undoIsClear());
        }
        ev.push_back (press (q + 5 * L + 20000, Command::undo));
        {
            const auto r = render (in, 128, fs, {}, ev);
            CHECK (r.looper->state() == State::stopped);
            CHECK (r.looper->loopSamples (0) == after2[0]);
            CHECK (r.looper->loopSamples (1) == after2[1]);
        }
        ev.push_back (press (q + 5 * L + 40000, Command::redo));
        const auto r = render (in, 128, fs, {}, ev);
        CHECK (r.looper->state() == State::empty);
        CHECK (r.looper->canUndo());
    }
    SECTION ("nothing to undo after a take alone")
    {
        auto ev = base;
        ev.push_back (press (q + L, Command::undo));
        const auto r = render (in, 128, fs, {}, ev);
        CHECK (!r.looper->canUndo());
        CHECK (r.looper->loopSamples (0) == takeOnly[0]);
    }
}

TEST_CASE ("Looper: a second press inside a 10 ms fade acts when the fade ends", "[looper]")
{
    const double fs = 48000.0;
    const auto in = noiseInput (200000);
    const long long p = 1000, L = 40000, q = p + L;
    const int X = fadeLen (fs);
    // Stop, then Play 100 samples later (inside the stop's fade): it plays from 0 when the fade
    // has ended, exactly as a Play pressed then.
    const auto quick =
        render (in, 64, fs, {},
                { press (p), press (q), press (q + 9000, Command::stop), press (q + 9100) });
    const auto later =
        render (in, 64, fs, {},
                { press (p), press (q), press (q + 9000, Command::stop), press (q + 9000 + X) });
    CHECK (quick.out.data == later.out.data);
}

// -------------------------------------------------------------------------------------------------
// Invariance

TEST_CASE ("Looper: block size never changes a sample", "[looper][invariance]")
{
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    const double k = fs / 48000.0;
    const auto T = [k] (double t48) {
        return static_cast<long long> (std::llround (t48 * k));
    };
    const double bar = 4.0 * fs * 60.0 / 101.0;
    const auto in = noiseInput (static_cast<int> (T (900000)));
    auto s = clickSettings (bar);
    s.latencySamples = static_cast<int> (T (311));
    s.feedback = 0.8f;
    s.countIn = true;
    const auto grid = downbeats (bar, in.frames(), 17.25);
    // Count-in take, early close, overdub in and out, undo, redo, stop, play on a downbeat, a
    // second overdub, clear, undo the clear.
    const std::vector<TimedEvent> script {
        press (T (10000)),
        press (T (380000)),
        press (T (430000)),
        press (T (460000)),
        press (T (500000), Command::undo),
        press (T (520000), Command::redo),
        press (T (560000), Command::stop),
        press (T (580000)),
        press (T (700000)),
        press (T (720000)),
        press (T (800000), Command::clear),
        press (T (850000), Command::undo),
    };
    const auto events = merged (grid, script);
    const auto reference = render (in, 1, fs, s, events);
    REQUIRE (reference.looper->state() == State::stopped);
    REQUIRE (reference.looper->synced());
    for (int block : { 32, 64, 128, 512, 4096, 97, 441 })
    {
        INFO ("fs " << fs << " block " << block);
        const auto r = render (in, block, fs, s, events);
        CHECK (r.out.data == reference.out.data);
        CHECK (r.looper->loopSamples (0) == reference.looper->loopSamples (0));
        CHECK (r.looper->loopSamples (1) == reference.looper->loopSamples (1));
    }
}

TEST_CASE ("Looper: prepare at the same rate keeps the loop, another rate clears it", "[looper]")
{
    const double fs = 48000.0;
    const auto in = noiseInput (100000);
    auto r = render (in, 64, fs, {}, { press (1000), press (41000) });
    auto& lp = *r.looper;
    const auto loop = lp.loopSamples (0);
    lp.prepare ({ fs, 256, 2 });
    CHECK (lp.state() == State::stopped);
    CHECK (lp.loopSamples (0) == loop);
    lp.prepare ({ 44100.0, 256, 2 });
    CHECK (lp.state() == State::empty);
    CHECK (lp.length() == 0);
}

TEST_CASE ("Looper: without memory it leaves the signal alone and ignores presses", "[looper]")
{
    Looper lp;
    lp.prepare ({ 48000.0, 64, 2 });
    CHECK (lp.state() == State::unavailable);
    auto in = noiseInput (4096);
    auto out = in;
    const Looper::Event e { 0, Kind::command, Command::main };
    lp.process (out.view(), &e, 1);
    CHECK (out.data == in.data);
    CHECK (lp.state() == State::unavailable);
}

// -------------------------------------------------------------------------------------------------
// The metronome and the lock

TEST_CASE ("Metronome::downbeats predicts exactly where process() sounds step 0",
           "[looper][metronome]")
{
    using dsp::Metronome;
    Random rng (99);
    for (int round = 0; round < 12; ++round)
    {
        Metronome m;
        const double fs = round % 2 == 0 ? 48000.0 : 44100.0;
        m.prepare ({ fs, 4096, 2 });
        Metronome::Settings s;
        s.enabled = true;
        s.steps.fill (Metronome::Normal);
        s.signature = static_cast<int> (rng.unipolar() * 8.0f);
        s.bpm = 40.0f + rng.unipolar() * 300.0f;
        test::Buffer b (2, 4096);
        for (int block = 0; block < 60; ++block)
        {
            if (block % 9 == 5)
            {
                s.bpm = 40.0f + rng.unipolar() * 300.0f; // tempo changes re-anchor the grid
                s.signature = static_cast<int> (rng.unipolar() * 8.0f);
            }
            m.setSettings (s);
            const int n = 1 + static_cast<int> (rng.unipolar() * 4095.0f);
            std::array<int, 8> predicted {};
            const int count = m.downbeats (n, predicted.data(), 8);
            // Sample by sample, a downbeat is where the current step becomes 0.
            // (Two step-0 triggers never follow each other: a bar has at least two steps.)
            std::vector<int> actual;
            for (int i = 0; i < n; ++i)
            {
                const int before = m.currentStep();
                m.process (b.view().subView (0, 1));
                if (m.currentStep() == 0 && before != 0)
                    actual.push_back (i);
            }
            std::vector<int> got (predicted.begin(), predicted.begin() + count);
            INFO ("round " << round << " block " << block << " n " << n);
            CHECK (got == actual);
        }
    }
}

TEST_CASE ("Looper: a loop in bars keeps the click on its wraps, pass after pass",
           "[looper][bars][lock]")
{
    using dsp::Metronome;
    const double fs = 48000.0;
    const float bpm = 117.0f; // a bar of 98 461.54 samples: the grid's bars alternate in length
    Metronome m;
    m.prepare ({ fs, 512, 2 });
    Metronome::Settings ms;
    ms.enabled = true;
    ms.bpm = bpm;
    ms.signature = 2; // 4/4
    ms.steps.fill (Metronome::Normal);
    Looper lp;
    lp.prepare ({ fs, 512, 2 });
    lp.attach (&memory());
    test::Buffer b (2, 512);

    long long t = 0;
    int wraps = 0, wrapsOnDownbeats = 0;
    bool pressed = false, closed = false;
    for (int block = 0; t < static_cast<long long> (fs * 600); ++block)
    {
        const int n = block % 3 == 0 ? 512 : (block % 3 == 1 ? 97 : 300);
        m.setSettings (ms);
        std::array<int, 8> db {};
        const int nd = m.downbeats (n, db.data(), 8);
        std::vector<Looper::Event> ev;
        for (int k = 0; k < nd; ++k)
            ev.push_back ({ db[static_cast<size_t> (k)], Kind::downbeat, Command::main });
        if (!pressed && t > 30000)
        {
            ev.push_back ({ 0, Kind::command, Command::main });
            pressed = true;
        }
        else if (!closed && t > 360000) // past half of bar 3: closes on the "1" after it
        {
            ev.push_back ({ 0, Kind::command, Command::main });
            closed = true;
        }
        Looper::Settings s;
        s.clickRunning = m.isRunning();
        s.clickBpm = bpm;
        s.clickSignature = 2;
        s.barSamples = m.barSamples();
        lp.setSettings (s);
        auto view = b.view().subView (0, n);
        view.clear();
        lp.process (view, ev.data(), static_cast<int> (ev.size()));
        for (int k = 0; k < lp.numWraps(); ++k)
        {
            m.alignDownbeat (lp.wrap (k));
            ++wraps;
            // After the alignment the click's step 0 is due on the wrap's own sample.
            std::array<int, 8> again {};
            const int na = m.downbeats (n, again.data(), 8);
            for (int j = 0; j < na; ++j)
                if (again[static_cast<size_t> (j)] == lp.wrap (k))
                    ++wrapsOnDownbeats;
        }
        // Before the alignment the grid's own downbeat was there or one sample later.
        m.process (view);
        t += n;
    }
    REQUIRE (lp.synced());
    CHECK (lp.bars() == 3);
    CHECK (wraps >= 95); // ten minutes of a three-bar loop, 6.15 s a pass
    CHECK (wrapsOnDownbeats == wraps);
}
