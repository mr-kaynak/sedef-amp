// SPDX-License-Identifier: AGPL-3.0-or-later
// What must not change the notes (docs/design/guitar-to-midi.md, "Why the early reading can be
// trusted, and when"): the block size, the input's level, a DC offset, where in time the playing
// starts, the sample rate. And what the derivation promises: every semitone from F#1 to E6 named
// within the time the rule gives, the hardest signals of the model named right (among them strings
// whose partials start together, as a pluck starts them), the limit it predicts for a string that
// gives one pulse a period, and a pinned event stream, so that any change of behaviour shows here.
//
// Everything is deterministic: the signals are sums of partials whose phases and noise come from
// sedef::Random (xorshift32, integer arithmetic: the same numbers on every platform; no
// standard-library distribution is used anywhere in these signals), there is no clock and no
// thread, and the pinned signals are rounded to 16 bits, so that the last bit of a platform's sin()
// cannot reach the converter. The converter's own arithmetic rounds a little differently on each
// platform: what is exact where is said at kPinnedSlack.
#include "MidiTestSignals.h"
#include "sedef/core/Biquad.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace sedef;
using namespace sedef::test::midi;
using midi::GuitarToMidi;
using midi::LowNote;

namespace
{
constexpr double kFs = 48000.0;

midi::Settings fastest()
{
    midi::Settings s;
    s.octave = 0;
    return s;
}

midi::Settings steady()
{
    auto s = fastest();
    s.timing = midi::Timing::steady;
    return s;
}

midi::Settings withBend()
{
    auto s = fastest();
    s.mode = midi::Mode::notesAndBend;
    return s;
}

midi::Settings inRange (LowNote low)
{
    auto s = fastest();
    s.lowNote = low;
    return s;
}

/// The range a note is played in here: an eight-string's under B1, a seven-string's under E2.
LowNote rangeFor (int pitch)
{
    return pitch < 35 ? LowNote::fSharp1 : pitch < 40 ? LowNote::b1 : LowNote::e2;
}

double lowestHzOf (LowNote low)
{
    return GuitarToMidi::kRanges[low == LowNote::fSharp1 ? 2 : low == LowNote::b1 ? 1 : 0].lowestHz;
}

/// One event sent: the sample it belongs to and its bytes.
struct Sent
{
    std::int64_t at = 0;
    std::uint32_t bytes = 0;
    bool operator== (const Sent& o) const { return at == o.at && bytes == o.bytes; }
};

std::vector<Sent> sent (const std::vector<float>& x, double fs, const midi::Settings& s,
                        std::size_t block, LowNote learned = LowNote::e2)
{
    GuitarToMidi g;
    g.setSettings (s);
    g.prepare (fs);
    g.setLearnedLowNote (learned);
    midi::EventList out;
    std::vector<Sent> all;
    for (std::size_t start = 0; start < x.size(); start += block)
    {
        const auto n = static_cast<int> (std::min (block, x.size() - start));
        out.clear();
        g.process (x.data() + start, n, out);
        for (const auto& e : out)
        {
            std::uint32_t bytes = 0;
            for (int k = 0; k < e.size; ++k)
                bytes = (bytes << 8) | e.bytes[k];
            all.push_back ({ static_cast<std::int64_t> (start) + e.offset, bytes });
        }
    }
    return all;
}

/// The note-ons of a run: sample, note and velocity.
struct On
{
    std::int64_t at = 0;
    int note = 0, velocity = 0;
};

std::vector<On> noteOns (const std::vector<Sent>& events)
{
    std::vector<On> ons;
    for (const auto& e : events)
        if ((e.bytes >> 16 & 0xF0u) == 0x90u && (e.bytes & 0x7Fu) != 0)
            ons.push_back ({ e.at, static_cast<int> (e.bytes >> 8 & 0x7Fu),
                             static_cast<int> (e.bytes & 0x7Fu) });
    return ons;
}

std::vector<int> pitches (const std::vector<On>& ons)
{
    std::vector<int> p;
    for (const auto& o : ons)
        p.push_back (o.note);
    return p;
}

/// Rounded to 16 bits, as a recording is.
std::vector<float> rounded (std::vector<float> x)
{
    for (auto& v : x)
        v = static_cast<float> (std::lround (static_cast<double> (v) * 32768.0)) / 32768.0f;
    return x;
}

/// When the early reading's rule lets a note of `hz` go, after its pick, in a range whose
/// lowest note is `lowestHz` (the design's t(f)): the attack, one period, and the stretch that
/// must repeat (0.6 of a period and kEarlyWindowSeconds at least when nothing an octave under the
/// note is in the range; a whole period and kEarlyAloneSeconds at least otherwise).
double ruleSeconds (double hz, double lowestHz)
{
    const double period = 1.0 / hz;
    const bool nothingUnder = 2.0 * period > 1.0 / lowestHz;
    const double repeat = nothingUnder
                              ? std::max (GuitarToMidi::kEarlyWindowSeconds,
                                          static_cast<double> (GuitarToMidi::kEarlyShare) * period)
                              : std::max (period, GuitarToMidi::kEarlyAloneSeconds);
    return GuitarToMidi::kAttackSkipSeconds + period + repeat;
}

/// What the rule allows a note of `hz` to leave, in a range whose lowest note is `lowestHz`.
double ruleLimit (double hz, double lowestHz)
{
    return 2.0 / hz > 1.0 / lowestHz ? static_cast<double> (GuitarToMidi::kEarlyClean)
                                     : static_cast<double> (GuitarToMidi::kEarlyAlone);
}

/// The derivation's own measure, computed here from first principles on the signal itself (no
/// decimation, no sums kept as the signal comes): the band the converter reads (40 Hz to 1.5 kHz,
/// cut twice above), the stretch from kAttackSkipSeconds after the known pick to `until` seconds
/// after it, and what the best period within 1.5 % of the known one leaves unexplained of it.
double leftAtTruePeriod (const std::vector<float>& x, double fs, double pickSeconds, double hz,
                         double until)
{
    struct Section
    {
        BiquadCoeffs c;
        double s0 = 0.0, s1 = 0.0;
        double process (double v)
        {
            const double y = c.b0 * v + s0;
            s0 = c.b1 * v - c.a1 * y + s1;
            s1 = c.b2 * v - c.a2 * y;
            return y;
        }
    };
    Section hp { design::highpass (fs, 40.0, 0.7071) };
    Section lp1 { design::lowpass (fs, 1500.0, 0.7071) }, lp2 = lp1;
    const auto from =
        static_cast<std::size_t> ((pickSeconds + GuitarToMidi::kAttackSkipSeconds) * fs);
    const auto to = std::min (x.size(), static_cast<std::size_t> ((pickSeconds + until) * fs));
    std::vector<double> band;
    for (std::size_t i = 0; i < to; ++i)
    {
        const double v = lp2.process (lp1.process (hp.process (x[i])));
        if (i >= from)
            band.push_back (v);
    }
    double best = 2.0;
    const double period = fs / hz;
    for (double lag = 0.985 * period; lag <= 1.015 * period; lag += 0.25)
    {
        const auto whole = static_cast<std::size_t> (lag);
        const double f = lag - static_cast<double> (whole);
        double diff = 0.0, energy = 0.0;
        for (std::size_t i = whole + 1; i < band.size(); ++i)
        {
            const double a = band[i];
            const double b = (1.0 - f) * band[i - whole] + f * band[i - whole - 1];
            diff += (a - b) * (a - b);
            energy += a * a + b * b;
        }
        if (energy > 0.0)
            best = std::min (best, diff / energy);
    }
    return best;
}

/// A note the signal alone does not settle waits for the tracker: two periods and the 8 ms the
/// MIDI layer was always allowed, or the 16 ms of a note above E3 with the same slack.
double trackerSeconds (double hz)
{
    return std::max (2.0 / hz + 0.008, 0.0185);
}

/// What the rule's time may be exceeded by: one hop of the analysis (0.5 ms), the pick's date
/// (1 ms), and the band filter's delay with one decimated sample (0.5 ms).
constexpr double kRuleMarginSeconds = 0.002;

/// Seconds from `at` to the first note-on of `note` after it, -1 if none within 80 ms.
double onAfter (const std::vector<On>& ons, int note, double at, double fs)
{
    for (const auto& o : ons)
    {
        const double t = static_cast<double> (o.at) / fs;
        if (o.note == note && t >= at - 0.002 && t <= at + 0.08)
            return t - at;
    }
    return -1.0;
}

std::vector<float> shortLine()
{
    return bassLine (1, Pickup::bridge, 124.0, kFs, 10, false).signal;
}

/// A string whose partials all start together, as a pluck starts them: in sine phase (a magnetic
/// pickup hears the string's velocity) or in cosine phase (a piezo hears the force at the bridge,
/// which the release changes at once: a step). Plucked at `pluck` of its length, heard at `pickup`
/// (0: no pickup comb), its fundamental at `fundamental` of the model's.
test::Pluck together (double hz, double pluck, double pickup, double fundamental, bool step)
{
    test::Pluck p = pluckFor (Pickup::bridge, hz, 0.25f, 1);
    p.pluckPosition = pluck;
    p.pickupPosition = pickup;
    p.fundamentalGain = fundamental;
    p.pickNoise = 0.0f;
    p.startPhase = step ? 0.5 * kPiD : 0.0;
    return p;
}

/// A string picked twice, at 0.1 and 0.6 s, as the design's recordings of strings whose
/// partials start together were made: partials n f0 sqrt((1 + B n^2) / (1 + B)) up to 7 kHz with
/// B = 1e-4, plucked at `pluck` of the string and heard at `pickup` (0: a piezo, no comb), the
/// fundamental at `fundamental`, partial n decaying in 0.6 / (1 + 0.1 (n - 1) + (f_n / 2 kHz)^2)
/// seconds, every one in sine phase (a pulse a period) or cosine phase (a step), 0.25 at most, a
/// 0.5 ms attack, damped over 5 ms after 0.395 s; uniform noise 1e-4 rms from sedef::Random.
/// Unlike pluckFor(), no glide and fast-fading upper partials: closer to a string plucked hard
/// near the bridge.
Scenario recordedTogether (int pitch, double pluck, double pickup, double fundamental, bool step)
{
    const double f0 = hzOf (pitch);
    std::vector<double> amp, hz, tau;
    double sum = 0.0;
    for (int n = 1; n < 400; ++n)
    {
        const double nd = n;
        const double ratio = nd * std::sqrt ((1.0 + 1.0e-4 * nd * nd) / 1.0001);
        if (ratio * f0 >= 7000.0)
            break;
        double a = std::sin (nd * kPiD * pluck) / nd *
                   (pickup > 0.0 ? std::sin (nd * kPiD * pickup) : 1.0);
        if (n == 1)
            a *= fundamental;
        amp.push_back (a);
        hz.push_back (ratio * f0);
        tau.push_back (0.6 /
                       (1.0 + 0.1 * (nd - 1.0) + (ratio * f0 / 2000.0) * (ratio * f0 / 2000.0)));
        sum += std::abs (a);
    }
    const double phase = step ? 0.5 * kPiD : 0.0;
    const auto length = static_cast<std::size_t> (0.4 * kFs);
    std::vector<double> one (length, 0.0);
    for (std::size_t i = 0; i < length; ++i)
    {
        const double t = static_cast<double> (i) / kFs;
        double x = 0.0;
        for (std::size_t k = 0; k < amp.size(); ++k)
            x += amp[k] * std::exp (-t / tau[k]) * std::sin (kTwoPiD * hz[k] * t + phase);
        const double attack = t < 5.0e-4 ? 0.5 - 0.5 * std::cos (kPiD * t / 5.0e-4) : 1.0;
        const double mute = std::clamp ((0.4 - 0.005 - t) / 0.005, 0.0, 1.0);
        one[i] = 0.25 / sum * x * attack * mute;
    }
    Scenario sc;
    sc.name = "together";
    sc.signal.assign (static_cast<std::size_t> (1.2 * kFs), 0.0f);
    for (double at : { 0.1, 0.6 })
        for (std::size_t i = 0; i < length; ++i)
            sc.signal[static_cast<std::size_t> (at * kFs) + i] += static_cast<float> (one[i]);
    Random rng (9u);
    for (auto& v : sc.signal)
        v += 1.7320508e-4f * rng.bipolar();
    sc.notes.push_back ({ pitch, 0.1, 0.5, true, 0.25f });
    sc.notes.push_back ({ pitch, 0.6, 1.0, true, 0.25f });
    return sc;
}

/// The same playing `k` samples later.
std::vector<float> later (const std::vector<float>& x, std::size_t k)
{
    std::vector<float> y (k, 0.0f);
    y.insert (y.end(), x.begin(), x.end());
    return y;
}
} // namespace

