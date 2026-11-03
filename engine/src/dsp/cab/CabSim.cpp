// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/cab/CabSim.h"

#include "sedef/core/DelayLine.h"
#include "sedef/core/Math.h"
#include "sedef/core/RtHandoff.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/cab/CabModels.h"
#include "sedef/dsp/cab/ConvolutionKernel.h"
#include "sedef/dsp/cab/PartitionedConvolver.h"
#include "sedef/dsp/cab/RoomAmbience.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <pffft/pffft.h>

namespace sedef::dsp
{
namespace
{
constexpr int kChunk = kControlBlock;
constexpr double kCrossfadeSeconds = 0.03;
constexpr double kPrerollSeconds = 0.005; // incoming voice runs muted while its filters settle
constexpr double kGainRampSeconds = 0.02;
constexpr double kSwitchRampSeconds = 0.03;
constexpr double kPlacementSmoothingSeconds = 0.03; // one-pole time constant
constexpr double kMaxPlacementDelaySeconds = 0.004;
constexpr double kMaxBleedSeconds = 0.0025; // the far speaker of a 4x12 is ~1.7 ms further
constexpr float kOffAxisRest = 1.0e-7f;     // relative state below which the off-axis stages stop
constexpr float kOffAxisFloor = 1.0e-12f;   // and absolute (-240 dB), for when the input is silent
constexpr double kMaxReflectionSeconds = 0.003;
constexpr float kSnapTolerance = 1.0e-4f;
constexpr float kRoomPanWidth = 0.5f; // room sends follow the mic pan, narrower
constexpr int kNumSlots = CabSim::kNumMics * CabSim::kNumCabs;

/// What a voice renders. Two voices per mic let every change of cab, mic type or IR crossfade.
struct VoiceConfig
{
    int cab = 0;
    int type = 0; ///< procedural model 0..9, or kCustomIrType with a kernel
    const ConvolutionKernel* kernel = nullptr;

    bool operator== (const VoiceConfig&) const = default;
};

float sanitise (float v, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (v) ? std::clamp (v, lo, hi) : fallback;
}

struct PanGains
{
    float l, r;
};

PanGains constantPowerPan (float pan) noexcept
{
    const float angle = (sanitise (pan, -1.0f, 1.0f, 0.0f) + 1.0f) * (0.25f * kPi);
    return { std::cos (angle), std::sin (angle) };
}

/// Fractional read that is exact (no interpolation filtering) at delay 0, so distance 0 adds no
/// delay at all. Hermite needs one sample of look-back, hence linear below one sample.
float readFractional (const DelayLine& line, float delay) noexcept
{
    return delay < 1.0f ? line.readLinear (delay) : line.readCubic (delay);
}

float smoothstep (float t) noexcept
{
    return t * t * (3.0f - 2.0f * t);
}

/// S-shaped ramp for switched values (enable, polarity): unlike a linear ramp it has no slope
/// corners, which keeps a polarity flip from spraying treble. Continuous controls (level, pan)
/// use LinearSmoothed, which follows automation without restarting a curve every block.
class SwitchRamp
{
public:
    void reset (double sampleRate, double seconds) noexcept
    {
        step_ = static_cast<float> (1.0 / std::max (1.0, seconds * sampleRate));
        snap();
    }

    void setTarget (float target) noexcept
    {
        if (target == target_)
            return;
        start_ = value_;
        target_ = target;
        progress_ = 0.0f;
    }

    void snap() noexcept
    {
        value_ = start_ = target_;
        progress_ = 1.0f;
    }

    float next() noexcept
    {
        if (progress_ < 1.0f)
        {
            progress_ = std::min (1.0f, progress_ + step_);
            value_ = start_ + (target_ - start_) * smoothstep (progress_);
        }
        return value_;
    }

    bool isRamping() const noexcept { return progress_ < 1.0f; }
    float target() const noexcept { return target_; }

private:
    float value_ = 0.0f, start_ = 0.0f, target_ = 0.0f;
    float progress_ = 1.0f;
    float step_ = 1.0f;
};
} // namespace

struct CabSim::Impl
{
    struct Voice
    {
        VoiceConfig config;
        bool live = false;

        // Procedural chain: [reflection] -> body | room tap | bleed tap -> near stages (position,
        // proximity, air) + the other speakers -> capsule stages (mic, off axis) -> time of
        // flight + floor bounce. A custom IR voice uses stages[0, kOffAxisStages) for its
        // off-axis filter.
        std::array<cab::Stage, cab::kMaxStages> stages {};
        int numBody = 0;
        int capsuleIndex = 0;
        int offAxisIndex = 0;
        float norm = 1.0f;
        float roomNorm = 1.0f;

        DelayLine reflection;
        int reflectionDelay = 1;
        float reflectionGain = 0.0f;
        float reflectionCoeff = 0.0f;
        float reflectionState = 0.0f;

        DelayLine air; // time of flight and floor bounce
        float arrival = 0.0f, bounce = 1.0f, bounceGain = 0.0f, level = 1.0f;
        float nextArrival = 0.0f, nextBounce = 1.0f, nextBounceGain = 0.0f, nextLevel = 1.0f;
        float bounceState = 0.0f;

