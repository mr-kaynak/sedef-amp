// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Fathom.h"

#include "HallFdn.h"
#include "PlateTank.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/DelayLine.h"
#include "sedef/core/Math.h"
#include "sedef/core/Smoothed.h"

#include <array>

namespace sedef::dsp
{
namespace
{
constexpr double kInputFadeSeconds = 0.02; // spillover: new input fades out over 20 ms
constexpr double kMixRampSeconds = 0.03;
constexpr double kModeFadeSeconds = 0.06; // plate <-> hall crossfade
constexpr double kPreDelayGlideSeconds = 0.05;
constexpr double kParamGlideSeconds = 0.06; // decay and cut frequencies
constexpr double kMaxPreDelayMs = 200.0;
constexpr float kSilence = 1.0e-5f;       // -100 dBFS
constexpr double kIdleHoldSeconds = 0.25; // longer than the maximum pre-delay

// The cut frequencies also voice the tanks: highCut sets how fast the top end decays.
constexpr float kPlateDampingRatio = 2.5f;
constexpr float kPlateBandwidthRatio = 4.0f;

float clampFinite (float v, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (v) ? std::clamp (v, lo, hi) : fallback;
}

Fathom::Settings sanitise (const Fathom::Settings& in) noexcept
{
    const Fathom::Settings d;
    Fathom::Settings s = in;
    s.mode = in.mode == Fathom::Hall ? Fathom::Hall : Fathom::Plate;
    s.mix = clampFinite (in.mix, 0.0f, 1.0f, d.mix);
    s.decaySeconds = clampFinite (in.decaySeconds, 1.0f, 60.0f, d.decaySeconds);
    s.lowCutHz = clampFinite (in.lowCutHz, 20.0f, 500.0f, d.lowCutHz);
    s.highCutHz = clampFinite (in.highCutHz, 800.0f, 5000.0f, d.highCutHz);
    s.preDelayMs = clampFinite (in.preDelayMs, 1.0f, 200.0f, d.preDelayMs);
    return s;
}

/// Advances a control-rate smoother by one step; true if its value changed.
bool advance (ExpSmoothed& s) noexcept
{
    if (s.isSmoothing())
    {
        s.next();
        return true;
    }
    if (s.current() != s.target())
    {
        s.setCurrentAndTarget (s.target());
        return true;
    }
    return false;
}

/// Equal-power shape for a 0..1 crossfade ramp (the two tails are uncorrelated).
float equalPower (float ramp) noexcept
{
    return std::sin (0.5f * kPi * ramp);
}
} // namespace

struct Fathom::Impl
{
    double fs = 48000.0;
    bool prepared = false;
    Settings settings;

    reverb::PlateTank plate;
    reverb::HallFdn hall;
    std::array<LinearSmoothed, 2> modeRamp; // 0..1 per engine (Plate, Hall)
    std::array<bool, 2> engineActive { true, false };

    DelayLine preL, preR;
    ExpSmoothed preDelay;               // samples, per sample
    ExpSmoothed decay, lowCut, highCut; // control rate
    LinearSmoothed inputGain, dryGain, wetGain;
    Biquad lowCutFilter, highCutFilter;

    int controlCountdown = 0;
    bool idle = true;
    int silentSamples = 0;
    int idleHoldSamples = 0;
    float wetPeak = 0.0f, inPeak = 0.0f;

    std::array<float, kControlBlock> pl {}, pr {}, plateL {}, plateR {}, hallL {}, hallR {};