TEST_CASE ("Guitar to MIDI sends the same events at the same samples in any block size",
           "[midi][invariance][block]")
{
    // Blocks of 1, 32, 64, 128, 512 and 4096 samples and two odd sizes: every event the same,
    // at the same sample, in Fastest, Steady and Notes + Bend. Nothing in the converter is
    // counted in blocks: its clocks run on samples, and an event's sample is where it was decided.
    const std::vector<std::vector<float>> signals = {
        shortLine(),
        sloppyLine (2, Pickup::neck, 120.0, kFs, 10).signal,
        repeated (40, Pickup::bridge, 1, 4, 0.25, kFs).signal,
        bentString (110.0, 100.0, 0.3, true, kFs).sc.signal,
    };
    for (const auto& settings : { fastest(), steady(), withBend() })
        for (std::size_t k = 0; k < signals.size(); ++k)
        {
            const auto reference = sent (signals[k], kFs, settings, 64);
            REQUIRE (!reference.empty());
            for (std::size_t block : { 1u, 32u, 97u, 128u, 441u, 512u, 4096u })
            {
                INFO ("signal " << k << ", block " << block);
                CHECK (sent (signals[k], kFs, settings, block) == reference);
            }
        }
}

TEST_CASE ("Guitar to MIDI plays the same notes at any level and over a DC offset",
           "[midi][invariance][level]")
{
    const std::vector<std::vector<float>> signals = {
        shortLine(),
        repeated (45, Pickup::neck, 2, 4, 0.25, kFs).signal,
    };
    for (const auto& settings : { fastest(), steady() })
        for (const auto& x : signals)
        {
            const auto reference = sent (x, kFs, settings, 64);
            REQUIRE (noteOns (reference).size() >= 4);
            // A power of two scales every value exactly, the filters' and the sums' too, and every
            // level by the same number of dB: the same events at the same samples.
            for (const float gain : { 0.125f, 4.0f })
            {
                INFO ("gain " << gain);
                auto y = x;
                for (auto& v : y)
                    v *= gain;
                CHECK (sent (y, kFs, settings, 64) == reference);
            }
            // Any other gain rounds every sample anew, and a DC offset leaves the band's
            // high-pass a step to answer at the start: the same notes in the same order, each
            // note-on within one millisecond (the level is kept as one value a millisecond, and a
            // pick is dated from where it rises over the floor; a rounding can move that by one).
            const auto ref = noteOns (reference);
            for (int variant = 0; variant < 3; ++variant)
            {
                INFO ("variant " << variant);
                auto y = x;
                const float gain = variant == 0 ? 0.1f : variant == 1 ? 3.98f : 1.0f;
                const float dc = variant == 2 ? 0.05f : 0.0f;
                for (auto& v : y)
                    v = gain * v + dc;
                const auto ons = noteOns (sent (y, kFs, settings, 64));
                REQUIRE (pitches (ons) == pitches (ref));
                for (std::size_t k = 0; k < ons.size(); ++k)
                {
                    CHECK (std::abs (ons[k].at - ref[k].at) <= 48);
                    CHECK (std::abs (ons[k].velocity - ref[k].velocity) <= 1);
                }
            }
        }
}

