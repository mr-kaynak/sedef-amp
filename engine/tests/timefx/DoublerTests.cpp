// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimeFxTestUtils.h"
#include "sedef/Engine.h"
#include "sedef/core/BypassFader.h"
#include "sedef/dsp/Doubler.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <limits>
#include <numeric>

using namespace sedef;
using namespace sedef::test;
using Catch::Approx;
using sedef::dsp::Doubler;

namespace
{
constexpr double kFs = 48000.0;

void run (Doubler& d, Buffer& b, int blockSize, const std::vector<float>* source = nullptr,
          int lead = 0)
{
    forEachBlock (b.view(), blockSize, [&] (AudioView v, int start) {
        if (source != nullptr)
            d.setOnsetSource (source->data() + start, lead);
        d.process (v);
    });
}

double correlation (const std::vector<float>& a, const std::vector<float>& b, size_t from,
                    size_t to, long lag)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        const double x = a[i];
        const double y = b[static_cast<size_t> (static_cast<long> (i) + lag)];
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }
    return ab / std::sqrt (aa * bb + 1e-30);
}

/// Mono white noise in both channels.
Buffer monoNoise (int frames, float amp = 0.5f)
{
    Buffer b (2, frames);
    fillNoise (b[0], amp, 13);
    b[1] = b[0];
    return b;
}

/// Noise "notes": bursts with a 1 ms attack and an 80 ms decay, 140..460 ms apart. With
/// `gapped`, each burst lasts 80 ms and is followed by 100 ms or more of silence.
std::vector<float> noteBursts (double fs, double seconds, std::vector<size_t>& onsets,
                               bool gapped = false, std::uint32_t seed = 5)
{
    Random r (seed);
    std::vector<float> x (static_cast<size_t> (seconds * fs), 0.0f);
    for (double t = 0.2; t < seconds - 0.5;
         t += gapped ? 0.18 + 0.2 * r.unipolar() : 0.14 + 0.32 * r.unipolar())
    {
        const auto i0 = static_cast<size_t> (t * fs);
        onsets.push_back (i0);
        const float amp = 0.3f * (0.7f + 0.3f * r.unipolar());
        const auto len = static_cast<size_t> ((gapped ? 0.08 : 0.6) * fs);
        for (size_t i = i0; i < std::min (x.size(), i0 + len); ++i)
        {
            const double tt = static_cast<double> (i - i0) / fs;
            const double env = std::min (1.0, tt / 1e-3) * std::exp (-tt / 0.08);
            x[i] += amp * static_cast<float> (env) * r.bipolar();
        }
    }
    return x;
}

/// Offset (ms) at which the note starting at `onset` on the left reappears on the right: the
/// lag (0..42 ms) of the best correlation over the note's first 30 ms.
double noteOffsetMs (const std::vector<float>& l, const std::vector<float>& r, size_t onset,
                     double fs)
{
    const auto win = static_cast<size_t> (0.03 * fs);
    const auto maxLag = static_cast<long> (0.042 * fs);
    long best = 0;
    double bestValue = -1.0;
    for (long lag = 0; lag < maxLag; ++lag)
    {
        double acc = 0.0;
        for (size_t i = onset; i < onset + win; ++i)
            acc += static_cast<double> (l[i]) * r[i + static_cast<size_t> (lag)];
        if (acc > bestValue)
        {
            bestValue = acc;
            best = lag;
        }
    }
    return static_cast<double> (best) / fs * 1.0e3;
}

struct OffsetStats
{
    double median = 0.0, sd = 0.0, lo = 0.0, hi = 0.0, meanStep = 0.0, lag1 = 0.0;
};

OffsetStats offsetStats (std::vector<double> v)
{
    OffsetStats s;
    const double mean = std::accumulate (v.begin(), v.end(), 0.0) / static_cast<double> (v.size());
    double var = 0.0, num = 0.0, steps = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        var += (v[i] - mean) * (v[i] - mean);
        if (i > 0)
        {
            num += (v[i] - mean) * (v[i - 1] - mean);
            steps += std::abs (v[i] - v[i - 1]);
        }
    }
    s.sd = std::sqrt (var / static_cast<double> (v.size() - 1));
    s.lag1 = var > 0.0 ? num / var : 0.0;
    s.meanStep = steps / static_cast<double> (v.size() - 1);
    std::sort (v.begin(), v.end());
    s.median = v[v.size() / 2];
    s.lo = v.front();
    s.hi = v.back();
    return s;
}

