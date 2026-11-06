// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Trench.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// Level detector: rumble and pick noise out, then mean |x|.
constexpr double kDetectorHpHz = 45.0;
constexpr double kDetectorHpQ = 0.5;
constexpr double kDetectorLpHz = 1100.0;
constexpr double kDetectorLpQ = 0.7071;

// Divider: a Schmitt trigger on the cosine of the half phase, so phase wiggles of up to
// asin (0.25) around a crossing cannot toggle the flip-flop twice.
constexpr float kHalfHysteresis = 0.25f;

// Envelope and gate.
constexpr double kLevelSeconds = 0.010; // mean |x|
constexpr float kOpenLevel = 0.0025f;   // -52 dBFS mean |x|
constexpr float kCloseLevel = 0.0012f;  // -58 dBFS
constexpr double kGateAttackSeconds = 0.004;
constexpr double kGateReleaseSeconds = 0.015;
constexpr float kSubScale = 0.95f; // at noon the sub is within ~2 dB of the note's level

constexpr float kToneDarkHz = 150.0f;
constexpr float kToneBrightHz = 3000.0f;
constexpr double kToneQ = 0.6;
constexpr double kDcHz = 10.0;

constexpr double kGainRampSeconds = 0.03;
constexpr double kFilterSmoothingSeconds = 0.02;

float onePole (double seconds, double fs) noexcept
{
    return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * fs)));
}
} // namespace

void Trench::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);

    levelCoeff_ = onePole (kLevelSeconds, sampleRate_);
    gateAttack_ = onePole (kGateAttackSeconds, sampleRate_);
    gateRelease_ = onePole (kGateReleaseSeconds, sampleRate_);

    direct_.reset (sampleRate_, kGainRampSeconds);
    sub1_.reset (sampleRate_, kGainRampSeconds);
    sub2_.reset (sampleRate_, kGainRampSeconds);
    tone_.reset (sampleRate_, kFilterSmoothingSeconds);

    for (auto& t : trackers_)
    {
        t.mono.prepare (sampleRate_);
        t.hp.c = design::highpass (sampleRate_, kDetectorHpHz, kDetectorHpQ);
        t.lp.c = design::lowpass (sampleRate_, kDetectorLpHz, kDetectorLpQ);
        t.dc.c = design::highpass1 (sampleRate_, kDcHz);
    }
    applySettings (true);
    reset();
}

void Trench::reset() noexcept
{
    for (auto& t : trackers_)
    {
        t.mono.reset();
        for (auto* s : { &t.hp, &t.lp, &t.tone, &t.dc })
            s->clear();
        t.tone.c = toneCoeffs_;
        t.level = 0.0f;
        t.halfRe = 1.0f;
        t.halfIm = 0.0f;
        t.previousHalf = 1.0f;
        t.sub1 = {};
        t.sub2 = {};
        t.gate = 0.0f;
        t.levelOpen = false;
        t.pendingAmp = 0.0f;
    }
    primed_ = false;
}

void Trench::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

Trench::TrackState Trench::trackState() const noexcept
{
    const auto& t = trackers_[0];
    return { t.mono.hz(), t.mono.clarity(), t.gate };
}

void Trench::applySettings (bool snap) noexcept
{
    const float direct = 2.0f * std::clamp (settings_.direct, 0.0f, 1.0f);
    const float sub1 = 2.0f * kSubScale * std::clamp (settings_.sub1, 0.0f, 1.0f);
    const float sub2 = 2.0f * kSubScale * std::clamp (settings_.sub2, 0.0f, 1.0f);
    const float tone = std::clamp (settings_.tone, 0.0f, 1.0f);
    if (snap)
    {
        direct_.setCurrentAndTarget (direct);
        sub1_.setCurrentAndTarget (sub1);
        sub2_.setCurrentAndTarget (sub2);
        tone_.setCurrentAndTarget (tone);
        updateTone (tone);
    }
    else
    {
        direct_.setTarget (direct);
        sub1_.setTarget (sub1);
        sub2_.setTarget (sub2);
        tone_.setTarget (tone);
    }
}

void Trench::updateTone (float tone) noexcept
{
    toneCoeffs_ = design::lowpass (sampleRate_, mapExp (tone, kToneDarkHz, kToneBrightHz), kToneQ);
    for (auto& t : trackers_)
        t.tone.c = toneCoeffs_;
}

void Trench::flush (Tracker& t) noexcept
{
    // Silence decays every state towards zero; keep it out of the denormal range.
    for (auto* section : { &t.hp, &t.lp, &t.tone, &t.dc })
        section->flush();
    for (auto* v : { &t.level, &t.gate, &t.pendingAmp })
        *v = std::abs (*v) < 1.0e-20f ? 0.0f : *v;
}

