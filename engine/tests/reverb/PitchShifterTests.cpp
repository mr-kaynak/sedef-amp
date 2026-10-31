// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ReverbTestUtils.h"
#include "TestUtils.h"
#include "sedef/dsp/PitchShifter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace sedef;
using Catch::Approx;
using PitchShifter = dsp::PitchShifter;

namespace
{
void run (PitchShifter& p, test::Buffer& b, int blockSize, int start = 0, int end = -1)
{
    if (end < 0)
        end = b.frames();
    for (int pos = start; pos < end; pos += blockSize)
        p.process (b.view().subView (pos, std::min (blockSize, end - pos)));
}

constexpr double kFs = 48000.0;
constexpr int kFft = 131072;

/// Shifts a mono signal and returns the spectrum of 2 s of output, after 0.5 s of settling.
std::vector<double> shiftedSpectrum (int semitones, const std::vector<double>& notesHz)
{
    PitchShifter p;
    p.setSettings ({ semitones });
    p.prepare ({ kFs, 256, 1 });
    test::Buffer b (1, static_cast<int> (2.5 * kFs));
    std::vector<float> note (b[0].size());
    for (double hz : notesHz)
    {
        test::fillSine (note, hz, kFs, 0.6f / static_cast<float> (notesHz.size()));
        for (size_t i = 0; i < note.size(); ++i)
            b[0][i] += note[i];
    }
    run (p, b, 256);
    const auto start = static_cast<size_t> (0.5 * kFs);
    return test::magnitudeSpectrum (b[0].data() + start, static_cast<int> (2.0 * kFs), kFft);
}

double db (double x)
{
    return 20.0 * std::log10 (x + 1e-30);
}
} // namespace

TEST_CASE ("PitchShifter output is finite for random settings and full-scale noise",
           "[reverb][transpose]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const int block = GENERATE (1, 17, 64, 512);
    CAPTURE (fs, block);

    PitchShifter p;
    p.prepare ({ fs, block, 2 });
    Random rng (static_cast<std::uint32_t> (fs) * 3u + static_cast<std::uint32_t> (block));
    test::Buffer b (2, static_cast<int> (fs));
    test::fillNoise (b[0], 1.0f, 31);
    test::fillNoise (b[1], 1.0f, 32);

    const int changeEvery = static_cast<int> (0.04 * fs);
    int nextChange = 0;
    for (int pos = 0; pos < b.frames(); pos += block)
    {
        if (pos >= nextChange)
        {
            p.setSettings ({ static_cast<int> (rng.unipolar() * 25.0f) - 12 });
            nextChange += changeEvery;
        }
        p.process (b.view().subView (pos, std::min (block, b.frames() - pos)));
    }
    CHECK (test::allFinite (b[0]));
    CHECK (test::allFinite (b[1]));
    CHECK (test::peak (b[0]) < 3.0f);
    CHECK (test::peak (b[1]) < 3.0f);
}

TEST_CASE ("PitchShifter shifts a sine by the requested interval", "[reverb][transpose]")
{
    const auto [semitones, expectedHz] = GENERATE (table<int, double> ({ { -2, 196.0 },
                                                                         { 12, 440.0 },
                                                                         { -12, 110.0 },
                                                                         { -1, 207.65 },
                                                                         { 5, 293.66 },
                                                                         { -7, 146.83 } }));
    const auto mag = shiftedSpectrum (semitones, { 220.0 });
    const auto peak = test::strongestPeak (mag, kFs, kFft, 40.0, 4000.0);

    // Purity: almost all energy sits at the shifted frequency.
    double inBand = 0.0, total = 0.0;
    for (size_t k = 1; k < mag.size(); ++k)
    {
        const double hz = static_cast<double> (k) * kFs / kFft;
        const double e = mag[k] * mag[k];
        total += e;
        if (std::abs (hz - expectedHz) < 0.03 * expectedHz)
            inBand += e;
    }
    CAPTURE (semitones, peak.hz, inBand / total);
    CHECK (peak.hz == Approx (expectedHz).epsilon (0.005));
    CHECK (inBand / total > 0.9);
}

TEST_CASE ("PitchShifter keeps every note of a chord", "[reverb][transpose]")
{
    // Shifts by 3, 4 or 7 semitones would land a note on another chord tone.
    const int semitones = GENERATE (-2, -5, 2, 12);
    const std::vector<double> chord { 196.0, 246.94, 293.66 }; // G major triad
    const double ratio = std::exp2 (semitones / 12.0);
    const auto mag = shiftedSpectrum (semitones, chord);

    std::vector<test::SpectralPeak> peaks;
    double strongest = 0.0;
    for (double hz : chord)
    {
        const double target = hz * ratio;
        peaks.push_back (test::strongestPeak (mag, kFs, kFft, 0.97 * target, 1.03 * target));
        strongest = std::max (strongest, peaks.back().magnitude);
    }
    for (size_t i = 0; i < chord.size(); ++i)
    {
        const double target = chord[i] * ratio;
        // The unshifted note must be gone (it is >= 2 semitones from every shifted note).
        const auto leftover =
            test::strongestPeak (mag, kFs, kFft, 0.99 * chord[i], 1.01 * chord[i]);
        CAPTURE (semitones, chord[i], target, peaks[i].hz, db (peaks[i].magnitude / strongest),
                 db (leftover.magnitude / strongest));
        CHECK (peaks[i].hz == Approx (target).epsilon (0.005));
        CHECK (db (peaks[i].magnitude / strongest) > -6.0);
        CHECK (db (leftover.magnitude / strongest) < -25.0);
    }
}