TEST_CASE ("Guitar to MIDI's notes move with the playing when it starts later",
           "[midi][invariance][shift]")
{
    // Every clock of the converter runs on the samples since prepare(): the band is read every
    // 6th sample at 48 kHz, analysed every 24th, the level kept every 48th. Started a multiple of
    // 48 samples later the playing meets the same grid: every event exactly that many samples
    // later. Started at any other sample it meets the grid at another phase: the same notes,
    // each within one millisecond's grid (48 samples).
    const auto x = shortLine();
    for (const auto& settings : { fastest(), steady() })
    {
        const auto reference = sent (x, kFs, settings, 64);
        for (std::size_t k : { 48u, 480u, 4800u })
        {
            INFO ("shift " << k);
            auto moved = reference;
            for (auto& e : moved)
                e.at += static_cast<std::int64_t> (k);
            CHECK (sent (later (x, k), kFs, settings, 64) == moved);
        }
        const auto ref = noteOns (reference);
        for (std::size_t k : { 1u, 7u, 13u, 101u, 997u })
        {
            INFO ("shift " << k);
            const auto ons = noteOns (sent (later (x, k), kFs, settings, 64));
            REQUIRE (pitches (ons) == pitches (ref));
            for (std::size_t n = 0; n < ons.size(); ++n)
                CHECK (std::abs (ons[n].at - static_cast<std::int64_t> (k) - ref[n].at) <= 48);
        }
    }
}

