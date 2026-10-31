// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TestUtils.h"
#include "sedef/dsp/cab/CabSim.h"

#include <complex>
#include <pffft/pffft.h>
#include <vector>

namespace sedef::test::cab
{

using dsp::CabSim;

/// Settings with only mic A audible (centre pan, no room).
inline CabSim::Settings soloMicA (int cab, int type, float position = 0.5f, float distance = 0.0f)
{
    CabSim::Settings s;
    s.cab = cab;
    s.mics[0].type = type;
    s.mics[0].position = position;
    s.mics[0].distance = distance;
    s.mics[1].enabled = false;
    return s;
}

/// Impulse response (left + right folded, so a centred mic reads at unity: L = R = x / sqrt 2).
inline std::vector<float> impulseResponse (CabSim& cab, const CabSim::Settings& s, double fs,
                                           int length = 16384, int block = 256)
{
    cab.prepare ({ fs, block, 2 });
    cab.setSettings (s);
    cab.reset();
    std::vector<float> in (static_cast<size_t> (length), 0.0f), out (in.size(), 0.0f);
    in[0] = 1.0f;
    Buffer b (2, block);
    for (int start = 0; start < length; start += block)
    {
        const int n = std::min (block, length - start);
        auto view = b.view().subView (0, n);
        cab.process (in.data() + start, view);
        for (int i = 0; i < n; ++i)
            out[static_cast<size_t> (start + i)] =
                (b[0][static_cast<size_t> (i)] + b[1][static_cast<size_t> (i)]) * 0.70710678f;
    }
    return out;
}

/// Complex response of an impulse response at one frequency (direct DFT).
inline std::complex<double> responseAt (const std::vector<float>& ir, double fs, double hz)
{
    std::complex<double> acc = 0.0;
    const std::complex<double> step = std::polar (1.0, -kTwoPiD * hz / fs);
    std::complex<double> rot = 1.0;
    for (float h : ir)
    {
        acc += static_cast<double> (h) * rot;
        rot *= step;
    }
    return acc;
}

inline double responseDb (const std::vector<float>& ir, double fs, double hz)
{
    return 20.0 * std::log10 (std::max (std::abs (responseAt (ir, fs, hz)), 1e-12));
}

/// Power spectrum |H(k)|^2 for k = 0..N/2 of a zero-padded FFT.
inline std::vector<double> powerSpectrum (const std::vector<float>& ir, int n)
{
    PFFFT_Setup* setup = pffft_new_setup (n, PFFFT_REAL);
    auto* buf =
        static_cast<float*> (pffft_aligned_malloc (sizeof (float) * static_cast<size_t> (n)));
    for (int i = 0; i < n; ++i)
        buf[i] = i < static_cast<int> (ir.size()) ? ir[static_cast<size_t> (i)] : 0.0f;
    pffft_transform_ordered (setup, buf, buf, nullptr, PFFFT_FORWARD);
    std::vector<double> p (static_cast<size_t> (n / 2 + 1));
    p[0] = static_cast<double> (buf[0]) * buf[0];
    p[static_cast<size_t> (n / 2)] = static_cast<double> (buf[1]) * buf[1];
    for (int k = 1; k < n / 2; ++k)
        p[static_cast<size_t> (k)] = static_cast<double> (buf[2 * k]) * buf[2 * k] +
                                     static_cast<double> (buf[2 * k + 1]) * buf[2 * k + 1];
    pffft_aligned_free (buf);
    pffft_destroy_setup (setup);
    return p;
}

/// Mean power (dB) of an impulse response's spectrum between lo and hi Hz.
inline double bandDb (const std::vector<float>& ir, double fs, double lo, double hi)
{
    int n = 1;
    while (n < static_cast<int> (ir.size()))
        n *= 2;
    n = std::max (n, 4096);
    const auto p = powerSpectrum (ir, n);
    double sum = 0.0;
    int count = 0;
    for (int k = 0; k <= n / 2; ++k)
    {
        const double hz = k * fs / n;
        if (hz >= lo && hz <= hi)
        {
            sum += p[static_cast<size_t> (k)];
            ++count;
        }
    }
    return 10.0 * std::log10 (std::max (sum / std::max (count, 1), 1e-30));
}

} // namespace sedef::test::cab