    void applySettings (bool snap) noexcept
    {
        const auto& s = settings;
        const float target = s.enabled ? 1.0f : 0.0f;
        // Mix law: equal power, dry = cos(mix pi/2), wet = sin(mix pi/2). While disabled the dry
        // path returns to unity so the rig is unaffected while the tail rings out.
        const float angle = 0.5f * kPi * s.mix;
        const float dry = s.enabled ? std::cos (angle) : 1.0f;
        const float wet = std::sin (angle);
        const float preSamples =
            std::max (1.0f, static_cast<float> (std::round (s.preDelayMs * 0.001 * fs)));

        if (snap)
        {
            inputGain.setCurrentAndTarget (target);
            dryGain.setCurrentAndTarget (dry);
            wetGain.setCurrentAndTarget (wet);
            preDelay.setCurrentAndTarget (preSamples);
            decay.setCurrentAndTarget (s.decaySeconds);
            lowCut.setCurrentAndTarget (s.lowCutHz);
            highCut.setCurrentAndTarget (s.highCutHz);
            for (int e = 0; e < 2; ++e)
            {
                modeRamp[static_cast<size_t> (e)].setCurrentAndTarget (s.mode == e ? 1.0f : 0.0f);
                engineActive[static_cast<size_t> (e)] = s.mode == e;
            }
            updateDecay();
            updateCuts();
            return;
        }

        inputGain.setTarget (target);
        dryGain.setTarget (dry);
        wetGain.setTarget (wet);
        preDelay.setTarget (preSamples);
        decay.setTarget (s.decaySeconds);
        lowCut.setTarget (s.lowCutHz);
        highCut.setTarget (s.highCutHz);
        for (int e = 0; e < 2; ++e)
        {
            const bool on = s.mode == e;
            modeRamp[static_cast<size_t> (e)].setTarget (on ? 1.0f : 0.0f);
            if (on)
                engineActive[static_cast<size_t> (e)] = true;
        }
    }

    void updateDecay() noexcept
    {
        const float d = decay.current();
        const float hc = highCut.current();
        plate.setDecay (d);
        plate.setDamping (hc * kPlateDampingRatio);
        plate.setBandwidth (hc * kPlateBandwidthRatio);
        hall.setDecay (d, hc);
    }

    void updateCuts() noexcept
    {
        lowCutFilter.setCoeffs (design::highpass (fs, lowCut.current()));
        highCutFilter.setCoeffs (design::lowpass (fs, highCut.current()));
    }

    void clearEngine (size_t e) noexcept
    {
        if (e == 0)
        {
            plate.clear();
            plateL.fill (0.0f);
            plateR.fill (0.0f);
        }
        else
        {
            hall.clear();
            hallL.fill (0.0f);
            hallR.fill (0.0f);
        }
    }

    void controlTick() noexcept
    {
        const bool decayChanged = advance (decay);
        const bool lowChanged = advance (lowCut);
        const bool highChanged = advance (highCut);
        if (decayChanged || highChanged)
            updateDecay();
        if (lowChanged || highChanged)
            updateCuts();

        // Retire an engine once its fade-out is complete; its tail must not come back later.
        for (size_t e = 0; e < 2; ++e)
            if (engineActive[e] && modeRamp[e].target() == 0.0f && !modeRamp[e].isSmoothing())
            {
                clearEngine (e);
                engineActive[e] = false;
            }

        if (engineActive[0])
            plate.controlTick();
        if (engineActive[1])
            hall.controlTick();
        lowCutFilter.snapToZero();
        highCutFilter.snapToZero();

        // Spillover: after disabling, keep running until the tail is inaudible, then idle.
        if (!settings.enabled && !inputGain.isSmoothing())
        {
            silentSamples =
                wetPeak < kSilence && inPeak < kSilence ? silentSamples + kControlBlock : 0;
            if (silentSamples >= idleHoldSamples)
                idle = true;
        }
        else
        {
            silentSamples = 0;
        }
        wetPeak = inPeak = 0.0f;
    }

    void processChunk (float* left, float* right, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = inputGain.next();
            preL.push (left[i] * g);
            preR.push ((right != nullptr ? right[i] : left[i]) * g);
            const float d = preDelay.next();
            const float a = preL.readLinear (d);
            const float b = preR.readLinear (d);
            pl[static_cast<size_t> (i)] = a;
            pr[static_cast<size_t> (i)] = b;
            inPeak = std::max (inPeak, std::max (std::abs (a), std::abs (b)));
        }

        if (engineActive[0])
            plate.process (pl.data(), pr.data(), plateL.data(), plateR.data(), n);
        if (engineActive[1])
            hall.process (pl.data(), pr.data(), hallL.data(), hallR.data(), n);

        const bool crossfading = modeRamp[0].isSmoothing() || modeRamp[1].isSmoothing();
        float gp = modeRamp[0].target();
        float gh = modeRamp[1].target();