TEST_CASE ("Guitar to MIDI plays the same at 44.1, 48, 88.2 and 96 kHz", "[midi][invariance][rate]")
{
    // The same string at four rates: the same note, within the rule's time, and within 1.5 ms of
    // its time at 48 kHz: the analysis runs at 7350, 8000, 7350 and 7385 Hz, so the rule's
    // windows, its hop (4 decimated samples, 0.50 to 0.54 ms) and the millisecond the level is
    // kept in fall differently; one hop and one millisecond at most between two rates.
    for (int pitch : { 40, 45, 52, 59, 64 })
        for (auto pickup : { Pickup::bridge, Pickup::neck })
        {
            double at48 = -1.0;
            for (double fs : { 48000.0, 44100.0, 88200.0, 96000.0 })
            {
                INFO ("note " << pitch << " at " << fs << " Hz");
                const auto sc =
                    twice (pluckFor (pickup, hzOf (pitch), 0.25f, 1), fs, -80.0f, 3, 0.4, 0.6);
                const auto ons = noteOns (sent (sc.signal, fs, fastest(), 64));
                REQUIRE (ons.size() == 2);
                const double t = onAfter (ons, pitch, 0.4, fs);
                REQUIRE (t >= 0.0);
                const double rule = ruleSeconds (hzOf (pitch), 70.0);
                const bool settled =
                    leftAtTruePeriod (sc.signal, fs, 0.4, hzOf (pitch), rule + 0.001) <=
                    0.5 * ruleLimit (hzOf (pitch), 70.0);
                CHECK (t <= (settled ? rule + kRuleMarginSeconds : trackerSeconds (hzOf (pitch))));
                if (fs == 48000.0)
                    at48 = t;
                else
                    CHECK (std::abs (t - at48) <= 0.0015);
            }
        }
}

