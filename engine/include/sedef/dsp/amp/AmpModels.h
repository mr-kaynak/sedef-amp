// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Biquad.h"
#include "sedef/core/Oversampler.h"
#include "sedef/core/Smoothed.h"
#include "sedef/dsp/amp/AmpSection.h"
#include "sedef/dsp/amp/AmpStages.h"
#include "sedef/dsp/amp/ToneStack.h"

#include <algorithm>
#include <array>

/// The three amp models behind AmpSection. Internal; see docs/dsp/amps.md for the voicings.
namespace sedef::dsp::amp
{

/// Knob ramps. Filter coefficients follow at control rate (every kControlBlock samples).
inline constexpr double kKnobRampSeconds = 0.03;
/// Switches (voice, bright, shift) glide more slowly: their filters sit ahead of high gain.
inline constexpr double kSwitchRampSeconds = 0.08;

/// `output` knob: -24 dB at 0, unity at 0.5, +12 dB at 1, linear in dB on each half.
inline float outputLevel (float knob) noexcept
{
    const float db = knob <= 0.5f ? -24.0f + 48.0f * knob : 24.0f * (knob - 0.5f);
    return dbToGain (db);
}

/// A fixed set of smoothed 0..1 controls.
template <size_t N>
class KnobSet
{
public:
    void prepare (double sampleRate) noexcept
    {
        for (auto& k : k_)
            k.reset (sampleRate, kKnobRampSeconds);
    }

    void setRampSeconds (size_t i, double sampleRate, double seconds) noexcept
    {
        k_[i].reset (sampleRate, seconds);
    }

    void set (size_t i, float v) noexcept { k_[i].setTarget (v); }
    void snap() noexcept
    {
        for (auto& k : k_)
            k.setCurrentAndTarget (k.target());
    }

    /// Advances all ramps by n samples; true if any value moved.
    bool advance (int n) noexcept
    {
        bool moved = false;
        for (auto& k : k_)
            if (k.isSmoothing())
            {
                k.skip (n);
                moved = true;
            }
        return moved;
    }

    float operator[] (size_t i) const noexcept { return k_[i].current(); }

private:
    std::array<LinearSmoothed, N> k_;
};

/// Shared scaffolding: rates, the oversampler, and splitting blocks into control-rate chunks.
/// Derived classes implement processChunk (at most kControlBlock frames).
template <typename Derived>
class ModelBase
{
public:
    void process (const AudioView& io) noexcept
    {
        for (int start = 0; start < io.numFrames(); start += kControlBlock)
        {
            const int n = std::min (kControlBlock, io.numFrames() - start);
            static_cast<Derived*> (this)->processChunk (io.subView (start, n));
        }
    }

    int latencySamples() const noexcept { return os_.latencySamples(); }

protected:
    void prepareBase (double sampleRate)
    {
        fs_ = sampleRate;
        const int factorLog2 = Oversampler::factorLog2For (sampleRate);
        os_.prepare (kControlBlock, kMaxChannels, factorLog2);
        fsOs_ = sampleRate * static_cast<double> (1 << factorLog2);
    }

    double fs_ = 48000.0;
    double fsOs_ = 192000.0;
    Oversampler os_;
};

/// Clean: two triode stages around a tweed-style tone stack, a push-pull power amp with gentle sag.
class LagoonModel : public ModelBase<LagoonModel>
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void setSettings (const LagoonSettings& s) noexcept;
    void processChunk (const AudioView& io) noexcept;

private:
    enum Knob : size_t
    {
        Volume,
        Bass,
        Cut,
        Master,
        Output,
        NumKnobs
    };

    void updateCoeffs() noexcept;
    void snapState() noexcept;

    KnobSet<NumKnobs> knobs_;
    FirstOrder inHpf_;
    TriodeStage v1_, v2_;
    FirstOrder v1Lpf_, v2Hpf_, v2Shelf_, v2Lpf_, piHpf_;
    MixShelf bright_;
    ToneStackFilter stack_;
    PowerAmp pa_;
    Biquad otHpf_;
    FirstOrder otLpf_, cut_;
    GainRamp brightG_, v2Drive_, paDrive_, out_;
};

/// Crunch: cascaded stages into a cathode follower and British-style tone stack; three voicings.
class ReefModel : public ModelBase<ReefModel>
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void setSettings (const ReefSettings& s) noexcept;
    void processChunk (const AudioView& io) noexcept;

private:
    enum Knob : size_t
    {
        Voice, ///< 0..2, morphs between voicings
        Gain,
        Bass,
        Mid,
        Treble,
        Presence,
        Master,
        Output,
        NumKnobs
    };

    void updateCoeffs() noexcept;
    void snapState() noexcept;

    KnobSet<NumKnobs> knobs_;
    FirstOrder inHpf_;
    MixShelf cathode_, bright_, voiceTop_, presence_, weight_;
    TriodeStage v1a_, v1b_, v2a_;
    FirstOrder v1aLpf_, gainHpf_, v1bLpf_, v2aHpf_, v2aLpf_, piHpf_;
    ToneStackFilter stack_;
    PowerAmp pa_;
    Biquad otHpf_;
    FirstOrder otLpf_;
    GainRamp cathodeG_, brightG_, voiceTopG_, presenceG_, weightG_;
    GainRamp v1bDrive_, paDrive_, out_;
};

/// Lead: four cascaded stages with progressively tighter coupling, a tone stack with a movable mid
/// scoop, and a power amp with presence and resonance.
class AbyssModel : public ModelBase<AbyssModel>
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void setSettings (const AbyssSettings& s) noexcept;
    void processChunk (const AudioView& io) noexcept;

private:
    enum Knob : size_t
    {
        Bright, ///< 0/1, smoothed
        Gain,
        Bass,
        Mid,
        Treble,
        Shift, ///< 0/1, smoothed
        Presence,
        Master,
        Output,
        NumKnobs
    };

    void updateCoeffs() noexcept;
    void snapState() noexcept;

    KnobSet<NumKnobs> knobs_;
    FirstOrder inHpf_, v1Shelf_;
    TriodeStage v1_, v2_, v3_, v4_;
    FirstOrder v1Lpf_, gainHpf_, v2Lpf_, v3Hpf_, v3Shelf_, v3Lpf_, v4Hpf_, v4Lpf_;
    FirstOrder piHpf_, resonance_;
    MixShelf bright_, presence_;
    ToneStackFilter stack_;
    PowerAmp pa_;
    Biquad otHpf_;
    FirstOrder otLpf_;
    GainRamp brightG_, presenceG_, v2Drive_, paDrive_, out_;
};

} // namespace sedef::dsp::amp
