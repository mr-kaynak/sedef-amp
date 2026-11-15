// SPDX-License-Identifier: AGPL-3.0-or-later
// Drums (docs/drums.md): the groove data, the timing to the sample on the metronome's grid,
// block-size and sample-rate invariance, pinned renders, the kit's bounds and its physics.
//
// Everything is deterministic: the kit is integer arithmetic with seeded noise, the tests use no
// clock and no unseeded randomness (block sizes come from sedef::Random with fixed seeds).
#include "DrumsTestUtils.h"
#include "reverb/ReverbTestUtils.h"
#include "sedef/dsp/drums/GroovePlayer.h"
#include "sedef/params/Parameters.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cinttypes>
#include <cstdio>
#include <string>

using namespace sedef;
using namespace sedef::test;
using namespace sedef::test::drums;
using sedef::dsp::drums::GroovePlayer;
using sedef::dsp::drums::grooves;

namespace
{
constexpr std::array<double, 4> kRates { 44100.0, 48000.0, 88200.0, 96000.0 };

int numGrooves()
{
    return static_cast<int> (grooves().size());
}

int signatureOf (int groove)
{
    return grooves()[static_cast<size_t> (groove)].signature;
}

/// The events a groove should fire, computed here from the metronome's grid formula
/// (docs/drums.md, sections 3.1 to 3.3) for a metronome started at sample 0.
std::vector<Fired> expectedEvents (const GrooveSetup& g, int frames)
{
    const int signature = g.signature >= 0 ? g.signature : signatureOf (g.groove);
    const int steps = Metronome::stepsFor (signature);
    const double interval = stepSamples (g.fs, g.bpm, signature);
    GroovePlayer player;
    player.setSettings ({ g.groove, g.intensity, g.fillEvery });
    player.start();
    std::vector<Fired> out;
    for (long long k = 0;; ++k)
    {
        const auto t = static_cast<long long> (std::ceil (static_cast<double> (k) * interval));
        if (t >= frames)
            break;
        const auto next =
            static_cast<long long> (std::ceil (static_cast<double> (k + 1) * interval));
        std::array<GroovePlayer::StepEvent, GroovePlayer::kMaxStepEvents> events;
        const int n = player.onStep (static_cast<int> (k % steps), signature, events.data(),
                                     static_cast<int> (events.size()));
        std::vector<Fired> step;
        for (int i = 0; i < n; ++i)
        {
            const auto& e = events[static_cast<size_t> (i)];
            const long long kk = e.slotsPerStep;
            long long when = t + (2LL * e.slot * (next - t) + kk) / (2LL * kk);
            when += std::llround (static_cast<double> (e.offsetMs) * 1.0e-3 * g.fs);
            when = std::clamp (when, t, next - 1);
            step.push_back ({ when, e.instrument, e.velocity });
        }
        std::stable_sort (step.begin(), step.end(),
                          [] (const Fired& a, const Fired& b) { return a.sample < b.sample; });
        for (const auto& e : step)
            if (e.sample < frames)
                out.push_back (e);
    }
    return out;
}

std::string describe (const std::vector<Fired>& v, size_t at)
{
    if (at >= v.size())
        return "(none)";
    return std::to_string (v[at].sample) + " inst " +
           std::to_string (static_cast<int> (v[at].instrument)) + " vel " +
           std::to_string (v[at].velocity);
}

void checkSameEvents (const std::vector<Fired>& got, const std::vector<Fired>& expected)
{
    REQUIRE (!expected.empty());
    size_t first = 0;
    while (first < got.size() && first < expected.size() && got[first] == expected[first])
        ++first;
    INFO ("first difference at event " << first << ": got " << describe (got, first)
                                       << ", expected " << describe (expected, first));
    CHECK (got.size() == expected.size());
    CHECK (first == std::min (got.size(), expected.size()));
}

/// Deterministic odd block sizes from 1 to 700.
auto oddBlocks (std::uint32_t seed)
{
    return [rng = Random (seed)] (int) mutable {
        return 1 + 2 * static_cast<int> (rng.unipolar() * 350.0f);
    };
}

double dbOf (double v)
{
    return 20.0 * std::log10 (std::max (v, 1.0e-12));
}

/// Energy (seconds · amplitude²) between 20 Hz and 16 kHz in the first `seconds` of a signal: what
/// is heard, the same measure at every rate (white noise above 16 kHz differs by rate).
double inBandEnergy (const std::vector<float>& v, double fs, double seconds)
{
    const int n = static_cast<int> (seconds * fs);
    const int size = nextPow2 (n);
    const auto mag = magnitudeSpectrum (v.data(), n, size);
    double e = 0.0;
    for (size_t k = 0; k < mag.size(); ++k)
    {
        const double hz = static_cast<double> (k) * fs / size;
        if (hz >= 20.0 && hz < 16000.0)
            e += mag[k] * mag[k];
    }
    return 2.0 * e / (static_cast<double> (size) * fs);
}

double rmsOf (const std::vector<float>& v, size_t from, size_t to)
{
    double acc = 0.0;
    for (size_t i = from; i < std::min (to, v.size()); ++i)
        acc += static_cast<double> (v[i]) * v[i];
    return std::sqrt (acc / static_cast<double> (std::max<size_t> (1, to - from)));
}
} // namespace