        // The other speakers: delayed, low-passed copies of the body output.
        DelayLine bleedLine;
        int numBleed = 0;
        std::array<cab::Stage, cab::kMaxNeighbours> bleedLp {};
        cab::Stage bleedShelf; // gives the neighbours' low end back (see cab::placement)
        std::array<float, cab::kMaxNeighbours> bleedDelay {}, bleedGain {};
        std::array<float, cab::kMaxNeighbours> nextBleedDelay {}, nextBleedGain {};

        bool offAxisLive = false; // the off-axis stages run (identity and skipped on axis)

        cab::PartitionedConvolver convolver;
    };

    struct Mic
    {
        std::array<Voice, 2> voices;
        int active = 0;
        bool fading = false; // voices[active] fades in, the other one out
        float fade = 0.0f;   // starts below 0: the pre-roll
        bool idle = true;

        ExpSmoothed x, y, distance, offAxis;       // pose in Cartesian coordinates; angle / 60
        cab::MicPose applied;                      // what the voices' filters are designed for
        LinearSmoothed gainL, gainR, roomL, roomR; // level x pan
        SwitchRamp micSwitch;                      // 0 off, +1 on, -1 on and inverted
        SwitchRamp roomSwitch;
    };

    /// One custom IR slot. `busy` is a non-blocking ownership flag between the audio thread and
    /// collectGarbage(): the audio thread keeps it while a voice is still crossfading out of a
    /// retired kernel, so the collector cannot free that kernel under it. Neither side waits.
    struct Slot
    {
        RtHandoff<ConvolutionKernel> handoff;
        std::atomic<bool> busy { false };
        const ConvolutionKernel* current = nullptr;      // audio thread
        const ConvolutionKernel* retiredInUse = nullptr; // audio thread
        bool holding = false;                            // audio thread owns `busy`
    };

    ~Impl()
    {
        if (fft != nullptr)
            pffft_destroy_setup (fft);
    }

    // --- configuration --------------------------------------------------------------------------

    void prepare (const ProcessSpec& spec)
    {
        fs = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
        calibration = cab::calibrate (fs);

        partition = ConvolutionKernel::partitionSizeFor (fs);
        const int maxTaps = static_cast<int> (std::floor (fs * ConvolutionKernel::kMaxSeconds));
        tailCapacity = std::max (1, (maxTaps + partition - 1) / partition - 1);
        if (fft != nullptr)
            pffft_destroy_setup (fft);
        fft = pffft_new_setup (2 * partition, PFFFT_REAL);
        if (fft == nullptr)
            throw std::bad_alloc();

        const int reflectionMax = static_cast<int> (std::ceil (kMaxReflectionSeconds * fs)) + 4;
        const int airMax = static_cast<int> (std::ceil (kMaxPlacementDelaySeconds * fs)) + 4;
        // Bleed taps are read up to one control block back from the newest sample.
        const int bleedMax = static_cast<int> (std::ceil (kMaxBleedSeconds * fs)) + kChunk + 2;
        for (auto& mic : mics)
        {
            for (auto& v : mic.voices)
            {
                v.convolver.prepare (fft, partition, tailCapacity);
                v.reflection.prepare (reflectionMax);
                v.air.prepare (airMax);
                v.bleedLine.prepare (bleedMax);
            }
            for (auto* g : { &mic.x, &mic.y, &mic.distance, &mic.offAxis })
                g->reset (fs, kPlacementSmoothingSeconds);
            for (auto* g : { &mic.gainL, &mic.gainR, &mic.roomL, &mic.roomR })
                g->reset (fs, kGainRampSeconds);
            mic.micSwitch.reset (fs, kSwitchRampSeconds);
            mic.roomSwitch.reset (fs, kSwitchRampSeconds);
        }
        room.prepare (fs);
        fadeStep = static_cast<float> (1.0 / (kCrossfadeSeconds * fs));
        preroll = static_cast<float> (kPrerollSeconds / kCrossfadeSeconds);
        bounceCoeff = cab::onePoleCoeff (cab::kBounceLpHz, fs);
        prepared = true;
        applyTargets();
        reset();
    }

    /// Public reset: clears state and makes the next setSettings() start in its final state, as
    /// every module does after prepare/reset (no crossfade from the default settings).
    void reset() noexcept
    {
        resetState();
        snapNext = true;
    }

    /// Settings arrive once per block just before process(). The first ones after a reset snap;
    /// if audio was processed before them (on the defaults), they glide instead of cutting.
    void setSettings (const Settings& s) noexcept
    {
        settings = s;
        applyTargets();
        if (snapNext)
        {
            snapNext = false;
            resetState();
        }
    }

    /// Clears all state and snaps voices and smoothers to the current targets. Does not re-arm
    /// snapNext, so setSettings() can use it without snapping every block.
    void resetState() noexcept
    {
        if (!prepared)
            return;
        for (auto& mic : mics)
            for (auto& v : mic.voices)
                release (v);
        pollKernels();
        for (int m = 0; m < kNumMics; ++m)
        {
            Mic& mic = mics[static_cast<size_t> (m)];
            for (auto* g : { &mic.x, &mic.y, &mic.distance, &mic.offAxis })
                g->setCurrentAndTarget (g->target());
            for (auto* g : { &mic.gainL, &mic.gainR, &mic.roomL, &mic.roomR })
                g->setCurrentAndTarget (g->target());
            mic.micSwitch.snap();
            mic.roomSwitch.snap();
            wake (m);
        }
        room.reset();
        roomRunning = false;
        roomQuietSamples = 0;
    }

