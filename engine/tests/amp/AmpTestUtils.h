// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TestUtils.h"
#include "sedef/core/Biquad.h"
#include "sedef/dsp/amp/AmpSection.h"

#include <complex>
#include <limits>
#include <vector>

namespace sedef::test
{

/// One plucked string, added into `acc` from `start` seconds: a sum of decaying partials shaped by
/// the pluck position. `decay` scales every partial's time constant and `rolloff` is the spectral
/// slope (1.1 for an open string; a palm mute is shorter and darker).
inline void addString (std::vector<double>& acc, double fs, double f0, double start,
                       double strength, Random& rng, double decay = 1.0, double rolloff = 1.1)
{
    const auto first = static_cast<size_t> (start * fs);
    for (int k = 1; k * f0 < 8000.0; ++k)
    {
        const double pluck = std::abs (std::sin (kPiD * k * 0.18));
        const double amp = strength * pluck / std::pow (k, rolloff);
        const double tau = decay * 1.6 / (1.0 + 0.35 * k);
        const double phase = kTwoPiD * rng.unipolar();
        const double w = kTwoPiD * k * f0 / fs;
        for (size_t i = first; i < acc.size(); ++i)
        {
            const double t = static_cast<double> (i - first) / fs;
            if (t > 6.0 * tau)
                break;
            acc[i] +=
                amp * std::exp (-t / tau) * std::sin (w * static_cast<double> (i - first) + phase);
        }
    }
}

/// The unscaled open E-major strum behind guitarStrum (one stroke per second, alternating
/// down/up).
inline std::vector<double> strumRaw (double fs, double seconds, std::uint32_t seed)
{
    std::vector<double> acc (static_cast<size_t> (seconds * fs), 0.0);
    const double strings[] = { 82.41, 123.47, 164.81, 207.65, 246.94, 329.63 };
    Random rng (seed);
    int stroke = 0;
    for (double t0 = 0.05; t0 < seconds; t0 += 1.0, ++stroke)
    {
        const double strength = 0.8 + 0.4 * rng.unipolar();
        for (int s = 0; s < 6; ++s)
        {
            const int order = stroke % 2 == 0 ? s : 5 - s;
            addString (acc, fs, strings[s], t0 + 0.009 * order, strength, rng);
        }
    }
    return acc;
}

/// Gain that brings the 6 s reference strum to `dbfsRms`. Single notes and chugs share it, so
/// "a note at -18 dBFS" is one string picked as hard as in a strum at -18 dBFS RMS.
inline double strumScale (double fs, double dbfsRms = -18.0)
{
    static double cachedFs = 0.0, cachedScale = 1.0; // the strum is slow to build; tests are serial
    if (fs != cachedFs)
    {
        const auto acc = strumRaw (fs, 6.0, 7);
        double power = 0.0;
        for (double v : acc)
            power += v * v;
        cachedScale = 1.0 / std::sqrt (power / static_cast<double> (acc.size()));
        cachedFs = fs;
    }
    return std::pow (10.0, dbfsRms / 20.0) * cachedScale;
}

inline std::vector<float> toFloat (const std::vector<double>& acc, double scale)
{
    std::vector<float> out (acc.size());
    for (size_t i = 0; i < acc.size(); ++i)
        out[i] = static_cast<float> (acc[i] * scale);
    return out;
}

/// A deterministic stand-in for a guitar DI: an open E-major chord strummed once per second
/// (alternating down/up strokes), each string a sum of decaying partials shaped by the pluck
/// position. Normalised to -18 dBFS RMS; strum peaks land around -4 dBFS.
inline std::vector<float> guitarStrum (double fs, double seconds, std::uint32_t seed = 7)
{
    const auto acc = strumRaw (fs, seconds, seed);
    double power = 0.0;
    for (double v : acc)
        power += v * v;
    const double scale =
        std::pow (10.0, -18.0 / 20.0) / std::sqrt (power / static_cast<double> (acc.size()));
    return toFloat (acc, scale);
}

/// One single note (a string of the strum model) picked at t = 0.05 s, as hard as the strings of
/// a strum at `dbfsRms`. At -18 dBFS a G3 peaks around -14 dBFS.
inline std::vector<float> guitarNote (double fs, double seconds, double hz, double dbfsRms = -18.0,
                                      std::uint32_t seed = 5)
{
    std::vector<double> acc (static_cast<size_t> (seconds * fs), 0.0);
    Random rng (seed);
    addString (acc, fs, hz, 0.05, 1.0, rng);
    return toFloat (acc, strumScale (fs, dbfsRms));
}

/// Palm-muted E5 chugs (E2, B2, E3), eighth notes at `bpm`, every fourth one accented. The palm
/// shortens and darkens the strings.
inline std::vector<float> palmChugs (double fs, double seconds, double bpm = 150.0,
                                     double dbfsRms = -18.0, std::uint32_t seed = 9)
{
    std::vector<double> acc (static_cast<size_t> (seconds * fs), 0.0);
    Random rng (seed);
    const double strings[] = { 82.41, 123.47, 164.81 };
    int hit = 0;
    for (double t0 = 0.05; t0 < seconds - 0.1; t0 += 30.0 / bpm, ++hit)
    {
        const double strength = (hit % 4 == 0 ? 1.1 : 0.85) + 0.1 * rng.unipolar();
        for (int s = 0; s < 3; ++s)
            addString (acc, fs, strings[s], t0 + 0.004 * s, strength, rng, 0.07, 1.6);
    }
    return toFloat (acc, strumScale (fs, dbfsRms));
}

inline double dbfs (double rmsValue)
{
    return 20.0 * std::log10 (std::max (rmsValue, 1.0e-12));
}

/// Runs mono audio through an amp in fixed-size blocks, in place.
inline void runAmp (dsp::AmpSection& amp, std::vector<float>& audio, int block = 64)
{
    for (size_t start = 0; start < audio.size(); start += static_cast<size_t> (block))
    {
        const int n =
            static_cast<int> (std::min (static_cast<size_t> (block), audio.size() - start));
        float* p = audio.data() + start;
        amp.process (AudioView (&p, 1, n));
    }
}

/// Total harmonic distortion (harmonics 2..15) of a steady sine at hz in the second half of v.
inline double thd (const std::vector<float>& v, double hz, double fs)
{
    const int n = 32768;
    const float* d = v.data() + v.size() - n;
    const double fundamental = toneMagnitude (d, n, hz, fs);
    double harmonics = 0.0;
    for (int k = 2; k <= 15 && k * hz < 0.45 * fs; ++k)
    {
        const double h = toneMagnitude (d, n, k * hz, fs);
        harmonics += h * h;
    }
    return std::sqrt (harmonics) / std::max (fundamental, 1.0e-12);
}

/// Prepares an amp for mono processing with the given settings applied and settled.
inline void prepareAmp (dsp::AmpSection& amp, const dsp::AmpSection::Settings& s,
                        double fs = 48000.0, int block = 512)
{
    amp.setSettings (s);
    amp.prepare ({ fs, block, 1 });
    amp.setSettings (s);
}

/// A steady sine at `dbfsRms`.
inline std::vector<float> sineAt (double hz, double fs, double seconds, double dbfsRms)
{
    std::vector<float> v (static_cast<size_t> (seconds * fs));
    fillSine (v, hz, fs, static_cast<float> (std::sqrt (2.0) * std::pow (10.0, dbfsRms / 20.0)));
    return v;
}

/// Short-term RMS envelope in dBFS: `windowMs` windows every `hopMs`.
inline std::vector<double> envelopeDb (const std::vector<float>& v, double fs, double windowMs,
                                       double hopMs)
{
    const auto win = static_cast<size_t> (windowMs * 1e-3 * fs);
    const auto hop = static_cast<size_t> (hopMs * 1e-3 * fs);
    std::vector<double> env;
    for (size_t i = 0; i + win <= v.size(); i += hop)
        env.push_back (dbfs (rms (v.data() + i, static_cast<int> (win))));
    return env;
}

/// Sustain: seconds from the envelope's peak (20 ms RMS windows) until it first falls `dropDb`
/// below that peak.
inline double sustainSeconds (const std::vector<float>& v, double fs, double dropDb = 6.0)
{
    constexpr double hopMs = 5.0;
    const auto env = envelopeDb (v, fs, 20.0, hopMs);
    const auto peakIt = std::max_element (env.begin(), env.end());
    auto it = peakIt;
    while (it != env.end() && *it >= *peakIt - dropDb)
        ++it;
    return static_cast<double> (it - peakIt) * hopMs * 1e-3;
}

/// First level at which `values` reaches `threshold` going up through `levels` (linear
/// interpolation); NaN if it never does.
inline double crossingLevel (const std::vector<double>& levels, const std::vector<double>& values,
                             double threshold)
{
    if (values.empty())
        return std::numeric_limits<double>::quiet_NaN();
    if (values[0] >= threshold)
        return levels[0];
    for (size_t i = 1; i < levels.size(); ++i)
        if (values[i] >= threshold)
            return levels[i - 1] + (threshold - values[i - 1]) / (values[i] - values[i - 1]) *
                                       (levels[i] - levels[i - 1]);
    return std::numeric_limits<double>::quiet_NaN();
}

/// Input level (dBFS RMS) at which a 220 Hz sine reaches `thdTarget` THD, searched in 2 dB steps
/// from -90 dBFS up to 0 dBFS. NaN if it stays cleaner.
inline double thdCrossing (const dsp::AmpSection::Settings& s, double fs = 48000.0,
                           double thdTarget = 0.1)
{
    std::vector<double> levels, values;
    for (int db = -90; db <= 0; db += 2)
    {
        dsp::AmpSection amp;
        prepareAmp (amp, s, fs);
        auto v = sineAt (220.0, fs, 1.0, db);
        runAmp (amp, v);
        levels.push_back (db);
        values.push_back (thd (v, 220.0, fs));
        if (values.back() >= thdTarget)
            break;
    }
    return crossingLevel (levels, values, thdTarget);
}

/// Aliasing of a sine through a nonlinearity: the largest component (dB re. the fundamental)
/// between 50 Hz and 20 kHz that is not a harmonic, measured over the last n samples of v. Every
/// image of a harmonic, whether folded at the oversampled rate or at the base rate, lands on
/// |k f0 mod fs| (the oversampling factor is a power of two), so those are the frequencies
/// measured, exactly; a grid scan would fall between them.
inline double worstAliasDb (const std::vector<float>& v, double f0, double fs, int n = 65536)
{
    const float* d = v.data() + v.size() - static_cast<size_t> (n);
    const double fundamental = toneMagnitude (d, n, f0, fs);
    double worst = 0.0;
    for (int k = 2; k * f0 < 20.0 * fs; ++k)
    {
        double f = std::fmod (k * f0, fs);
        if (f > 0.5 * fs)
            f = fs - f;
        const double nearest = std::round (f / f0) * f0;
        if (f < 50.0 || f > 20000.0 || std::abs (f - nearest) < 40.0)
            continue;
        worst = std::max (worst, toneMagnitude (d, n, f, fs));
    }
    return 20.0 * std::log10 (std::max (worst, 1.0e-15) / fundamental);
}

/// One palm-muted E5 chug (E2, B2, E3) at t = 0.05 s, as hard as the accented hits of palmChugs.
inline std::vector<float> palmChug (double fs, double seconds, double dbfsRms = -18.0,
                                    std::uint32_t seed = 9)
{
    std::vector<double> acc (static_cast<size_t> (seconds * fs), 0.0);
    Random rng (seed);
    const double strings[] = { 82.41, 123.47, 164.81 };
    for (int s = 0; s < 3; ++s)
        addString (acc, fs, strings[s], 0.05 + 0.004 * s, 1.1, rng, 0.07, 1.6);
    return toFloat (acc, strumScale (fs, dbfsRms));
}

/// A ringing chord of the given strings, picked together at t = 0.05 s as hard as a strum at
/// `dbfsRms`.
inline std::vector<float> ringingChord (double fs, double seconds, const std::vector<double>& hz,
                                        double dbfsRms = -18.0, std::uint32_t seed = 13)
{
    std::vector<double> acc (static_cast<size_t> (seconds * fs), 0.0);
    Random rng (seed);
    for (size_t s = 0; s < hz.size(); ++s)
        addString (acc, fs, hz[s], 0.05 + 0.006 * static_cast<double> (s), 1.0, rng);
    return toFloat (acc, strumScale (fs, dbfsRms));
}

/// Seconds from a pick at `onset` until the envelope (5 ms RMS windows, 1 ms hop) falls for good
/// below its peak minus `dropDb`: how long a palm-muted chug rings before it stops.
inline double ringSeconds (const std::vector<float>& v, double fs, double dropDb,
                           double onset = 0.05)
{
    const auto env = envelopeDb (v, fs, 5.0, 1.0);
    const double peakDb = *std::max_element (env.begin(), env.end());
    size_t last = 0;
    for (size_t i = 0; i < env.size(); ++i)
        if (env[i] >= peakDb - dropDb)
            last = i;
    return static_cast<double> (last) * 1.0e-3 + 0.0025 - onset; // window centre
}

/// How far the output falls between successive chugs of palmChugs (eighths at `bpm`): the mean
/// over chugs (skipping the first two) of the envelope's peak minus its lowest point before the
/// next chug, in dB. A tight amp separates every chug; a fuzz runs them together.
inline double chugDipDb (const std::vector<float>& v, double fs, double bpm)
{
    const auto env = envelopeDb (v, fs, 5.0, 1.0);
    const double period = 30.0 / bpm;
    double sum = 0.0;
    int count = 0;
    for (int hit = 2;; ++hit)
    {
        const double t0 = 0.05 + period * hit;
        const auto a = static_cast<size_t> (t0 * 1000.0);
        const auto b = static_cast<size_t> ((t0 + period) * 1000.0) - 3;
        if (b >= env.size() || t0 + period > static_cast<double> (v.size()) / fs - 0.1)
            break;
        const auto [lo, hi] = std::minmax_element (env.begin() + static_cast<long> (a),
                                                   env.begin() + static_cast<long> (b));
        sum += *hi - *lo;
        ++count;
    }
    return count > 0 ? sum / count : 0.0;
}

/// Power spectrum (Hann window) of n samples (a power of two) from `start`: bin k is k fs / n.
inline std::vector<double> powerSpectrum (const std::vector<float>& v, size_t start, size_t n)
{
    std::vector<std::complex<double>> a (n);
    for (size_t i = 0; i < n; ++i)
        a[i] = static_cast<double> (v[start + i]) *
               (0.5 - 0.5 * std::cos (kTwoPiD * static_cast<double> (i) / static_cast<double> (n)));
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const std::complex<double> w (std::cos (kTwoPiD / static_cast<double> (len)),
                                      -std::sin (kTwoPiD / static_cast<double> (len)));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> wk (1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k, wk *= w)
            {
                const auto x = a[i + k], y = a[i + k + len / 2] * wk;
                a[i + k] = x + y;
                a[i + k + len / 2] = x - y;
            }
        }
    }
    std::vector<double> p (n / 2);
    for (size_t k = 0; k < n / 2; ++k)
        p[k] = std::norm (a[k]);
    return p;
}