// -------------------------------------------------------------------------------------------------
// Data

TEST_CASE ("Every groove is well formed and every meter has one", "[drums]")
{
    std::array<int, 8> perSignature {};
    const auto& groovesParam = spec (ParamId::drumsGroove);
    REQUIRE (groovesParam.numChoices() == numGrooves());
    for (int g = 0; g < numGrooves(); ++g)
    {
        const auto& groove = grooves()[static_cast<size_t> (g)];
        INFO ("groove " << groove.name);
        CHECK (std::string (groovesParam.choiceLabel (g)) == std::string (groove.name));
        CHECK (!groove.feel.empty());
        REQUIRE (groove.signature >= 0);
        REQUIRE (groove.signature < 8);
        ++perSignature[static_cast<size_t> (groove.signature)];
        const auto barSlots =
            static_cast<size_t> (Metronome::stepsFor (groove.signature) * groove.slotsPerStep);
        REQUIRE (!groove.lanes.empty());
        for (const auto& lane : groove.lanes)
        {
            INFO ("lane " << lane.pattern);
            CHECK (lane.pattern.size() == barSlots);
            CHECK (lane.lo >= 1);
            CHECK (lane.lo <= lane.hi);
            CHECK (lane.hi <= 5);
            // Never early, and at most 5 ms late (section 3.3).
            CHECK (lane.offsetMs >= 0.0f);
            CHECK (lane.offsetMs <= 5.0f);
            for (char c : lane.pattern)
                CHECK ((c == '.' || sedef::dsp::drums::velocityOf (c) > 0));
        }
        // The fill family's grid is the groove's.
        const auto& fill = sedef::dsp::drums::fill (groove.fill, 0);
        CHECK (static_cast<size_t> (fill.shortSlots) <= barSlots);
    }
    for (int s = 0; s < 8; ++s)
    {
        INFO ("signature " << s);
        CHECK (perSignature[static_cast<size_t> (s)] > 0);
        // A groove index of another meter plays this meter's first groove.
        const int g = sedef::dsp::drums::grooveFor ((s + 3) % numGrooves(), s);
        CHECK (signatureOf (g) == s);
    }
    for (int family = 0; family < 3; ++family)
        for (int variant = 0; variant < 2; ++variant)
        {
            const auto& f = sedef::dsp::drums::fill (
                static_cast<sedef::dsp::drums::FillFamily> (family), variant);
            CHECK (f.shortSlots < f.longSlots);
            for (const auto& lane : f.lanes)
                CHECK (lane.pattern.size() == static_cast<size_t> (f.longSlots));
        }
}

TEST_CASE ("No groove, intensity and fill setting overflows a bar or a step", "[drums]")
{
    for (int g = 0; g < numGrooves(); ++g)
        for (int intensity = 1; intensity <= 5; ++intensity)
            for (int every : { 0, 2, 4, 8 })
            {
                GroovePlayer player;
                player.setSettings ({ g, intensity, every });
                player.start();
                const int signature = signatureOf (g);
                const int steps = Metronome::stepsFor (signature);
                int barEvents = 0;
                for (int k = 0; k < steps * 9; ++k)
                {
                    std::array<GroovePlayer::StepEvent, GroovePlayer::kMaxStepEvents + 1> events;
                    const int n = player.onStep (k % steps, signature, events.data(),
                                                 static_cast<int> (events.size()));
                    CHECK (n < GroovePlayer::kMaxStepEvents);
                    barEvents = k % steps == 0 ? n : barEvents + n;
                    CHECK (barEvents < GroovePlayer::kMaxBarEvents);
                }
            }
}