/// Per-note offsets of the double over a phrase of noise notes.
std::vector<double> phraseOffsets (const Doubler::Settings& settings, double fs, int* retimed)
{
    std::vector<size_t> onsets;
    const auto x = noteBursts (fs, 16.0, onsets);
    Buffer b (2, static_cast<int> (x.size()));
    b[0] = x;
    b[1] = x;
    Doubler d;
    d.prepare ({ fs, 512, 2 });
    d.setSettings (settings);
    run (d, b, 64);
    if (retimed != nullptr)
        *retimed = d.retimedNotes();
    std::vector<double> offsets;
    for (size_t k = 0; k + 1 < onsets.size(); ++k)
        offsets.push_back (noteOffsetMs (b[0], b[1], onsets[k], fs));
    return offsets;
}

/// Short bright "pick" clicks every 150..400 ms: an onset source with nothing audible in the
/// signal being doubled.
std::vector<float> pickClicks (double fs, int frames, std::vector<size_t>* at = nullptr)
{
    std::vector<float> s (static_cast<size_t> (frames), 0.0f);
    Random r (21);
    for (double t = 0.1; t < frames / fs - 0.05; t += 0.15 + 0.25 * r.unipolar())
    {
        const auto i0 = static_cast<size_t> (t * fs);
        if (at != nullptr)
            at->push_back (i0);
        for (size_t i = i0; i < i0 + static_cast<size_t> (0.004 * fs); ++i)
            s[i] = 0.4f * r.bipolar();
    }
    return s;
}

/// Energy above 3 kHz (8th-order Butterworth high-pass) against the signal's total, in dB. A
/// click on a low tone shows up as broadband energy; smooth glides and fades leave almost none.
double hfShareDb (const std::vector<float>& v, size_t start)
{
    std::array<Biquad, 4> hp;
    const double qs[] = { 0.5098, 0.6013, 0.9000, 2.5629 };
    for (size_t k = 0; k < 4; ++k)
        hp[k].setCoeffs (design::highpass (kFs, 3000.0, qs[k]));
    double high = 0.0, all = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        float y = v[i];
        for (auto& f : hp)
            y = f.processSample (0, y);
        if (i >= start)
        {
            high += static_cast<double> (y) * y;
            all += static_cast<double> (v[i]) * v[i];
        }
    }
    return 10.0 * std::log10 (high / all + 1e-30);
}

/// Frequency (Hz) a window of a sine implies, from interpolated upward zero crossings.
double zeroCrossingHz (const std::vector<float>& v, int start, int length, double fs)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (int i = start + 1; i < start + length; ++i)
    {
        const float p = v[static_cast<size_t> (i - 1)], q = v[static_cast<size_t> (i)];
        if (p < 0.0f && q >= 0.0f)
        {
            const double t = (i - 1) + static_cast<double> (p / (p - q));
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
    }
    return (crossings - 1) * fs / (last - first);
}
} // namespace

TEST_CASE ("Doubler output is finite for random settings and full-scale noise", "[timefx][doubler]")
{
    const double fs = GENERATE (from_range (kTestRates));
    const int blockSize = GENERATE (from_range (kTestBlockSizes));
    Doubler d;
    d.prepare ({ fs, 512, 2 });
    Random rng (4);
    Buffer b (2, static_cast<int> (fs * 0.5));
    fillNoise (b[0], 1.0f, 1);
    fillNoise (b[1], 1.0f, 2);
    int sinceChange = 1 << 20;
    forEachBlock (b.view(), blockSize, [&] (AudioView v, int) {
        if ((sinceChange += v.numFrames()) >= 1000)
        {
            // Includes out-of-range values.
            d.setSettings (
                { mapLin (rng.unipolar(), 0.0f, 25.0f), mapLin (rng.unipolar(), -0.5f, 1.5f),
                  mapLin (rng.unipolar(), -0.5f, 1.5f), mapLin (rng.unipolar(), -0.5f, 1.5f) });
            sinceChange = 0;
        }
        d.process (v);
    });
    CHECK (allFinite (b[0]));
    CHECK (allFinite (b[1]));
    CHECK (peak (b[0]) < 1.5f);
    CHECK (peak (b[1]) < 1.5f);

    // Non-finite settings fall back to the defaults.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    d.setSettings ({ nan, nan, nan, nan });
    fillNoise (b[0], 1.0f, 3);
    run (d, b, blockSize);
    CHECK (allFinite (b[0]));
    CHECK (allFinite (b[1]));
}

