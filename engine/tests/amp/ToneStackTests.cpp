// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestUtils.h"
#include "sedef/dsp/amp/ToneStack.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <complex>

using namespace sedef;
using namespace sedef::dsp::amp;
using Catch::Approx;
using cplx = std::complex<double>;

namespace
{
/// Independent reference: writes the nodal equations of the passive network and solves them
/// with complex Gaussian elimination at one frequency. Shares nothing with the closed form.
///
/// Nodes: A (C1 / treble top), O (treble wiper = output), B (treble bottom / bass), C (bass /
/// mid top), W (mid wiper), D (slope resistor / C2 / C3). The input is an ideal 1 V source.
cplx nodalResponse (const ToneStackComponents& c, double t, double m, double l, double hz)
{
    enum
    {
        A,
        O,
        B,
        C,
        W,
        D,
        N
    };
    std::array<std::array<cplx, N>, N> y {};
    std::array<cplx, N> rhs {};
    const cplx s (0.0, kTwoPiD * hz);

    // A pot segment of zero ohms becomes a 1 milliohm resistor (keeps the matrix regular).
    const auto res = [] (double r) {
        return cplx (1.0 / std::max (r, 1.0e-3), 0.0);
    };
    const auto between = [&] (int i, int j, cplx g) {
        y[i][i] += g;
        y[j][j] += g;
        y[i][j] -= g;
        y[j][i] -= g;
    };
    const auto toInput = [&] (int i, cplx g) {
        y[i][i] += g;
        rhs[i] += g; // times Vin = 1
    };
    const auto toGround = [&] (int i, cplx g) {
        y[i][i] += g;
    };

    toInput (A, s * c.c1);
    between (A, O, res ((1.0 - t) * c.r1));
    between (O, B, res (t * c.r1));
    between (B, C, res (l * c.r2));
    between (C, W, res ((1.0 - m) * c.r3));
    toGround (W, res (m * c.r3));
    toInput (D, res (c.r4));
    between (D, B, s * c.c2);
    between (D, W, s * c.c3);

    for (int col = 0; col < N; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < N; ++r)
            if (std::abs (y[r][col]) > std::abs (y[pivot][col]))
                pivot = r;
        std::swap (y[col], y[pivot]);
        std::swap (rhs[col], rhs[pivot]);
        for (int r = col + 1; r < N; ++r)
        {
            const cplx f = y[r][col] / y[col][col];
            for (int k = col; k < N; ++k)
                y[r][k] -= f * y[col][k];
            rhs[r] -= f * rhs[col];
        }
    }
    std::array<cplx, N> v {};
    for (int r = N - 1; r >= 0; --r)
    {
        cplx acc = rhs[r];
        for (int k = r + 1; k < N; ++k)
            acc -= y[r][k] * v[k];
        v[r] = acc / y[r][r];
    }
    return v[O];
}

cplx analogResponse (const ToneStackAnalog& h, double hz)
{
    const cplx s (0.0, kTwoPiD * hz);
    cplx num = 0.0, den = 0.0, sk = 1.0;
    for (size_t k = 0; k < 4; ++k, sk *= s)
    {
        num += h.b[k] * sk;
        den += h.a[k] * sk;
    }
    return num / den;
}

cplx digitalResponse (const ToneStackCoeffs& c, double hz, double fs)
{
    const cplx z1 = std::polar (1.0, -kTwoPiD * hz / fs);
    cplx num = 0.0, den = 0.0, zk = 1.0;
    for (size_t k = 0; k < 4; ++k, zk *= z1)
    {
        num += c.b[k] * zk;
        den += c.a[k] * zk;
    }
    return num / den;
}

double db (cplx h)
{
    return 20.0 * std::log10 (std::abs (h));
}

struct Setting
{
    double t, m, l;
};

std::vector<Setting> settings()
{
    std::vector<Setting> v { { 0.0, 0.0, 0.0 },   { 1.0, 1.0, 1.0 }, { 0.5, 0.5, 0.1 },
                             { 0.68, 0.32, 0.1 }, { 0.2, 0.8, 0.5 }, { 0.9, 0.05, 0.02 },
                             { 0.0, 1.0, 1.0 },   { 1.0, 0.0, 0.0 } };
    Random r (42);
    for (int i = 0; i < 24; ++i)
        v.push_back ({ r.unipolar(), r.unipolar(), logTaper (r.unipolar()) });
    return v;
}

