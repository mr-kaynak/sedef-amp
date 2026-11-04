// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/capture/CaptureStage.h"

#include "sedef/core/DelayLine.h"
#include "sedef/core/Math.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/amp/AmpSection.h"
#include "sedef/dsp/cab/CabSim.h"
#include "sedef/dsp/capture/RateBridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace sedef::dsp
{
namespace
{
constexpr double kGainRampSeconds = 0.02;

/// Model input beyond this (+120 dBFS) is never a guitar: clamped, so no model sees a number it
/// could overflow on.
constexpr float kMaxModelInput = 1.0e6f;

float smoothstep (float x) noexcept
{
    return x * x * (3.0f - 2.0f * x);
}

/// Hands models to the audio thread, which never frees one. Like RtHandoff, with room for more
/// than one retired model and a collector that frees every retired model except the one still
/// fading out, so the newest model always gets through, whether or not a collector runs on a
/// timer.
///
///   publish()                          any non-real-time thread
///   take(), retire(), stoppedFading()  audio thread
///   collect()                          any non-real-time thread
class ModelHandoff
{
public:
    ModelHandoff() = default;
    ModelHandoff (const ModelHandoff&) = delete;
    ModelHandoff& operator= (const ModelHandoff&) = delete;
    ~ModelHandoff()
    {
        delete incoming_.exchange (nullptr);
        for (auto& slot : retired_)
            delete slot.exchange (nullptr);
    }

    void publish (std::unique_ptr<CaptureModel> model)
    {
        // A model the audio thread never took is replaced: take() exchanges it out, so it can't
        // be the one deleted here.
        delete incoming_.exchange (model.release(), std::memory_order_acq_rel);
    }

    /// The newly published model, if there is one and room to retire the one it replaces.
    CaptureModel* take() noexcept
    {
        if (incoming_.load (std::memory_order_acquire) == nullptr || freeSlot() < 0)
            return nullptr;
        return incoming_.exchange (nullptr, std::memory_order_acq_rel);
    }

    /// After take(): `old` goes to the collector. If it still plays (a fade), the collector
    /// leaves it alone until stoppedFading().
    void retire (CaptureModel* old, bool stillPlaying) noexcept
    {
        if (old == nullptr)
            return;
        if (stillPlaying)
            fading_.store (old); // before it is visible in a slot (sequentially consistent)
        // take() found a free slot, and only this thread fills them.
        retired_[static_cast<std::size_t> (freeSlot())].store (old);
    }

    void stoppedFading() noexcept { fading_.store (nullptr); }

    void collect()
    {
        for (auto& slot : retired_)
        {
            // The slot first, then `fading_`: a model seen in its slot was marked fading before.
            CaptureModel* p = slot.load();
            if (p != nullptr && p != fading_.load() && slot.compare_exchange_strong (p, nullptr))
                delete p;
        }
    }

private:
    int freeSlot() const noexcept
    {
        for (std::size_t i = 0; i < retired_.size(); ++i)
            if (retired_[i].load (std::memory_order_acquire) == nullptr)
                return static_cast<int> (i);
        return -1;
    }

    std::atomic<CaptureModel*> incoming_ { nullptr };
    std::array<std::atomic<CaptureModel*>, 4> retired_ {};
    std::atomic<CaptureModel*> fading_ { nullptr };
};
} // namespace

struct CaptureStage::Impl
{
    ~Impl() { delete current; }

    // The spec loader threads build for: written by prepare() under a sequence count, so a
    // reader never sees half of one and half of another. `rtSpec` is the audio thread's copy.
    std::atomic<unsigned> specVersion { 0 };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> maxBlock { 512 };
    std::atomic<int> latency { 0 };
    std::atomic<OversamplingMode> oversampling { OversamplingMode::lowLatency };
    CaptureSpec rtSpec;
    int ampLatency = 0;

    ModelHandoff handoff;
    CaptureModel* current = nullptr;  // audio thread; owned
    CaptureModel* previous = nullptr; // audio thread: retired, fading out or carrying a fade
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

    /// Any thread.
    CaptureSpec spec() const noexcept
    {
        for (;;)
        {
            const unsigned v = specVersion.load (std::memory_order_acquire);
            const CaptureSpec s { sampleRate.load (std::memory_order_relaxed),
                                  maxBlock.load (std::memory_order_relaxed),
                                  latency.load (std::memory_order_relaxed),
                                  oversampling.load (std::memory_order_relaxed) };
            std::atomic_thread_fence (std::memory_order_acquire);
            if ((v & 1u) == 0 && specVersion.load (std::memory_order_relaxed) == v)
                return s;
            std::this_thread::yield(); // prepare() is writing it
        }
    }

    /// prepare() only (one thread at a time).
    void setSpec (const CaptureSpec& s) noexcept
    {
        const unsigned v = specVersion.load (std::memory_order_relaxed);
        specVersion.store (v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        sampleRate.store (s.sampleRate, std::memory_order_relaxed);
        maxBlock.store (s.maxBlockSize, std::memory_order_relaxed);
        latency.store (s.latencySamples, std::memory_order_relaxed);
        oversampling.store (s.oversampling, std::memory_order_relaxed);
        specVersion.store (v + 2, std::memory_order_release);
        rtSpec = s;
    }

    /// Audio thread.
    bool usable (const CaptureModel* m) const noexcept
    {
        return m != nullptr && !m->empty() && m->spec() == rtSpec;
    }

    /// The model fading out has gone quiet: the collector may free it.
    void endFade() noexcept
    {
        previous = nullptr;
        handoff.stoppedFading();
    }

    /// Audio thread, once per block: picks up a newly published model.
    void poll() noexcept
    {
        if (previous != nullptr)
            return; // one fade at a time; the newest model waits for this one to finish
        CaptureModel* next = handoff.take();
        if (next == nullptr)
            return;
        CaptureModel* old = current;
        current = next;
        settleLeft = 0; // a new model arrives prewarmed
        // Keep playing the old one: into the new model, or into the amp if the new one can't
        // play (cleared, or built for another spec).
        const bool audible = path.current() > 0.0f || path.target() > 0.0f;
        const bool fadeOut = audible && usable (old);
        handoff.retire (old, fadeOut);
        if (fadeOut)
        {
            previous = old;
            modelFade.setCurrentAndTarget (0.0f);
            if (usable (current))
                modelFade.setTarget (1.0f);
        }
    }

    /// One model over the block: the input scaled by the trims (and the interface calibration
    /// when the capture knows its input level), into `voice`. Non-finite input becomes silence
    /// and absurd input is clamped: a recurrent model would keep a NaN in its state for good.
    void runVoice (CaptureModel& m, const float* in, int n) noexcept
    {
        float* v = voice.data();
        const float scale = m.calibrated() ? m.inputScale() : 1.0f;
        for (int i = 0; i < n; ++i)
        {
            const float x =
                in[i] * gainIn[static_cast<std::size_t> (i)] *
                (m.calibrated() ? gainIface[static_cast<std::size_t> (i)] * scale : 1.0f);
            v[i] = std::isfinite (x) ? std::clamp (x, -kMaxModelInput, kMaxModelInput) : 0.0f;
        }
        m.process (v, v, n);
    }

    /// The capture path into `capture` (before the output trim).
    void runCapture (const float* in, int n) noexcept
    {
        float* out = capture.data();
        const bool cur = usable (current);
        if (previous != nullptr && !usable (previous))
            endFade(); // built for another spec: it can't run (prepare() drops it anyway)
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
            endFade();
    }

    /// The capture path is silent: whatever was fading out is over, whether into the amp or
    /// into a capture that is no longer heard.
    void endFadeIfSilent() noexcept
    {
        if (previous != nullptr && path.current() <= 0.0f && !path.isSmoothing())
            endFade();
    }
};

CaptureStage::CaptureStage() : impl_ (std::make_unique<Impl>()) {}
CaptureStage::~CaptureStage() = default;

int CaptureStage::latencyFor (double sampleRate, int ampLatency, OversamplingMode mode) noexcept
{
    // Room for the full filter for captures made at any of the common rates, so how well a
    // capture plays never depends on the amp's latency or on which capture is loaded.
    int latency = ampLatency;
    for (const double modelRate : { 44100.0, 48000.0, 88200.0, 96000.0 })
        latency = std::max (latency,
                            mode == OversamplingMode::lowLatency
                                ? capture::RateBridge::minimumPhaseLatency (sampleRate, modelRate)
                                : capture::RateBridge::minimumLatency (sampleRate, modelRate));
    return latency;
}

void CaptureStage::prepare (const ProcessSpec& spec, int ampLatency)
{
    auto& d = *impl_;
    const int block = std::max (1, spec.maxBlockSize);
    const int latency = latencyFor (spec.sampleRate, ampLatency, spec.oversampling);
    d.ampLatency = ampLatency;
    d.ampDelay.prepare (std::max (1, latency - ampLatency));
    for (auto* v : { &d.dry, &d.ampOut, &d.capture, &d.voice, &d.gainIn, &d.gainIface, &d.direct })
        v->assign (static_cast<std::size_t> (block), 0.0f);
    for (auto* s : { &d.path, &d.cabPath, &d.modelFade })
        s->reset (spec.sampleRate, kFadeSeconds);
    for (auto* s : { &d.inGain, &d.ifaceGain, &d.outGain })
        s->reset (spec.sampleRate, kGainRampSeconds);
    d.setSpec ({ spec.sampleRate, block, latency, spec.oversampling });
    reset(); // drops a fade in progress: the model fading out was built for the old spec
    d.handoff.collect();
}

void CaptureStage::reset() noexcept
{
    // Real-time safe (the engine resets chains on a stereo-mode switch). A model's state can't
    // be cleared here; it simply carries on, which is inaudible because its input carries on too.
    auto& d = *impl_;
    if (d.previous != nullptr)
        d.endFade(); // the reset snaps to the final state
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

const CaptureFile* CaptureStage::playingFile() const noexcept
{
    const auto* m = impl_->current;
    return m != nullptr ? m->file() : nullptr;
}

bool CaptureStage::captureAudible() const noexcept
{
    return impl_->path.target() > 0.5f && !impl_->path.isSmoothing();
}

void CaptureStage::process (AmpSection& amp, const AmpSection::Settings& ampSettings,
                            const AudioView& io) noexcept
{
    auto& d = *impl_;
    const int n = std::min (io.numFrames(), d.rtSpec.maxBlockSize);
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
        if (const int pad = d.rtSpec.latencySamples - d.ampLatency; pad > 0)
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
    d.endFadeIfSilent();

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
    d.endFadeIfSilent(); // the fade to the amp may have finished inside this block
    d.snap = false;
}

void CaptureStage::processCab (CabSim& cab, const CabSim::Settings& cabSettings, const float* in,
                               const AudioView& out) noexcept
{
    auto& d = *impl_;
    const int n = std::min (out.numFrames(), d.rtSpec.maxBlockSize);
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
    // An empty model stands for "cleared": the handoff can't publish null.
    if (model == nullptr)
        model = std::make_unique<CaptureModel>();
    collectGarbage(); // leaves at most the one fading out, so the new one always gets through
    impl_->handoff.publish (std::move (model));
}

void CaptureStage::collectGarbage()
{
    impl_->handoff.collect();
}

} // namespace sedef::dsp