TEST_CASE ("Doubler decorrelates a mono source", "[timefx][doubler]")
{
    const float spread = GENERATE (3.0f, 6.0f, 20.0f);
    Doubler d;
    d.prepare ({ kFs, 512, 2 });
    d.setSettings ({ spread });
    Buffer b = monoNoise (static_cast<int> (kFs * 1.5));
    run (d, b, 64);
    INFO ("spread " << spread);
    CHECK (std::abs (correlation (b[0], b[1], 24000, 72000, 0)) < 0.1);
}

TEST_CASE ("Doubler spread sets the offset between the sides", "[timefx][doubler]")
{
    for (float spread : { 4.0f, 10.0f, 18.0f })
    {
        // Variation 0: one fixed offset, so the whole phrase correlates at one lag.
        Doubler d;
        d.prepare ({ kFs, 512, 2 });
        d.setSettings ({ spread, 0.0f });
        Buffer b = monoNoise (static_cast<int> (kFs * 2.0));
        run (d, b, 128);

        long bestLag = 0;
        double best = -1.0, floor = 0.0;
        const long maxLag = static_cast<long> (0.025 * kFs);
        for (long lag = 0; lag < maxLag; ++lag)
        {
            const double c = correlation (b[0], b[1], 24000, 48000, lag);
            floor += std::abs (c) / static_cast<double> (maxLag);
            if (c > best)
            {
                best = c;
                bestLag = lag;
            }
        }
        INFO ("spread " << spread << " measured lag " << bestLag << " peak " << best);
        CHECK (std::abs (static_cast<double> (bestLag) - spread * 1.0e-3 * kFs) < 0.5e-3 * kFs);
        CHECK (best > 10.0 * floor);
        CHECK (d.retimedNotes() == 0);
    }
}

TEST_CASE ("Doubler leaves the played part untouched", "[timefx][doubler]")
{
    // Zero latency: at full width the left side is the input, bit for bit, whatever the other
    // settings; with the mix at 0 both sides are.
    const int blockSize = GENERATE (1, 64, 512);
    for (float mix : { 0.0f, 0.4f, 1.0f })
    {
        Doubler d;
        d.prepare ({ kFs, 512, 2 });
        d.setSettings ({ 9.0f, 1.0f, 1.0f, mix });
        std::vector<size_t> onsets;
        Buffer b (2, static_cast<int> (kFs * 3.0));
        b[0] = noteBursts (kFs, 3.0, onsets);
        fillNoise (b[1], 0.2f, 8);
        const auto dryL = b[0];
        const auto dryR = b[1];
        run (d, b, blockSize);
        INFO ("mix " << mix << " block " << blockSize);
        CHECK (b[0] == dryL);
        if (mix == 0.0f)
            CHECK (b[1] == dryR);
        else
            CHECK (b[1] != dryR);
    }
}

