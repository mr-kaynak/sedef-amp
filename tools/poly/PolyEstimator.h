// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Chord estimator for the feasibility study in docs/design/guitar-to-midi-chords.md. Offline
// tool code, not shipped and not real-time safe (it allocates as it goes); it is causal, though:
// it sees the signal in pushes and every event carries the time of the last sample it had seen.
//
// One channel in, note events out:
//   decimate to about 12 kHz -> Hann STFT of `window` seconds every `hop` ->
//   non-negative least squares of the magnitude spectrum on a dictionary of inharmonic note
//   atoms (three smooth frequency bands per note) with an l1 penalty -> note strengths ->
//   onset-driven note logic (new notes are decided `window` after an onset).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <pffft/pffft.h>
#include <vector>

namespace sedef::poly
{

inline constexpr double kPi = 3.14159265358979323846;

struct Config
{
    // The latency parameter: the analysis window, and the wait after an onset before the notes
    // it brought are decided.
    double window = 0.080;
    double decide = -1.0;   ///< seconds after an onset; < 0: equal to `window`
    double hop = 0.005;     ///< between analysis frames
    int lowNote = 40;       ///< E2
    int highNote = 88;      ///< E6
    double bottomHz = 70.0; ///< under the lowest fundamental's lobe, over the mains
    double topHz = 4000.0;  ///< partials above are not used
    int maxPartials = 24;
    double stiffness = 7.0e-5; ///< B of the templates
    double tuneCents = 12.0;   ///< each lobe is flat over +- this (tuning, fretting, the glide)
    int bands = 1;             ///< smooth frequency bands per note (free gain each)
    double sparsity = 0.1;     ///< l1 weight, as a share of the largest correlation
    int sweeps = 12;           ///< coordinate-descent sweeps per frame (warm started)
    double compress = 0.5;     ///< the spectrum is raised to this power
    int perSemitone = 10;      ///< bins of the line spectrum per semitone
    double resolve = 0.2;      ///< a line's frequency is good to this over the window length
    double lineFloor = 0.01;   ///< lines under this share of the strongest are dropped
    double oddShare = 0.0;     ///< odd partials 3, 5, 7 stand at least this high against 2, 4, 6
    double tilt = 0.5;         ///< partial k weighs k^-tilt in a template

    // Note logic.
    double onShare = 0.35;   ///< a new note: strength over this share of the strongest
    double rise = 1.25;      ///< and this many times its strength before the onset
    double offShare = 0.10;  ///< a note ends under this share of its own peak
    double keepShare = 0.15; ///< at a decision, a sounding note under this share ends
    double offHold = 0.10;   ///< for this long
    double decideFor = 0.03; ///< notes may start for this long after the decision point
    double spread = 0.06;    ///< a strum's strings are struck within this
    double gateDb = -62.0;   ///< frame level under which nothing sounds
    int maxNotes = 6;
    double onsetRise = 0.07; ///< flux over the level before it, for an onset
    double onsetGap = 0.012; ///< two onsets are at least this far apart
    bool trace = false;
};

struct NoteEvent
{
    double time = 0.0; ///< seconds, of the last input sample seen when it was decided
    int pitch = 0;
    bool on = false;
    float strength = 0.0f;
};

class Estimator
{
public:
    Estimator (double sampleRate, const Config& config) : cfg (config), fs (sampleRate)
    {
        factor = std::max (1, static_cast<int> (std::lround (fs / 12000.0)));
        fa = fs / factor;
        buildDecimator();
        hop = std::max (1, static_cast<int> (std::lround (cfg.hop * fa)));
        frame = std::max (32, static_cast<int> (std::lround (cfg.window * fa)));
        fftSize = 64;
        while (fftSize < 4 * frame)
            fftSize *= 2;
        setup = pffft_new_setup (fftSize, PFFFT_REAL);
        in = static_cast<float*> (
            pffft_aligned_malloc (sizeof (float) * static_cast<std::size_t> (fftSize)));
        out = static_cast<float*> (
            pffft_aligned_malloc (sizeof (float) * static_cast<std::size_t> (fftSize)));
        work = static_cast<float*> (
            pffft_aligned_malloc (sizeof (float) * static_cast<std::size_t> (fftSize)));
        hann.resize (static_cast<std::size_t> (frame));
        for (int i = 0; i < frame; ++i)
            hann[static_cast<std::size_t> (i)] =
                static_cast<float> (0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / frame));
        ring.assign (static_cast<std::size_t> (frame), 0.0f);