TEST_CASE ("Cymbal partials keep clear of harmonic ratios", "[drums]")
{
    // Section 5.4: every pair at least 2 % from p/q with p, q <= 6.
    DrumKit kit;
    kit.prepare (48000.0);
    for (auto inst : { Instrument::hatClosed, Instrument::ride, Instrument::crash })
    {
        const auto info = kit.bankInfo (inst);
        REQUIRE (info.numModes >= 6);
        for (int a = 0; a < info.numModes; ++a)
            for (int b = a + 1; b < info.numModes; ++b)
            {
                const double ratio =
                    info.hz[static_cast<size_t> (b)] / info.hz[static_cast<size_t> (a)];
                for (int p = 1; p <= 6; ++p)
                    for (int q = 1; q < p; ++q)
                    {
                        INFO (info.hz[static_cast<size_t> (a)] << " and "
                                                               << info.hz[static_cast<size_t> (b)]);
                        CHECK (std::abs (ratio / (static_cast<double> (p) / q) - 1.0) >= 0.0199);
                    }
            }
    }
}

// -------------------------------------------------------------------------------------------------
// Grooves and bars

TEST_CASE ("Straight rock plays the bar the design writes", "[drums]")
{
    // Section 4.4 at intensity 3, 120 bpm, 48 kHz: a step is 24000 samples, a slot 6000.
    GrooveSetup g;
    g.fillEvery = 0;
    const auto r = renderGroove (g, 96000);
    std::vector<long long> kick, snare, hat;
    for (const auto& e : r.events)
        (e.instrument == Instrument::kick    ? kick
         : e.instrument == Instrument::snare ? snare
                                             : hat)
            .push_back (e.sample);
    CHECK (kick == std::vector<long long> { 0, 48000, 60000 });
    CHECK (snare == std::vector<long long> { 24000 + 144, 72000 + 144 }); // 3 ms late
    CHECK (hat == std::vector<long long> { 0, 12000, 24000, 36000, 48000, 60000, 72000, 84000 });
    // Accents on the beats, eighths between: scaled by intensity 3 (92 %).
    CHECK (r.events.front().velocity == GroovePlayer::scaleVelocity (124, 3));
}

TEST_CASE ("Fills come on the last bar of each phrase, with a crash after them", "[drums]")
{
    GroovePlayer player;
    player.setSettings ({ 0, 3, 2 });
    player.start();
    std::array<GroovePlayer::StepEvent, GroovePlayer::kMaxStepEvents> events;
    auto stepHas = [&] (int step, Instrument inst) {
        const int n = player.onStep (step, 2, events.data(), static_cast<int> (events.size()));
        return std::any_of (events.begin(), events.begin() + n,
                            [inst] (const auto& e) { return e.instrument == inst; });
    };
    // Bar 0: the groove; its beat 4 has hats, no toms.
    CHECK (!stepHas (0, Instrument::crash));
    stepHas (1, Instrument::kick);
    stepHas (2, Instrument::kick);
    CHECK (!stepHas (3, Instrument::tomFloor));
    // Bar 1 is a fill (every 2 bars): variant A, the floor tom on its last beat, no hats there.
    CHECK (!stepHas (0, Instrument::crash));
    CHECK (player.fillBar());
    stepHas (1, Instrument::kick);
    CHECK (stepHas (2, Instrument::snare));
    CHECK (!stepHas (3, Instrument::hatClosed));
    // Bar 2: crash and kick on the "1", no hat there.
    const int n = player.onStep (0, 2, events.data(), static_cast<int> (events.size()));
    CHECK (std::any_of (events.begin(), events.begin() + n,
                        [] (const auto& e) { return e.instrument == Instrument::crash; }));
    CHECK (std::none_of (events.begin(), events.begin() + n, [] (const auto& e) {
        return e.slot == 0 && e.instrument == Instrument::hatClosed;
    }));
    CHECK (!player.fillBar());
    // Bar 3: the second fill is variant B (the mid tom).
    for (int s = 1; s < 4; ++s)
        player.onStep (s, 2, events.data(), static_cast<int> (events.size()));
    player.onStep (0, 2, events.data(), static_cast<int> (events.size()));
    player.onStep (1, 2, events.data(), static_cast<int> (events.size()));
    CHECK (stepHas (3, Instrument::tomMid));
}