    void applyTargets() noexcept
    {
        for (int m = 0; m < kNumMics; ++m)
        {
            const MicSettings& s = settings.mics[static_cast<size_t> (m)];
            Mic& mic = mics[static_cast<size_t> (m)];
            const float defaultAzimuth = Settings {}.mics[static_cast<size_t> (m)].azimuthDeg;
            const cab::MicPose pose = cab::MicPose::polar (
                sanitise (s.position, 0.0f, 1.0f, 0.5f),
                std::isfinite (s.azimuthDeg) ? std::fmod (s.azimuthDeg, 360.0f) : defaultAzimuth,
                sanitise (s.distance, 0.0f, 1.0f, 0.0f),
                sanitise (s.offAxisDeg, 0.0f, cab::kMaxOffAxisDeg, 0.0f));
            mic.x.setTarget (pose.x);
            mic.y.setTarget (pose.y);
            mic.distance.setTarget (pose.distance);
            mic.offAxis.setTarget (pose.offAxisDeg / cab::kMaxOffAxisDeg);

            const float level = dbToGain (sanitise (s.levelDb, -100.0f, 24.0f, 0.0f));
            const PanGains pan = constantPowerPan (s.pan);
            mic.gainL.setTarget (level * pan.l);
            mic.gainR.setTarget (level * pan.r);
            mic.micSwitch.setTarget (s.enabled ? (s.invertPolarity ? -1.0f : 1.0f) : 0.0f);

            const float roomLevel = dbToGain (sanitise (s.roomLevelDb, -100.0f, 24.0f, -6.0f));
            const PanGains roomPan =
                constantPowerPan (sanitise (s.pan, -1.0f, 1.0f, 0.0f) * kRoomPanWidth);
            mic.roomL.setTarget (roomLevel * roomPan.l);
            mic.roomR.setTarget (roomLevel * roomPan.r);
            mic.roomSwitch.setTarget (s.roomEnabled ? 1.0f : 0.0f);
        }
    }

    // --- custom IR slots ------------------------------------------------------------------------

    bool voiceUses (const ConvolutionKernel* k) const noexcept
    {
        for (const auto& mic : mics)
            for (const auto& v : mic.voices)
                if (v.live && v.config.kernel == k)
                    return true;
        return false;
    }

    /// Audio thread, once per block: picks up newly published kernels.
    void pollKernels() noexcept
    {
        for (auto& s : slots)
        {
            if (s.holding)
            {
                if (voiceUses (s.retiredInUse))
                    continue; // acquire() could not swap anyway: the retire slot is occupied
                s.retiredInUse = nullptr;
                s.holding = false;
                s.busy.store (false, std::memory_order_release);
            }

            bool expected = false;
            if (!s.busy.compare_exchange_strong (expected, true, std::memory_order_acquire))
                continue; // the collector is freeing this slot right now; try next block

            const ConvolutionKernel* k = s.handoff.acquire();
            if (k != s.current)
            {
                const ConvolutionKernel* old = s.current;
                s.current = k;
                if (old != nullptr && voiceUses (old))
                {
                    s.retiredInUse = old;
                    s.holding = true;
                    continue;
                }
            }
            s.busy.store (false, std::memory_order_release);
        }
    }

    void collectGarbage()
    {
        for (auto& s : slots)
        {
            bool expected = false;
            if (s.busy.compare_exchange_strong (expected, true, std::memory_order_acquire))
            {
                s.handoff.collect();
                s.busy.store (false, std::memory_order_release);
            }
        }
    }

    // --- voices ---------------------------------------------------------------------------------

    VoiceConfig targetConfig (int m) const noexcept
    {
        const int cab = std::clamp (settings.cab, 0, kNumCabs - 1);
        const int type = std::clamp (settings.mics[static_cast<size_t> (m)].type, 0, kCustomIrType);
        if (type == kCustomIrType)
        {
            const ConvolutionKernel* k = slots[static_cast<size_t> (m * kNumCabs + cab)].current;
            if (k != nullptr && std::abs (k->sampleRate() - fs) < 0.5 &&
                mics[0].voices[0].convolver.accepts (*k))
                return { cab, kCustomIrType, k };
            return { cab, 0, nullptr }; // no usable IR: fall back to Dyn Classic
        }
        return { cab, type, nullptr };
    }

    static void release (Voice& v) noexcept
    {
        v.live = false;
        v.config.kernel = nullptr;
    }

    void setPlacementTargets (Voice& v, const cab::Placement& pl) const noexcept
    {
        v.nextArrival = static_cast<float> (pl.arrivalSec * fs);
        v.nextBounce = static_cast<float> (pl.bounceSec * fs);
        v.nextBounceGain = pl.bounceGain;
        v.nextLevel = pl.levelGain;
        for (int k = 0; k < v.numBleed; ++k)
        {
            const cab::Bleed& b = pl.bleed[static_cast<size_t> (k)];
            v.nextBleedDelay[static_cast<size_t> (k)] = static_cast<float> (b.delaySec * fs);
            v.nextBleedGain[static_cast<size_t> (k)] = b.gain;
        }
    }

