// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestUtils.h"
#include "sedef/Engine.h"
#include "sedef/core/InputFold.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

using namespace sedef;

namespace
{
constexpr double kFs = 48000.0;

/// A decaying 110 Hz note with a few partials, roughly a plucked string at DI level.
std::vector<float> pluck (int frames, float amp = 0.25f)
{
    std::vector<float> v (static_cast<size_t> (frames));
    for (int i = 0; i < frames; ++i)
    {
        const double t = i / kFs;
        const double env = std::exp (-t / 0.6);
        double s = 0.0;
        for (int k = 1; k <= 5; ++k)
            s += std::sin (2.0 * 3.141592653589793 * 110.0 * k * t) / k;
        v[static_cast<size_t> (i)] = static_cast<float> (amp * env * s);
    }
    return v;
}

std::vector<float> fold (InputFold& f, const std::vector<float>& l, const std::vector<float>& r,
                         int block = 64)
{
    std::vector<float> out (l.size());
    for (size_t i = 0; i < l.size(); i += static_cast<size_t> (block))
    {
        const int n =
            static_cast<int> (std::min<size_t> (static_cast<size_t> (block), l.size() - i));
        f.process (l.data() + i, r.data() + i, out.data() + i, n);
    }
    return out;
}

float maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    float m = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max (m, std::abs (a[i] - b[i]));
    return m;
}
} // namespace

TEST_CASE ("InputFold: a guitar on one input passes at unity from the first note", "[core][fold]")
{
    const auto note = pluck (48000);
    for (const bool left : { true, false })
    {
        InputFold f;
        f.prepare (kFs);
        const std::vector<float> silent (note.size(), 0.0f);
        const auto out = left ? fold (f, note, silent) : fold (f, silent, note);
        CHECK (maxAbsDiff (out, note) == 0.0f);
    }
}

TEST_CASE ("InputFold: the interface's noise on the unused input doesn't count as signal",
           "[core][fold]")
{
    const auto note = pluck (48000);
    std::vector<float> hiss (note.size());
    test::fillNoise (hiss, 3.0e-5f, 7); // about -90 dBFS
    InputFold f;
    f.prepare (kFs);
    const auto out = fold (f, note, hiss);
    CHECK (f.leftGain() == 1.0f);
    CHECK (f.rightGain() == 0.0f);
    // Before the note has any level (its first sample is 0) the hiss is still averaged in; from
    // the first sample that carries the note on, the output is the note exactly.
    CHECK (std::abs (out[0]) <= 1.0e-4f);
    float worst = 0.0f;
    for (size_t i = 1; i < note.size(); ++i)
        worst = std::max (worst, std::abs (out[i] - note[i]));
    CHECK (worst == 0.0f);
}

TEST_CASE ("InputFold: a mono source on both inputs and two real signals are averaged",
           "[core][fold]")
{
    const auto note = pluck (24000);
    {
        InputFold f;
        f.prepare (kFs);
        const auto out = fold (f, note, note);
        CHECK (maxAbsDiff (out, note) < 1.0e-7f); // 0.5 x + 0.5 x
    }
    {
        std::vector<float> other (note.size());
        test::fillNoise (other, 0.1f, 3);
        InputFold f;
        f.prepare (kFs);
        (void)fold (f, note, other);
        CHECK (f.leftGain() == 0.5f);
        CHECK (f.rightGain() == 0.5f);
    }
}

TEST_CASE ("InputFold: silence between notes keeps the decision", "[core][fold]")
{
    const auto note = pluck (24000);
    std::vector<float> l = note, r (note.size(), 0.0f);
    std::vector<float> gap (96000);
    test::fillNoise (gap, 3.0e-5f, 11); // two seconds of interface noise on both sides
    l.insert (l.end(), gap.begin(), gap.end());
    r.insert (r.end(), gap.begin(), gap.end());
    l.insert (l.end(), note.begin(), note.end());
    r.insert (r.end(), note.size(), 0.0f);

    InputFold f;
    f.prepare (kFs);
    const auto out = fold (f, l, r);
    CHECK (f.leftGain() == 1.0f);
    const size_t second = note.size() + gap.size();
    float worst = 0.0f;
    for (size_t i = 0; i < note.size(); ++i)
        worst = std::max (worst, std::abs (out[second + i] - note[i]));
    CHECK (worst == 0.0f);
}

TEST_CASE ("InputFold: a change of routing glides without a click", "[core][fold]")
{
    // The left guitar plays alone, then a second signal joins on the right: the gains move from
    // (1, 0) to (0.5, 0.5) at no more than 1 / (50 ms) per sample.
    const int frames = 48000;
    std::vector<float> l (static_cast<size_t> (frames)), r (static_cast<size_t> (frames), 0.0f);
    test::fillSine (l, 220.0, kFs, 0.3f);
    std::vector<float> join (static_cast<size_t> (frames / 2));
    test::fillSine (join, 330.0, kFs, 0.3f);
    std::copy (join.begin(), join.end(), r.begin() + frames / 2);

    InputFold f;
    f.prepare (kFs);
    float prevL = -1.0f, maxStep = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        float y = 0.0f;
        f.process (&l[static_cast<size_t> (i)], &r[static_cast<size_t> (i)], &y, 1);
        if (i > 100) // after the first decision, which snaps by design
            maxStep = std::max (maxStep, std::abs (f.leftGain() - prevL));
        prevL = f.leftGain();
    }
    CHECK (f.leftGain() == 0.5f);
    CHECK (f.rightGain() == 0.5f);
    CHECK (maxStep <= static_cast<float> (1.0 / (0.05 * kFs)) * 1.0001f);
}

TEST_CASE ("Engine: Summed input with one side silent sounds like a mono input", "[engine][fold]")
{
    constexpr int kBlock = 256;
    const auto note = pluck (kBlock * 160);
    const std::vector<float> silent (note.size(), 0.0f);
    auto p = ParamSnapshot::defaults(); // stereoMode 0 = Summed
    Transport t;

    Engine mono, stereo;
    mono.prepare (kFs, kBlock, 1, 2);
    stereo.prepare (kFs, kBlock, 2, 2);
    test::Buffer outMono (2, kBlock), outStereo (2, kBlock);
    float worst = 0.0f, level = 0.0f;
    for (size_t at = 0; at + kBlock <= note.size(); at += kBlock)
    {
        std::array<const float*, 2> inMono { note.data() + at, nullptr };
        std::array<const float*, 2> inStereo { note.data() + at, silent.data() + at };
        mono.process (inMono.data(), outMono.ptrs.data(), kBlock, p, t);
        stereo.process (inStereo.data(), outStereo.ptrs.data(), kBlock, p, t);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
            {
                worst = std::max (worst, std::abs (outMono[c][static_cast<size_t> (i)] -
                                                   outStereo[c][static_cast<size_t> (i)]));
                level = std::max (level, std::abs (outMono[c][static_cast<size_t> (i)]));
            }
    }
    REQUIRE (level > 1.0e-3f);
    CHECK (worst == 0.0f);
}