TEST_CASE ("Groove changes wait for the bar line", "[drums]")
{
    GroovePlayer player;
    player.setSettings ({ 0, 1, 0 });
    player.start();
    std::array<GroovePlayer::StepEvent, GroovePlayer::kMaxStepEvents> events;
    auto count = [&] (int step) {
        return player.onStep (step, 2, events.data(), static_cast<int> (events.size()));
    };
    const int quiet = count (0); // intensity 1: kick and a quarter hat
    player.setSettings ({ 0, 5, 0 });
    CHECK (count (1) == 2); // beat 2 of the old bar: snare and hat only
    count (2);
    count (3);
    CHECK (count (0) > quiet); // the new bar: intensity 5
    CHECK (player.bar() == 1);
}

TEST_CASE ("A groove written for another meter plays the metronome's meter's first", "[drums]")
{
    GrooveSetup g;
    g.groove = 0;    // 4/4
    g.signature = 4; // 6/8
    g.fillEvery = 0;
    const auto r = renderGroove (g, barFrames (g.fs, g.bpm, 4, 2));
    GrooveSetup sixEight = g;
    sixEight.groove = sedef::dsp::drums::grooveFor (0, 4);
    CHECK (signatureOf (sixEight.groove) == 4);
    checkSameEvents (r.events, expectedEvents (sixEight, barFrames (g.fs, g.bpm, 4, 2)));
}

// -------------------------------------------------------------------------------------------------
// Timing (sections 3.1 to 3.4)

TEST_CASE ("Drum events land on their samples at every tempo and in every meter", "[drums]")
{
    for (double fs : { 48000.0, 44100.0 })
        for (float bpm : { 80.0f, 117.0f, 120.0f, 200.0f })
            for (int g = 0; g < numGrooves(); ++g)
                for (int intensity : { 1, 3, 5 })
                {
                    GrooveSetup setup;
                    setup.fs = fs;
                    setup.bpm = bpm;
                    setup.groove = g;
                    setup.intensity = intensity;
                    setup.fillEvery = 2;
                    const int frames = barFrames (fs, bpm, signatureOf (g), 4);
                    INFO ("fs " << fs << " bpm " << bpm << " groove " << g << " intensity "
                                << intensity);
                    const auto r = renderGroove (setup, frames, 64);
                    checkSameEvents (r.events, expectedEvents (setup, frames));
                }
}

TEST_CASE ("A groove's downbeat is the click's step 0", "[drums]")
{
    // The same metronome settings in click mode: its clicks start where the drums' steps fire.
    for (int g = 0; g < numGrooves(); ++g)
    {
        GrooveSetup setup;
        setup.groove = g;
        setup.bpm = 117.0f;
        setup.fillEvery = 0;
        const int signature = signatureOf (g);
        const int frames = barFrames (setup.fs, setup.bpm, signature, 3);
        const auto drums = renderGroove (setup, frames, 97);
        const double interval = stepSamples (setup.fs, setup.bpm, signature);
        const int steps = Metronome::stepsFor (signature);
        for (int bar = 0; bar < 3; ++bar)
        {
            const auto downbeat =
                static_cast<long long> (std::ceil (static_cast<double> (bar * steps) * interval));
            INFO ("groove " << g << " bar " << bar);
            // The kick on the "1", at velocity > 0, exactly there.
            CHECK (std::any_of (drums.events.begin(), drums.events.end(), [&] (const Fired& e) {
                return e.sample == downbeat && e.instrument == Instrument::kick;
            }));
        }
    }
}

TEST_CASE ("Drums render the same at any block size", "[drums]")
{
    for (int g = 0; g < numGrooves(); ++g)
    {
        GrooveSetup setup;
        setup.groove = g;
        setup.intensity = 1 + g % 5;
        setup.fillEvery = 2;
        setup.bpm = 133.0f;
        const int frames = barFrames (setup.fs, setup.bpm, signatureOf (g), 3);
        const auto reference = renderGroove (setup, frames, 64);
        INFO ("groove " << g);
        for (int block : { 1, 7, 333, 4096 })
        {
            const auto r = renderGroove (setup, frames, block);
            CHECK (r.events == reference.events);
            CHECK (r.left == reference.left);
            CHECK (r.right == reference.right);
        }
        const auto odd =
            renderGroove (setup, frames, oddBlocks (static_cast<std::uint32_t> (g + 1)));
        CHECK (odd.events == reference.events);
        CHECK (odd.left == reference.left);
        CHECK (odd.right == reference.right);
    }
}