        // The short transform that times onsets: 10.7 ms, the same hop.
        shortFrame = std::max (16, static_cast<int> (std::lround (0.0107 * fa)));
        shortFft = 32;
        while (shortFft < shortFrame)
            shortFft *= 2;
        shortSetup = pffft_new_setup (shortFft, PFFFT_REAL);
        shortHann.resize (static_cast<std::size_t> (shortFrame));
        for (int i = 0; i < shortFrame; ++i)
            shortHann[static_cast<std::size_t> (i)] =
                static_cast<float> (0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / shortFrame));
        shortMag.assign (static_cast<std::size_t> (shortFft / 2), 0.0f);
        for (auto& h : shortHistory)
            h.assign (static_cast<std::size_t> (shortFft / 2), 0.0f);

        buildDictionary();
        const auto notes = static_cast<std::size_t> (noteCount());
        strength.assign (notes, 0.0f);
        before.assign (notes, 0.0f);
        sounding.assign (notes, 0);
        started.assign (notes, 0);
        peak.assign (notes, 0.0f);
        under.assign (notes, 0.0);
        last.assign (notes, 0.0f);
    }

    ~Estimator()
    {
        pffft_destroy_setup (setup);
        pffft_destroy_setup (shortSetup);
        pffft_aligned_free (in);
        pffft_aligned_free (out);
        pffft_aligned_free (work);
    }

    Estimator (const Estimator&) = delete;
    Estimator& operator= (const Estimator&) = delete;

    int noteCount() const { return cfg.highNote - cfg.lowNote + 1; }
    double analysisRate() const { return fa; }
    int atoms() const { return static_cast<int> (atomNote.size()); }
    int bins() const { return binCount; }
    /// The strengths of the last frame, one per note from lowNote.
    const std::vector<float>& strengths() const { return strength; }

    void push (const float* x, int n, std::vector<NoteEvent>& events)
    {
        for (int i = 0; i < n; ++i)
        {
            ++seen;
            fir[firAt] = x[i];
            firAt = (firAt + 1) % fir.size();
            if (++phase < factor)
                continue;
            phase = 0;
            double acc = 0.0;
            std::size_t at = firAt;
            for (double c : taps)
            {
                acc += c * fir[at];
                at = (at + 1) % fir.size();
            }
            ring[ringAt] = static_cast<float> (acc);
            ringAt = (ringAt + 1) % ring.size();
            if (++sinceFrame >= hop)
            {
                sinceFrame = 0;
                analyse (events);
            }
        }
    }

    /// Ends every sounding note (the end of the input).
    void flush (std::vector<NoteEvent>& events)
    {
        for (int k = 0; k < noteCount(); ++k)
            if (sounding[static_cast<std::size_t> (k)] != 0)
            {
                events.push_back ({ now(), cfg.lowNote + k, false, 0.0f });
                sounding[static_cast<std::size_t> (k)] = 0;
            }
    }

