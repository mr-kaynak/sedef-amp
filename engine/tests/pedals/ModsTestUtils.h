// SPDX-License-Identifier: AGPL-3.0-or-later
// Helpers for the mods pedals' tests (Conch, Anemone, Gyre, Jetty, Beacon): frequency responses
// of static settings, notch and peak finding, block-size invariance.
#pragma once

#include "PedalTestUtils.h"

#include <cmath>
#include <complex>
#include <vector>

namespace sedef::test
{

inline constexpr double kAllSampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };

/// Log-spaced frequencies from lo to hi, `perOctave` points per octave.
inline std::vector<double> logFrequencies (double lo, double hi, int perOctave = 96)
{
    std::vector<double> f;
    const int n = static_cast<int> (std::ceil (std::log2 (hi / lo) * perOctave));
    for (int i = 0; i <= n; ++i)
        f.push_back (lo * std::exp2 (static_cast<double> (i) / perOctave));
    return f;
}

/// |H(f)| in dB of an impulse response (direct DFT at each frequency, double precision).
inline std::vector<double> responseDb (const std::vector<float>& h, const std::vector<double>& f,
                                       double fs)
{
    std::vector<double> out;
    out.reserve (f.size());
    for (double hz : f)
    {
        const std::complex<double> step = std::polar (1.0, -kTwoPiD * hz / fs);
        std::complex<double> w (1.0, 0.0), acc (0.0, 0.0);
        for (size_t i = 0; i < h.size(); ++i)
        {
            acc += static_cast<double> (h[i]) * w;
            w *= step;
            if ((i & 1023u) == 1023u)
                w /= std::abs (w); // keep the rotation on the unit circle
        }
        out.push_back (20.0 * std::log10 (std::max (std::abs (acc), 1e-12)));
    }
    return out;
}

/// Impulse response of a module with fixed settings (snapped, so it is LTI from sample 0 when
/// the settings do not modulate).
template <typename Module>
std::vector<float> impulseResponse (const typename Module::Settings& s, double fs, int length,
                                    float amp = 1.0f)
{
    Module m;
    m.prepare ({ fs, 512, 1 });
    m.setSettings (s);
    Buffer b (1, length);
    b[0][0] = amp;
    processInBlocks (b, 512, [&] (AudioView v) { m.process (v); });
    for (auto& x : b[0])
        x /= amp;
    return b[0];
}

struct Extremum
{
    double hz = 0.0;
    double db = 0.0;
};

/// Local minima of a response that are at least `depthDb` below both neighbouring maxima.
inline std::vector<Extremum> findNotches (const std::vector<double>& db,
                                          const std::vector<double>& f, double depthDb)
{
    std::vector<Extremum> notches;
    for (size_t i = 1; i + 1 < db.size(); ++i)
    {
        if (!(db[i] < db[i - 1] && db[i] <= db[i + 1]))
            continue;
        // The nearest rise on each side: walk until the response falls again.
        double l = db[i], r = db[i];
        for (size_t j = i; j-- > 0 && db[j] >= l;)
            l = db[j];
        for (size_t j = i + 1; j < db.size() && db[j] >= r; ++j)
            r = db[j];
        if (std::min (l, r) - db[i] >= depthDb)
        {
            // Parabolic refinement on the log-frequency grid.
            const double a = db[i - 1], b = db[i], c = db[i + 1];
            const double denom = a - 2.0 * b + c;
            const double off = denom != 0.0 ? 0.5 * (a - c) / denom : 0.0;
            const double logF = std::log2 (f[i]) + off * (std::log2 (f[i + 1]) - std::log2 (f[i]));
            notches.push_back ({ std::exp2 (logF), b });
        }
    }
    return notches;
}

inline Extremum findPeak (const std::vector<double>& db, const std::vector<double>& f)
{
    size_t best = 0;
    for (size_t i = 1; i < db.size(); ++i)
        if (db[i] > db[best])
            best = i;
    if (best == 0 || best + 1 >= db.size())
        return { f[best], db[best] };
    const double a = db[best - 1], b = db[best], c = db[best + 1];
    const double denom = a - 2.0 * b + c;
    const double off = denom != 0.0 ? 0.5 * (a - c) / denom : 0.0;
    const double logF = std::log2 (f[best]) + off * (std::log2 (f[best + 1]) - std::log2 (f[best]));
    return { std::exp2 (logF), b - 0.25 * (a - c) * off };
}