/// Intermodulation of a chord: spectral power more than `tolHz` from every harmonic of every note,
/// re. the power near those harmonics, between 30 Hz and `hiHz`, in dB. The DI signals here have
/// exactly harmonic partials, so whatever lands between the harmonics is intermodulation: the
/// sum and difference tones that blur a chord's notes into one buzz.
inline double inharmonicDb (const std::vector<float>& v, double fs,
                            const std::vector<double>& notes, size_t start, size_t n = 131072,
                            double hiHz = 5000.0, double tolHz = 2.5)
{
    const auto p = powerSpectrum (v, start, n);
    const double binHz = fs / static_cast<double> (n);
    double harmonic = 0.0, other = 0.0;
    for (size_t k = 0; k < p.size(); ++k)
    {
        const double f = static_cast<double> (k) * binHz;
        if (f < 30.0 || f > hiHz)
            continue;
        bool near = false;
        for (double f0 : notes)
            near = near || std::abs (f - std::round (f / f0) * f0) <= tolHz;
        (near ? harmonic : other) += p[k];
    }
    return 10.0 * std::log10 (std::max (other, 1e-30) / std::max (harmonic, 1e-30));
}

/// Two-tone intermodulation: two sines at `hz` (each at `dbfsRms`) through `process`; returns
/// inharmonicDb of the steady output.
template <typename ProcessFn>
double twoToneImdDb (ProcessFn&& process, double f1, double f2, double fs, double dbfsRms)
{
    std::vector<float> v (static_cast<size_t> (3.0 * fs));
    const double a = std::sqrt (2.0) * std::pow (10.0, dbfsRms / 20.0);
    for (size_t i = 0; i < v.size(); ++i)
    {
        const double t = static_cast<double> (i) / fs;
        v[i] = static_cast<float> (a * (std::sin (kTwoPiD * f1 * t) + std::sin (kTwoPiD * f2 * t)));
    }
    process (v);
    return inharmonicDb (v, fs, { f1, f2 }, v.size() - 131072);
}

