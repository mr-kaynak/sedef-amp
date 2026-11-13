// SPDX-License-Identifier: AGPL-3.0-or-later
// Transpose (docs/dsp/transpose.md, section 8). Every signal is synthesised or seeded; nothing
// depends on time or unseeded randomness. Tests over many shifts or rates are tagged [long]
// (the sanitizer builds skip them; the release build runs them).
#include "ReverbTestUtils.h"
#include "TestUtils.h"
#include "reverb/TransposeEval.h"
#include "sedef/core/Random.h"
#include "sedef/dsp/PitchShifter.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <string>
#include <vector>

using namespace sedef;
namespace tp = sedef::test::transpose;
using dsp::PitchShifter;

namespace
{
struct Change
{
    size_t at;     ///< sample index
    int semitones; ///< the setting from there on
};

/// `in` shifted by `semitones`, processed in blocks of `block` frames; a block also ends where a
/// change is due, so every block size sees the changes at the same samples.
std::vector<float> shift (const std::vector<float>& in, double fs, int semitones, int block,
                          const std::vector<Change>& changes = {})
{
    PitchShifter p;
    p.setSettings ({ semitones });
    p.prepare ({ fs, block, 1 });
    std::vector<float> x = in;
    size_t pos = 0, next = 0;
    while (pos < x.size())
    {
        while (next < changes.size() && changes[next].at <= pos)
            p.setSettings ({ changes[next++].semitones });
        size_t end = std::min (x.size(), pos + static_cast<size_t> (block));
        if (next < changes.size())
            end = std::min (end, changes[next].at);
        p.process (x.data() + pos, static_cast<int> (end - pos));
        pos = end;
    }
    return x;
}

/// A sustained note: slow decay, so its partials hold still over the measurement window.
tp::Score sustainedNote (double hz, std::uint32_t seed)
{
    tp::Score s;
    s.name = "note";
    s.seconds = 2.2;
    tp::Note n;
    n.start = 0.05;
    n.hz = hz;
    n.level = 0.3;
    n.decay = 4.0;
    n.inharmonicity = hz < 150.0 ? 1.5e-4 : 0.6e-4;
    n.seed = seed;
    s.notes.push_back (n);
    s.onsets = { 0.05 };
    return s;
}

struct PitchError
{
    double worstCents = 0.0;
    int partials = 0;
};

/// The strongest partials of `in` (up to 8, at least 30 dB over the rest, up to `maxHz`) against
/// the same partials of `out`, which should sit at `ratio` times their frequency. Both are
/// measured the same way (Hann, zero-padded FFT, log-parabolic peak) over the same 1.2 s, so a
/// merged pair of partials compares with its own merged image.
PitchError pitchError (const std::vector<float>& in, const std::vector<float>& out, double fs,
                       double ratio, double maxHz)
{
    const auto start = static_cast<size_t> (0.8 * fs);
    const int len = static_cast<int> (1.2 * fs);
    const int nfft = test::nextPow2 (2 * len);
    const auto mi = test::magnitudeSpectrum (in.data() + start, len, nfft);
    const auto mo = test::magnitudeSpectrum (out.data() + start, len, nfft);
    const double binHz = fs / nfft;

    struct Peak
    {
        double hz, mag;
    };
    std::vector<Peak> peaks;
    double strongest = 0.0;
    for (size_t k = 2; k + 2 < mi.size() && static_cast<double> (k) * binHz < maxHz; ++k)
        if (static_cast<double> (k) * binHz > 60.0 && mi[k] > mi[k - 1] && mi[k] >= mi[k + 1])
        {
            peaks.push_back ({ static_cast<double> (k) * binHz, mi[k] });
            strongest = std::max (strongest, mi[k]);
        }
    std::sort (peaks.begin(), peaks.end(),
               [] (const Peak& a, const Peak& b) { return a.mag > b.mag; });
    PitchError e;
    std::vector<double> taken;
    for (const auto& p : peaks)
    {
        if (p.mag < 0.0316 * strongest || e.partials == 8)
            break;
        const bool near = std::any_of (taken.begin(), taken.end(), [&] (double hz) {
            return std::abs (hz - p.hz) < 0.02 * hz;
        });
        if (near)
            continue;
        taken.push_back (p.hz);
        const auto pin = test::strongestPeak (mi, fs, nfft, p.hz * 0.995, p.hz * 1.005);
        const double want = pin.hz * ratio;
        const auto pout = test::strongestPeak (mo, fs, nfft, want * 0.995, want * 1.005);
        e.worstCents = std::max (e.worstCents, std::abs (1200.0 * std::log2 (pout.hz / want)));
        ++e.partials;
    }
    return e;
}

/// Index of the largest |x| in [from, to).
size_t argMaxAbs (const std::vector<float>& x, size_t from, size_t to)
{
    size_t best = from;
    for (size_t i = from; i < to && i < x.size(); ++i)
        if (std::abs (x[i]) > std::abs (x[best]))
            best = i;
    return best;
}

tp::Shifter engineShifter (PitchShifter& p)
{
    tp::Shifter s;
    s.prepare = [&p] (double fs, int block) {
        p.prepare ({ fs, block, 1 });
    };
    s.setSemitones = [&p] (int st) {
        p.setSettings ({ st });
    };
    s.process = [&p] (float* x, int n) {
        p.process (x, n);
    };
    s.latency = [&p] {
        return p.latencySamples();
    };
    return s;
}
} // namespace