    /// A custom IR keeps its own position and distance; turning the capsule still changes what
    /// it hears, so the generic dynamic capsule's off-axis model follows the IR.
    void designIrOffAxis (Voice& v, float angle, bool glide) const noexcept
    {
        const cab::MicModel& capsule = cab::micModel (cab::kIrCapsuleType);
        cab::Placement pl;
        const float amount = cab::offAxisAmount (angle);
        pl.presenceCutDb = -capsule.presenceDb * amount;
        pl.offAxisShelfDb = capsule.offAxisDb * amount;
        std::array<cab::Stage, cab::kOffAxisStages> fresh {};
        cab::designOffAxisStages (capsule, pl, fs, fresh.data());
        for (int i = 0; i < cab::kOffAxisStages; ++i)
        {
            if (glide)
                v.stages[static_cast<size_t> (i)].rampTo (fresh[static_cast<size_t> (i)].c);
            else
                v.stages[static_cast<size_t> (i)].c = fresh[static_cast<size_t> (i)].c;
        }
        v.nextLevel = cab::polarGain (capsule, angle);
    }

    static int proximityIndex (const Voice& v) noexcept { return v.numBody + cab::kPositionStages; }

    void startVoice (const Mic& mic, Voice& v, const VoiceConfig& cfg) noexcept
    {
        v.config = cfg;
        v.live = true;
        const cab::MicPose& pose = mic.applied;
        for (auto& st : v.stages)
        {
            st.reset();
            st.ramp = false;
        }
        if (cfg.kernel != nullptr)
        {
            v.convolver.reset();
            designIrOffAxis (v, pose.offAxisDeg, false);
            v.level = v.nextLevel;
            v.offAxisLive = pose.offAxisDeg > 0.0f;
            return;
        }

        const cab::CabModel& cabModel = cab::cabModel (cfg.cab);
        const cab::MicModel& micModel = cab::micModel (cfg.type);
        const cab::Placement pl = cab::placement (cabModel, micModel, pose);
        v.numBody = cab::designBodyStages (cabModel, fs, v.stages.data());
        cab::designPositionStages (cabModel, pose.position(), fs, v.stages.data() + v.numBody);
        cab::designProximityStage (micModel, pl, fs, v.stages.data() + proximityIndex (v));
        cab::designAirStage (pl, fs, v.stages.data() + proximityIndex (v) + 1);
        v.capsuleIndex = v.numBody + cab::kNearStages;
        v.offAxisIndex =
            v.capsuleIndex + cab::designMicStages (micModel, fs, v.stages.data() + v.capsuleIndex);
        cab::designOffAxisStages (micModel, pl, fs, v.stages.data() + v.offAxisIndex);
        v.offAxisLive = pose.offAxisDeg > 0.0f;

        v.norm = calibration.voice[static_cast<size_t> (cfg.cab)][static_cast<size_t> (cfg.type)];
        v.roomNorm = calibration.body[static_cast<size_t> (cfg.cab)];

        v.reflection.reset();
        v.reflectionDelay =
            std::max (1, static_cast<int> (std::lround (cabModel.reflectionMs * 0.001 * fs)));
        v.reflectionGain = cabModel.reflectionGain;
        v.reflectionCoeff = cab::onePoleCoeff (cabModel.reflectionLpHz, fs);
        v.reflectionState = 0.0f;

        v.numBleed = pl.numBleed;
        v.bleedLine.reset();
        for (int k = 0; k < v.numBleed; ++k)
        {
            auto& lp = v.bleedLp[static_cast<size_t> (k)];
            lp.reset();
            lp.ramp = false;
            lp.c = cab::designBleedFilter (pl.bleed[static_cast<size_t> (k)], fs);
        }
        v.bleedShelf.reset();
        v.bleedShelf.ramp = false;
        v.bleedShelf.c = cab::designBleedShelf (pl, fs);

        v.air.reset();
        v.bounceState = 0.0f;
        setPlacementTargets (v, pl);
        v.arrival = v.nextArrival;
        v.bounce = v.nextBounce;
        v.bounceGain = v.nextBounceGain;
        v.level = v.nextLevel;
        v.bleedDelay = v.nextBleedDelay;
        v.bleedGain = v.nextBleedGain;
    }

    /// Brings an idle mic back: no crossfade, the voice starts clean on the current target. While
    /// idle, processMic() did not advance the smoothers, so they snap too; that is inaudible
    /// because both switches sit at exactly 0 until they ramp in.
    void wake (int m) noexcept
    {
        Mic& mic = mics[static_cast<size_t> (m)];
        for (auto* g : { &mic.x, &mic.y, &mic.distance, &mic.offAxis })
            g->setCurrentAndTarget (g->target());
        for (auto* g : { &mic.gainL, &mic.gainR, &mic.roomL, &mic.roomR })
            g->setCurrentAndTarget (g->target());
        mic.applied = { mic.x.current(), mic.y.current(), mic.distance.current(),
                        mic.offAxis.current() * cab::kMaxOffAxisDeg };
        release (mic.voices[0]);
        release (mic.voices[1]);
        mic.active = 0;
        mic.fading = false;
        mic.fade = 0.0f;
        mic.idle = false;
        startVoice (mic, mic.voices[0], targetConfig (m));
    }