TEST_CASE ("Guitar to MIDI names every semitone from F#1 to E6 within the rule's time",
           "[midi][invariance][bound]")
{
    // Strings whose pitch, partials and pick time are known exactly: every semitone, on a bright
    // bridge pickup and a neck pickup, two sets of partial phases (the pick's phase), one over
    // noise 60 dB under the note and one 35 dB under it. The note is right. And whenever the
    // signal settles it (the derivation's measure, computed in this file on the signal itself
    // with the pick and the period known), it goes out within t(f) of the design, computed from
    // the rule's constants, plus the margin; a note the signal does not settle is out within the
    // tracker's time. Notes under a guitar's low E are played in the eight-string range, the
    // rest in a six-string's.
    double worst = -1.0;
    int count = 0, settled = 0;
    for (int pitch = 30; pitch <= 88; ++pitch)
    {
        auto settings = fastest();
        settings.lowNote = pitch < 40 ? LowNote::fSharp1 : LowNote::e2;
        const double lowestHz = lowestHzOf (settings.lowNote);
        const double hz = hzOf (pitch);
        const double rule = ruleSeconds (hz, lowestHz);
        for (auto pickup : { Pickup::bridge, Pickup::neck })
            for (std::uint32_t seed = 1; seed <= 2; ++seed)
            {
                const float noiseDb = seed == 1 ? -80.0f : -55.0f;
                INFO ("note " << pitch << ", seed " << seed << ", noise " << noiseDb
                              << (pickup == Pickup::neck ? ", neck" : ", bridge"));
                const auto sc =
                    twice (pluckFor (pickup, hz, 0.25f, 1), kFs, noiseDb, seed, 0.4, 0.5);
                const auto ons = noteOns (sent (sc.signal, kFs, settings, 64));
                REQUIRE (ons.size() == 2);
                CHECK (ons[0].note == pitch);
                const double t = onAfter (ons, pitch, 0.4, kFs);
                REQUIRE (t >= 0.0);
                // The signal settles it when, by the rule's time and a millisecond on, the true
                // period leaves no more than half of what the rule allows.
                const bool clear = leftAtTruePeriod (sc.signal, kFs, 0.4, hz, rule + 0.001) <=
                                   0.5 * ruleLimit (hz, lowestHz);
                CHECK (t <= (clear ? rule + kRuleMarginSeconds : trackerSeconds (hz)));
                if (clear)
                {
                    worst = std::max (worst, t - rule);
                    ++settled;
                }
                ++count;
            }
    }
    std::printf ("every semitone F#1 to E6: %d notes, %d settled by the signal alone; the latest "
                 "of those %.2f ms after the rule's time\n",
                 count, settled, 1000.0 * worst);
    // Nearly all of them are: the bound is not an empty promise.
    CHECK (settled >= count * 3 / 4);
}