TEST_CASE ("Drums keep their timing and sound at every sample rate", "[drums]")
{
    // The same events, each within 1.5 samples of the exact time in seconds (the grid's ceil, half
    // a sample for a slot, half for an offset), and the same instruments and velocities.
    for (int g : { 0, 4, 9, 12 })
    {
        GrooveSetup at48;
        at48.groove = g;
        at48.fillEvery = 2;
        const auto ref = renderGroove (at48, barFrames (48000.0, 120.0, signatureOf (g), 4));
        for (double fs : kRates)
        {
            GrooveSetup setup = at48;
            setup.fs = fs;
            const auto r = renderGroove (setup, barFrames (fs, 120.0, signatureOf (g), 4));
            INFO ("groove " << g << " at " << fs);
            REQUIRE (r.events.size() == ref.events.size());
            for (size_t i = 0; i < r.events.size(); ++i)
            {
                CHECK (r.events[i].instrument == ref.events[i].instrument);
                CHECK (r.events[i].velocity == ref.events[i].velocity);
                const double exact = static_cast<double> (ref.events[i].sample) / 48000.0;
                CHECK (std::abs (static_cast<double> (r.events[i].sample) / fs - exact) <=
                       1.5 / fs + 1.5 / 48000.0);
            }
        }
    }
    // Each instrument: its energy in the audio band over the first 100 ms within 0.75 dB, and its
    // fall to -40 dB under its peak within 10 % (the noise parts are a different noise at each
    // rate).
    for (int i = 0; i < sedef::dsp::drums::kNumInstruments; ++i)
    {
        const auto inst = static_cast<Instrument> (i);
        const auto ref = strikeOnce (inst, 124, 48000.0, 48000 * 3);
        const double refPeak = std::max (peak (ref.left), peak (ref.right));
        const double refLevel =
            inBandEnergy (ref.left, 48000.0, 0.1) + inBandEnergy (ref.right, 48000.0, 0.1);
        auto fall = [] (const Render& r, double fs, double p) {
            for (size_t k = r.left.size(); k-- > 0;)
                if (std::max (std::abs (r.left[k]), std::abs (r.right[k])) > 0.01 * p)
                    return static_cast<double> (k) / fs;
            return 0.0;
        };
        const double refFall = fall (ref, 48000.0, refPeak);
        for (double fs : kRates)
        {
            const auto r = strikeOnce (inst, 124, fs, static_cast<int> (fs * 3.0));
            const double p = std::max (peak (r.left), peak (r.right));
            const double level = inBandEnergy (r.left, fs, 0.1) + inBandEnergy (r.right, fs, 0.1);
            INFO ("instrument " << i << " at " << fs);
            CHECK (std::abs (10.0 * std::log10 (level / refLevel)) <= 0.75);
            CHECK (std::abs (fall (r, fs, p) / refFall - 1.0) <= 0.1);
        }
    }
}

// -------------------------------------------------------------------------------------------------
// Pinned renders (section 5.1): the same bits on every platform.

TEST_CASE ("Every groove renders the pinned bits", "[drums]")
{
    // Four bars at 120 bpm and 48 kHz, intensity 3, fills every 2 bars (both variants and the
    // crash), Level 0 dB and a centred Tone, in 64-sample blocks.
    static constexpr std::array<std::uint64_t, 13> kPinned {
        0xf9251a6b44182ea4ull, // 0
        0x3117ed7ccdbfdf1aull, // 1
        0xcf7935c6f9aba281ull, // 2
        0x567be9ec466c6218ull, // 3
        0x95729c8180ea1222ull, // 4
        0x5fb39e57a0b77a1cull, // 5
        0xa463bf216f01a145ull, // 6
        0x56ee945b2de73c2dull, // 7
        0xce615e020590628cull, // 8
        0x31b4de892cdd8222ull, // 9
        0x01ecaa26489f4fc1ull, // 10
        0xaf88298ac3210cdcull, // 11
        0xae4907c6a51b8ce7ull, // 12
    };
    REQUIRE (static_cast<int> (kPinned.size()) == numGrooves());
    bool all = true;
    for (int g = 0; g < numGrooves(); ++g)
    {
        GrooveSetup setup;
        setup.groove = g;
        setup.fillEvery = 2;
        const auto r = renderGroove (setup, barFrames (48000.0, 120.0, signatureOf (g), 4));
        const std::uint64_t h = hashOf (r);
        if (h != kPinned[static_cast<size_t> (g)])
        {
            all = false;
            std::printf ("groove %2d: 0x%016" PRIx64 "ull\n", g, h);
        }
    }
    CHECK (all);
}