/// Output level (dB) of a steady sine in the last 16384 samples minus its input level: the gain
/// of a nonlinear process at one drive level.
template <typename ProcessFn>
double sineGainDb (ProcessFn&& process, double hz, double fs, double dbfsRms)
{
    auto v = sineAt (hz, fs, 0.5, dbfsRms);
    process (v);
    return dbfs (rms (v, v.size() - 16384)) - dbfsRms;
}

/// Input level (dBFS RMS) where a sine at hz is compressed by `dropDb` re. its small-signal gain,
/// searched upwards from -100 dBFS in 1 dB steps; NaN if it never is.
template <typename ProcessFn>
double compressionPoint (ProcessFn&& process, double hz, double fs, double dropDb)
{
    const double small = sineGainDb (process, hz, fs, -100.0);
    std::vector<double> levels, loss;
    for (int db = -100; db <= 0; ++db)
    {
        levels.push_back (db);
        loss.push_back (small - sineGainDb (process, hz, fs, db));
        if (loss.back() >= dropDb)
            break;
    }
    return crossingLevel (levels, loss, dropDb);
}

/// Octave-band levels in dB re. the total RMS; each band is two RBJ band-passes in series.
inline std::vector<double> octaveBands (const std::vector<float>& v, double fs)
{
    std::vector<double> out;
    const double total = rms (v);
    for (double fc = 63.0; fc < 17000.0; fc *= 2.0)
    {
        Biquad a, b;
        a.setCoeffs (design::bandpass (fs, fc, 1.41));
        b.setCoeffs (design::bandpass (fs, fc, 1.41));
        auto x = v;
        for (auto& s : x)
            s = b.processSample (0, a.processSample (0, s));
        out.push_back (20.0 * std::log10 (std::max (1e-12, static_cast<double> (rms (x)) / total)));
    }
    return out;
}

/// A rough guitar-cab stand-in (4x12-ish): 2nd-order high-pass at 80 Hz, resonance bump, and a
/// 4th-order roll-off above 5 kHz with a presence peak.
inline void roughCab (std::vector<float>& v, double fs)
{
    Biquad hp, bump, pres, lp1, lp2;
    hp.setCoeffs (design::highpass (fs, 80.0, 0.8));
    bump.setCoeffs (design::peaking (fs, 110.0, 1.2, 4.0));
    pres.setCoeffs (design::peaking (fs, 2500.0, 1.0, 3.0));
    lp1.setCoeffs (design::lowpass (fs, 5000.0, 0.8));
    lp2.setCoeffs (design::lowpass (fs, 5000.0, 0.6));
    for (auto& s : v)
        s = lp2.processSample (
            0, lp1.processSample (
                   0, pres.processSample (0, bump.processSample (0, hp.processSample (0, s)))));
}

} // namespace sedef::test
