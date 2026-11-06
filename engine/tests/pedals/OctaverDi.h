// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Synthetic guitar DI for the octaver: the playing that made the first Trench tracker chop on a
// real guitar (bright pickups, pick noise, ringing strings, vibrato and bends, palm-muted
// sixteenths, legato runs), and the measurements its acceptance bars are written in.

#include "../analysis/PitchTestSignals.h"
#include "PedalTestUtils.h"
#include "sedef/dsp/Trench.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace sedef::test::octaver
{

inline constexpr double kE2 = 82.4069, kA2 = 110.0, kD3 = 146.832, kE3 = 164.814, kA3 = 220.0,
                        kE4 = 329.628, kE5 = 659.255;

/// One note to judge: [start, end) in seconds, its pitch over time given by `pluck` started at
/// `pickAt` (so glides, steps, bends and vibrato are part of the expectation).
struct NoteSpan
{
    double start = 0.0, end = 0.0;
    Pluck pluck;
    double pickAt = 0.0;

    double hzAt (double t) const { return pluck.hzAt (t - pickAt); }
};

struct Scenario
{
    std::string name;
    double fs = 48000.0;
    std::vector<float> signal;
    std::vector<NoteSpan> notes;
};

/// A magnetic bridge pickup, picked near the bridge: the 2nd and 3rd partials stand 5-8 dB above
/// the fundamental.
inline Pluck bridgePluck (double f1, std::uint32_t seed = 1)
{
    Pluck p;
    p.f1 = f1;
    p.pluckPosition = 0.1;
    p.pickupPosition = 0.07;
    p.pickNoise = 0.06f;
    p.seed = seed;
    return p;
}

/// A piezo-like note: a weak fundamental, the 2nd partial 4 dB above it.
inline Pluck piezoPluck (double f1, std::uint32_t seed = 1)
{
    Pluck p;
    p.f1 = f1;
    p.pluckPosition = 0.2;
    p.fundamentalGain = 0.5;
    p.pickNoise = 0.06f;
    p.seed = seed;
    return p;
}

inline Scenario single (std::string name, const Pluck& p, double fs, double seconds = 1.5)
{
    Scenario s {
        std::move (name), fs, std::vector<float> (static_cast<size_t> (seconds * fs)), {}
    };
    const double pick = 0.1;
    addPluck (s.signal, fs, static_cast<size_t> (pick * fs), p);
    s.notes.push_back ({ pick, seconds, p, pick });
    return s;
}

/// The new note is picked while the previous one (a fourth below or a fifth above) still rings,
/// about 6 dB down.
inline Scenario ringingNeighbour (double f1, double neighbourCents, double fs)
{
    Scenario s { "ringing neighbour", fs, std::vector<float> (static_cast<size_t> (1.6 * fs)), {} };
    Pluck old = bridgePluck (f1 * std::exp2 (neighbourCents / 1200.0), 5);
    old.decaySeconds = 3.0;
    old.peak = 0.17f; // 6 dB under the new note when it is picked, 0.4 s later
    addPluck (s.signal, fs, static_cast<size_t> (0.1 * fs), old);
    Pluck now = bridgePluck (f1, 6);
    addPluck (s.signal, fs, static_cast<size_t> (0.5 * fs), now);
    s.notes.push_back ({ 0.5, 1.6, now, 0.5 });
    return s;
}

/// Vibrato of +-30 cents at 5.5 Hz from 0.3 s after the pick.
inline Scenario vibrato (double f1, double fs)
{
    Pluck p = bridgePluck (f1);
    p.vibratoAt = 0.3;
    p.vibratoCents = 30.0;
    p.decaySeconds = 3.0;
    auto s = single ("vibrato", p, fs, 1.6);
    return s;
}

/// A whole-step bend over 150 ms, 0.4 s after the pick, held.
inline Scenario bend (double f1, double fs)
{
    Pluck p = bridgePluck (f1);
    p.rampAt = 0.4;
    p.rampSeconds = 0.15;
    p.rampCents = 200.0;
    p.decaySeconds = 3.0;
    return single ("bend", p, fs, 1.6);
}

/// Palm-muted sixteenths at `bpm`: short, dark notes, each damped as the next is picked.
inline Scenario palmMuted (double f1, double fs, double bpm = 140.0, int count = 16)
{
    const double step = 60.0 / bpm / 4.0;
    Scenario s { "palm-muted 16ths",
                 fs,
                 std::vector<float> (static_cast<size_t> ((0.2 + step * count + 0.2) * fs)),
                 {} };
    for (int k = 0; k < count; ++k)
    {
        Pluck p = bridgePluck (f1, static_cast<std::uint32_t> (11 + k));
        p.pluckPosition = 0.15;
        p.pickupPosition = 0.12;
        p.decaySeconds = 0.08;
        p.maxPartialHz = 2500.0;
        p.peak = 0.25f;
        p.pickNoise = 0.05f;
        p.muteAt = step - 0.004;
        const double at = 0.1 + step * k;
        addPluck (s.signal, fs, static_cast<size_t> (at * fs), p);
        s.notes.push_back ({ at, at + step - 0.004, p, at });
    }
    return s;
}

/// A legato run (hammer-ons): one pick, then whole steps every 150 ms.
inline Scenario legatoRun (double f1, double fs)
{
    Pluck p = bridgePluck (f1);
    p.decaySeconds = 3.0;
    p.stepAt = 0.4;
    p.stepCents = 200.0;
    p.stepCount = 5;
    p.stepEvery = 0.15;
    Scenario s { "legato run", fs, std::vector<float> (static_cast<size_t> (1.8 * fs)), {} };
    const double pick = 0.1;
    addPluck (s.signal, fs, static_cast<size_t> (pick * fs), p);
    s.notes.push_back ({ pick, pick + p.stepAt, p, pick });
    for (int k = 0; k < p.stepCount; ++k)
    {
        const double from = pick + p.stepAt + k * p.stepEvery;
        const double to = k + 1 < p.stepCount ? from + p.stepEvery : 1.8;
        s.notes.push_back ({ from, to, p, pick });
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// Running Trench and judging each note.

/// Level of each palm-muted note's sub against its input (dB, RMS over the note), the spread of
/// those levels, and the gate's crossings of 0.5 between the first lock and the last note's end.
struct ChugStats
{
    std::vector<double> levelDb;
    double spreadDb = 0.0;
    int flips = 0;
};

struct Run
{
    std::vector<float> out;
    std::vector<double> time; ///< end of each logged block, seconds
    std::vector<float> hz;    ///< trackState().hz after the block
    std::vector<float> gate;  ///< trackState().subGain after the block
};

inline Run runTrench (const std::vector<float>& in, double fs, const dsp::Trench::Settings& s,
                      int block = 16)
{
    dsp::Trench t;
    t.prepare ({ fs, block, 1 });
    t.setSettings (s);
    Run r;
    r.out = in;
    for (size_t start = 0; start < in.size(); start += static_cast<size_t> (block))
    {
        const int n =
            static_cast<int> (std::min<size_t> (static_cast<size_t> (block), in.size() - start));
        t.process (AudioView (r.out.data() + start, n));
        const auto st = t.trackState();
        r.time.push_back (static_cast<double> (start + static_cast<size_t> (n)) / fs);
        r.hz.push_back (st.hz);
        r.gate.push_back (st.subGain);
    }
    return r;
}

struct NoteStats
{
    double lockMs = 1.0e9;    ///< pick (or step) to the gate past 0.5 on the right note
    int flips = 0;            ///< gate crossings of 0.5 after the lock, before the note's end
    double held = 0.0;        ///< share of the note after the lock with the sub >= 90 % settled
    double amDb = 0.0;        ///< peak-to-trough of the sub's level above 5 Hz, dB
    double cents = 0.0;       ///< mean sub frequency against f / divisor, steady part
    double rightOctave = 0.0; ///< share of sub periods within 600 cents of f / divisor
    double medianCents = 0.0; ///< median of the sub periods against f / divisor
    int periods = 0;
};

/// The input as the sub's envelope hears it: 45 Hz - 1.1 kHz (Trench's level detector band).
inline std::vector<float> envelopeBand (const std::vector<float>& in, double fs)
{
    auto out = in;
    for (const auto& c : { design::highpass (fs, 45.0, 0.5), design::lowpass (fs, 1100.0, 0.7071) })
    {
        float s0 = 0.0f, s1 = 0.0f;
        for (auto& x : out)
        {
            const float y = c.b0 * x + s0;
            s0 = c.b1 * x - c.a1 * y + s1;
            s1 = c.b2 * x - c.a2 * y;
            x = y;
        }
    }
    return out;
}

/// Judges one note of a sub-only render (`divisor` 2 for Sub -1, 4 for Sub -2). `band` is the
/// input through envelopeBand().
inline NoteStats judge (const Run& r, const std::vector<float>& band, double fs, const NoteSpan& n,
                        double divisor = 2.0, double tailSeconds = 0.02)
{
    const auto& in = band;
    NoteStats st;
    const double f = n.hzAt (n.start + 0.5 * (n.end - n.start));
    // Lock: the gate past 0.5 while the tracker names the note (within 50 cents).
    double lock = -1.0;
    for (size_t k = 0; k < r.time.size(); ++k)
    {
        const double t = r.time[k];
        if (t <= n.start || t >= n.end)
            continue;
        if (r.gate[k] > 0.5f && r.hz[k] > 0.0f &&
            std::abs (centsError (r.hz[k], n.hzAt (t))) < 50.0)
        {
            lock = t;
            break;
        }
    }
    if (lock < 0.0)
        return st;
    st.lockMs = 1000.0 * (lock - n.start);
    const double until = n.end - tailSeconds;

    bool on = true;
    for (size_t k = 0; k < r.time.size(); ++k)
        if (r.time[k] > lock && r.time[k] < until)
        {
            const bool now = r.gate[k] > 0.5f;
            st.flips += now != on ? 1 : 0;
            on = now;
        }

    // Level of the sub against the input, both over two sub periods, every millisecond.
    const auto window = static_cast<size_t> (std::lround (divisor * 2.0 / f * fs));
    std::vector<double> ratioDb, times;
    const double halfWindow = 0.5 * static_cast<double> (window) / fs;
    for (double t = lock + halfWindow; t + halfWindow < until; t += 0.001)
    {
        const auto from = static_cast<size_t> ((t * fs) - 0.5 * static_cast<double> (window));
        const float s = rms (r.out.data() + from, static_cast<int> (window));
        const float x = rms (in.data() + from, static_cast<int> (window));
        if (x <= 0.0f)
            continue;
        ratioDb.push_back (db (s / x));
        times.push_back (t);
    }
    if (ratioDb.size() > 10)
    {
        auto sorted = ratioDb;
        std::nth_element (sorted.begin(),
                          sorted.begin() + static_cast<std::ptrdiff_t> (sorted.size() / 2),
                          sorted.end());
        const double settled = sorted[sorted.size() / 2];
        size_t above = 0;
        for (double v : ratioDb)
            above += v >= settled + db (0.9) ? 1 : 0;
        // Counted from the lock: the stretch before the first full window counts as held only if
        // the first window is.
        st.held = static_cast<double> (above) / static_cast<double> (ratioDb.size());

        // Modulation above 5 Hz: the level minus its 200 ms running mean.
        const size_t half = 100;
        double lo = 0.0, hi = 0.0;
        for (size_t k = half; k + half < ratioDb.size(); ++k)
        {
            double mean = 0.0;
            for (size_t j = k - half; j <= k + half; ++j)
                mean += ratioDb[j];
            mean /= static_cast<double> (2 * half + 1);
            lo = std::min (lo, ratioDb[k] - mean);
            hi = std::max (hi, ratioDb[k] - mean);
        }
        st.amDb = hi - lo;
    }

    // Pitch: every sub period after the lock, against the note's pitch at that moment.
    const auto from = static_cast<size_t> ((lock + 0.03) * fs);
    const auto zeros = risingZeroCrossings (r.out, from);
    int right = 0, count = 0;
    std::vector<double> periodCents;
    for (size_t k = 1; k < zeros.size(); ++k)
    {
        const double t = 0.5 * (zeros[k] + zeros[k - 1]) / fs;
        if (zeros[k] / fs >= until)
            break;
        const double hz = fs / (zeros[k] - zeros[k - 1]);
        const double c = centsError (hz, n.hzAt (t) / divisor);
        right += std::abs (c) < 600.0 ? 1 : 0;
        periodCents.push_back (c);
        ++count;
    }
    st.periods = count;
    if (!periodCents.empty())
    {
        std::nth_element (periodCents.begin(),
                          periodCents.begin() +
                              static_cast<std::ptrdiff_t> (periodCents.size() / 2),
                          periodCents.end());
        st.medianCents = periodCents[periodCents.size() / 2];
    }
    st.rightOctave = count > 0 ? static_cast<double> (right) / count : 0.0;
    if (zeros.size() > 2)
    {
        // Mean frequency over whole periods, against the mean target over the same span.
        size_t last = 0;
        while (last + 1 < zeros.size() && zeros[last + 1] / fs < until)
            ++last;
        if (last >= 2)
        {
            const double span = (zeros[last] - zeros[0]) / fs;
            const double hz = static_cast<double> (last) / span;
            double target = 0.0;
            const int steps = 200;
            for (int k = 0; k < steps; ++k)
                target += n.hzAt (zeros[0] / fs + span * (k + 0.5) / steps) / divisor / steps;
            st.cents = centsError (hz, target);
        }
    }
    return st;
}

inline ChugStats judgeChugs (const Run& r, const std::vector<float>& band, double fs,
                             const std::vector<NoteSpan>& notes, double fromSeconds)
{
    ChugStats st;
    double lo = 1.0e9, hi = -1.0e9;
    for (const auto& n : notes)
    {
        const auto a = static_cast<size_t> (n.start * fs), b = static_cast<size_t> (n.end * fs);
        const double l = db (rms (r.out.data() + a, static_cast<int> (b - a)) /
                             rms (band.data() + a, static_cast<int> (b - a)));
        st.levelDb.push_back (l);
        if (n.start >= fromSeconds)
        {
            lo = std::min (lo, l);
            hi = std::max (hi, l);
        }
    }
    st.spreadDb = hi - lo;
    bool on = true;
    for (size_t k = 0; k < r.time.size(); ++k)
        if (r.time[k] > fromSeconds && r.time[k] < notes.back().end - 0.02)
        {
            const bool now = r.gate[k] > 0.5f;
            st.flips += now != on ? 1 : 0;
            on = now;
        }
    return st;
}

} // namespace sedef::test::octaver