std::vector<double> logFrequencies (double lo, double hi, int n)
{
    std::vector<double> f (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        f[static_cast<size_t> (i)] = lo * std::pow (hi / lo, i / (n - 1.0));
    return f;
}

const ToneStackComponents kShiftedStack { 470e-12, 18e-9, 18e-9, 220e3, 1e6, 25e3, 33e3 };
} // namespace

TEST_CASE ("Tone stack closed form matches an independent nodal analysis", "[amp][tonestack]")
{
    const auto parts = GENERATE (kTweedStack, kBritishStack, kShiftedStack);
    double worst = 0.0;
    for (const auto& st : settings())
    {
        const auto h = toneStackAnalog (parts, st.t, st.m, st.l);
        for (double f : logFrequencies (20.0, 20000.0, 200))
            worst = std::max (worst, std::abs (db (analogResponse (h, f)) -
                                               db (nodalResponse (parts, st.t, st.m, st.l, f))));
    }
    CAPTURE (worst);
    CHECK (worst < 0.01);
}

TEST_CASE ("Tone stack bilinear transform is exact at the warped frequency", "[amp][tonestack]")
{
    // The bilinear transform maps analog frequency (2 fs / 2pi) tan(pi f / fs) onto digital f, so
    // comparing there isolates the discretisation arithmetic from the (expected) warp.
    const double fs = GENERATE (44100.0, 48000.0, 176400.0, 192000.0);
    double worst = 0.0;
    for (const auto& parts : { kTweedStack, kBritishStack })
        for (const auto& st : settings())
        {
            const auto d = toneStackDigital (toneStackAnalog (parts, st.t, st.m, st.l), fs);
            for (double f : logFrequencies (20.0, std::min (20000.0, 0.45 * fs), 120))
            {
                const double warped = fs / kPiD * std::tan (kPiD * f / fs);
                const double err = std::abs (db (digitalResponse (d, f, fs)) -
                                             db (nodalResponse (parts, st.t, st.m, st.l, warped)));
                worst = std::max (worst, err);
            }
        }
    CAPTURE (worst);
    CHECK (worst < 0.1);
}

TEST_CASE ("Tone stack at the oversampled rate tracks the circuit across the audio band",
           "[amp][tonestack]")
{
    // What the amps actually run: no pre-warp at >= 176.4 kHz.
    const double fs = GENERATE (176400.0, 192000.0);
    double worstTo10k = 0.0, worstTo20k = 0.0;
    for (const auto& parts : { kTweedStack, kBritishStack, kShiftedStack })
        for (const auto& st : settings())
        {
            const auto d = toneStackDigital (toneStackAnalog (parts, st.t, st.m, st.l), fs);
            for (double f : logFrequencies (20.0, 20000.0, 200))
            {
                const double err = std::abs (db (digitalResponse (d, f, fs)) -
                                             db (nodalResponse (parts, st.t, st.m, st.l, f)));
                double& worst = f <= 10000.0 ? worstTo10k : worstTo20k;
                worst = std::max (worst, err);
            }
        }
    CAPTURE (worstTo10k, worstTo20k);
    CHECK (worstTo10k < 0.1);
    CHECK (worstTo20k < 0.5);
}

TEST_CASE ("Tone stack filter output matches its designed response", "[amp][tonestack]")
{
    const double fs = 192000.0;
    const auto d = toneStackDigital (toneStackAnalog (kBritishStack, 0.7, 0.3, logTaper (0.6)), fs);
    for (double f : { 60.0, 400.0, 700.0, 3000.0, 12000.0 })
    {
        ToneStackFilter filter;
        filter.setCoeffs (d);
        const double measured = test::measureGainDb (
            [&] (AudioView v) {
                float* p = v.channel (0);
                for (int i = 0; i < v.numFrames(); ++i)
                    p[i] = filter.processSample (0, p[i]);
            },
            f, fs, 1 << 17);
        CHECK (measured == Approx (db (digitalResponse (d, f, fs))).margin (0.05));
    }
}

TEST_CASE ("Bass pot log taper", "[amp][tonestack]")
{
    CHECK (logTaper (0.0) == 0.0);
    CHECK (logTaper (0.5) == Approx (0.1));
    CHECK (logTaper (1.0) == Approx (1.0));
    double prev = -1.0;
    for (int i = 0; i <= 100; ++i)
    {
        const double v = logTaper (i / 100.0);
        CHECK (v > prev);
        prev = v;
    }
}
