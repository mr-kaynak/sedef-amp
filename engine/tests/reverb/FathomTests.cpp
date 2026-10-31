// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ReverbTestUtils.h"
#include "TestUtils.h"
#include "sedef/dsp/Fathom.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace sedef;
using Catch::Approx;
using Fathom = dsp::Fathom;

namespace
{
Fathom::Settings wetOnly (int mode, float decaySeconds)
{
    Fathom::Settings s;
    s.enabled = true;
    s.mode = mode;
    s.mix = 1.0f;
    s.decaySeconds = decaySeconds;
    s.lowCutHz = 20.0f;
    s.highCutHz = 5000.0f;
    s.preDelayMs = 1.0f;
    return s;
}

void run (Fathom& f, test::Buffer& b, int blockSize, int start = 0, int end = -1)
{
    if (end < 0)
        end = b.frames();
    for (int pos = start; pos < end; pos += blockSize)
        f.process (b.view().subView (pos, std::min (blockSize, end - pos)));
}

Fathom::Settings randomSettings (Random& rng)
{
    Fathom::Settings s;
    s.enabled = rng.unipolar() > 0.2f;
    s.mode = rng.unipolar() > 0.5f ? Fathom::Hall : Fathom::Plate;
    s.mix = rng.unipolar();
    s.decaySeconds = mapExp (rng.unipolar(), 1.0f, 60.0f);
    s.lowCutHz = mapExp (rng.unipolar(), 20.0f, 500.0f);
    s.highCutHz = mapExp (rng.unipolar(), 800.0f, 5000.0f);
    s.preDelayMs = mapExp (rng.unipolar(), 1.0f, 200.0f);
    return s;
}

float windowRms (const std::vector<float>& v, double fs, double from, double to)
{
    const auto a = static_cast<size_t> (from * fs);
    const auto b = static_cast<size_t> (to * fs);
    return test::rms (v.data() + a, static_cast<int> (b - a));
}
} // namespace

TEST_CASE ("Fathom output is finite for random settings and full-scale noise", "[reverb][fathom]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0);
    const int block = GENERATE (1, 17, 64, 512);
    CAPTURE (fs, block);

    Fathom f;
    f.prepare ({ fs, block, 2 });
    Random rng (static_cast<std::uint32_t> (fs) + static_cast<std::uint32_t> (block));
    test::Buffer b (2, static_cast<int> (fs));
    test::fillNoise (b[0], 1.0f, 11);
    test::fillNoise (b[1], 1.0f, 12);

    const int changeEvery = static_cast<int> (0.05 * fs);
    int nextChange = 0;
    for (int pos = 0; pos < b.frames(); pos += block)
    {
        if (pos >= nextChange)
        {
            f.setSettings (randomSettings (rng));
            nextChange += changeEvery;
        }
        f.process (b.view().subView (pos, std::min (block, b.frames() - pos)));
    }
    CHECK (test::allFinite (b[0]));
    CHECK (test::allFinite (b[1]));
    CHECK (test::peak (b[0]) < 50.0f);
    CHECK (test::peak (b[1]) < 50.0f);
}

TEST_CASE ("Fathom decay matches the RT60 setting", "[reverb][fathom]")
{
    const int mode = GENERATE (Fathom::Plate, Fathom::Hall);
    const float decay = GENERATE (2.0f, 5.0f);
    const double fs = 48000.0;

    Fathom f;
    f.setSettings (wetOnly (mode, decay));
    f.prepare ({ fs, 512, 2 });
    test::Buffer b (2, static_cast<int> ((1.3 * decay + 0.3) * fs));
    b[0][0] = b[1][0] = 1.0f;
    run (f, b, 512);

    // RT60 is quoted at mid frequencies (500 Hz - 1 kHz octaves).
    test::bandLimit (b[0], fs, 350.0, 1400.0);
    test::bandLimit (b[1], fs, 350.0, 1400.0);
    const double rt60 = test::rt60FromDecay (b[0], b[1], fs);
    CAPTURE (mode, decay, rt60);
    CHECK (rt60 == Approx (decay).epsilon (0.2));
}