TEST_CASE ("Doubler re-times the double at every note", "[timefx][doubler]")
{
    // At each onset the double draws a new offset, spread * 2^(variation * z), with z partly
    // carried over from the notes before. Measured from the audio, note by note.
    int retimed = 0;
    const auto tight = offsetStats (phraseOffsets ({ 8.0f, 0.0f }, kFs, &retimed));
    INFO ("variation 0: median " << tight.median << " sd " << tight.sd);
    CHECK (retimed == 0);
    CHECK (tight.sd < 0.1);
    CHECK (tight.median == Approx (8.0).margin (0.1));

    const auto offsets = phraseOffsets ({ 8.0f, 0.5f }, kFs, &retimed);
    const auto s = offsetStats (offsets);
    INFO ("variation 0.5: notes " << offsets.size() << " retimed " << retimed << " median "
                                  << s.median << " sd " << s.sd << " range " << s.lo << ".." << s.hi
                                  << " mean step " << s.meanStep << " lag1 " << s.lag1);
    CHECK (retimed >= static_cast<int> (0.9 * static_cast<double> (offsets.size())));
    CHECK (s.median == Approx (8.0).epsilon (0.2));
    CHECK (s.sd > 1.5);
    CHECK (s.lo >= 2.4);
    CHECK (s.hi <= 24.0); // z is clamped to +-2.5
    CHECK (s.meanStep > 1.0);
    CHECK (s.lag1 > 0.2); // a take that runs late tends to stay late for a few notes

    const auto loose = offsetStats (phraseOffsets ({ 8.0f, 1.0f }, kFs, nullptr));
    INFO ("variation 1: sd " << loose.sd << " range " << loose.lo << ".." << loose.hi);
    CHECK (loose.sd > 1.6 * s.sd);
    CHECK (loose.lo >= 1.7); // the 2.5 ms floor, less the detune's drift
    CHECK (loose.hi <= 40.5);
}

TEST_CASE ("Doubler offsets are the same in milliseconds at every rate", "[timefx][doubler]")
{
    const auto reference = offsetStats (phraseOffsets ({ 10.0f, 0.5f }, 48000.0, nullptr));
    for (double fs : { 44100.0, 96000.0, 192000.0 })
    {
        int retimed = 0;
        const auto s = offsetStats (phraseOffsets ({ 10.0f, 0.5f }, fs, &retimed));
        INFO ("fs " << fs << " median " << s.median << " sd " << s.sd);
        CHECK (retimed > 20);
        CHECK (s.median == Approx (reference.median).epsilon (0.15));
        CHECK (s.sd == Approx (reference.sd).epsilon (0.35));
    }
}

TEST_CASE ("Doubler: each note arrives once, at its own offset", "[timefx][doubler]")
{
    // Notes after silence. If a re-timing came too late, part of the note would come out of the
    // old read position first; the right side must stay silent until the note's own arrival.
    struct Case
    {
        float spread;
        int lead; // samples by which the onset source runs ahead of the input
    };
    for (const auto c :
         { Case { 3.0f, 0 }, Case { 6.0f, 0 }, Case { 15.0f, 0 }, Case { 3.0f, 110 } })
    {
        std::vector<size_t> onsets;
        const auto x = noteBursts (kFs, 10.0, onsets, true);
        Buffer b (2, static_cast<int> (x.size()));
        for (size_t i = 0; i < x.size(); ++i)
        {
            const auto j = static_cast<long> (i) - c.lead;
            b[0][i] = b[1][i] = j >= 0 ? x[static_cast<size_t> (j)] : 0.0f;
        }
        Doubler d;
        d.prepare ({ kFs, 512, 2 });
        d.setSettings ({ c.spread, 1.0f });
        run (d, b, 64, c.lead > 0 ? &x : nullptr, c.lead);

        int clean = 0, measured = 0;
        double worstLeak = 0.0;
        for (size_t k = 0; k + 1 < onsets.size(); ++k)
        {
            const size_t on = onsets[k] + static_cast<size_t> (c.lead);
            const double offset = noteOffsetMs (b[0], b[1], on, kFs);
            const auto arrival = on + static_cast<size_t> (offset * 1.0e-3 * kFs);
            // Energy on the right before the arrival (less a 0.3 ms interpolation skirt), against
            // the note's first 10 ms there.
            double before = 0.0, note = 0.0;
            for (size_t i = on; i + static_cast<size_t> (0.0003 * kFs) < arrival; ++i)
                before += static_cast<double> (b[1][i]) * b[1][i];
            for (size_t i = arrival; i < arrival + static_cast<size_t> (0.01 * kFs); ++i)
                note += static_cast<double> (b[1][i]) * b[1][i];
            const double leak = before / (note + 1e-30);
            worstLeak = std::max (worstLeak, leak);
            clean += leak < 1.0e-6 ? 1 : 0;
            ++measured;
        }
        INFO ("spread " << c.spread << " lead " << c.lead << " notes " << measured << " retimed "
                        << d.retimedNotes() << " worst leak "
                        << 10.0 * std::log10 (worstLeak + 1e-30) << " dB");
        CHECK (clean == measured);
        CHECK (d.retimedNotes() >= measured / 2);
    }
}