// -------------------------------------------------------------------------------------------------
// The kit (section 5)

TEST_CASE ("The kit never exceeds its bound, however densely it is played", "[drums]")
{
    // Every instrument at the top velocity every 64 samples (a 32nd at 375 bpm), then grooves at
    // their busiest and fastest through the metronome.
    for (double fs : kRates)
    {
        DrumKit kit;
        kit.prepare (fs);
        const std::int64_t bound = kit.peakBound();
        CHECK (bound < (std::int64_t { 1 } << 30)); // far from the int32 range
        std::vector<std::int32_t> l (64), r (64);
        std::int64_t worst = 0;
        for (int round = 0; round < static_cast<int> (fs) / 64 * 2; ++round)
        {
            for (int i = 0; i < sedef::dsp::drums::kNumInstruments; ++i)
                kit.strike (static_cast<Instrument> ((i + round) % 9), 127);
            kit.render (l.data(), r.data(), 64);
            for (int k = 0; k < 64; ++k)
                worst = std::max<std::int64_t> (
                    worst, std::max (std::abs (std::int64_t { l[static_cast<size_t> (k)] }),
                                     std::abs (std::int64_t { r[static_cast<size_t> (k)] })));
        }
        INFO ("fs " << fs << " worst " << worst << " bound " << bound);
        CHECK (worst <= bound);
    }
    for (int g = 0; g < numGrooves(); ++g)
    {
        GrooveSetup setup;
        setup.groove = g;
        setup.intensity = 5;
        setup.fillEvery = 2;
        setup.bpm = 240.0f;
        DrumKit kit;
        kit.prepare (setup.fs);
        const double bound = static_cast<double> (kit.peakBound()) / 8388608.0;
        const auto r = renderGroove (setup, barFrames (setup.fs, 240.0, signatureOf (g), 8));
        INFO ("groove " << g);
        CHECK (std::max (peak (r.left), peak (r.right)) <= bound);
        // A practice level: under 0 dBFS at Level 0 dB.
        CHECK (std::max (peak (r.left), peak (r.right)) < 1.0f);
    }
}

TEST_CASE ("A silent kit stops and outputs exact zeros", "[drums]")
{
    DrumKit kit;
    kit.prepare (48000.0);
    for (int i = 0; i < sedef::dsp::drums::kNumInstruments; ++i)
        kit.strike (static_cast<Instrument> (i), 127);
    std::vector<std::int32_t> l (4800), r (4800);
    int blocks = 0;
    while (kit.sounding() && blocks < 400) // 40 s
    {
        kit.render (l.data(), r.data(), 4800);
        ++blocks;
    }
    CHECK (!kit.sounding());
    kit.render (l.data(), r.data(), 4800);
    CHECK (std::all_of (l.begin(), l.end(), [] (std::int32_t v) { return v == 0; }));
    CHECK (std::all_of (r.begin(), r.end(), [] (std::int32_t v) { return v == 0; }));
}