    /// The pose glides (one-pole, in Cartesian coordinates so a move across the cone or across
    /// the azimuth wrap takes the short way); filters follow once per control block.
    void updatePlacement (Mic& mic, int n) noexcept
    {
        const auto glide = [n] (ExpSmoothed& s) {
            float v = s.skip (n);
            if (std::abs (v - s.target()) < kSnapTolerance)
            {
                s.setCurrentAndTarget (s.target());
                v = s.target();
            }
            return v;
        };
        const cab::MicPose pose { glide (mic.x), glide (mic.y), glide (mic.distance),
                                  glide (mic.offAxis) * cab::kMaxOffAxisDeg };
        if (pose == mic.applied)
            return;
        const cab::MicPose old = mic.applied;
        mic.applied = pose;
        const bool moved = pose.x != old.x || pose.y != old.y;
        const bool pulled = pose.distance != old.distance;
        const bool turned = pose.offAxisDeg != old.offAxisDeg;
        const float position = pose.position();
        const bool across = position != old.position();

        const auto rampGroup = [] (cab::Stage* to, const cab::Stage* from, int count) {
            for (int i = 0; i < count; ++i)
                to[i].rampTo (from[i].c);
        };
        std::array<cab::Stage, cab::kPositionStages> fresh {};
        for (auto& v : mic.voices)
        {
            if (!v.live)
                continue;
            if (v.config.kernel != nullptr)
            {
                if (turned)
                {
                    designIrOffAxis (v, pose.offAxisDeg, true);
                    v.offAxisLive = true;
                }
                continue;
            }
            const cab::CabModel& cabModel = cab::cabModel (v.config.cab);
            const cab::MicModel& micModel = cab::micModel (v.config.type);
            const cab::Placement pl = cab::placement (cabModel, micModel, pose);
            if (across)
            {
                cab::designPositionStages (cabModel, position, fs, fresh.data());
                rampGroup (v.stages.data() + v.numBody, fresh.data(), cab::kPositionStages);
            }
            if (pulled || turned)
            {
                cab::designProximityStage (micModel, pl, fs, fresh.data());
                rampGroup (v.stages.data() + proximityIndex (v), fresh.data(), 1);
            }
            if (pulled)
            {
                cab::designAirStage (pl, fs, fresh.data());
                rampGroup (v.stages.data() + proximityIndex (v) + 1, fresh.data(), 1);
            }
            if (turned)
            {
                cab::designOffAxisStages (micModel, pl, fs, fresh.data());
                rampGroup (v.stages.data() + v.offAxisIndex, fresh.data(), cab::kOffAxisStages);
                v.offAxisLive = true;
            }
            // The neighbours' filters glide too: stepping them once per block instead leaves
            // three times the residue under automation.
            if (moved || pulled)
                for (int k = 0; k < v.numBleed; ++k)
                    v.bleedLp[static_cast<size_t> (k)].rampTo (
                        cab::designBleedFilter (pl.bleed[static_cast<size_t> (k)], fs));
            if (v.numBleed > 0)
                v.bleedShelf.rampTo (cab::designBleedShelf (pl, fs));
            setPlacementTargets (v, pl);
        }
    }

    /// The other speakers, added to the miked cone's sound before the capsule. Delays and gains
    /// are interpolated across the block like the time of flight; K neighbours run in one pass so
    /// their filter recursions overlap, gliding (Ramp) or not.
    template <int K, bool Ramp>
    void runBleed (Voice& v, float* out, int n) noexcept
    {
        const float inv = 1.0f / static_cast<float> (n);
        std::array<cab::Stage*, K + 1> st {};
        std::array<BiquadCoeffs, K + 1> c {}, d {};
        std::array<float, K + 1> s1 {}, s2 {};
        std::array<float, K> delay {}, dDelay {}, gain {}, dGain {};
        for (size_t k = 0; k <= static_cast<size_t> (K); ++k)
        {
            st[k] = k < static_cast<size_t> (K) ? &v.bleedLp[k] : &v.bleedShelf;
            c[k] = st[k]->c;
            s1[k] = st[k]->s1;
            s2[k] = st[k]->s2;
            if constexpr (Ramp)
            {
                const BiquadCoeffs& t = st[k]->ramp ? st[k]->target : st[k]->c;
                d[k] = { (t.b0 - c[k].b0) * inv, (t.b1 - c[k].b1) * inv, (t.b2 - c[k].b2) * inv,
                         (t.a1 - c[k].a1) * inv, (t.a2 - c[k].a2) * inv };
            }
            if (k < static_cast<size_t> (K))
            {
                delay[k] = v.bleedDelay[k];
                dDelay[k] = (v.nextBleedDelay[k] - v.bleedDelay[k]) * inv;
                gain[k] = v.bleedGain[k];
                dGain[k] = (v.nextBleedGain[k] - v.bleedGain[k]) * inv;
            }
        }
        const auto section = [&] (size_t k, float x) {
            if constexpr (Ramp)
            {
                c[k].b0 += d[k].b0;
                c[k].b1 += d[k].b1;
                c[k].b2 += d[k].b2;
                c[k].a1 += d[k].a1;
                c[k].a2 += d[k].a2;
            }
            const float y = c[k].b0 * x + s1[k];
            s1[k] = c[k].b1 * x - c[k].a1 * y + s2[k];
            s2[k] = c[k].b2 * x - c[k].a2 * y;
            return y;
        };
        for (int i = 0; i < n; ++i)
        {
            // The whole block is already in the line: sample i sits n - 1 - i samples back.
            const auto step = static_cast<float> (i + 1);
            const auto back = static_cast<float> (n - 1 - i);
            float sum = 0.0f;
            for (size_t k = 0; k < static_cast<size_t> (K); ++k)
                sum += (gain[k] + dGain[k] * step) *
                       section (k, v.bleedLine.readCubic (delay[k] + dDelay[k] * step + back));
            out[i] += section (static_cast<size_t> (K), sum);
        }
        for (size_t k = 0; k <= static_cast<size_t> (K); ++k)
        {
            st[k]->s1 = flushToZero (s1[k]);
            st[k]->s2 = flushToZero (s2[k]);
            if constexpr (Ramp)
            {
                if (st[k]->ramp)
                {
                    st[k]->c = st[k]->target;
                    st[k]->ramp = false;
                }
            }
        }
        v.bleedDelay = v.nextBleedDelay;
        v.bleedGain = v.nextBleedGain;
    }