TEST_CASE ("Doubler times notes from the onset source", "[timefx][doubler]")
{
    // A held tone has no onsets of its own; the source's clicks still re-time the double.
    Buffer b (2, static_cast<int> (kFs * 4.0));
    fillSine (b[0], 220.0, kFs, 0.3f);
    b[1] = b[0];
    const auto source = pickClicks (kFs, b.frames());
    Doubler a, withSource;
    for (auto* d : { &a, &withSource })
    {
        d->prepare ({ kFs, 512, 2 });
        d->setSettings ({ 8.0f, 0.5f });
    }
    Buffer copy = b;
    run (a, b, 64);
    run (withSource, copy, 64, &source, 0);
    CHECK (a.retimedNotes() <= 1); // the tone's own start
    CHECK (withSource.retimedNotes() > 8);
}

TEST_CASE ("Doubler keeps the level of each side", "[timefx][doubler]")
{
    Doubler d;
    d.prepare ({ kFs, 512, 2 });
    d.setSettings ({ 8.0f, 0.0f });
    Buffer b (2, static_cast<int> (kFs));
    fillSine (b[0], 440.0, kFs, 0.5f);
    b[1] = b[0];
    const float in = rms (b[0], 4800);
    run (d, b, 64);
    CHECK (20.0f * std::log10 (rms (b[0], 4800) / in) == Approx (0.0f).margin (0.01f));
    // The double is lifted 1 dB and softened above ~7.5 kHz.
    CHECK (20.0f * std::log10 (rms (b[1], 4800) / in) == Approx (1.0f).margin (0.3f));
}

TEST_CASE ("Doubler varies each note's level a little", "[timefx][doubler]")
{
    // A held 440 Hz tone re-timed by clicks: between re-timings the double holds the note's
    // level, +1 dB +-(1.5 dB * variation), plus the tone tilt's small share at 440 Hz.
    Buffer b (2, static_cast<int> (kFs * 6.0));
    fillSine (b[0], 440.0, kFs, 0.3f);
    b[1] = b[0];
    std::vector<size_t> clicks;
    const auto source = pickClicks (kFs, b.frames(), &clicks);
    Doubler d;
    d.prepare ({ kFs, 512, 2 });
    d.setSettings ({ 6.0f, 1.0f });
    run (d, b, 64, &source, 0);
    double lo = 1e9, hi = -1e9;
    for (size_t k = 0; k + 1 < clicks.size(); ++k)
    {
        const auto from = clicks[k] + static_cast<size_t> (0.015 * kFs);
        const auto length = static_cast<int> (clicks[k + 1] - from);
        if (length < 2400)
            continue;
        const double db =
            20.0 * std::log10 (rms (b[1].data() + from, length) / (0.3 / std::sqrt (2.0)));
        lo = std::min (lo, db);
        hi = std::max (hi, db);
    }
    INFO ("per-note level " << lo << ".." << hi << " dB");
    CHECK (lo > 1.0 - 1.5 - 0.6);
    CHECK (hi < 1.0 + 1.5 + 0.6);
    CHECK (hi - lo > 0.5); // it does vary
}

TEST_CASE ("Doubler adds only a few cents of pitch difference", "[timefx][doubler]")
{
    // A held 1 kHz tone re-timed by clicks: per note the double holds a detune of up to
    // +-(6 cents * variation), plus about a cent of slow wander. Windows that contain a re-timing
    // are skipped (the phase steps there are not pitch).
    for (float variation : { 0.5f, 1.0f })
    {
        Doubler d;
        d.prepare ({ kFs, 512, 2 });
        d.setSettings ({ 12.0f, variation });
        Buffer b (2, static_cast<int> (kFs * 8.0));
        fillSine (b[0], 1000.0, kFs, 0.5f);
        b[1] = b[0];
        std::vector<size_t> clicks;
        const auto source = pickClicks (kFs, b.frames(), &clicks);
        run (d, b, 64, &source, 0);
        const int window = 2400;
        double worstCents = 0.0, meanAbs = 0.0;
        int windows = 0;
        for (int start = 4800; start + window < b.frames(); start += window / 2)
        {
            const bool retime = std::any_of (clicks.begin(), clicks.end(), [&] (size_t c) {
                return static_cast<int> (c) + 2400 > start && static_cast<int> (c) < start + window;
            });
            if (retime)
                continue;
            const double cents =
                1200.0 * std::log2 (zeroCrossingHz (b[1], start, window, kFs) / 1000.0);
            worstCents = std::max (worstCents, std::abs (cents));
            meanAbs += std::abs (cents);
            ++windows;
        }
        meanAbs /= windows;
        INFO ("variation " << variation << " windows " << windows << " worst " << worstCents
                           << " mean |cents| " << meanAbs);
        CHECK (windows > 20);
        CHECK (worstCents < 6.0 * variation + 2.0);
        CHECK (meanAbs > 0.25 * variation); // it does differ
    }
}