TEST_CASE ("Fathom with a 60 s decay sustains but stays bounded", "[reverb][fathom]")
{
    const int mode = GENERATE (Fathom::Plate, Fathom::Hall);
    const double fs = 48000.0;
    const int second = static_cast<int> (fs);

    Fathom f;
    f.setSettings (wetOnly (mode, 60.0f));
    f.prepare ({ fs, 512, 2 });

    test::Buffer b (2, second);
    std::vector<float> rmsPerSecond;
    float maxPeak = 0.0f;
    bool finite = true;
    Random rng (99);
    for (int s = 0; s < 130; ++s)
    {
        const bool noise = s < 120;
        for (int i = 0; i < second; ++i)
        {
            b[0][static_cast<size_t> (i)] = noise ? rng.bipolar() : 0.0f;
            b[1][static_cast<size_t> (i)] = noise ? rng.bipolar() : 0.0f;
        }
        run (f, b, 512);
        finite = finite && test::allFinite (b[0]) && test::allFinite (b[1]);
        maxPeak = std::max ({ maxPeak, test::peak (b[0]), test::peak (b[1]) });
        rmsPerSecond.push_back (std::hypot (test::rms (b[0]), test::rms (b[1])));
    }
    CAPTURE (mode, maxPeak, rmsPerSecond[10], rmsPerSecond[59], rmsPerSecond[119],
             rmsPerSecond[129]);
    CHECK (finite);
    CHECK (maxPeak < 100.0f);
    // Steady state: no growth over the second minute.
    CHECK (rmsPerSecond[119] < 1.2f * rmsPerSecond[59]);
    // Ten seconds after the input stops, a 60 s tail has lost only about 10 dB.
    CHECK (rmsPerSecond[129] > 0.15f * rmsPerSecond[119]);
}

TEST_CASE ("Fathom at mix 0 is a dry passthrough", "[reverb][fathom]")
{
    const int mode = GENERATE (Fathom::Plate, Fathom::Hall);
    Fathom::Settings s = wetOnly (mode, 4.0f);
    s.mix = 0.0f;
    Fathom f;
    f.setSettings (s);
    f.prepare ({ 48000.0, 64, 2 });

    test::Buffer b (2, 48000);
    test::fillNoise (b[0], 0.8f, 3);
    test::fillNoise (b[1], 0.8f, 4);
    const auto dryL = b[0];
    const auto dryR = b[1];
    run (f, b, 64);
    CHECK (b[0] == dryL);
    CHECK (b[1] == dryR);
}

TEST_CASE ("Fathom spillover: disabling lets the tail ring out, then idles", "[reverb][fathom]")
{
    const int mode = GENERATE (Fathom::Plate, Fathom::Hall);
    const double fs = 48000.0;
    const int on = static_cast<int> (0.5 * fs);

    SECTION ("tail continues after disabling, then dies")
    {
        Fathom f;
        auto s = wetOnly (mode, 1.5f);
        f.setSettings (s);
        f.prepare ({ fs, 64, 2 });
        test::Buffer b (2, static_cast<int> (6.0 * fs));
        Random rng (1);
        for (int i = 0; i < on; ++i)
            b[0][static_cast<size_t> (i)] = b[1][static_cast<size_t> (i)] = 0.5f * rng.bipolar();
        run (f, b, 64, 0, on);
        s.enabled = false;
        f.setSettings (s);
        run (f, b, 64, on);

        CHECK (windowRms (b[0], fs, 0.6, 0.8) > 1.0e-2f);
        CHECK (windowRms (b[0], fs, 1.5, 1.7) > 1.0e-4f);
        // Silent input, tail gone: the output is exactly the (zero) input.
        CHECK (windowRms (b[0], fs, 5.0, 6.0) == 0.0f);
        CHECK (windowRms (b[1], fs, 5.0, 6.0) == 0.0f);
    }

    SECTION ("new input does not enter after disabling")
    {
        Fathom f;
        auto s = wetOnly (mode, 1.5f);
        f.setSettings (s);
        f.prepare ({ fs, 64, 2 });
        test::Buffer b (2, static_cast<int> (6.0 * fs));
        test::fillNoise (b[0], 0.5f, 21);
        test::fillNoise (b[1], 0.5f, 22);
        const auto dryL = b[0];
        run (f, b, 64, 0, on);
        s.enabled = false;
        f.setSettings (s);
        run (f, b, 64, on);

        // Wet part = output - input (dry gain is back at unity).
        std::vector<float> wet (b[0].size());
        for (size_t i = 0; i < wet.size(); ++i)
            wet[i] = b[0][i] - dryL[i];
        CHECK (windowRms (wet, fs, 0.6, 0.8) > 1.0e-2f);
        CHECK (windowRms (wet, fs, 5.0, 6.0) == 0.0f);
    }
}

TEST_CASE ("Fathom left and right outputs are decorrelated", "[reverb][fathom]")
{
    const int mode = GENERATE (Fathom::Plate, Fathom::Hall);
    const double fs = 48000.0;
    Fathom f;
    f.setSettings (wetOnly (mode, 2.5f));
    f.prepare ({ fs, 256, 2 });

    // Mono source: identical left and right input.
    test::Buffer b (2, static_cast<int> (3.0 * fs));
    test::fillNoise (b[0], 0.5f, 5);
    std::copy (b[0].begin(), b[0].end(), b[1].begin());
    run (f, b, 256);
    const auto start = static_cast<int> (0.5 * fs);
    const double rho =
        test::correlation (b[0].data() + start, b[1].data() + start, b.frames() - start);
    CAPTURE (mode, rho);
    CHECK (std::abs (rho) < 0.5);
}

