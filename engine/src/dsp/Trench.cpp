// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Trench.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// Detector: rumble and pick noise out, then the tracking low-pass (two Butterworth sections whose
// corner sits kTrackRatio above the note being divided). While it has no note, the corner rests
// low, where every guitar fundamental's harmonics are attenuated more than the fundamental.
constexpr double kDetectorHpHz = 45.0;
constexpr double kDetectorHpQ = 0.5;
constexpr double kDetectorLpHz = 1100.0;
constexpr double kDetectorLpQ = 0.7071;
constexpr float kTrackRatio = 1.5f;
constexpr float kSearchHz = 200.0f;
constexpr float kMinTrackHz = 60.0f;
constexpr float kMaxTrackHz = 1100.0f;
constexpr float kCornerDeadband = 0.12f;
constexpr double kTrackGlideSeconds = 0.008;
constexpr double kSearchGlideSeconds = 0.02;

// Schmitt trigger: thresholds at +-kHysteresis of the filtered signal's recent peak.
constexpr float kHysteresis = 0.3f;
constexpr double kLowestHz = 40.0;    // longer periods mean the tracker has lost the note
constexpr double kHighestHz = 1400.0; // shorter ones are noise, not a note
constexpr double kAgreement = 0.08;   // consecutive periods within 8 % count as the same note

// Envelope and gate.
constexpr double kLevelSeconds = 0.010; // mean |x|
constexpr float kOpenLevel = 0.0025f;   // -52 dBFS mean |x|
constexpr float kCloseLevel = 0.0012f;  // -58 dBFS
constexpr double kGateAttackSeconds = 0.004;
constexpr double kGateReleaseSeconds = 0.015;
constexpr double kConfidenceSeconds = 0.006;
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
    confidenceCoeff_ = onePole (kConfidenceSeconds, sampleRate_);
    minPeriod_ = sampleRate_ / kHighestHz;
    maxPeriod_ = sampleRate_ / kLowestHz;

    direct_.reset (sampleRate_, kGainRampSeconds);
    sub1_.reset (sampleRate_, kGainRampSeconds);
    sub2_.reset (sampleRate_, kGainRampSeconds);
    tone_.reset (sampleRate_, kFilterSmoothingSeconds);

    applySettings (true);
    reset();
}