TEST_CASE ("Doubler: a long note's detune keeps the offset near its draw", "[timefx][doubler]")
{
    // One onset, then 8 s of steady noise: the detune drifts the read position, and turns round
    // before the drift leaves clamp(0.3 * offset, 0.5 ms, 3 ms) (plus the ~0.2 ms wander).
    for (float spread : { 3.0f, 15.0f })
    {
        Buffer b (2, static_cast<int> (kFs * 8.0));
        fillNoise (b[0], 0.3f, 31);
        b[1] = b[0];
        std::vector<float> source (b[0].size(), 0.0f);
        for (size_t i = 4800; i < 4800 + 192; ++i)
            source[i] = 0.5f; // one "pick" at 0.1 s
        Doubler d;
        d.prepare ({ kFs, 512, 2 });
        d.setSettings ({ spread, 1.0f });
        run (d, b, 64, &source, 0);
        double lo = 1e9, hi = -1e9;
        for (size_t t = 24000; t + 4000 < b[0].size(); t += 12000)
        {
            const double lag = noteOffsetMs (b[0], b[1], t, kFs);
            lo = std::min (lo, lag);
            hi = std::max (hi, lag);
        }
        const double limit = std::clamp (0.3 * 0.5 * (lo + hi), 0.5, 3.0);
        INFO ("spread " << spread << " retimed " << d.retimedNotes() << " offset " << lo << ".."
                        << hi << " ms, drift limit ~" << limit << " ms");
        CHECK (d.retimedNotes() >= 1);
        CHECK (hi - lo > 0.3);               // the detune does move it
        CHECK (hi - lo < 2.0 * limit + 0.5); // but not away
        CHECK (lo > 1.5);
    }
}

TEST_CASE ("Doubler re-timing is click-free on a held tone", "[timefx][doubler]")
{
    // The worst case for a crossfade between two read positions: a pure tone held across it, with
    // no new attack to cover it. A 220 Hz sine at 0.5 has a second difference of ~4.1e-4 and a
    // step of 1 % of its amplitude would show as 5e-3; the crossfades may bend the waveform a
    // little, but leave no broadband energy (a click would put it at -40 dB or more).
    const int blockSize = GENERATE (1, 64, 512);
    for (float spread : { 3.0f, 20.0f })
    {
        Doubler d;
        d.prepare ({ kFs, 512, 2 });
        d.setSettings ({ spread, 1.0f });
        Buffer b (2, static_cast<int> (kFs * 4.0));
        fillSine (b[0], 220.0, kFs, 0.5f);
        b[1] = b[0];
        const auto source = pickClicks (kFs, b.frames());
        run (d, b, blockSize, &source, 0);
        INFO ("spread " << spread << " block " << blockSize << " retimed " << d.retimedNotes()
                        << " max second difference " << maxSecondDifference (b[1], 4800)
                        << " energy above 3 kHz " << hfShareDb (b[1], 4800) << " dB");
        CHECK (d.retimedNotes() > 8);
        CHECK (maxSecondDifference (b[1], 4800) < 2.0e-3f);
        CHECK (hfShareDb (b[1], 4800) < -80.0);
    }
}

