// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TestUtils.h"
#include "sedef/core/Biquad.h"
#include "sedef/dsp/amp/AmpSection.h"

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