void Trench::reset() noexcept
{
    for (auto& t : trackers_)
    {
        t = Tracker {};
        t.hp.c = design::highpass (sampleRate_, kDetectorHpHz, kDetectorHpQ);
        t.lp.c = design::lowpass (sampleRate_, kDetectorLpHz, kDetectorLpQ);
        t.dc.c = design::highpass1 (sampleRate_, kDcHz);
        t.tone.c = toneCoeffs_;
        t.trackHz = t.targetHz = kSearchHz;
        updateTracking (t);
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
    return { t.period > 0.0 ? static_cast<float> (sampleRate_ / t.period) : 0.0f, t.confidence,
             t.gate };
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

void Trench::updateTracking (Tracker& t) noexcept
{
    // Once per control block: the tracking corner glides to the note (or back to rest). It follows
    // the median of the last three agreeing periods and only moves for a real change of note:
    // a corner that chased every period would shift the filter's delay, and with it the next edge,
    // and the tracker would start to wobble around the note.
    const bool tracking = t.period > 0.0 && t.recentCount >= 3;
    if (tracking)
    {
        auto r = t.recent;
        std::sort (r.begin(), r.end());
        const float hz = std::clamp (kTrackRatio * static_cast<float> (sampleRate_ / r[1]),
                                     kMinTrackHz, kMaxTrackHz);
        if (std::abs (hz - t.targetHz) > kCornerDeadband * t.targetHz)
            t.targetHz = hz;
    }
    else
    {
        t.targetHz = kSearchHz;
    }
    const double glide = tracking ? kTrackGlideSeconds : kSearchGlideSeconds;
    const auto k = static_cast<float> (1.0 - std::exp (-kControlBlock / (glide * sampleRate_)));
    t.trackHz += k * (t.targetHz - t.trackHz);
    t.track1.c = design::lowpass (sampleRate_, t.trackHz, 0.7071);
    t.track2.c = t.track1.c;
}

void Trench::flush (Tracker& t) noexcept
{
    // Silence decays every state towards zero; keep it out of the denormal range.
    for (auto* section : { &t.hp, &t.lp, &t.track1, &t.track2, &t.tone, &t.dc })
        section->flush();
    for (auto* v : { &t.level, &t.peak, &t.gate, &t.pendingAmp })
        *v = std::abs (*v) < 1.0e-20f ? 0.0f : *v;
}

float Trench::subSample (Tracker& t, float x, float g1, float g2) noexcept
{
    // 1. Detector: band-limit, envelope, tracking low-pass, Schmitt trigger.
    const float det = t.lp.process (t.hp.process (x));
    t.level += levelCoeff_ * (std::abs (det) - t.level);
    const float d = t.track2.process (t.track1.process (det));

    // The peak decays over about a period and a half of the note, so the thresholds follow a
    // fading string without dropping below the harmonics' wiggles.
    const double releaseSamples = std::clamp (t.period > 0.0 ? 1.5 * t.period : 0.01 * sampleRate_,
                                              0.003 * sampleRate_, 0.03 * sampleRate_);
    const auto release = static_cast<float> (std::exp (-1.0 / releaseSamples));
    t.peak = std::max (std::abs (d), t.peak * release);
    const float threshold = kHysteresis * t.peak;

    t.sinceEdge += 1.0;
    bool edge = false;
    double ago = 0.0; // how long before this sample the edge happened, 0..1 samples
    if (!t.high)
    {
        if (d > threshold && t.peak > 1.0e-6f)
        {
            t.high = true;
            const float before = t.previous - t.previousThreshold;
            const float after = d - threshold;
            const float frac = before < 0.0f ? before / (before - after) : 0.0f;
            ago = 1.0 - static_cast<double> (std::clamp (frac, 0.0f, 1.0f));
            edge = true;
        }
    }
    else if (d < -threshold)
    {
        t.high = false;
    }
    t.previous = d;
    t.previousThreshold = threshold;

    // 2. Flip-flops, with polyBLEP corrections split between the previous sample (still pending,
    //    output now) and this one.
    float out1 = t.sub1.pending;
    float out2 = t.sub2.pending;
    float now1 = t.sub1.value;
    float now2 = t.sub2.value;
    if (edge)
    {
        const double interval = t.sinceEdge - ago;
        if (interval >= minPeriod_)
        {
            const bool agrees =
                t.period > 0.0 && std::abs (interval - t.period) < kAgreement * t.period;
            t.agreeing = agrees ? t.agreeing + 1 : 0;
            if (agrees)
            {
                t.recent[static_cast<size_t> (t.recentCount % 3)] = interval;
                ++t.recentCount;
            }
            t.confidence =
                agrees ? t.confidence + 0.5f * (1.0f - t.confidence) : 0.5f * t.confidence;
            t.period = interval <= maxPeriod_ ? interval : 0.0;
            t.sinceEdge = ago;

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
    }
    if (t.sinceEdge > maxPeriod_)
    {
        // Lost: no edge for longer than the lowest note's period.
        t.period = 0.0;
        t.agreeing = 0;
        t.recentCount = 0;
        t.confidence = 0.0f;
        t.sinceEdge = maxPeriod_;
    }
    t.sub1.pending = now1;
    t.sub2.pending = now2;

    // 3. Gate and envelope: the subs speak only while there is a note and the tracker follows it.
    if (t.levelOpen ? t.level < kCloseLevel : t.level > kOpenLevel)
        t.levelOpen = !t.levelOpen;
    const float target = t.levelOpen ? t.confidence : 0.0f;
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
        {
            auto& t = trackers_[static_cast<size_t> (ch)];
            flush (t);
            updateTracking (t);
        }

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