TEST_CASE ("The kick starts sharp and falls to its pitch twice as fast as its tone", "[drums]")
{
    // Section 5.3: the glide is g_max at a top-velocity stroke and decays as e1², so at a time t
    // it is g_max · exp(-2t/τ) with τ = Q / (π f) = 10 / (π · 56.9 Hz).
    const double fs = 48000.0;
    DrumKit kit;
    kit.prepare (fs);
    kit.strike (Instrument::kick, 127);
    const double f01 = kit.bankInfo (Instrument::kick).hz[0];
    const double tau = 10.0 / (kPiD * f01);
    std::int32_t l = 0, r = 0;
    auto frequencyAt = [&] (int sample, int& now) {
        while (now < sample)
        {
            kit.render (&l, &r, 1);
            ++now;
        }
        const double a = kit.bankInfo (Instrument::kick).fundamentalPhase;
        kit.render (&l, &r, 1);
        ++now;
        double d = kit.bankInfo (Instrument::kick).fundamentalPhase - a;
        while (d < 0.0)
            d += kTwoPiD;
        return d * fs / kTwoPiD;
    };
    int now = 0;
    for (double t : { 0.0, 0.5 * tau, tau, 3.0 * tau })
    {
        const double f = frequencyAt (static_cast<int> (t * fs), now);
        const double expected = f01 * (1.0 + 0.45 * std::exp (-2.0 * t / tau));
        INFO ("t " << t << " got " << f << " expected " << expected);
        CHECK (std::abs (f / expected - 1.0) < 0.01);
    }
}

TEST_CASE ("A closed hat chokes a ringing open one", "[drums]")
{
    const double fs = 48000.0;
    const int at = static_cast<int> (0.2 * fs);
    auto render = [&] (bool choke) {
        DrumKit kit;
        kit.prepare (fs);
        kit.strike (Instrument::hatOpen, 124);
        std::vector<std::int32_t> l (static_cast<size_t> (fs)), r (static_cast<size_t> (fs));
        kit.render (l.data(), r.data(), at);
        if (choke)
            kit.strike (Instrument::hatClosed, 124);
        kit.render (l.data() + at, r.data() + at, static_cast<int> (fs) - at);
        std::vector<float> v (l.size());
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = static_cast<float> (l[i]);
        return v;
    };
    const auto open = render (false);
    const auto choked = render (true);
    // 150 ms after the closed stroke, at least 25 dB under the open hat left alone.
    const auto from = static_cast<size_t> (at + 0.15 * fs);
    const auto to = static_cast<size_t> (at + 0.17 * fs);
    CHECK (dbOf (rmsOf (choked, from, to)) <= dbOf (rmsOf (open, from, to)) - 25.0);
}

TEST_CASE ("A soft snare stroke buzzes for less time than a hard one", "[drums]")
{
    // Section 5.4: the wires buzz while the head is over a threshold 30 dB under a top stroke, so
    // a stroke at amplitude u buzzes for τ · ln(u / θ).
    const double fs = 48000.0;
    auto buzz = [fs] (int velocity) {
        DrumKit kit;
        kit.prepare (fs);
        kit.strike (Instrument::snare, velocity);
        std::int32_t l = 0, r = 0;
        int n = 0;
        while (kit.bankInfo (Instrument::snare).wireDrive > 0.0 && n < static_cast<int> (fs))
        {
            kit.render (&l, &r, 1);
            ++n;
        }
        return static_cast<double> (n) / fs;
    };
    DrumKit kit;
    kit.prepare (fs);
    const double tau = 30.0 / (kPiD * kit.bankInfo (Instrument::snare).hz[0]);
    const double hard = buzz (124);
    const double soft = buzz (40);
    const auto u = [] (int v) {
        return std::pow (v / 127.0, 1.5);
    };
    const double theta = std::pow (10.0, -30.0 / 20.0);
    CHECK (soft < hard);
    CHECK (std::abs (hard / (tau * std::log (u (124) / theta)) - 1.0) < 0.03);
    CHECK (std::abs (soft / (tau * std::log (u (40) / theta)) - 1.0) < 0.03);
}

// -------------------------------------------------------------------------------------------------
// Level, tone and the transport (sections 3.5 and 5.7)

TEST_CASE ("Level scales the drums and a centred Tone leaves them exact", "[drums]")
{
    GrooveSetup g;
    g.fillEvery = 0;
    const int frames = barFrames (g.fs, g.bpm, 2, 1);
    const auto flat = renderGroove (g, frames);
    // At 0 dB and a centred Tone the output is the kit's integers / 2^23, exactly.
    for (float v : flat.left)
        CHECK (std::ldexp (v, 23) == std::round (std::ldexp (v, 23)));
    GrooveSetup louder = g;
    louder.levelDb = 6.0f;
    const auto up = renderGroove (louder, frames);
    CHECK (std::abs (peak (up.left) / peak (flat.left) - dbToGain (6.0f)) < 1.0e-4);

    // Tone 10: highs up and lows down by about 6 dB each.
    GrooveSetup bright = g;
    bright.tone = 1.0f;
    const auto hi = renderGroove (bright, frames);
    const double lowFlat = toneMagnitude (flat.left.data(), frames, 60.0, g.fs);
    const double lowBright = toneMagnitude (hi.left.data(), frames, 60.0, g.fs);
    CHECK (dbOf (lowBright) - dbOf (lowFlat) == Catch::Approx (-6.0).margin (0.5));
}