TEST_CASE ("Transpose reports 3 ms shifting down, 10 ms up and nothing at 0", "[transpose]")
{
    const double fs = GENERATE (44100.0, 48000.0, 88200.0, 96000.0);
    CAPTURE (fs);
    PitchShifter p;
    p.prepare ({ fs, 64, 1 });
    for (int st = -12; st <= 12; ++st)
    {
        CAPTURE (st);
        p.setSettings ({ st });
        const int want =
            st == 0 ? 0 : static_cast<int> (std::lround ((st < 0 ? 0.003 : 0.010) * fs));
        CHECK (p.latencySamples() == want);
        CHECK (PitchShifter::latencyFor (st, fs) == want);
    }
}

TEST_CASE ("Transpose at 0 semitones is bit-exact, with no latency", "[transpose]")
{
    const double fs = 48000.0;
    const int block = GENERATE (1, 64, 333);
    CAPTURE (block);
    std::vector<float> in (static_cast<size_t> (0.5 * fs));
    test::fillNoise (in, 0.5f, 3);
    const auto chord = tp::render (tp::scores::chord(), fs);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] += chord[i];

    PitchShifter p;
    p.prepare ({ fs, block, 1 });
    CHECK (p.latencySamples() == 0);
    CHECK_FALSE (p.isActive());
    CHECK (shift (in, fs, 0, block) == in);

    // And again once a shift has faded back to 0: from the end of the 20 ms fade on.
    const auto changeAt = static_cast<size_t> (0.25 * fs);
    const auto out = shift (in, fs, -3, block, { { changeAt, 0 } });
    const size_t fadeEnd = changeAt + static_cast<size_t> (std::ceil (0.020 * fs));
    CHECK (std::equal (out.begin() + static_cast<std::ptrdiff_t> (fadeEnd), out.end(),
                       in.begin() + static_cast<std::ptrdiff_t> (fadeEnd)));
    CHECK (out != in); // it did shift before
}

TEST_CASE ("Transpose plays every attack exactly its reported latency late", "[transpose]")
{
    // Clicks after near-silence, then over a ringing note: each must come out at its input
    // sample plus latencySamples(), to the sample.
    const double fs = 48000.0;
    const int st = GENERATE (-12, -7, -5, -3, -2, -1, 1, 2, 3, 5, 7, 12);
    CAPTURE (st);
    std::vector<float> in (static_cast<size_t> (0.6 * fs));
    test::fillNoise (in, 1.0e-4f, 9);
    tp::Score ring;
    ring.seconds = 0.6;
    tp::Note n;
    n.start = 0.30;
    n.hz = 110.0;
    n.level = 0.05;
    n.decay = 2.0;
    ring.notes.push_back (n);
    const auto note = tp::render (ring, fs);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] += note[i];
    const std::vector<size_t> clicks { static_cast<size_t> (0.15 * fs) + 7,
                                       static_cast<size_t> (0.40 * fs) + 3,
                                       static_cast<size_t> (0.52 * fs) + 11 };
    for (auto c : clicks)
        in[c] += 0.6f;

    PitchShifter p;
    p.setSettings ({ st });
    p.prepare ({ fs, 64, 1 });
    const auto latency = static_cast<size_t> (p.latencySamples());
    const auto out = shift (in, fs, st, 64);
    for (auto c : clicks)
    {
        CAPTURE (c);
        const auto window = static_cast<size_t> (0.02 * fs);
        CHECK (argMaxAbs (out, c + latency - window / 2, c + latency + window) == c + latency);
    }
}

TEST_CASE ("Transpose output does not depend on the block size", "[transpose]")
{
    const double fs = 48000.0;
    auto in = tp::render (tp::scores::chord(), fs);
    in.resize (static_cast<size_t> (1.2 * fs));
    const auto hits = tp::render (tp::scores::hits(), fs);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] += hits[i];
    // A change of direction, off, on again, and a change during that fade.
    const std::vector<Change> changes { { static_cast<size_t> (0.40 * fs), 4 },
                                        { static_cast<size_t> (0.70 * fs), 0 },
                                        { static_cast<size_t> (0.85 * fs), -12 },
                                        { static_cast<size_t> (0.86 * fs) + 5, 7 } };
    const auto reference = shift (in, fs, -3, 64, changes);
    REQUIRE (test::allFinite (reference));
    for (int block : { 1, 7, 333, 4096 })
    {
        CAPTURE (block);
        CHECK (shift (in, fs, -3, block, changes) == reference);
    }
}