/// Q of the peak from its -3 dB bandwidth (interpolated on the log grid).
inline double peakQ (const std::vector<double>& db, const std::vector<double>& f)
{
    const Extremum p = findPeak (db, f);
    const double target = p.db - 3.0103;
    size_t i = 0;
    while (i < f.size() && f[i] < p.hz)
        ++i;
    size_t hi = i;
    while (hi < db.size() && db[hi] > target)
        ++hi;
    size_t lo = i;
    while (lo > 0 && db[lo] > target)
        --lo;
    if (hi >= db.size() || lo == 0)
        return 0.0;
    const auto cross = [&] (size_t a, size_t b) {
        const double t = (target - db[a]) / (db[b] - db[a]);
        return std::exp2 (std::log2 (f[a]) + t * (std::log2 (f[b]) - std::log2 (f[a])));
    };
    const double fHi = cross (hi - 1, hi);
    const double fLo = cross (lo, lo + 1);
    return p.hz / (fHi - fLo);
}

/// Renders `in` through a fresh module in blocks of `block` (prepared for `maxBlock`).
template <typename Module>
std::vector<float> renderBlocks (const typename Module::Settings& s, const std::vector<float>& in,
                                 double fs, int block, int maxBlock = 4096)
{
    Module m;
    m.prepare ({ fs, maxBlock, 1 });
    m.setSettings (s);
    Buffer b (1, static_cast<int> (in.size()));
    b[0] = in;
    processInBlocks (
        b, block, [&] (AudioView v) { m.process (v); }, [&] (int) { m.setSettings (s); });
    return b[0];
}

/// Largest difference between renders with block sizes 1, 7, 64, 512 and 4096.
template <typename Module>
float blockSizeSpread (const typename Module::Settings& s, double fs)
{
    std::vector<float> in (static_cast<size_t> (fs / 2));
    fillGuitarTone (in, fs);
    const auto ref = renderBlocks<Module> (s, in, fs, 4096);
    float worst = 0.0f;
    for (int block : { 1, 7, 64, 512 })
    {
        const auto out = renderBlocks<Module> (s, in, fs, block);
        for (size_t i = 0; i < out.size(); ++i)
            worst = std::max (worst, std::abs (out[i] - ref[i]));
    }
    return worst;
}

/// Level change (dB) of the guitar reference tone through a module.
template <typename Module>
double guitarGainDb (const typename Module::Settings& s, double fs = 48000.0, double seconds = 2.0)
{
    const int frames = static_cast<int> (seconds * fs);
    Buffer b (1, frames);
    fillGuitarTone (b[0], fs);
    const float in = rms (b[0], static_cast<size_t> (frames / 2));
    Module m;
    m.prepare ({ fs, 512, 1 });
    m.setSettings (s);
    processInBlocks (b, 512, [&] (AudioView v) { m.process (v); });
    return db (rms (b[0], static_cast<size_t> (frames / 2)) / in);
}

/// RMS above `hz` (4th-order high-pass run over the whole signal, measured from `start`, so the
/// filter's own start-up transient is not counted).
inline float settledRmsAbove (const std::vector<float>& v, double hz, double fs, size_t start)
{
    std::vector<float> x = v;
    for (int pass = 0; pass < 2; ++pass)
    {
        const double cw = std::cos (kTwoPiD * hz / fs), sw = std::sin (kTwoPiD * hz / fs);
        const double alpha = sw / (2.0 * 0.7071067811865476);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0 + cw) * 0.5 / a0, b1 = -(1.0 + cw) / a0, b2 = b0;
        const double a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
        double s1 = 0.0, s2 = 0.0;
        for (auto& s : x)
        {
            const double in = s;
            const double out = b0 * in + s1;
            s1 = b1 * in - a1 * out + s2;
            s2 = b2 * in - a2 * out;
            s = static_cast<float> (out);
        }
    }
    return rms (x, start);
}

/// RMS envelope in windows of `window` samples.
inline std::vector<float> rmsEnvelope (const std::vector<float>& v, int window, size_t start = 0)
{
    std::vector<float> env;
    for (size_t i = start; i + static_cast<size_t> (window) <= v.size();
         i += static_cast<size_t> (window))
        env.push_back (rms (v.data() + i, window));
    return env;
}

} // namespace sedef::test