TEST_CASE ("PitchShifter at 0 semitones is a bit-exact passthrough", "[reverb][transpose]")
{
    const int channels = GENERATE (1, 2);
    const int block = GENERATE (1, 64, 333);

    SECTION ("from the start")
    {
        PitchShifter p;
        p.prepare ({ 48000.0, block, channels });
        test::Buffer b (channels, 48000);
        for (int c = 0; c < channels; ++c)
            test::fillNoise (b[c], 1.0f, static_cast<std::uint32_t> (40 + c));
        const auto dry = b.data;
        run (p, b, block);
        CHECK (b.data == dry);
    }

    SECTION ("after returning from a shift")
    {
        PitchShifter p;
        p.setSettings ({ 7 });
        p.prepare ({ 48000.0, block, channels });
        test::Buffer b (channels, 48000);
        for (int c = 0; c < channels; ++c)
            test::fillNoise (b[c], 1.0f, static_cast<std::uint32_t> (50 + c));
        const auto dry = b.data;
        const int half = 24000;
        run (p, b, block, 0, half);
        p.setSettings ({ 0 });
        run (p, b, block, half);
        // A splice fade in progress (<= 40 ms), then the transition fade (30 ms), within at most
        // one block: from then on the output is the input.
        const auto settled = static_cast<size_t> (half + 1920 + 1440 + 2 * block);
        for (int c = 0; c < channels; ++c)
        {
            const auto& out = b[c];
            const auto& in = dry[static_cast<size_t> (c)];
            CHECK (std::equal (out.begin() + static_cast<std::ptrdiff_t> (settled), out.end(),
                               in.begin() + static_cast<std::ptrdiff_t> (settled)));
        }
    }
}

TEST_CASE ("PitchShifter semitone automation is click-free", "[reverb][transpose]")
{
    const double fs = 48000.0;
    const int block = 64;
    test::Buffer b (2, static_cast<int> (6.0 * fs));
    test::fillSine (b[0], 110.0, fs, 0.5f);
    test::fillSine (b[1], 165.0, fs, 0.5f);

    PitchShifter p;
    p.prepare ({ fs, block, 2 });
    for (int pos = 0; pos < b.frames(); pos += block)
    {
        const double t = pos / fs;
        p.setSettings ({ static_cast<int> (std::lround (12.0 * std::sin (kTwoPiD * 0.4 * t))) });
        p.process (b.view().subView (pos, std::min (block, b.frames() - pos)));
    }

    // The steepest legitimate slope: a 0.5 * sqrt(2) sine (equal-power fade) at 330 Hz.
    const float slopeLimit = 2.0f * 0.71f * kTwoPi * 330.0f / static_cast<float> (fs);
    for (int c = 0; c < 2; ++c)
    {
        float maxStep = 0.0f;
        for (size_t i = 1; i < b[c].size(); ++i)
            maxStep = std::max (maxStep, std::abs (b[c][i] - b[c][i - 1]));
        CAPTURE (c, maxStep, slopeLimit);
        CHECK (test::allFinite (b[c]));
        CHECK (maxStep < slopeLimit);
    }
}

TEST_CASE ("PitchShifter shifts each stereo channel independently", "[reverb][transpose]")
{
    PitchShifter p;
    p.setSettings ({ -3 });
    p.prepare ({ kFs, 128, 2 });
    test::Buffer b (2, static_cast<int> (2.5 * kFs));
    test::fillSine (b[0], 220.0, kFs, 0.5f);
    test::fillSine (b[1], 330.0, kFs, 0.5f);
    run (p, b, 128);
    const auto start = static_cast<size_t> (0.5 * kFs);
    const double ratio = std::exp2 (-3.0 / 12.0);
    const auto l =
        test::magnitudeSpectrum (b[0].data() + start, static_cast<int> (2.0 * kFs), kFft);
    const auto r =
        test::magnitudeSpectrum (b[1].data() + start, static_cast<int> (2.0 * kFs), kFft);
    CHECK (test::strongestPeak (l, kFs, kFft, 40.0, 4000.0).hz ==
           Approx (220.0 * ratio).epsilon (0.005));
    CHECK (test::strongestPeak (r, kFs, kFft, 40.0, 4000.0).hz ==
           Approx (330.0 * ratio).epsilon (0.005));
}