    void addBleed (Voice& v, float* out, int n) noexcept
    {
        bool ramping = v.bleedShelf.ramp;
        for (int k = 0; k < v.numBleed; ++k)
            ramping = ramping || v.bleedLp[static_cast<size_t> (k)].ramp;
        switch (v.numBleed)
        {
        case 1:
            ramping ? runBleed<1, true> (v, out, n) : runBleed<1, false> (v, out, n);
            break;
        case 2:
            ramping ? runBleed<2, true> (v, out, n) : runBleed<2, false> (v, out, n);
            break;
        default:
            ramping ? runBleed<3, true> (v, out, n) : runBleed<3, false> (v, out, n);
            break;
        }
    }

    /// Off-axis stages: identity on axis, so they run only while the capsule is (or was just)
    /// turned, and stop once back on axis with their memory negligible.
    void runOffAxis (Voice& v, cab::Stage* st, float* out, int n, bool onAxis) noexcept
    {
        if (!v.offAxisLive)
            return;
        cab::runStages (st, cab::kOffAxisStages, out, n);
        if (!onAxis)
            return;
        float peakOut = 0.0f, state = 0.0f;
        for (int i = 0; i < n; ++i)
            peakOut = std::max (peakOut, std::abs (out[i]));
        for (int i = 0; i < cab::kOffAxisStages; ++i)
            state = std::max ({ state, std::abs (st[i].s1), std::abs (st[i].s2) });
        if (state <= std::max (kOffAxisFloor, kOffAxisRest * peakOut))
        {
            v.offAxisLive = false;
            for (int i = 0; i < cab::kOffAxisStages; ++i)
                st[i].reset();
        }
    }

    /// Custom IR: the off-axis filter and the polar gain.
    void processIrOffAxis (Voice& v, float* out, int n, float angle) noexcept
    {
        if (!v.offAxisLive)
            return;
        const float inv = 1.0f / static_cast<float> (n);
        const float dLevel = (v.nextLevel - v.level) * inv;
        for (int i = 0; i < n; ++i)
            out[i] *= v.level + dLevel * static_cast<float> (i + 1);
        v.level = v.nextLevel;
        runOffAxis (v, v.stages.data(), out, n, angle == 0.0f && v.level == 1.0f);
    }

    void processVoice (Voice& v, const float* in, float* out, float* roomOut, int n,
                       float angle) noexcept
    {
        const auto bytes = sizeof (float) * static_cast<size_t> (n);
        if (v.config.kernel != nullptr)
        {
            v.convolver.process (*v.config.kernel, in, out, n);
            // The room mic does not care where the close capsule points.
            std::memcpy (roomOut, out, bytes);
            processIrOffAxis (v, out, n, angle);
            return;
        }

        if (v.reflectionGain != 0.0f)
        {
            for (int i = 0; i < n; ++i)
            {
                v.reflection.push (in[i]);
                v.reflectionState +=
                    v.reflectionCoeff * (v.reflection.read (v.reflectionDelay) - v.reflectionState);
                out[i] = in[i] + v.reflectionGain * v.reflectionState;
            }
            v.reflectionState = flushToZero (v.reflectionState);
        }
        else
        {
            std::memcpy (out, in, bytes);
        }

        cab::runStages (v.stages.data(), v.numBody, out, n);
        for (int i = 0; i < n; ++i)
            roomOut[i] = out[i] * v.roomNorm;
        if (v.numBleed > 0)
            for (int i = 0; i < n; ++i)
                v.bleedLine.push (out[i]);
        cab::runStages (v.stages.data() + v.numBody, cab::kNearStages, out, n);
        if (v.numBleed > 0)
            addBleed (v, out, n);
        cab::runStages (v.stages.data() + v.capsuleIndex, v.offAxisIndex - v.capsuleIndex, out, n);
        runOffAxis (v, v.stages.data() + v.offAxisIndex, out, n, angle == 0.0f);

        // Time of flight and floor bounce, interpolated across the block while distance glides.
        const float inv = 1.0f / static_cast<float> (n);
        const float dArrival = (v.nextArrival - v.arrival) * inv;
        const float dBounce = (v.nextBounce - v.bounce) * inv;
        const float dGain = (v.nextBounceGain - v.bounceGain) * inv;
        const float dLevel = (v.nextLevel - v.level) * inv;
        for (int i = 0; i < n; ++i)
        {
            const auto step = static_cast<float> (i + 1);
            v.air.push (out[i]);
            const float direct = readFractional (v.air, v.arrival + dArrival * step);
            const float reflected = readFractional (v.air, v.bounce + dBounce * step);
            v.bounceState += bounceCoeff * (reflected - v.bounceState);
            out[i] = (direct + (v.bounceGain + dGain * step) * v.bounceState) *
                     (v.level + dLevel * step) * v.norm;
        }
        v.bounceState = flushToZero (v.bounceState);
        v.arrival = v.nextArrival;
        v.bounce = v.nextBounce;
        v.bounceGain = v.nextBounceGain;
        v.level = v.nextLevel;
    }