TEST_CASE ("Guitar to MIDI names the model's hardest signals right", "[midi][invariance][hard]")
{
    // The cases the derivation says are closest to a wrong note. None may give one; a note that
    // the early reading cannot settle waits for the tracker, and is still right.
    // Right: every note sent is the note played, and the second pick (the first has the very
    // first note's wait) starts it once, within the tracker's time and a little. (A note that
    // ends within a millisecond of its start sounds nothing: the tracker sends one at the
    // damping of a piezo's low F#1, in 0.8.0 as now; it is not the early reading's. See the
    // design, "Left open".)
    const auto right = [] (const Scenario& sc, const midi::Settings& s) {
        const auto run = convert (sc.signal, kFs, s);
        const int pitch = sc.notes[0].pitch;
        const double second = sc.notes[1].start;
        int starts = 0, sounding = 0;
        for (const auto& n : run.notes)
        {
            if (n.off >= 0.0 && n.off - n.on < 0.001)
                continue;
            if (n.pitch != pitch)
                return false;
            ++sounding;
            starts += n.on >= second - 0.002 && n.on <= second + 0.08 ? 1 : 0;
        }
        return starts == 1 && sounding == 2;
    };
    const auto scored = [] (const Scenario& sc, const midi::Settings& s) {
        const auto st = score (convert (sc.signal, kFs, s), sc, 0);
        return st.correct == st.expected && st.wrongPitch == 0 && st.extra == 0;
    };
    SECTION ("a weak fundamental under a strong second partial")
    {
        // The octave above leaves twice the odd partials' share, over any stretch: with the
        // fundamental 30 dB down the 3rd, 5th and 7th partials still hold it over kEarlyOther.
        for (int pitch : { 40, 45, 50, 55 })
            for (double gain : { 0.3, 0.1, 0.03 })
            {
                INFO ("note " << pitch << ", fundamental at " << gain);
                auto p = pluckFor (Pickup::bridge, hzOf (pitch), 0.25f, 2);
                p.fundamentalGain = gain;
                CHECK (right (twice (p, kFs, -80.0f, 2, 0.4, 0.8), fastest()));
            }
    }
    SECTION ("a note an octave or a twelfth from one that still rings")
    {
        for (double hz : { 110.0, 164.814, 220.0 })
            for (double cents : { -1200.0, 1200.0, -1900.0, 1900.0 })
            {
                const double old = hz * std::exp2 (cents / 1200.0);
                if (old < 82.0 || old > 700.0)
                    continue;
                INFO (hz << " Hz over a note " << cents << " cents from it");
                CHECK (scored (overRinging (hz, cents, kFs), fastest()));
            }
    }
    SECTION ("a note a semitone from one that still rings")
    {
        // Two strings a semitone apart beat, 6 to 13 times a second here, and each beat can
        // start the new note again (it did before the early reading, in the same places): no
        // note but the two played is ever sent, and the new one comes within the tracker's time.
        for (double hz : { 110.0, 164.814, 220.0 })
            for (double cents : { -100.0, 100.0 })
            {
                INFO (hz << " Hz over a note " << cents << " cents from it");
                const auto sc = overRinging (hz, cents, kFs);
                const auto run = convert (sc.signal, kFs, fastest());
                for (const auto& n : run.notes)
                    CHECK ((n.pitch == sc.notes[0].pitch || n.pitch == sc.notes[1].pitch));
                CHECK (score (run, sc, 0).correct == 2);
            }
    }
    SECTION ("a pick as loud in its noise as in its note, at eight phases")
    {
        // The attack's noise is in the stretch's first milliseconds at any phase of the pick:
        // it leaves more than the rule allows until it has gone, and the note waits.
        for (int pitch : { 40, 47, 55, 64 })
            for (std::uint32_t seed = 1; seed <= 8; ++seed)
            {
                INFO ("note " << pitch << ", seed " << seed);
                auto p = pluckFor (Pickup::bridge, hzOf (pitch), 0.25f, seed);
                p.pickNoise = 0.25f;
                CHECK (right (twice (p, kFs, -80.0f, seed, 0.4, 0.8), fastest()));
            }
    }
    SECTION ("the stiffest string")
    {
        // Inharmonicity of 4e-4, four times a wound string's: the partials drift apart within
        // one period, the true period leaves more, and the low notes wait for the tracker.
        for (int pitch : { 30, 35, 40, 45, 50, 59 })
        {
            INFO ("note " << pitch);
            auto p = pluckFor (Pickup::bridge, hzOf (pitch), 0.25f, 1);
            p.inharmonicity = 4.0e-4;
            CHECK (right (twice (p, kFs, -80.0f, 1, 0.4, 0.8), inRange (rangeFor (pitch))));
        }
    }
    SECTION ("partials that start together: one pulse a period, and a piezo's step")
    {
        // A pluck starts every partial at once. Heard by a magnetic pickup near the bridge the
        // string is one pulse a period, and until the pulse has come round twice the windows
        // hold the quiet between pulses, which repeats at many periods (an A2 was read as B2,
        // an A#2 as C#3, before kEarlyDensity). Heard by a piezo, the release is a step, and the
        // band's high-pass answers it with a ring that makes the first periods of a low string
        // repeat at a longer period (an F#1 was read as F1 until the dip was placed again from
        // the later half of the windows). Every semitone from F#1 to C4, three plucks, both.
        const struct
        {
            double pluck, pickup, fundamental;
        } plucks[] = { { 0.06, 0.05, 0.2 }, { 0.1, 0.05, 1.0 }, { 0.18, 0.0, 0.5 } };
        int wrong = 0, runs = 0;
        for (int pitch = 30; pitch <= 60; ++pitch)
            for (const auto& k : plucks)
                for (bool step : { false, true })
                {
                    INFO ("note " << pitch << ", pluck " << k.pluck << ", pickup " << k.pickup
                                  << (step ? ", step" : ", pulse"));
                    const auto sc =
                        twice (together (hzOf (pitch), k.pluck, k.pickup, k.fundamental, step), kFs,
                               -80.0f, 1, 0.4, 0.55);
                    const bool ok = right (sc, inRange (rangeFor (pitch)));
                    CHECK (ok);
                    wrong += ok ? 0 : 1;
                    ++runs;
                }
        std::printf ("partials that start together: %d of %d strings with a wrong or extra note\n",
                     wrong, runs);
    }
    SECTION ("a knock under the pick")
    {
        // Something slow at the pick (the body knocked, a piezo's step): the band's high-pass
        // answers it with a ring at about 28 Hz, which repeats at every short period and leaves
        // a quarter of that at half of one (the design, 10): half the period must leave what a
        // string's octave above does, or the shortest period is read as a note.
        for (int pitch : { 40, 45, 52, 57 })
            for (double knock : { 0.5, 2.0 })
            {
                INFO ("note " << pitch << ", knock " << knock);
                auto sc = twice (pluckFor (Pickup::bridge, hzOf (pitch), 0.25f, 1), kFs, -80.0f, 1,
                                 0.4, 0.8);
                const auto at = static_cast<std::size_t> (0.4 * kFs);
                for (std::size_t i = at; i < sc.signal.size(); ++i)
                    sc.signal[i] += static_cast<float> (
                        knock * std::exp (-static_cast<double> (i - at) / (0.03 * kFs)));
                CHECK (right (sc, fastest()));
            }
    }
    SECTION ("partials that start together, as the design's recordings were made")
    {
        // The strings on which each guard of the rule was needed (the design, "Why the early
        // reading can be trusted", 7): a bridge pickup's pulse plucked at 0.18 of the string,
        // where without kEarlyDensity an E2 was read as F2; and a piezo's step, where without the
        // later half's place an F#1 was read as F1.
        for (int pitch = 40; pitch <= 48; ++pitch)
        {
            INFO ("pulse, note " << pitch);
            CHECK (right (recordedTogether (pitch, 0.18, 0.05, 0.2, false),
                          inRange (rangeFor (pitch))));
        }
        for (int pitch = 30; pitch <= 38; ++pitch)
        {
            INFO ("step, note " << pitch);
            CHECK (
                right (recordedTogether (pitch, 0.18, 0.0, 0.5, true), inRange (rangeFor (pitch))));
        }
    }
}