TEST_CASE ("Drums start on the downbeat, stop cleanly and switch with the click", "[drums]")
{
    Metronome m;
    m.prepare ({ 48000.0, 512, 2 });
    GrooveSetup g;
    auto s = settingsFor (g);
    Buffer b (2, 512);
    const Metronome::FiredEvent* fired = nullptr;

    // Off: nothing. On: the kick on the first sample of the block.
    s.enabled = false;
    m.setSettings (s);
    m.process (b.view());
    CHECK (m.firedEvents (&fired) == 0);
    s.enabled = true;
    m.setSettings (s);
    m.process (b.view());
    const int started = m.firedEvents (&fired);
    REQUIRE (started > 0);
    CHECK (fired[0].offset == 0);
    CHECK (std::any_of (fired, fired + started, [] (const Metronome::FiredEvent& e) {
        return e.offset == 0 && e.instrument == Instrument::kick;
    }));

    // Stop: no more events, the kit rings out to exact silence.
    s.enabled = false;
    m.setSettings (s);
    bool silent = false;
    for (int i = 0; i < 48000 / 512 * 10 && !silent; ++i)
    {
        b.view().clear();
        m.process (b.view());
        CHECK (m.firedEvents (&fired) == 0);
        silent = std::all_of (b[0].begin(), b[0].end(), [] (float v) { return v == 0.0f; });
    }
    CHECK (silent);

    // Click mode: no drum events; switching to Drums brings them in on the next step.
    Metronome click;
    click.prepare ({ 48000.0, 512, 2 });
    auto cs = settingsFor (g);
    cs.mode = 0;
    cs.steps.fill (Metronome::Normal);
    click.setSettings (cs);
    int stepsSeen = 0;
    for (int i = 0; i < 100; ++i) // 1.07 s: steps at 0, 0.5 s, 1 s
    {
        click.process (b.view());
        stepsSeen += click.firedEvents (&fired);
    }
    CHECK (stepsSeen == 0);
    cs.mode = 1;
    click.setSettings (cs);
    long long firstDrum = -1;
    for (int i = 100; i < 200 && firstDrum < 0; ++i)
    {
        click.process (b.view());
        if (click.firedEvents (&fired) > 0)
            firstDrum = static_cast<long long> (i) * 512 + fired[0].offset;
    }
    CHECK (firstDrum == 72000); // the step at 1.5 s, beat 4 of the first bar
}

TEST_CASE ("Drums output is finite for random settings, rates and block sizes", "[drums]")
{
    Random rng (0xD0D0u);
    for (double fs : kRates)
    {
        Metronome m;
        m.prepare ({ fs, 700, 2 });
        Buffer b (2, static_cast<int> (fs * 2.0));
        for (int start = 0; start < b.frames();)
        {
            Metronome::Settings s;
            s.enabled = rng.unipolar() < 0.9f;
            s.mode = rng.unipolar() < 0.8f ? 1 : 0;
            s.signature = static_cast<int> (rng.unipolar() * 8.0f);
            s.bpm = 40.0f + 200.0f * rng.unipolar();
            s.drums.groove = static_cast<int> (rng.unipolar() * 13.0f);
            s.drums.intensity = 1 + static_cast<int> (rng.unipolar() * 5.0f);
            s.drums.fillEvery = 2 * static_cast<int> (rng.unipolar() * 5.0f);
            s.drumsLevelDb = -30.0f + 36.0f * rng.unipolar();
            s.drumsTone = rng.unipolar();
            m.setSettings (s);
            const int len =
                std::min (b.frames() - start, 1 + static_cast<int> (rng.unipolar() * 699.0f));
            m.process (b.view().subView (start, len));
            start += len;
        }
        CHECK (allFinite (b[0]));
        CHECK (allFinite (b[1]));
    }
}