    static bool feedsRoom (const Mic& mic) noexcept
    {
        return mic.roomSwitch.target() != 0.0f || mic.roomSwitch.isRamping();
    }

    static bool audible (const Mic& mic) noexcept
    {
        return mic.micSwitch.target() != 0.0f || mic.micSwitch.isRamping() || feedsRoom (mic);
    }

    void processMic (int m, int n) noexcept
    {
        Mic& mic = mics[static_cast<size_t> (m)];
        if (!audible (mic))
        {
            if (!mic.idle)
            {
                mic.idle = true;
                release (mic.voices[0]);
                release (mic.voices[1]);
                mic.fading = false;
            }
            return;
        }
        if (mic.idle)
            wake (m);

        const VoiceConfig target = targetConfig (m);
        if (!mic.fading && !(target == mic.voices[static_cast<size_t> (mic.active)].config))
        {
            mic.active ^= 1;
            startVoice (mic, mic.voices[static_cast<size_t> (mic.active)], target);
            mic.fading = true;
            mic.fade = -preroll;
        }

        updatePlacement (mic, n);

        Voice& incoming = mic.voices[static_cast<size_t> (mic.active)];
        const float angle = mic.applied.offAxisDeg;
        processVoice (incoming, inBuf.data(), micBuf.data(), roomFeed.data(), n, angle);
        if (mic.fading)
        {
            Voice& outgoing = mic.voices[static_cast<size_t> (mic.active ^ 1)];
            processVoice (outgoing, inBuf.data(), fadeBuf.data(), fadeRoom.data(), n, angle);
            for (int i = 0; i < n; ++i)
            {
                mic.fade = std::min (1.0f, mic.fade + fadeStep);
                const float g = smoothstep (std::max (0.0f, mic.fade));
                micBuf[static_cast<size_t> (i)] =
                    fadeBuf[static_cast<size_t> (i)] +
                    g * (micBuf[static_cast<size_t> (i)] - fadeBuf[static_cast<size_t> (i)]);
                roomFeed[static_cast<size_t> (i)] =
                    fadeRoom[static_cast<size_t> (i)] +
                    g * (roomFeed[static_cast<size_t> (i)] - fadeRoom[static_cast<size_t> (i)]);
            }
            if (mic.fade >= 1.0f)
            {
                mic.fading = false;
                release (outgoing);
            }
        }

        for (int i = 0; i < n; ++i)
        {
            const auto idx = static_cast<size_t> (i);
            const float close = micBuf[idx] * mic.micSwitch.next();
            const float send = roomFeed[idx] * mic.roomSwitch.next();
            accL[idx] += close * mic.gainL.next();
            accR[idx] += close * mic.gainR.next();
            roomInL[idx] += send * mic.roomL.next();
            roomInR[idx] += send * mic.roomR.next();
        }
    }

    void process (const float* in, const AudioView& out) noexcept
    {
        const int total = out.numFrames();
        if (total <= 0 || out.numChannels() <= 0)
            return;
        if (!prepared || in == nullptr)
        {
            out.clear();
            return;
        }
        snapNext = false; // already running on the current settings: changes glide from here

        pollKernels();

        float* left = out.channel (0);
        float* right = out.numChannels() > 1 ? out.channel (1) : nullptr;
        for (int start = 0; start < total; start += kChunk)
        {
            const int n = std::min (kChunk, total - start);
            const auto bytes = sizeof (float) * static_cast<size_t> (n);
            std::memcpy (inBuf.data(), in + start, bytes); // `in` may alias an output channel
            std::memset (accL.data(), 0, bytes);
            std::memset (accR.data(), 0, bytes);
            std::memset (roomInL.data(), 0, bytes);
            std::memset (roomInR.data(), 0, bytes);

            bool roomFed = false;
            for (int m = 0; m < kNumMics; ++m)
            {
                processMic (m, n);
                roomFed = roomFed || feedsRoom (mics[static_cast<size_t> (m)]);
            }

            // The room keeps ringing after its sends close, then stops costing CPU.
            if (roomFed)
            {
                roomRunning = true;
                roomQuietSamples = 0;
            }
            if (roomRunning)
            {
                room.process (roomInL.data(), roomInR.data(), accL.data(), accR.data(), n);
                if (!roomFed && (roomQuietSamples += n) > room.tailSamples())
                {
                    roomRunning = false;
                    room.reset();
                }
            }

            if (right != nullptr)
            {
                std::memcpy (left + start, accL.data(), bytes);
                std::memcpy (right + start, accR.data(), bytes);
            }
            else
            {
                for (int i = 0; i < n; ++i)
                    left[start + i] =
                        (accL[static_cast<size_t> (i)] + accR[static_cast<size_t> (i)]) *
                        0.70710678f;
            }
        }
    }