TEST_CASE ("Fathom high cut removes energy above 5 kHz", "[reverb][fathom]")
{
    const int mode = GENERATE (Fathom::Plate, Fathom::Hall);
    const double fs = 48000.0;
    const int fft = 65536;

    auto render = [&] (float highCut) {
        auto s = wetOnly (mode, 2.0f);
        s.highCutHz = highCut;
        Fathom f;
        f.setSettings (s);
        f.prepare ({ fs, 256, 2 });
        test::Buffer b (2, static_cast<int> (2.0 * fs));
        test::fillNoise (b[0], 0.5f, 8);
        std::copy (b[0].begin(), b[0].end(), b[1].begin());
        run (f, b, 256);
        const auto start = static_cast<size_t> (fs);
        return test::magnitudeSpectrum (b[0].data() + start, fft / 2, fft);
    };

    std::vector<float> input (static_cast<size_t> (fft / 2));
    test::fillNoise (input, 0.5f, 8);
    const auto inSpec = test::magnitudeSpectrum (input.data(), fft / 2, fft);
    const auto bright = render (5000.0f);
    const auto dark = render (800.0f);

    auto fractionAbove = [&] (const std::vector<double>& m) {
        return test::energyAbove (m, fs, fft, 5000.0) / test::energyAbove (m, fs, fft, 0.0);
    };
    const double darkVsBrightDb = 10.0 * std::log10 (test::energyAbove (dark, fs, fft, 5000.0) /
                                                     test::energyAbove (bright, fs, fft, 5000.0));
    CAPTURE (mode, fractionAbove (inSpec), fractionAbove (bright), darkVsBrightDb);
    CHECK (fractionAbove (bright) < 0.5 * fractionAbove (inSpec));
    CHECK (darkVsBrightDb < -20.0);
}

TEST_CASE ("Fathom automation and mode or enable switching are click-free", "[reverb][fathom]")
{
    const double fs = 48000.0;
    const int block = 64;
    const int frames = static_cast<int> (8.0 * fs);

    test::Buffer b (2, frames);
    test::fillSine (b[0], 220.0, fs, 0.5f);
    std::copy (b[0].begin(), b[0].end(), b[1].begin());

    Fathom f;
    Fathom::Settings s = wetOnly (Fathom::Plate, 3.0f);
    s.mix = 0.5f;
    f.setSettings (s);
    f.prepare ({ fs, block, 2 });

    auto sweep = [] (double t, double hz) {
        return static_cast<float> (0.5 + 0.5 * std::sin (kTwoPiD * hz * t));
    };
    for (int pos = 0; pos < frames; pos += block)
    {
        const double t = pos / fs;
        s.mix = sweep (t, 0.37);
        s.decaySeconds = mapExp (sweep (t, 0.23), 1.0f, 60.0f);
        s.lowCutHz = mapExp (sweep (t, 0.31), 20.0f, 500.0f);
        s.highCutHz = mapExp (sweep (t, 0.41), 800.0f, 5000.0f);
        s.preDelayMs = mapExp (sweep (t, 0.29), 1.0f, 200.0f);
        s.mode = static_cast<int> (t / 0.5) % 2;
        s.enabled = !((t > 2.0 && t < 2.6) || (t > 4.1 && t < 4.3) || t > 6.5);
        f.setSettings (s);
        f.process (b.view().subView (pos, std::min (block, frames - pos)));
    }

    CHECK (test::allFinite (b[0]));
    const float clicksL = test::clickPeak (b[0], fs, 8000.0, 4800);
    const float clicksR = test::clickPeak (b[1], fs, 8000.0, 4800);
    CAPTURE (clicksL, clicksR);
    CHECK (clicksL < 5.0e-3f);
    CHECK (clicksR < 5.0e-3f);
}

TEST_CASE ("Fathom handles mono buffers and reset", "[reverb][fathom]")
{
    Fathom f;
    f.setSettings (wetOnly (Fathom::Hall, 3.0f));
    f.prepare ({ 48000.0, 128, 1 });
    test::Buffer mono (1, 48000);
    test::fillNoise (mono[0], 0.5f, 9);
    run (f, mono, 128);
    CHECK (test::allFinite (mono[0]));
    CHECK (test::rms (mono[0]) > 0.01f);

    f.reset();
    test::Buffer silent (1, 4800);
    run (f, silent, 128);
    CHECK (test::peak (silent[0]) == 0.0f);
}