TEST_CASE ("Guitar to MIDI waits two periods for a string that gives one pulse a period",
           "[midi][invariance][pulse]")
{
    // The derivation's prediction for a low string plucked near the bridge with a finger and
    // heard near the bridge: its first pulse is inside the attack, and the stretch holds nothing
    // that repeats until the second pulse has come round to the third, two periods after the
    // pick. So its note goes out no sooner than two periods (less the pick's date, a millisecond),
    // where a string whose partials do not start together goes out after 1.6 periods.
    for (int pitch = 40; pitch <= 47; ++pitch)
    {
        INFO ("note " << pitch);
        const double hz = hzOf (pitch);
        const auto pulse = twice (together (hz, 0.08, 0.05, 0.2, false), kFs, -80.0f, 1, 0.4, 0.55);
        const auto ons = noteOns (sent (pulse.signal, kFs, fastest(), 64));
        REQUIRE (ons.size() == 2);
        CHECK (ons[1].note == pitch);
        const double t = onAfter (ons, pitch, 0.4, kFs);
        REQUIRE (t >= 0.0);
        CHECK (t >= 2.0 / hz - 0.001);
        CHECK (t <= trackerSeconds (hz));
    }
}

TEST_CASE ("Guitar to MIDI's early reading names no note the tracker could not",
           "[midi][invariance][high]")
{
    // Strings around and above the tracker's highest note (it reads up to A6; a guitar's highest
    // fret is E6): the early reading looks no higher (kEarlyHighestHz), so Fastest sends the
    // notes Steady sends, sooner. Over the tracker's reach a string is either not a note (B6) or
    // read an octave down by both (C#7).
    for (int pitch : { 89, 93, 95, 97 })
        for (auto pickup : { Pickup::bridge, Pickup::neck })
        {
            INFO ("note " << pitch << (pickup == Pickup::neck ? ", neck" : ", bridge"));
            const auto sc =
                twice (pluckFor (pickup, hzOf (pitch), 0.25f, 1), kFs, -80.0f, 1, 0.4, 0.5);
            CHECK (pitches (noteOns (sent (sc.signal, kFs, fastest(), 64))) ==
                   pitches (noteOns (sent (sc.signal, kFs, steady(), 64))));
        }
}