TEST_CASE ("Doubler switched on mid-note does not step", "[timefx][doubler]")
{
    // The engine's slot resets the Doubler when it comes out of bypass and fades it in over
    // 10 ms, so the delay line restarts from silence under a note that is already sounding. The
    // take starts as the undelayed input and fades over once the line has filled; reading the
    // empty line would step by the full tone, half-way through the slot's fade.
    for (float spread : { 3.0f, 20.0f })
    {
        Doubler d;
        d.prepare ({ kFs, 512, 2 });
        BypassFader fader;
        fader.prepare (kFs);
        fader.snap (false);
        Buffer b (2, static_cast<int> (kFs * 2.0));
        fillSine (b[0], 220.0, kFs, 0.5f);
        b[1] = b[0];
        Buffer dry (2, 64);
        forEachBlock (b.view(), 64, [&] (AudioView v, int start) {
            if (start == 48000) // switched on after a second, the way StereoSlot does it
            {
                fader.setEnabled (true);
                d.setSettings ({ spread, 1.0f });
                d.reset();
                d.setSettings ({ spread, 1.0f });
            }
            if (fader.isFullyBypassed())
                return;
            const AudioView dryView = dry.view().subView (0, v.numFrames());
            dryView.copyFrom (v);
            d.process (v);
            if (!fader.isFullyActive())
                fader.mix (dryView, v);
        });
        // From 50 ms in: the sine's own start (and the high-pass settling on it) is not the test.
        INFO ("spread " << spread << " max second difference " << maxSecondDifference (b[1], 2)
                        << " energy above 3 kHz " << hfShareDb (b[1], 2400) << " dB");
        CHECK (maxSecondDifference (b[1], 2) < 2.0e-3f);
        CHECK (hfShareDb (b[1], 2400) < -80.0);
    }
}

TEST_CASE ("Doubler automation glides without clicks", "[timefx][doubler]")
{
    const int blockSize = GENERATE (1, 64, 512);
    Doubler d;
    d.prepare ({ kFs, 512, 2 });
    d.setSettings ({ 3.0f });
    Buffer b (2, static_cast<int> (kFs * 3.0));
    fillSine (b[0], 220.0, kFs, 0.5f);
    b[1] = b[0];
    const auto source = pickClicks (kFs, b.frames());
    int sinceChange = 0, step = 0;
    forEachBlock (b.view(), blockSize, [&] (AudioView v, int start) {
        if ((sinceChange += v.numFrames()) >= 4800)
        {
            // Every control jumps end to end every 100 ms while notes keep re-timing the double.
            ++step;
            const bool odd = step % 2 == 1;
            d.setSettings ({ odd ? 20.0f : 3.0f, step % 3 == 0 ? 1.0f : 0.0f, odd ? 0.0f : 1.0f,
                             step % 4 < 2 ? 1.0f : 0.0f });
            sinceChange = 0;
        }
        d.setOnsetSource (source.data() + start, 0);
        d.process (v);
    });
    // 220 Hz at 0.5 has a second difference of ~4.1e-4; the spread glide bends pitch by under a
    // semitone and the width and mix ramps take 30 ms. Skip the start, where the delayed sine
    // begins with a kink of its own.
    INFO ("max second difference " << maxSecondDifference (b[0], 2400) << " / "
                                   << maxSecondDifference (b[1], 2400) << ", energy above 3 kHz "
                                   << hfShareDb (b[0], 2400) << " / " << hfShareDb (b[1], 2400)
                                   << " dB");
    CHECK (maxSecondDifference (b[0], 2400) < 2.0e-3f);
    CHECK (maxSecondDifference (b[1], 2400) < 2.0e-3f);
    CHECK (hfShareDb (b[0], 2400) < -80.0);
    CHECK (hfShareDb (b[1], 2400) < -80.0);
}

TEST_CASE ("Doubler width pans the two takes", "[timefx][doubler]")
{
    std::vector<size_t> onsets;
    const auto x = noteBursts (kFs, 2.0, onsets);
    for (float width : { 0.0f, 0.5f })
    {
        Doubler d;
        d.prepare ({ kFs, 512, 2 });
        d.setSettings ({ 8.0f, 0.5f, width });
        Buffer b (2, static_cast<int> (x.size()));
        b[0] = x;
        b[1] = x;
        run (d, b, 64);
        float worstDifference = 0.0f;
        for (size_t i = 0; i < x.size(); ++i)
            worstDifference = std::max (worstDifference, std::abs (b[0][i] - b[1][i]));
        const double c = correlation (b[0], b[1], 4800, x.size() - 4800, 0);
        INFO ("width " << width << " correlation " << c << " worst L-R " << worstDifference);
        if (width == 0.0f)
            CHECK (worstDifference < 1.0e-6f); // both takes in the centre: a mono pair
        else
            CHECK ((c > 0.3 && c < 0.95));
    }
}