TEST_CASE ("Transpose setting changes are click-free", "[transpose]")
{
    // A ringing chord while the setting jumps every 40 ms (seeded). Every change crossfades, so
    // no output step is steeper than the shifted chord's own: the input's steepest, doubled for
    // the octave up, times the equal-power fade's sqrt 2, times 2 for headroom.
    const double fs = 48000.0;
    auto in = tp::render (tp::scores::chord(), fs);
    in.resize (static_cast<size_t> (1.5 * fs));
    Random rng (17);
    std::vector<Change> changes;
    for (auto at = static_cast<size_t> (0.3 * fs); at < in.size();
         at += static_cast<size_t> (0.04 * fs))
        changes.push_back ({ at, static_cast<int> (rng.unipolar() * 25.0f) - 12 });
    const auto out = shift (in, fs, 0, 64, changes);
    REQUIRE (test::allFinite (out));

    float inSlope = 0.0f, outSlope = 0.0f;
    for (size_t i = 1; i < in.size(); ++i)
    {
        inSlope = std::max (inSlope, std::abs (in[i] - in[i - 1]));
        outSlope = std::max (outSlope, std::abs (out[i] - out[i - 1]));
    }
    CAPTURE (inSlope, outSlope);
    CHECK (outSlope <= 2.0f * std::sqrt (2.0f) * 2.0f * inSlope);
    CHECK (test::peak (out) <= 2.0f * test::peak (in));
}

TEST_CASE ("Transpose output is finite for random settings and full-scale noise", "[transpose]")
{
    const double fs = GENERATE (44100.0, 96000.0);
    const int block = GENERATE (1, 512);
    CAPTURE (fs, block);
    std::vector<float> in (static_cast<size_t> (0.3 * fs));
    test::fillNoise (in, 1.0f, 31);
    Random rng (static_cast<std::uint32_t> (fs) + static_cast<std::uint32_t> (block));
    std::vector<Change> changes;
    for (size_t at = 0; at < in.size(); at += static_cast<size_t> (0.03 * fs))
        changes.push_back ({ at, static_cast<int> (rng.unipolar() * 25.0f) - 12 });
    const auto out = shift (in, fs, 0, block, changes);
    CHECK (test::allFinite (out));
    CHECK (test::peak (out) < 3.0f);
}

TEST_CASE ("Transpose keeps a note in tune at a few steps", "[transpose]")
{
    // The full sweep, chords included, is [long] below; this subset runs in every build.
    const double fs = 48000.0;
    const int st = GENERATE (-5, 7);
    CAPTURE (st);
    const double ratio = std::exp2 (st / 12.0);
    const auto in = tp::render (sustainedNote (110.0, 5), fs);
    const auto e = pitchError (in, shift (in, fs, st, 64), fs, ratio, 3000.0);
    CAPTURE (e.partials, e.worstCents);
    CHECK (e.partials >= 4);
    CHECK (e.worstCents <= 2.0);
}

TEST_CASE ("Transpose keeps notes and chords in tune at every step", "[transpose][long]")
{
    const double fs = 48000.0;
    for (int st = -12; st <= 12; ++st)
    {
        if (st == 0)
            continue;
        CAPTURE (st);
        const double ratio = std::exp2 (st / 12.0);
        const double maxHz = std::min (3000.0, 3000.0 / ratio);
        int which = 0;
        for (const auto& score : { sustainedNote (tp::scores::kE2, 1), sustainedNote (196.0, 2),
                                   sustainedNote (329.63, 3), tp::scores::chord() })
        {
            CAPTURE (which++);
            const auto in = tp::render (score, fs);
            const auto e = pitchError (in, shift (in, fs, st, 64), fs, ratio, maxHz);
            CAPTURE (e.partials, e.worstCents);
            CHECK (e.partials >= 4);
            CHECK (e.worstCents <= 2.0);
        }
    }
}

