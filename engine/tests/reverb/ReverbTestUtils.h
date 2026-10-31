// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TestUtils.h"
#include "sedef/core/Biquad.h"

#include <cmath>
#include <pffft/pffft.h>
#include <vector>

namespace sedef::test
{

/// Hann-windowed magnitude spectrum of n samples, zero-padded to fftSize (a power of two >= n).
/// Bin k is k * fs / fftSize.
inline std::vector<double> magnitudeSpectrum (const float* x, int n, int fftSize)
{
    PFFFT_Setup* setup = pffft_new_setup (fftSize, PFFFT_REAL);
    const size_t bytes = sizeof (float) * static_cast<size_t> (fftSize);
    auto* in = static_cast<float*> (pffft_aligned_malloc (bytes));
    auto* out = static_cast<float*> (pffft_aligned_malloc (bytes));
    auto* work = static_cast<float*> (pffft_aligned_malloc (bytes));
    for (int i = 0; i < fftSize; ++i)
        in[i] =
            i < n ? x[i] * static_cast<float> (0.5 - 0.5 * std::cos (kTwoPiD * i / (n - 1))) : 0.0f;
    pffft_transform_ordered (setup, in, out, work, PFFFT_FORWARD);

    std::vector<double> mag (static_cast<size_t> (fftSize / 2 + 1));
    mag[0] = std::abs (out[0]);
    mag.back() = std::abs (out[1]);
    for (int k = 1; k < fftSize / 2; ++k)
        mag[static_cast<size_t> (k)] = std::hypot (out[2 * k], out[2 * k + 1]);

    pffft_aligned_free (in);
    pffft_aligned_free (out);
    pffft_aligned_free (work);
    pffft_destroy_setup (setup);
    return mag;
}

inline int nextPow2 (int n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

struct SpectralPeak
{
    double hz;
    double magnitude;
};

/// Largest peak between loHz and hiHz, frequency refined by a parabola on the log magnitude.
inline SpectralPeak strongestPeak (const std::vector<double>& mag, double fs, int fftSize,
                                   double loHz, double hiHz)
{
    const double binHz = fs / fftSize;
    const int lo = std::max (1, static_cast<int> (std::ceil (loHz / binHz)));
    const int hi = std::min (static_cast<int> (mag.size()) - 2, static_cast<int> (hiHz / binHz));
    int best = lo;
    for (int k = lo; k <= hi; ++k)
        if (mag[static_cast<size_t> (k)] > mag[static_cast<size_t> (best)])
            best = k;
    const auto b = static_cast<size_t> (best);
    const double a = std::log (mag[b - 1] + 1e-30);
    const double m = std::log (mag[b] + 1e-30);
    const double c = std::log (mag[b + 1] + 1e-30);
    const double den = a - 2.0 * m + c;
    const double p = den < 0.0 ? 0.5 * (a - c) / den : 0.0;
    return { (best + p) * binHz, mag[b] };
}

/// Energy of the spectrum above hz (sum of squared magnitudes).
inline double energyAbove (const std::vector<double>& mag, double fs, int fftSize, double hz)
{
    double e = 0.0;
    for (size_t k = static_cast<size_t> (hz * fftSize / fs); k < mag.size(); ++k)
        e += mag[k] * mag[k];
    return e;
}

/// In-place 2nd-order Butterworth band limiting (high-pass at lo, low-pass at hi).
inline void bandLimit (std::vector<float>& x, double fs, double lo, double hi)
{
    Biquad hp, lp;
    hp.setCoeffs (design::highpass (fs, lo));
    lp.setCoeffs (design::lowpass (fs, hi));
    for (auto& s : x)
        s = lp.processSample (0, hp.processSample (0, s));
}

/// RT60 from the Schroeder backward-integrated energy decay curve of a stereo impulse response:
/// least-squares line through the -5..-35 dB range (T30), extrapolated to -60 dB.
inline double rt60FromDecay (const std::vector<float>& l, const std::vector<float>& r, double fs)
{
    const size_t n = l.size();
    std::vector<double> edc (n);
    double acc = 0.0;
    for (size_t i = n; i-- > 0;)
    {
        acc += static_cast<double> (l[i]) * l[i] + static_cast<double> (r[i]) * r[i];
        edc[i] = acc;
    }
    const double total = edc[0];
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int count = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double db = 10.0 * std::log10 (edc[i] / total + 1e-300);
        if (db > -5.0)
            continue;
        if (db < -35.0)
            break;
        const double t = static_cast<double> (i) / fs;
        sx += t;
        sy += db;
        sxx += t * t;
        sxy += t * db;
        ++count;
    }
    const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx); // dB per second
    return -60.0 / slope;
}

/// Pearson correlation coefficient.
inline double correlation (const float* a, const float* b, int n)
{
    double ma = 0, mb = 0;
    for (int i = 0; i < n; ++i)
    {
        ma += a[i];
        mb += b[i];
    }
    ma /= n;
    mb /= n;
    double ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < n; ++i)
    {
        const double x = a[i] - ma, y = b[i] - mb;
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }
    return ab / std::sqrt (aa * bb + 1e-300);
}

/// Largest sample of the signal after a steep high-pass (two cascaded 2nd-order sections). A
/// smooth low-frequency signal reads near zero; a discontinuity reads roughly its step size.
inline float clickPeak (const std::vector<float>& x, double fs, double cornerHz, size_t skip = 0)
{
    Biquad a, b;
    a.setCoeffs (design::highpass (fs, cornerHz));
    b.setCoeffs (design::highpass (fs, cornerHz));
    float p = 0.0f;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const float y = b.processSample (0, a.processSample (0, x[i]));
        if (i >= skip)
            p = std::max (p, std::abs (y));
    }
    return p;
}

} // namespace sedef::test