TEST_CASE ("Doubler moving offsets keep the mono sum's comb shallow", "[timefx][doubler]")
{
    // In mono the two takes comb-filter. With a fixed 8 ms offset the notches sit at
    // 62.5 * (2k + 1) Hz; with per-note offsets they move from note to note, so over a phrase
    // those frequencies lose little against the comb's peaks at 125 * k Hz.
    std::vector<size_t> onsets;
    const auto x = noteBursts (kFs, 12.0, onsets);
    const auto notchDepthDb = [&] (float variation) {
        Doubler d;
        d.prepare ({ kFs, 512, 2 });
        d.setSettings ({ 8.0f, variation });
        Buffer b (2, static_cast<int> (x.size()));
        b[0] = x;
        b[1] = x;
        run (d, b, 64);
        std::vector<float> mono (x.size());
        for (size_t i = 0; i < x.size(); ++i)
            mono[i] = 0.5f * (b[0][i] + b[1][i]);
        const auto powerDb = [&] (double hz) {
            Biquad bp;
            bp.setCoeffs (design::bandpass (kFs, hz, 40.0));
            double acc = 0.0;
            for (size_t i = 0; i < mono.size(); ++i)
            {
                const float y = bp.processSample (0, mono[i]);
                acc += i > 4800 ? static_cast<double> (y) * y : 0.0;
            }
            return 10.0 * std::log10 (acc + 1e-30);
        };
        double depth = 0.0;
        for (int k = 2; k < 7; ++k)
            depth += powerDb (62.5 * (2 * k + 1)) - powerDb (125.0 * k);
        return depth / 5.0;
    };
    const double fixed = notchDepthDb (0.0), moving = notchDepthDb (0.5);
    INFO ("fixed offset " << fixed << " dB, per-note offsets " << moving << " dB");
    CHECK (fixed < -5.0);
    CHECK (moving > -2.0);
    CHECK (moving - fixed > 5.0);
}

TEST_CASE ("Doubler silence in is silence out; mono passes untouched", "[timefx][doubler]")
{
    Doubler d;
    d.prepare ({ 44100.0, 512, 2 });
    d.setSettings ({ 7.0f });
    Buffer b (2, 6000);
    run (d, b, 17);
    CHECK (peak (b[0]) == 0.0f);
    CHECK (peak (b[1]) == 0.0f);

    Buffer m (1, 3000);
    fillNoise (m[0], 0.5f, 6);
    const auto dry = m.data;
    run (d, m, 64);
    CHECK (m.data == dry);
}

TEST_CASE ("In the engine, the Doubler leaves the played side's timing alone", "[timefx][doubler]")
{
    // The whole rig with the Doubler on: the left output equals the rig without it, sample for
    // sample, so the Doubler adds no latency to what you play.
    std::vector<size_t> onsets;
    const auto x = noteBursts (kFs, 3.0, onsets);
    const auto render = [&] (bool doubler) {
        Engine engine;
        engine.prepare (kFs, 256, 1, 2);
        auto p = ParamSnapshot::defaults();
        p[ParamId::doublerEnabled] = doubler ? 1.0f : 0.0f;
        Buffer out (2, static_cast<int> (x.size()));
        Transport transport;
        for (int s = 0; s < out.frames(); s += 256)
        {
            const int n = std::min (256, out.frames() - s);
            const float* in[1] = { x.data() + s };
            float* o[2] = { out[0].data() + s, out[1].data() + s };
            engine.process (in, o, n, p, transport);
        }
        return out;
    };
    auto off = render (false);
    auto on = render (true);
    CHECK (on[0] == off[0]);
    CHECK (on[1] != off[1]);
}

TEST_CASE ("Doubler benchmark", "[.bench][timefx-bench]")
{
    Doubler d;
    d.prepare ({ 48000.0, 64, 2 });
    d.setSettings ({ 9.0f });
    benchmarkNsPerFrame ("Doubler", 10.0, [&] (AudioView v) { d.process (v); });
}