private:
    struct Entry
    {
        int bin;
        float weight;
    };

    double now() const { return static_cast<double> (seen) / fs; }

    void buildDecimator()
    {
        // Windowed sinc, cut at 0.45 of the analysis rate, 8 analysis samples long.
        const int length = factor == 1 ? 1 : 8 * factor + 1;
        taps.assign (static_cast<std::size_t> (length), 1.0);
        if (factor > 1)
        {
            const double cut = 0.45 / factor;
            double sum = 0.0;
            for (int i = 0; i < length; ++i)
            {
                const double t = i - 0.5 * (length - 1);
                const double s = t == 0.0 ? 2.0 * cut : std::sin (2.0 * kPi * cut * t) / (kPi * t);
                const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / length);
                taps[static_cast<std::size_t> (i)] = s * w;
                sum += s * w;
            }
            for (auto& c : taps)
                c /= sum;
        }
        fir.assign (taps.size(), 0.0f);
    }

    /// The line spectrum's axis: `perSemitone` bins a semitone from bottomHz.
    double binOf (double hz) const
    {
        return 12.0 * cfg.perSemitone * std::log2 (hz / cfg.bottomHz);
    }

    /// How far (in Hz, one standard deviation) a line's measured frequency may lie from where
    /// it is: the transform's resolution, and what merges into it.
    double lineSigma() const { return cfg.resolve * fa / frame; }

    /// Adds a bump of height `height` at `hz`, `sigmaHz` wide, to a row of the log axis.
    void bump (std::vector<float>& row, double hz, double sigmaHz, double height) const
    {
        const double centre = binOf (hz);
        const double sigma = std::max (0.6, binOf (hz + sigmaHz) - centre);
        const int from = std::max (0, static_cast<int> (std::floor (centre - 3.0 * sigma)));
        const int to = std::min (binCount - 1, static_cast<int> (std::ceil (centre + 3.0 * sigma)));
        for (int b = from; b <= to; ++b)
        {
            const double d = (b - centre) / sigma;
            auto& cell = row[static_cast<std::size_t> (b)];
            cell = std::max (cell, static_cast<float> (height * std::exp (-0.5 * d * d)));
        }
    }

    void buildDictionary()
    {
        const double binHz = fa / fftSize;
        firstBin = std::max (1, static_cast<int> (std::floor (cfg.bottomHz * 0.9 / binHz)));
        lastFftBin =
            std::min (fftSize / 2 - 2, static_cast<int> (std::ceil ((cfg.topHz * 1.05) / binHz)));
        binCount = static_cast<int> (std::ceil (binOf (cfg.topHz * 1.03))) + 1;

        auto band = [this] (int j, double hz) {
            if (cfg.bands <= 1)
                return 1.0;
            const double lo = std::log2 (120.0), hi = std::log2 (2400.0);
            const double step = (hi - lo) / (cfg.bands - 1);
            double x = (std::log2 (hz) - lo) / step; // 0 at the first centre
            x = std::clamp (x, 0.0, static_cast<double> (cfg.bands - 1));
            const double d = std::abs (x - j);
            return d >= 1.0 ? 0.0 : std::pow (std::cos (0.5 * kPi * d), 2.0);
        };

        std::vector<float> dense (static_cast<std::size_t> (binCount));
        for (int note = cfg.lowNote; note <= cfg.highNote; ++note)
        {
            const double f0 = 440.0 * std::exp2 ((note - 69) / 12.0);
            for (int j = 0; j < cfg.bands; ++j)
            {
                std::fill (dense.begin(), dense.end(), 0.0f);
                bool any = false;
                for (int k = 1; k <= cfg.maxPartials; ++k)
                {
                    const double f = k * f0 * std::sqrt (1.0 + cfg.stiffness * k * k);
                    if (f > cfg.topHz)
                        break;
                    const double gain = band (j, f) * std::pow (static_cast<double> (k), -cfg.tilt);
                    if (gain <= 1.0e-6)
                        continue;
                    any = true;
                    // A partial lies where the template says to within the tuning tolerance and
                    // what the unknown stiffness moves it (0.5 B k^2 relative, B known to +-70 %).
                    const double tune = f * (std::exp2 (cfg.tuneCents / 1200.0) - 1.0);
                    const double stiff = 0.5 * f * cfg.stiffness * 0.7 * k * k;
                    bump (dense, f,
                          std::sqrt (lineSigma() * lineSigma() + tune * tune + stiff * stiff),
                          gain);
                }
                if (!any)
                    continue;
                double norm = 0.0;
                for (float v : dense)
                    norm += static_cast<double> (v) * v;
                norm = std::sqrt (norm);
                if (norm <= 0.0)
                    continue;
                atomStart.push_back (static_cast<int> (entries.size()));
                atomNote.push_back (note - cfg.lowNote);
                for (int b = 0; b < binCount; ++b)
                    if (dense[static_cast<std::size_t> (b)] > 1.0e-4f)
                        entries.push_back (
                            { b, static_cast<float> (dense[static_cast<std::size_t> (b)] / norm) });
            }
        }
        atomStart.push_back (static_cast<int> (entries.size()));

        // The Gram matrix of the atoms.
        const auto count = atomNote.size();
        gram.assign (count * count, 0.0f);
        std::vector<float> col (static_cast<std::size_t> (binCount));
        for (std::size_t a = 0; a < count; ++a)
        {
            std::fill (col.begin(), col.end(), 0.0f);
            for (int e = atomStart[a]; e < atomStart[a + 1]; ++e)
                col[static_cast<std::size_t> (entries[static_cast<std::size_t> (e)].bin)] =
                    entries[static_cast<std::size_t> (e)].weight;
            for (std::size_t b = a; b < count; ++b)
            {
                double dot = 0.0;
                for (int e = atomStart[b]; e < atomStart[b + 1]; ++e)
                    dot +=
                        static_cast<double> (entries[static_cast<std::size_t> (e)].weight) *
                        col[static_cast<std::size_t> (entries[static_cast<std::size_t> (e)].bin)];
                gram[a * count + b] = gram[b * count + a] = static_cast<float> (dot);
            }
        }
        act.assign (count, 0.0f);
        corr.assign (count, 0.0f);
        spectrum.assign (static_cast<std::size_t> (binCount), 0.0f);
        mags.assign (static_cast<std::size_t> (fftSize / 2), 0.0f);
    }

    /// Positive spectral change over 8 ms on the short transform, relative to the level before:
    /// the onset function.
    float onsetFunction()
    {
        std::size_t at =
            (ringAt + ring.size() - static_cast<std::size_t> (shortFrame)) % ring.size();
        std::vector<float> buf (static_cast<std::size_t> (shortFft), 0.0f),
            spec (static_cast<std::size_t> (shortFft));
        for (int i = 0; i < shortFrame; ++i)
        {
            buf[static_cast<std::size_t> (i)] = ring[at] * shortHann[static_cast<std::size_t> (i)];
            at = (at + 1) % ring.size();
        }
        std::vector<float> w (static_cast<std::size_t> (shortFft));
        pffft_transform_ordered (shortSetup, buf.data(), spec.data(), w.data(), PFFFT_FORWARD);
        const auto half = static_cast<std::size_t> (shortFft / 2);
        // Against the largest each bin read over the last 40 ms: two partials in one bin beat,
        // and a beat's swell is not an onset.
        // The mean rise of the log magnitudes: a new string shows in the bins it adds to, however
        // loud the others ring.
        const double floor = std::pow (10.0, cfg.gateDb / 20.0) * 0.5;
        double up = 0.0;
        for (std::size_t b = 1; b < half; ++b)
        {
            const float m =
                std::hypot (spec[2 * b], spec[2 * b + 1]) * 2.0f / static_cast<float> (shortFrame);
            shortMag[b] = m;
            float old = 0.0f;
            for (const auto& h : shortHistory)
                old = std::max (old, h[b]);
            up += std::max (0.0, std::log ((static_cast<double> (m) + floor) /
                                           (static_cast<double> (old) + floor)));
        }
        shortAt = (shortAt + 1) % shortHistory.size();
        shortHistory[shortAt] = shortMag;
        return static_cast<float> (up / static_cast<double> (half - 1));
    }

    void analyse (std::vector<NoteEvent>& events)
    {
        const double t = now();

        // Onsets.
        const float flux = onsetFunction();
        if (flux > cfg.onsetRise && t - lastOnset >= cfg.onsetGap)
        {
            // The first onset of a strum keeps what sounded before it: a note is new, or struck
            // again, by what it gained on that.
            if (onsets.empty())
            {
                before = strength;
                std::fill (started.begin(), started.end(), 0);
            }
            lastOnset = t;
            onsets.push_back (t - 0.5 * shortFrame / fa);
            if (cfg.trace)
                std::printf ("  %.3f onset (flux %.2f)\n", onsets.back(),
                             static_cast<double> (flux));
        }

        // The spectrum.
        std::size_t at = ringAt;
        double power = 0.0;
        for (int i = 0; i < frame; ++i)
        {
            const float v = ring[at];
            power += static_cast<double> (v) * v;
            in[i] = v * hann[static_cast<std::size_t> (i)];
            at = (at + 1) % ring.size();
        }
        std::fill (in + frame, in + fftSize, 0.0f);
        pffft_transform_ordered (setup, in, out, work, PFFFT_FORWARD);
        const float scale = 4.0f / static_cast<float> (frame); // a sine of amplitude 1 reads 1
        float top = 0.0f;
        for (int k = firstBin - 1; k <= lastFftBin + 1; ++k)
        {
            mags[static_cast<std::size_t> (k)] = std::hypot (out[2 * k], out[2 * k + 1]) * scale;
            top = std::max (top, mags[static_cast<std::size_t> (k)]);
        }
        // The lines: local maxima, placed by a parabola through the log magnitudes.
        std::fill (spectrum.begin(), spectrum.end(), 0.0f);
        lines = 0;
        const double binHz = fa / fftSize;
        const float floor = std::max (static_cast<float> (cfg.lineFloor) * top, 1.0e-6f);
        for (int k = firstBin; k <= lastFftBin; ++k)
        {
            const float m = mags[static_cast<std::size_t> (k)];
            if (m < floor || m < mags[static_cast<std::size_t> (k - 1)] ||
                m <= mags[static_cast<std::size_t> (k + 1)])
                continue;
            const double l =
                std::log (static_cast<double> (mags[static_cast<std::size_t> (k - 1)]) + 1.0e-12);
            const double c = std::log (static_cast<double> (m) + 1.0e-12);
            const double r =
                std::log (static_cast<double> (mags[static_cast<std::size_t> (k + 1)]) + 1.0e-12);
            const double denom = l - 2.0 * c + r;
            const double shift = denom < 0.0 ? std::clamp (0.5 * (l - r) / denom, -0.5, 0.5) : 0.0;
            const double hz = (k + shift) * binHz;
            if (hz < cfg.bottomHz || hz > cfg.topHz * 1.03)
                continue;
            const double height = std::exp (c - 0.25 * (l - r) * shift);
            bump (spectrum, hz, lineSigma(),
                  cfg.compress == 1.0 ? height : std::pow (height, cfg.compress));
            ++lines;
        }
        const double levelDb = 10.0 * std::log10 (power / frame + 1.0e-20);

        solve();

        // Note strengths: the norm of what each note's atoms explain.
        const auto count = atomNote.size();
        last2 = last;
        last = strength;
        std::fill (strength.begin(), strength.end(), 0.0f);
        {
            std::size_t a = 0;
            while (a < count)
            {
                std::size_t b = a;
                double sq = 0.0;
                while (b < count && atomNote[b] == atomNote[a])
                    ++b;
                for (std::size_t i = a; i < b; ++i)
                    for (std::size_t j = a; j < b; ++j)
                        sq += static_cast<double> (act[i]) * act[j] * gram[i * count + j];
                strength[static_cast<std::size_t> (atomNote[a])] =
                    static_cast<float> (std::sqrt (std::max (0.0, sq)));
                a = b;
            }
        }
        // The sieve: a string shows its odd partials. A candidate whose 3rd, 5th and 7th partials
        // are missing while its 2nd, 4th and 6th stand is the octave under a note that sounds
        // (with whatever lies near its fundamental: a body resonance, a thump, hum).
        if (cfg.oddShare > 0.0)
            for (int k = 0; k < noteCount(); ++k)
            {
                if (strength[static_cast<std::size_t> (k)] <= 0.0f)
                    continue;
                const double f0 = 440.0 * std::exp2 ((cfg.lowNote + k - 69) / 12.0);
                double odd = 0.0, even = 0.0;
                int odds = 0, evens = 0;
                for (int h = 2; h <= 7; ++h)
                {
                    const double f = h * f0 * std::sqrt (1.0 + cfg.stiffness * h * h);
                    if (f > cfg.topHz)
                        break;
                    const int centre = static_cast<int> (std::lround (binOf (f)));
                    const int reach = std::max (
                        2, static_cast<int> (std::lround (binOf (f + lineSigma()) - binOf (f))));
                    float m = 0.0f;
                    for (int b = std::max (0, centre - reach);
                         b <= std::min (binCount - 1, centre + reach); ++b)
                        m = std::max (m, spectrum[static_cast<std::size_t> (b)]);
                    if (h % 2 == 1)
                    {
                        odd += m;
                        ++odds;
                    }
                    else
                    {
                        even += m;
                        ++evens;
                    }
                }
                if (odds >= 2 && evens >= 2 && odd / odds < cfg.oddShare * even / evens)
                    strength[static_cast<std::size_t> (k)] = 0.0f;
            }
        float strongest = 0.0f;
        for (float s : strength)
            strongest = std::max (strongest, s);

        // A strum is decided `wait` after its last onset (the window then holds every string
        // it struck), or `spread` after that point for its first onset if onsets keep coming.
        const double wait = cfg.decide < 0.0 ? cfg.window : cfg.decide;
        bool deciding = false;
        if (!onsets.empty())
        {
            const double from = std::min (onsets.back() + wait, onsets.front() + wait + cfg.spread);
            deciding = t >= from;
            if (t > onsets.back() + wait + cfg.decideFor)
                onsets.clear();
        }

        const bool open = levelDb > cfg.gateDb;
        int soundingNow = 0;
        for (auto s : sounding)
            soundingNow += s != 0 ? 1 : 0;

        if (cfg.trace && (deciding || soundingNow > 0))
        {
            std::printf ("  %.3f %s %5.1f dB:", t, deciding ? "D" : " ", levelDb);
            for (int k = 0; k < noteCount(); ++k)
                if (strength[static_cast<std::size_t> (k)] > 0.08f * strongest)
                    std::printf (" %d%s=%.0f%%", cfg.lowNote + k,
                                 sounding[static_cast<std::size_t> (k)] != 0 ? "*" : "",
                                 100.0 * static_cast<double> (
                                             strength[static_cast<std::size_t> (k)] / strongest));
            std::printf ("\n");
        }

        for (int k = 0; k < noteCount(); ++k)
        {
            const auto i = static_cast<std::size_t> (k);
            const float s = strength[i];
            if (sounding[i] != 0)
            {
                peak[i] = std::max (peak[i], s);
                const bool low = !open || s < cfg.offShare * peak[i];
                under[i] = low ? under[i] + hop / fa : 0.0;
                // A strum's decision is also where the notes it did not strike are seen to have
                // gone: under `keepShare` of the strongest then, a note ends at once.
                const bool gone = deciding && s < cfg.keepShare * strongest &&
                                  last[i] < cfg.keepShare * strongest;
                if (under[i] >= cfg.offHold || !open || gone)
                {
                    events.push_back ({ t, cfg.lowNote + k, false, 0.0f });
                    sounding[i] = 0;
                    --soundingNow;
                }
            }
        }
        if (deciding && open)
        {
            // Strongest first, so the cap keeps the notes that matter.
            std::vector<int> order;
            for (int k = 0; k < noteCount(); ++k)
            {
                const auto i = static_cast<std::size_t> (k);
                if (strength[i] >= cfg.onShare * strongest && strength[i] >= cfg.rise * before[i] &&
                    std::min (last[i], strength[i]) >= cfg.onShare * strongest)
                    order.push_back (k);
            }
            std::sort (order.begin(), order.end(), [this] (int a, int b) {
                return strength[static_cast<std::size_t> (a)] >
                       strength[static_cast<std::size_t> (b)];
            });
            for (int k : order)
            {
                const auto i = static_cast<std::size_t> (k);
                if (started[i] != 0)
                    continue; // once a strum
                started[i] = 1;
                if (sounding[i] != 0)
                {
                    // Struck again: the note starts over.
                    events.push_back ({ t, cfg.lowNote + k, false, 0.0f });
                    events.push_back ({ t, cfg.lowNote + k, true, strength[i] });
                }
                else
                {
                    if (soundingNow >= cfg.maxNotes)
                        continue;
                    events.push_back ({ t, cfg.lowNote + k, true, strength[i] });
                    sounding[i] = 1;
                    ++soundingNow;
                }
                peak[i] = strength[i];
                under[i] = 0.0;
            }
        }
    }

    /// Non-negative least squares with an l1 penalty, by cyclic coordinate descent on the normal
    /// equations, warm started from the last frame.
    void solve()
    {
        const auto count = atomNote.size();
        float largest = 0.0f;
        for (std::size_t a = 0; a < count; ++a)
        {
            double dot = 0.0;
            for (int e = atomStart[a]; e < atomStart[a + 1]; ++e)
                dot +=
                    static_cast<double> (entries[static_cast<std::size_t> (e)].weight) *
                    spectrum[static_cast<std::size_t> (entries[static_cast<std::size_t> (e)].bin)];
            corr[a] = static_cast<float> (dot);
            largest = std::max (largest, corr[a]);
        }
        const float penalty = static_cast<float> (cfg.sparsity) * largest;
        for (int sweep = 0; sweep < cfg.sweeps; ++sweep)
            for (std::size_t a = 0; a < count; ++a)
            {
                const float* row = gram.data() + a * count;
                float acc = corr[a] - penalty;
                for (std::size_t b = 0; b < count; ++b)
                    acc -= row[b] * act[b];
                acc += row[a] * act[a];
                act[a] = std::max (0.0f, acc / row[a]);
            }
    }

    Config cfg;
    double fs, fa = 12000.0;
    int factor = 4, hop = 60, frame = 720, fftSize = 2048, firstBin = 0, lastFftBin = 0,
        binCount = 0, lines = 0;
    int shortFrame = 128, shortFft = 128;
    std::vector<double> taps;
    std::vector<float> fir;
    std::size_t firAt = 0;
    int phase = 0, sinceFrame = 0;
    std::int64_t seen = 0;
    std::vector<float> ring, hann, shortHann, shortMag;
    std::array<std::vector<float>, 8> shortHistory;
    std::size_t shortAt = 0;
    std::size_t ringAt = 0;
    PFFFT_Setup* setup = nullptr;
    PFFFT_Setup* shortSetup = nullptr;
    float* in = nullptr;
    float* out = nullptr;
    float* work = nullptr;

    std::vector<Entry> entries;
    std::vector<int> atomStart, atomNote;
    std::vector<float> gram, act, corr, spectrum, mags;

    std::vector<float> strength, before, peak, last, last2;
    std::vector<char> sounding, started;
    std::vector<double> under;
    std::vector<double> onsets;
    double lastOnset = -1.0;
};

} // namespace sedef::poly