    double fs = 48000.0;
    bool prepared = false;
    bool snapNext = true; // the first setSettings() after prepare/reset snaps
    Settings settings;
    std::array<Mic, kNumMics> mics;
    std::array<Slot, kNumSlots> slots;
    cab::RoomAmbience room;
    bool roomRunning = false;
    int roomQuietSamples = 0;
    cab::Calibration calibration;
    PFFFT_Setup* fft = nullptr;
    int partition = 256;
    int tailCapacity = 1;
    float fadeStep = 0.0f;
    float preroll = 0.0f;
    float bounceCoeff = 0.0f;

    std::array<float, kChunk> inBuf {}, micBuf {}, roomFeed {}, fadeBuf {}, fadeRoom {};
    std::array<float, kChunk> accL {}, accR {}, roomInL {}, roomInR {};
};

CabSim::CabSim() : impl_ (new Impl()) {}

CabSim::~CabSim()
{
    delete impl_;
}

void CabSim::prepare (const ProcessSpec& spec)
{
    impl_->prepare (spec);
}

void CabSim::reset() noexcept
{
    impl_->reset();
}

void CabSim::setSettings (const Settings& s) noexcept
{
    impl_->setSettings (s);
}

void CabSim::process (const float* in, const AudioView& out) noexcept
{
    impl_->process (in, out);
}

int CabSim::latencySamples() const noexcept
{
    return 0;
}

std::unique_ptr<ConvolutionKernel> CabSim::makeKernel (const float* ir, int length,
                                                       double irSampleRate, double targetRate,
                                                       int /*maxBlockSize*/)
{
    // The convolver has no latency at any block size, so the host block size does not matter.
    if (ir == nullptr || length <= 0 || !(irSampleRate > 0.0) || !(targetRate > 0.0))
        return nullptr;

    const int maxLength =
        std::max (1, static_cast<int> (std::floor (targetRate * ConvolutionKernel::kMaxSeconds)));
    std::vector<float> taps = cab::resampleIr (ir, length, irSampleRate, targetRate, maxLength);
    if (taps.empty())
        return nullptr;

    // Truncated IRs get a 10 ms half-cosine fade so the cut does not click or ring.
    if (std::ceil (length * (targetRate / irSampleRate)) > maxLength)
    {
        const int fade = std::min (static_cast<int> (taps.size()),
                                   std::max (1, static_cast<int> (0.01 * targetRate)));
        const size_t first = taps.size() - static_cast<size_t> (fade);
        for (int i = 0; i < fade; ++i)
            taps[first + static_cast<size_t> (i)] *=
                0.5f +
                0.5f * std::cos (kPi * static_cast<float> (i + 1) / static_cast<float> (fade));
    }

    float peak = 0.0f;
    for (float& t : taps)
    {
        if (!std::isfinite (t))
            t = 0.0f;
        peak = std::max (peak, std::abs (t));
    }

    // Trailing silence (below -120 dB re peak) costs CPU and adds nothing.
    size_t used = 1;
    for (size_t i = taps.size(); i-- > 0;)
    {
        if (std::abs (taps[i]) > peak * 1.0e-6f)
        {
            used = i + 1;
            break;
        }
    }
    taps.resize (used);

    float gain = 1.0f;
    if (peak > 0.0f)
    {
        const double loudness =
            cab::irLoudnessDb (taps.data(), static_cast<int> (taps.size()), targetRate);
        gain = static_cast<float> (std::clamp (std::pow (10.0, -loudness / 20.0), 1.0e-3, 1.0e3));
        for (float& t : taps)
            t *= gain;
    }
    return ConvolutionKernel::fromTaps (taps.data(), static_cast<int> (taps.size()), targetRate,
                                        gain);
}

void CabSim::setCustomIr (int mic, int cab, std::unique_ptr<ConvolutionKernel> kernel)
{
    if (mic < 0 || mic >= kNumMics || cab < 0 || cab >= kNumCabs)
        return;
    // An empty kernel stands for "cleared": RtHandoff cannot publish null.
    if (kernel == nullptr)
        kernel = std::make_unique<ConvolutionKernel>();
    // Free what the audio thread has let go of first (respecting the busy flag), so the retire
    // slot is empty and the new kernel can be picked up on the next block.
    impl_->collectGarbage();
    impl_->slots[static_cast<size_t> (mic * kNumCabs + cab)].handoff.publish (std::move (kernel));
}

void CabSim::collectGarbage()
{
    impl_->collectGarbage();
}

} // namespace sedef::dsp
