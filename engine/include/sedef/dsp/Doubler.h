// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"
#include "sedef/core/Biquad.h"
#include "sedef/core/DelayLine.h"
#include "sedef/core/Lfo.h"
#include "sedef/core/Random.h"
#include "sedef/core/Smoothed.h"

#include <array>

namespace sedef::dsp
{

/// Double tracking. The left side is the part as played: untouched, with no delay. The right side
/// is a second "take" of it whose timing, pitch, level and tone are drawn afresh at every note
/// onset, the way a second performance differs from the first, and move only gently within a
/// note. See docs/dsp/doubler.md.
class Doubler
{
public:
    struct Settings
    {
        float spreadMs = 6.0f;  ///< 3..20 ms: typical gap between the takes (median over notes)
        float variation = 0.5f; ///< 0..1: how much the second take differs from note to note
        float width = 1.0f;     ///< 0..1: 1 = takes hard left and right, 0 = both in the centre
        float mix = 1.0f;       ///< 0..1: 0 = dry, 1 = the full double
    };

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// Optional, per call: the instrument signal that reaches this module's input `leadSamples`
    /// later (the DI ahead of amp and cab). Pick attacks are clearer there than in a compressed,
    /// distorted signal, and they show up sooner. Applies to the next process() only and must hold
    /// as many frames; without it, onsets are detected on the input itself.
    void setOnsetSource (const float* source, int leadSamples) noexcept;
    /// Stereo in place (2 channels). A mono stream passes untouched.
    void process (const AudioView& io) noexcept;

    /// Note onsets that re-timed the second take since reset() (diagnostics and tests).
    int retimedNotes() const noexcept { return retimed_; }

private:
    // A voice is one read position on the double's delay line. The active voice carries the take.
    // At an onset the other voice takes the new note's offset and the two crossfade while both
    // still read audio from before the note, so the note itself arrives once, at its new time.
    struct Voice
    {
        float factor = 1.0f; ///< offset as a multiple of the (gliding) spread
        float drift = 0.0f;  ///< samples: integrated detune within the note
        float slope = 0.0f;  ///< drift per sample (negative = sharper)
        float slopeTarget = 0.0f;
        float driftLimit = 0.0f;
    };

    void controlTick() noexcept;
    void processSamples (float* left, float* right, const float* source, int length) noexcept;
    void onset() noexcept;
    float gaussian() noexcept;
    float voiceDelay (const Voice& v) const noexcept;

    double sampleRate_ = 48000.0;
    Settings settings_;
    bool snapNext_ = true;

    DelayLine line_;
    std::array<Voice, 2> voices_;
    int active_ = 0;
    int fadeLength_ = 0, fadePos_ = 0; ///< crossfading while fadePos_ < fadeLength_
    float minOffset_ = 0.0f, maxOffset_ = 0.0f, minRead_ = 0.0f, maxRead_ = 0.0f;
    float minDriftLimit_ = 0.0f, maxDriftLimit_ = 0.0f;

    // Spread glides through two one-poles, so a change bends pitch smoothly instead of zipping.
    double targetSpread_ = 0.0, spread1_ = 0.0, spread2_ = 0.0, glideCoeff_ = 0.0;

    // Onset detector on the high-passed source (or mono sum): a fast against a slow power envelope.
    const float* source_ = nullptr;
    float lead_ = 0.0f;
    Biquad detectorHp_;
    float fastCoeff_ = 0.0f, slowCoeff_ = 0.0f, fast_ = 0.0f, slow_ = 0.0f;
    int sinceRise_ = 0, refractory_ = 0, refractoryLength_ = 0;
    float marginSamples_ = 0.0f, maxFade_ = 0.0f, minFade_ = 0.0f, minMove_ = 0.0f;

    // Per-note draws: a slowly wandering "phrase" component plus independent note jitter.
    Random rng_;
    float phrase_ = 0.0f;
    int retimed_ = 0;

    // The take's tone (gain of the band above the split) and level move to each note's draw over
    // the crossfade.
    float toneCoeff_ = 0.0f, toneLp_ = 0.0f;
    float hfGain_ = 1.0f, hfTarget_ = 1.0f, hfStep_ = 0.0f;
    float level_ = 1.0f, levelTarget_ = 1.0f, levelStep_ = 0.0f;
    int rampLeft_ = 0;
    // The double is softened (one-pole low-pass) and lifted 1 dB.
    float softenCoeff_ = 0.0f, softenGain_ = 1.0f, soften_ = 0.0f;
    bool primed_ = false; ///< the one-poles start from the first sample, not from zero

    // Micro pitch wander of the take; evaluated per control tick and interpolated in between.
    SmoothRandom wander_;
    float wanderMod_ = 0.0f, wanderStep_ = 0.0f, wanderSamples_ = 0.0f;
    float slopeCoeff_ = 0.0f;
    int tickCountdown_ = 0;

    // After a reset the delay line holds silence. Until it has filled past the take's offset the
    // take is the undelayed input, then it fades over, so switching on mid-note cannot step.
    int filled_ = 0;
    float warm_ = 0.0f, warmStep_ = 0.0f;

    // Width (constant-power pan of the two takes) and the dry/wet mix.
    LinearSmoothed panNear_, panFar_, mix_;
};

} // namespace sedef::dsp
