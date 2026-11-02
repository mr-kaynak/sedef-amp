// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/capture/CaptureStage.h"

#include "sedef/core/DelayLine.h"
#include "sedef/core/Math.h"
#include "sedef/core/RtHandoff.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/amp/AmpSection.h"
#include "sedef/dsp/cab/CabSim.h"
#include "sedef/dsp/capture/RateBridge.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace sedef::dsp
{
namespace
{
constexpr double kGainRampSeconds = 0.02;

float smoothstep (float x) noexcept
{
    return x * x * (3.0f - 2.0f * x);
}
} // namespace

struct CaptureStage::Impl
{
    // Spec (written by prepare, read by loader threads).
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> maxBlock { 512 };
    std::atomic<int> latency { 0 };
    int ampLatency = 0;

    // Model handoff, as CabSim's IR slots: `busy` is a non-blocking ownership flag the audio
    // thread keeps while a retired model is still fading out, so collectGarbage() can't free it.
    RtHandoff<CaptureModel> handoff;
    std::atomic<bool> busy { false };
    bool holding = false;             // audio thread owns `busy`
    CaptureModel* current = nullptr;  // audio thread
    CaptureModel* previous = nullptr; // audio thread: fading out, or carrying a fade to the amp
    LinearSmoothed modelFade;         // weight of `current` against `previous`

    Settings settings;
    bool snap = true, snapCab = true; // first block after prepare/reset: final state at once

    LinearSmoothed path;    // weight of the capture against the built-in amp
    LinearSmoothed cabPath; // weight of the cab against the straight path
    LinearSmoothed inGain, ifaceGain, outGain;
    bool ampIdle = false, cabIdle = false;
    int settleLeft = 0; // host samples of silence still to feed a capture that went quiet
    bool captureRan = false;

    DelayLine ampDelay; // lines the built-in amp up with the stage's latency
    std::vector<float> dry, ampOut, capture, voice, gainIn, gainIface, direct;

    CaptureSpec spec() const noexcept
    {
        return { sampleRate.load (std::memory_order_acquire),
                 maxBlock.load (std::memory_order_acquire),
                 latency.load (std::memory_order_acquire) };
    }

    bool usable (const CaptureModel* m) const noexcept
    {
        return m != nullptr && !m->empty() && m->spec() == spec();
    }

    /// Audio thread, once per block: picks up a newly published model.
    void poll() noexcept
    {
        if (holding)
        {
            if (previous != nullptr)
                return; // still fading out of it; acquire() could not swap anyway
            holding = false;
            busy.store (false, std::memory_order_release);
        }
        bool expected = false;
        if (!busy.compare_exchange_strong (expected, true, std::memory_order_acquire))
            return; // the collector is freeing the retired model right now; try next block

        CaptureModel* next = handoff.acquire();
        if (next != current)
        {
            CaptureModel* old = current;
            current = next;
            settleLeft = 0; // a new model arrives prewarmed
            const bool audible = path.current() > 0.0f || path.target() > 0.0f;
            if (audible && usable (old))
            {
                // Keep playing the old one: into the new model, or into the amp if the new one
                // can't play (cleared, or built for another rate).
                previous = old;
                modelFade.setCurrentAndTarget (0.0f);
                if (usable (current))
                    modelFade.setTarget (1.0f);
                holding = true;
                return;
            }
        }
        busy.store (false, std::memory_order_release);
    }

    /// One model over the block: the input scaled by the trims (and the interface calibration
    /// when the capture knows its input level), into `voice`.
    void runVoice (CaptureModel& m, const float* in, int n) noexcept
    {
        float* v = voice.data();
        const float scale = m.calibrated() ? m.inputScale() : 1.0f;
        for (int i = 0; i < n; ++i)
            v[i] = in[i] * gainIn[static_cast<std::size_t> (i)] *
                   (m.calibrated() ? gainIface[static_cast<std::size_t> (i)] * scale : 1.0f);
        m.process (v, v, n);
    }

    /// The capture path into `capture` (before the output trim).
    void runCapture (const float* in, int n) noexcept
    {
        float* out = capture.data();
        const bool cur = usable (current);
        if (previous == nullptr)
        {
            if (cur)
            {
                runVoice (*current, in, n);
                std::copy_n (voice.data(), n, out);
            }
            else
                std::fill_n (out, n, 0.0f);
            return;
        }

        runVoice (*previous, in, n);
        std::copy_n (voice.data(), n, out);
        if (!cur)
            return; // carries the fade to the amp alone
        runVoice (*current, in, n);
        for (int i = 0; i < n; ++i)
        {
            const float w = smoothstep (modelFade.next());
            out[i] += w * (voice[static_cast<std::size_t> (i)] - out[i]);
        }
        if (!modelFade.isSmoothing())
            previous = nullptr; // poll() lets go of it next block
    }
};

CaptureStage::CaptureStage() : impl_ (std::make_unique<Impl>()) {}
CaptureStage::~CaptureStage() = default;

int CaptureStage::latencyFor (double sampleRate, int ampLatency) noexcept
{
    return std::max (ampLatency, capture::RateBridge::minimumLatency (sampleRate, 48000.0));
}

void CaptureStage::prepare (const ProcessSpec& spec, int ampLatency)
{
    auto& d = *impl_;
    const int block = std::max (1, spec.maxBlockSize);
    const int latency = latencyFor (spec.sampleRate, ampLatency);
    d.ampLatency = ampLatency;
    d.ampDelay.prepare (std::max (1, latency - ampLatency));
    for (auto* v : { &d.dry, &d.ampOut, &d.capture, &d.voice, &d.gainIn, &d.gainIface, &d.direct })
        v->assign (static_cast<std::size_t> (block), 0.0f);
    for (auto* s : { &d.path, &d.cabPath, &d.modelFade })
        s->reset (spec.sampleRate, kFadeSeconds);
    for (auto* s : { &d.inGain, &d.ifaceGain, &d.outGain })
        s->reset (spec.sampleRate, kGainRampSeconds);
    d.sampleRate.store (spec.sampleRate, std::memory_order_release);
    d.maxBlock.store (block, std::memory_order_release);
    d.latency.store (latency, std::memory_order_release);
    reset();
}

void CaptureStage::reset() noexcept
{
    // Real-time safe (the engine resets chains on a stereo-mode switch). A model's state can't
    // be cleared here; it simply carries on, which is inaudible because its input carries on too.
    auto& d = *impl_;
    d.snap = d.snapCab = true;
    d.ampDelay.reset();
    d.ampIdle = d.cabIdle = false;
}

void CaptureStage::setSettings (const Settings& s) noexcept
{
    auto& d = *impl_;
    d.settings = s;
    const auto finite = [] (float v, float fallback) {
        return std::isfinite (v) ? v : fallback;
    };
    d.settings.inputDb = std::clamp (finite (s.inputDb, 0.0f), -24.0f, 24.0f);
    d.settings.outputDb = std::clamp (finite (s.outputDb, 0.0f), -24.0f, 24.0f);
    d.settings.interfaceDbu = std::clamp (finite (s.interfaceDbu, 12.0f), -10.0f, 30.0f);
}

int CaptureStage::latencySamples() const noexcept
{
    return impl_->latency.load (std::memory_order_acquire);
}

CaptureSpec CaptureStage::modelSpec() const noexcept
{
    return impl_->spec();
}

bool CaptureStage::captureAudible() const noexcept
{
    return impl_->path.target() > 0.5f && !impl_->path.isSmoothing();
}

void CaptureStage::process (AmpSection& amp, const AmpSection::Settings& ampSettings,
                            const AudioView& io) noexcept
{
    auto& d = *impl_;
    const int n = std::min (io.numFrames(), d.maxBlock.load (std::memory_order_relaxed));
    float* x = io.channel (0);
    d.poll();

    const auto& s = d.settings;
    const bool wantCapture = s.useCapture && d.usable (d.current);
    const float inTarget = dbToGain (s.inputDb);
    // Calibration: the player's interface level against the level the capture was made at
    // (CaptureModel::inputScale holds the latter).
    const float ifaceTarget = dbToGain (s.interfaceDbu);
    const float outTarget = dbToGain (s.outputDb);
    if (d.snap)
    {
        d.path.setCurrentAndTarget (wantCapture ? 1.0f : 0.0f);
        d.inGain.setCurrentAndTarget (inTarget);
        d.ifaceGain.setCurrentAndTarget (ifaceTarget);
        d.outGain.setCurrentAndTarget (outTarget);
    }
    d.path.setTarget (wantCapture ? 1.0f : 0.0f);
    d.inGain.setTarget (inTarget);
    d.ifaceGain.setTarget (ifaceTarget);
    d.outGain.setTarget (outTarget);

    const bool runAmp = d.path.current() < 1.0f || d.path.target() < 1.0f;
    const bool hasVoice = d.usable (d.current) || d.previous != nullptr;
    const bool runCapture = hasVoice && (d.path.current() > 0.0f || d.path.target() > 0.0f);

    std::copy_n (x, n, d.dry.data());

    if (runAmp)
    {
        if (d.ampIdle)
        {
            amp.reset(); // stale state from before the capture took over; the fade-in masks it
            d.ampDelay.reset();
            d.ampIdle = false;
        }
        amp.setSettings (ampSettings);
        std::copy_n (d.dry.data(), n, d.ampOut.data());
        amp.process (AudioView (d.ampOut.data(), n));
        if (const int pad = d.latency.load (std::memory_order_relaxed) - d.ampLatency; pad > 0)
            for (int i = 0; i < n; ++i)
            {
                d.ampDelay.push (d.ampOut[static_cast<std::size_t> (i)]);
                d.ampOut[static_cast<std::size_t> (i)] = d.ampDelay.read (pad);
            }
    }
    else
    {
        d.ampIdle = true;
    }

    for (int i = 0; i < n; ++i)
    {
        d.gainIn[static_cast<std::size_t> (i)] = d.inGain.next();
        d.gainIface[static_cast<std::size_t> (i)] = d.ifaceGain.next();
    }

    if (runCapture)
    {
        d.runCapture (d.dry.data(), n);
        for (int i = 0; i < n; ++i)
            d.capture[static_cast<std::size_t> (i)] *= d.outGain.next();
        d.captureRan = true;
    }
    else
    {
        d.outGain.skip (n);
        if (d.captureRan && d.usable (d.current))
            d.settleLeft = d.current->settleSamples();
        d.captureRan = false;
        // A capture that went quiet runs on silence until it is back in its prewarmed state, so
        // switching to it later doesn't replay the last notes it heard.
        if (d.settleLeft > 0 && d.usable (d.current))
        {
            std::fill_n (d.voice.data(), n, 0.0f);
            d.current->process (d.voice.data(), d.voice.data(), n);
            d.settleLeft -= n;
        }
    }
    if (d.previous != nullptr && !d.usable (d.current) && d.path.current() <= 0.0f &&
        !d.path.isSmoothing())
        d.previous = nullptr; // the fade to the amp is over

    if (!runCapture)
    {
        std::copy_n (d.ampOut.data(), n, x);
        d.path.skip (n);
    }
    else if (!runAmp)
    {
        std::copy_n (d.capture.data(), n, x);
        d.path.skip (n);
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            const float p = smoothstep (d.path.next());
            const float a = d.ampOut[static_cast<std::size_t> (i)];
            x[i] = a + p * (d.capture[static_cast<std::size_t> (i)] - a);
        }
    }
    // The fade to the amp may have finished inside this block.
    if (d.previous != nullptr && !d.usable (d.current) && d.path.current() <= 0.0f &&
        !d.path.isSmoothing())
        d.previous = nullptr;
    d.snap = false;
}