namespace
{
/// The pinned signals: short, clean, rounded to 16 bits.
std::vector<float> pinnedSignal (int which)
{
    switch (which)
    {
    case 0:
        return rounded (bassLine (3, Pickup::bridge, 132.0, kFs, 6, false).signal);
    case 1:
        return rounded (repeated (40, Pickup::neck, 1, 4, 0.25, kFs).signal);
    default:
        return rounded (pickedTwice (55, Pickup::neck, 2, kFs, true).signal);
    }
}

/// How far a pinned event may lie from its sample. The lists were printed on macOS on Apple
/// silicon, and there every event is at its sample. Elsewhere the last bits of the arithmetic
/// differ: MSVC builds with /fp:fast, compilers fuse multiply-adds differently, the maths
/// libraries round exp2 and log2 differently, and the tracker the MIDI layer reads is shared
/// with the amp's Trench, so its arithmetic stays as it is. A reading that meets its threshold
/// one step of the analysis later moves its event by that step: one hop is 24 samples, one
/// millisecond of the level 48. So elsewhere: the same events, in the same order, with the same
/// bytes (note, channel, velocity), each within 48 samples of its sample.
#if defined(__APPLE__) && defined(__aarch64__)
constexpr std::int64_t kPinnedSlack = 0;
#else
constexpr std::int64_t kPinnedSlack = 48;
#endif

void checkPinned (const std::vector<Sent>& got, const std::vector<Sent>& expected)
{
    CHECK (got.size() == expected.size());
    for (std::size_t k = 0; k < std::min (got.size(), expected.size()); ++k)
    {
        INFO ("event " << k << " at " << got[k].at << ", pinned at " << expected[k].at);
        CHECK (got[k].bytes == expected[k].bytes);
        CHECK (std::abs (got[k].at - expected[k].at) <= kPinnedSlack);
    }
}

void printEvents (const char* name, const std::vector<Sent>& events)
{
    std::printf ("    // %s\n    {", name);
    for (std::size_t k = 0; k < events.size(); ++k)
        std::printf ("%s{ %lld, 0x%06X }", k == 0 ? " " : ", ",
                     static_cast<long long> (events[k].at),
                     static_cast<unsigned> (events[k].bytes));
    std::printf (" },\n");
}
} // namespace

TEST_CASE ("Guitar to MIDI's pinned event streams, printed", "[.midipinned]")
{
    // Prints the lists below from the current build: run after a deliberate change of behaviour,
    // read the difference, and paste.
    for (int which = 0; which < 3; ++which)
    {
        printEvents ("Fastest", sent (pinnedSignal (which), kFs, fastest(), 64));
        printEvents ("Steady", sent (pinnedSignal (which), kFs, steady(), 64));
    }
}

TEST_CASE ("Guitar to MIDI's pinned event streams", "[midi][invariance][pinned]")
{
    // Three short signals (a bass line, one note four times, a fingered G3 twice), rounded to 16
    // bits, in Fastest and in Steady: every event, at its sample (within kPinnedSlack off the
    // platform the lists were printed on). A change of behaviour is a difference here;
    // "[.midipinned]" prints the lists to compare and paste.
    const std::vector<std::vector<Sent>> expected = {
        // Fastest
        { { 10070, 0x902858 },
          { 13847, 0x802800 },
          { 13847, 0x902861 },
          { 24503, 0x802800 },
          { 24503, 0x902A62 },
          { 46295, 0x802A00 },
          { 46295, 0x902B56 },
          { 68039, 0x802B00 },
          { 68039, 0x902C4F },
          { 73679, 0x802C00 },
          { 73679, 0x902A5C },
          { 78546, 0x802A00 } },
        // Steady
        { { 10070, 0x902858 },
          { 14469, 0x802800 },
          { 14469, 0x902861 },
          { 25378, 0x802800 },
          { 25378, 0x902A62 },
          { 47209, 0x802A00 },
          { 47209, 0x902B58 },
          { 69015, 0x802B00 },
          { 69015, 0x902C4F },
          { 74485, 0x802C00 },
          { 74485, 0x902A5C },
          { 80370, 0x802A00 } },
        // Fastest
        { { 7670, 0x902858 },
          { 17999, 0x802800 },
          { 17999, 0x902858 },
          { 29999, 0x802800 },
          { 29999, 0x902855 },
          { 41999, 0x802800 },
          { 41999, 0x902859 },
          { 60523, 0x802800 } },
        // Steady
        { { 7670, 0x902858 },
          { 18617, 0x802800 },
          { 18617, 0x902858 },
          { 30620, 0x802800 },
          { 30620, 0x902855 },
          { 42616, 0x802800 },
          { 42616, 0x90285B },
          { 62347, 0x802800 } },
        // Fastest
        { { 7673, 0x903758 }, { 29458, 0x803700 }, { 53423, 0x90375A }, { 87042, 0x803700 } },
        // Steady
        { { 7673, 0x903758 }, { 31282, 0x803700 }, { 54618, 0x90375A }, { 88866, 0x803700 } },
    };
    for (int which = 0; which < 3; ++which)
    {
        INFO ("signal " << which << ", Fastest");
        checkPinned (sent (pinnedSignal (which), kFs, fastest(), 64),
                     expected[static_cast<std::size_t> (2 * which)]);
    }
    for (int which = 0; which < 3; ++which)
    {
        INFO ("signal " << which << ", Steady");
        checkPinned (sent (pinnedSignal (which), kFs, steady(), 64),
                     expected[static_cast<std::size_t> (2 * which + 1)]);
    }
}