float Trench::subSample (Tracker& t, float x, float g1, float g2) noexcept
{
    // 1. Level: band-limited mean |x|.
    const float det = t.lp.process (t.hp.process (x));
    t.level += levelCoeff_ * (std::abs (det) - t.level);

    // 2. The note and its fundamental. The fundamental's phase is halved by following it
    //    sample by sample: the half angle turns by half of each step, so it stays continuous.
    t.mono.push (x);
    const float zr = t.mono.fundamentalRe();
    const float zi = t.mono.fundamentalIm();
    const float mag2 = zr * zr + zi * zi;
    if (mag2 > 1.0e-24f)
    {
        const float inv = 1.0f / std::sqrt (mag2);
        const float ur = zr * inv, ui = zi * inv;
        // v = u * conj (half)^2: the step from the doubled half angle to the new phase.
        const float cr = t.halfRe * t.halfRe - t.halfIm * t.halfIm;
        const float ci = -2.0f * t.halfRe * t.halfIm;
        const float vr = ur * cr - ui * ci;
        const float vi = ur * ci + ui * cr;
        const float sr = std::sqrt (std::max (0.0f, 0.5f * (1.0f + vr)));
        const float si = std::copysign (std::sqrt (std::max (0.0f, 0.5f * (1.0f - vr))), vi);
        const float nr = t.halfRe * sr - t.halfIm * si;
        const float ni = t.halfRe * si + t.halfIm * sr;
        const float norm = 1.0f / std::sqrt (nr * nr + ni * ni);
        t.halfRe = nr * norm;
        t.halfIm = ni * norm;
    }

    // 3. Schmitt trigger on cos (phase / 2): one edge per period of the note.
    bool edge = false;
    double ago = 0.0; // how long before this sample the edge happened, 0..1 samples
    {
        const float c = t.halfRe;
        const float threshold = t.sub1.value > 0.0f ? -kHalfHysteresis : kHalfHysteresis;
        if (t.sub1.value > 0.0f ? c < threshold : c > threshold)
        {
            const float before = t.previousHalf - threshold;
            const float after = c - threshold;
            const float frac = before != after ? before / (before - after) : 1.0f;
            ago = 1.0 - static_cast<double> (std::clamp (frac, 0.0f, 1.0f));
            edge = true;
        }
        t.previousHalf = c;
    }

    // 4. Flip-flops, with polyBLEP corrections split between the previous sample (still pending,
    //    output now) and this one. Sub -1 follows the trigger; sub -2 toggles on its falling edges.
    float out1 = t.sub1.pending;
    float out2 = t.sub2.pending;
    float now1 = t.sub1.value;
    float now2 = t.sub2.value;
    if (edge)
    {
        const auto before = static_cast<float> (ago * ago);
        const auto after = static_cast<float> ((1.0 - ago) * (1.0 - ago));
        const float h1 = -2.0f * t.sub1.value;
        t.sub1.value = -t.sub1.value;
        out1 += 0.5f * h1 * before;
        now1 = t.sub1.value - 0.5f * h1 * after;
        if (t.sub1.value < 0.0f)
        {
            const float h2 = -2.0f * t.sub2.value;
            t.sub2.value = -t.sub2.value;
            out2 += 0.5f * h2 * before;
            now2 = t.sub2.value - 0.5f * h2 * after;
        }
    }
    t.sub1.pending = now1;
    t.sub2.pending = now2;

    // 5. Gate and envelope: the subs speak only while there is a note and the tracker holds it.
    if (t.levelOpen ? t.level < kCloseLevel : t.level > kOpenLevel)
        t.levelOpen = !t.levelOpen;
    const float target = t.levelOpen && t.mono.voiced() ? 1.0f : 0.0f;
    t.gate += (target > t.gate ? gateAttack_ : gateRelease_) * (target - t.gate);

    const float amp = t.pendingAmp;
    t.pendingAmp = t.gate * t.level * (0.5f * kPi);
    const float sub = amp * (g1 * out1 + g2 * out2);
    return t.dc.process (t.tone.process (sub));
}

void Trench::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    if (n == 0 || channels == 0)
        return;

    for (int start = 0; start < n; start += kControlBlock)
    {
        const int end = std::min (n, start + kControlBlock);
        if (tone_.isSmoothing())
            updateTone (tone_.skip (end - start));
        for (int ch = 0; ch < channels; ++ch)
            flush (trackers_[static_cast<size_t> (ch)]);

        for (int i = start; i < end; ++i)
        {
            const float gd = direct_.next();
            const float g1 = sub1_.next();
            const float g2 = sub2_.next();
            for (int ch = 0; ch < channels; ++ch)
            {
                float& x = io.channel (ch)[i];
                const float sub = subSample (trackers_[static_cast<size_t> (ch)], x, g1, g2);
                x = gd * x + sub;
            }
        }
    }
}

} // namespace sedef::dsp