TEST_CASE ("Transpose clears the bars it is held to", "[transpose][long]")
{
    // Appendix A's bars at the shifts the two jobs use. The bars this design misses
    // (docs/dsp/transpose.md, section 10) are held at what it measures (48 kHz, with about 10 %
    // for other compilers' rounding), so they cannot get worse unnoticed.
    struct Expect
    {
        int st;
        double purityDb, flamDb, attackDb, riseMs, pitchP95, latencyRangeMs, preEchoDb;
    };
    const auto x = GENERATE (Expect { -1, -31.0, 2.0, 1.0, 3.0, 8.0, 0.5, -30.0 },
                             Expect { -2, -28.5, 3.5, 1.6, 1.0, 10.5, 0.5, -30.0 },
                             Expect { -3, -28.0, 5.0, 2.0, 5.0, 12.0, 0.7, -30.0 },
                             Expect { -5, -27.0, 6.5, 2.5, 5.0, 14.0, 0.5, -30.0 },
                             Expect { -12, -26.5, 10.5, 5.0, 1.5, 24.0, 4.0, -29.0 },
                             Expect { 12, -20.0, 7.5, 3.0, 1.5, 19.0, 2.5, -30.0 });
    CAPTURE (x.st);
    PitchShifter p;
    auto s = engineShifter (p);
    const auto e = tp::evaluate (s, 48000.0, x.st, false);
    const auto bars = tp::barsFor (x.st);
    std::string missed;
    for (const auto& m : tp::missedBars (e))
        missed += m + "; ";
    CAPTURE (missed);

    // Latency: at most 10 ms, the same for every attack, and what the host is told.
    const double latMean = 0.5 * (e.hits.latencyMeanMs + e.riff.latencyMeanMs);
    double lo = 1.0e9, hi = -1.0e9;
    for (const auto* t : { &e.hits, &e.riff })
        for (double l : t->latencyMs)
        {
            lo = std::min (lo, l);
            hi = std::max (hi, l);
        }
    CAPTURE (latMean, e.reportedMs, hi - lo);
    CHECK (latMean <= bars.latencyMs);
    CHECK (hi - lo <= x.latencyRangeMs);
    CHECK (std::abs (latMean - e.reportedMs) <=
           std::max (bars.reportErrorMs, 0.15 * x.latencyRangeMs));
    // Sustain: timbre and both warbles clear their bars; purity is held where measured.
    CAPTURE (e.chord.purityDb, e.chord.timbreDb, e.warble.partialDb, e.warble.envelopeDb);
    CHECK (e.chord.timbreDb <= bars.timbreDb);
    CHECK (e.warble.partialDb <= bars.partialWarbleDb);
    CHECK (e.warble.envelopeDb <= bars.envelopeWarbleDb);
    CHECK (e.chord.purityDb <= x.purityDb);
    // Attacks.
    CAPTURE (e.hits.preEchoDb, e.hits.flamDb, e.riff.flamDb, e.hits.attackDb, e.riff.attackDb,
             e.hits.riseMs, e.riff.riseMs);
    CHECK (e.hits.preEchoDb <= x.preEchoDb);
    CHECK (std::max (e.hits.flamDb, e.riff.flamDb) <= x.flamDb);
    CHECK (std::max (e.hits.attackDb, e.riff.attackDb) <= x.attackDb);
    CHECK (std::max (e.hits.riseMs, e.riff.riseMs) <= x.riseMs);
    // Pitch: the median clears its bar everywhere; the 95th percentile (the bend) is held.
    CAPTURE (e.pitch.medianCents, e.pitch.p95Cents, e.aliasDb);
    CHECK (e.pitch.medianCents <= bars.pitchMedianCents);
    CHECK (e.pitch.p95Cents <= x.pitchP95);
    if (x.st == 12)
        CHECK (e.aliasDb <= bars.aliasDb);
}

TEST_CASE ("Transpose behaves the same at every sample rate", "[transpose][long]")
{
    const double fs = GENERATE (44100.0, 88200.0, 96000.0);
    const int st = GENERATE (-2, 12);
    CAPTURE (fs, st);
    PitchShifter p;
    auto s = engineShifter (p);
    const auto e = tp::evaluate (s, fs, st, false);
    const double latMean = 0.5 * (e.hits.latencyMeanMs + e.riff.latencyMeanMs);
    const double want = st < 0 ? 3.0 : 10.0;
    CAPTURE (latMean, e.chord.purityDb, e.pitch.medianCents, e.warble.envelopeDb);
    CHECK (std::abs (latMean - want) <= (st < 0 ? 0.1 : 0.3));
    CHECK (e.reportedMs == Catch::Approx (want).margin (500.0 / fs));
    CHECK (e.chord.purityDb <= (st < 0 ? -28.0 : -20.0));
    CHECK (e.pitch.medianCents <= 2.0);
    CHECK (e.warble.envelopeDb <= tp::barsFor (st).envelopeWarbleDb);

    const double ratio = std::exp2 (st / 12.0);
    const auto in = tp::render (sustainedNote (110.0, 7), fs);
    const auto pe =
        pitchError (in, shift (in, fs, st, 64), fs, ratio, std::min (3000.0, 3000.0 / ratio));
    CAPTURE (pe.partials, pe.worstCents);
    CHECK (pe.worstCents <= 2.0);
}