void CaptureStage::processCab (CabSim& cab, const CabSim::Settings& cabSettings, const float* in,
                               const AudioView& out) noexcept
{
    auto& d = *impl_;
    const int n = std::min (out.numFrames(), d.maxBlock.load (std::memory_order_relaxed));
    // The cab steps aside only for a capture that is playing (or fading in) and has its own.
    const bool wantCab = !(d.path.target() > 0.5f && !d.settings.throughCab);
    if (d.snapCab)
        d.cabPath.setCurrentAndTarget (wantCab ? 1.0f : 0.0f);
    d.snapCab = false;
    d.cabPath.setTarget (wantCab ? 1.0f : 0.0f);
    const bool runCab = d.cabPath.current() > 0.0f || d.cabPath.target() > 0.0f;
    const bool fading = d.cabPath.isSmoothing();

    std::copy_n (in, n, d.direct.data()); // `in` may be out's first channel
    if (runCab)
    {
        if (d.cabIdle)
        {
            cab.reset();
            d.cabIdle = false;
        }
        cab.setSettings (cabSettings);
        cab.process (d.direct.data(), out);
    }
    else
    {
        d.cabIdle = true;
    }

    const float g = dbToGain (kDirectGainDb);
    float* l = out.channel (0);
    float* r = out.channel (1);
    if (!runCab)
    {
        for (int i = 0; i < n; ++i)
            l[i] = r[i] = g * d.direct[static_cast<std::size_t> (i)];
        d.cabPath.skip (n);
    }
    else if (fading)
    {
        for (int i = 0; i < n; ++i)
        {
            const float c = smoothstep (d.cabPath.next());
            const float s = g * d.direct[static_cast<std::size_t> (i)];
            l[i] = s + c * (l[i] - s);
            r[i] = s + c * (r[i] - s);
        }
    }
    else
    {
        d.cabPath.skip (n);
    }
}

void CaptureStage::setModel (std::unique_ptr<CaptureModel> model)
{
    // An empty model stands for "cleared": RtHandoff cannot publish null.
    if (model == nullptr)
        model = std::make_unique<CaptureModel>();
    collectGarbage(); // empty the retire slot so the audio thread can take the new one
    impl_->handoff.publish (std::move (model));
}

void CaptureStage::collectGarbage()
{
    auto& d = *impl_;
    bool expected = false;
    if (d.busy.compare_exchange_strong (expected, true, std::memory_order_acquire))
    {
        d.handoff.collect();
        d.busy.store (false, std::memory_order_release);
    }
}

} // namespace sedef::dsp