        for (int i = 0; i < n; ++i)
        {
            const auto k = static_cast<size_t> (i);
            if (crossfading)
            {
                gp = equalPower (modeRamp[0].next());
                gh = equalPower (modeRamp[1].next());
            }
            float wl = gp * plateL[k] + gh * hallL[k];
            float wr = gp * plateR[k] + gh * hallR[k];
            wetPeak = std::max (wetPeak, std::max (std::abs (wl), std::abs (wr)));

            wl = highCutFilter.processSample (0, lowCutFilter.processSample (0, wl));
            wr = highCutFilter.processSample (1, lowCutFilter.processSample (1, wr));

            const float dg = dryGain.next();
            const float wg = wetGain.next();
            if (right != nullptr)
            {
                left[i] = left[i] * dg + wl * wg;
                right[i] = right[i] * dg + wr * wg;
            }
            else
            {
                left[i] = left[i] * dg + 0.5f * (wl + wr) * wg;
            }
        }
    }
};

Fathom::Fathom() : impl_ (std::make_unique<Impl>()) {}

Fathom::~Fathom() = default;

void Fathom::prepare (const ProcessSpec& spec)
{
    auto& s = *impl_;
    s.fs = spec.sampleRate;
    s.plate.prepare (s.fs);
    s.hall.prepare (s.fs);

    const int maxPre = static_cast<int> (std::ceil (kMaxPreDelayMs * 0.001 * s.fs)) + 2;
    s.preL.prepare (maxPre);
    s.preR.prepare (maxPre);

    s.inputGain.reset (s.fs, kInputFadeSeconds);
    s.dryGain.reset (s.fs, kMixRampSeconds);
    s.wetGain.reset (s.fs, kMixRampSeconds);
    for (auto& r : s.modeRamp)
        r.reset (s.fs, kModeFadeSeconds);
    s.preDelay.reset (s.fs, kPreDelayGlideSeconds);
    const double controlRate = s.fs / kControlBlock;
    s.decay.reset (controlRate, kParamGlideSeconds);
    s.lowCut.reset (controlRate, kParamGlideSeconds);
    s.highCut.reset (controlRate, kParamGlideSeconds);
    s.idleHoldSamples = static_cast<int> (kIdleHoldSeconds * s.fs);

    s.prepared = true;
    reset();
}

void Fathom::reset() noexcept
{
    auto& s = *impl_;
    if (!s.prepared)
        return;
    s.plate.clear();
    s.hall.clear();
    s.preL.reset();
    s.preR.reset();
    s.lowCutFilter.reset();
    s.highCutFilter.reset();
    s.plateL.fill (0.0f);
    s.plateR.fill (0.0f);
    s.hallL.fill (0.0f);
    s.hallR.fill (0.0f);
    s.applySettings (true);
    s.controlCountdown = 0;
    s.silentSamples = 0;
    s.wetPeak = s.inPeak = 0.0f;
    s.idle = !s.settings.enabled;
}

void Fathom::setSettings (const Settings& settings) noexcept
{
    auto& s = *impl_;
    s.settings = sanitise (settings);
    if (!s.prepared)
        return;
    if (s.settings.enabled && s.idle)
    {
        s.idle = false;
        s.silentSamples = 0;
        // Leaving idle: the frozen tail is below -100 dBFS, so the wet side can jump to the new
        // settings. The dry gain and the input still ramp from where they are.
        const float dry = s.dryGain.current();
        s.applySettings (true);
        const float dryTarget = s.dryGain.target();
        s.dryGain.setCurrentAndTarget (dry);
        s.dryGain.setTarget (dryTarget);
        s.inputGain.setCurrentAndTarget (0.0f);
        s.inputGain.setTarget (1.0f);
        return;
    }
    s.applySettings (s.idle);
}

void Fathom::process (const AudioView& io) noexcept
{
    auto& s = *impl_;
    const int n = io.numFrames();
    if (!s.prepared || s.idle || n <= 0 || io.numChannels() <= 0)
        return;

    float* left = io.channel (0);
    float* right = io.numChannels() > 1 ? io.channel (1) : nullptr;
    int done = 0;
    while (done < n)
    {
        if (s.controlCountdown == 0)
        {
            s.controlTick();
            s.controlCountdown = kControlBlock;
            // Idle: the rest of the block is the untouched dry signal (dry gain is unity).
            if (s.idle)
                return;
        }
        const int len = std::min (n - done, s.controlCountdown);
        s.processChunk (left + done, right != nullptr ? right + done : nullptr, len);
        s.controlCountdown -= len;
        done += len;
    }
}

} // namespace sedef::dsp
