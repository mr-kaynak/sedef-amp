// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Doubler.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
constexpr float kMinSpreadMs = 3.0f;
constexpr float kMaxSpreadMs = 20.0f;
constexpr float kDefaultSpreadMs = 6.0f;

// Per-note offsets: spread * 2^(variation * z), z ~ unit normal (clamped). At variation 0.5 half
// the notes land within x0.71..x1.41 of the spread; at 1 within x0.5..x2.
constexpr float kTimingOctaves = 1.0f;
constexpr float kZLimit = 2.5f;
constexpr double kMinOffsetSeconds = 2.5e-3;
constexpr double kMaxOffsetSeconds = 40.0e-3;
// z mixes a "phrase" component (AR(1) across notes: a take that runs late tends to stay late for a
// few notes) with independent note jitter. Unit variance; lag-1 correlation 0.64 * 0.9 = 0.58.
constexpr float kPhraseRho = 0.9f;
constexpr float kPhraseWeight = 0.8f;
constexpr float kNoteWeight = 0.6f;

// Per-note detune, level and tone of the second take, uniform within +-(variation * these).
constexpr float kDetuneCents = 6.0f;
constexpr float kLevelDb = 1.5f;
constexpr float kToneDb = 3.0f;
constexpr double kToneSplitHz = 2000.0;

// The detune drifts the read position; it reverses before the drift exceeds
// clamp(0.3 * offset, 0.5 ms, 3 ms), so a long note's offset stays near its draw.
constexpr float kDriftLimitRatio = 0.3f;
constexpr double kMinDriftLimitSeconds = 0.5e-3;
constexpr double kMaxDriftLimitSeconds = 3.0e-3;
constexpr double kSlopeSeconds = 0.3;

// Micro pitch wander (~1 cent) so a held note is never frozen. Kept at 30 % at variation 0.
constexpr double kWanderSeconds = 0.2e-3;
constexpr float kWanderHz = 1.3f;
constexpr float kWanderFloor = 0.3f;

// Onset detector: power above 2.5 kHz, 1 ms against 40 ms. An onset is a rise of more than 4 dB
// over the recent level; the rise is timed from where the fast envelope left the slow one.
constexpr double kDetectorHz = 2500.0;
constexpr double kFastSeconds = 1.0e-3;
constexpr double kSlowSeconds = 40.0e-3;
constexpr float kOnsetRatio = 2.5f;
constexpr float kRiseRatio = 1.3f;
constexpr float kPowerFloor = 1.0e-7f; // about -70 dBFS
constexpr double kRefractorySeconds = 0.07;

// The crossfade to a new offset must finish before the note reaches either read position.
constexpr double kMarginSeconds = 0.5e-3;
constexpr double kMaxFadeSeconds = 6.0e-3;
constexpr double kMinFadeSeconds = 1.5e-3;
constexpr double kMinMoveSeconds = 0.25e-3;
// A re-timing replays (offset grows) or skips (offset shrinks) the audio between the two read
// positions, so it waits until the attack before the note has passed both by this much.
constexpr double kAttackGuardSeconds = 5.0e-3;
constexpr std::int64_t kLongAgo = -(std::int64_t { 1 } << 40);

constexpr double kWarmSeconds = 0.01;
constexpr double kGlideSeconds = 0.12;
constexpr double kSmoothSeconds = 0.03;
constexpr double kSoftenHz = 7500.0;
// The later take is lifted slightly, partly offsetting the precedence effect pulling the image
// towards the earlier side.
constexpr float kDoubleGainDb = 1.0f;

constexpr std::uint32_t kSeed = 0xD0B1E000u;

float finiteOr (float v, float fallback) noexcept
{
    return std::isfinite (v) ? v : fallback;
}
} // namespace

void Doubler::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    const double fs = sampleRate_;
    minOffset_ = static_cast<float> (kMinOffsetSeconds * fs);
    maxOffset_ = static_cast<float> (kMaxOffsetSeconds * fs);
    minDriftLimit_ = static_cast<float> (kMinDriftLimitSeconds * fs);
    maxDriftLimit_ = static_cast<float> (kMaxDriftLimitSeconds * fs);
    wanderSamples_ = static_cast<float> (kWanderSeconds * fs);
    minRead_ = static_cast<float> (0.5e-3 * fs);
    // Room for the drift to overshoot its limit while the detune turns round.
    maxRead_ = maxOffset_ + 2.0f * maxDriftLimit_ + wanderSamples_ + 4.0f;
    line_.prepare (static_cast<int> (std::ceil (maxRead_)) + 4);

    glideCoeff_ = 1.0 - std::exp (-1.0 / (kGlideSeconds * fs));
    detectorHp_.setCoeffs (design::highpass (fs, std::min (kDetectorHz, 0.45 * fs)));
    fastCoeff_ = static_cast<float> (1.0 - std::exp (-1.0 / (kFastSeconds * fs)));
    slowCoeff_ = static_cast<float> (1.0 - std::exp (-1.0 / (kSlowSeconds * fs)));
    refractoryLength_ = static_cast<int> (kRefractorySeconds * fs);
    marginSamples_ = static_cast<float> (kMarginSeconds * fs);
    maxFade_ = static_cast<float> (kMaxFadeSeconds * fs);
    minFade_ = static_cast<float> (kMinFadeSeconds * fs);
    minMove_ = static_cast<float> (kMinMoveSeconds * fs);
    holdoffLength_ = static_cast<int> (kSlowSeconds * fs);
    // An onset seen further ahead than this waits until this long before its note arrives.
    lookahead_ = static_cast<int> (std::ceil (maxFade_ + marginSamples_));
    attackGuard_ = static_cast<float> (kAttackGuardSeconds * fs);
    filledLimit_ = static_cast<int> (std::ceil (maxRead_)) + 8;
    toneCoeff_ = static_cast<float> (1.0 - std::exp (-kTwoPiD * kToneSplitHz / fs));
    slopeCoeff_ = static_cast<float> (1.0 - std::exp (-kControlBlock / (kSlopeSeconds * fs)));
    // A slope smoothed from +s to -s still travels about s * tau * (1 - ln 2) before it turns.
    slopeLead_ = static_cast<float> ((1.0 - std::log (2.0)) * kSlopeSeconds * fs);

    softenCoeff_ =
        static_cast<float> (1.0 - std::exp (-kTwoPiD * std::min (kSoftenHz, 0.45 * fs) / fs));
    softenGain_ = dbToGain (kDoubleGainDb);

    warmStep_ = static_cast<float> (1.0 / (kWarmSeconds * fs));
    panNear_.reset (fs, kSmoothSeconds);
    panFar_.reset (fs, kSmoothSeconds);
    mix_.reset (fs, kSmoothSeconds);
    targetSpread_ = static_cast<double> (settings_.spreadMs) * 1.0e-3 * fs;
    source_ = nullptr;
    lead_ = 0.0f;
    reset();
}

void Doubler::reset() noexcept
{
    line_.reset();
    detectorHp_.reset();
    soften_ = 0.0f;
    primed_ = false;
    for (auto& v : voices_)
        v = Voice { 1.0f, 0.0f, 0.0f, 0.0f, minDriftLimit_ };
    active_ = 0;
    fadeLength_ = fadePos_ = 0;
    spread1_ = spread2_ = targetSpread_;
    fast_ = slow_ = 0.0f;
    sinceRise_ = refractory_ = 0;
    holdoff_ = holdoffLength_;
    above_ = false;
    clock_ = 0;
    lastAttackRise_ = kLongAgo;
    pendingCount_ = 0;
    rng_.seed (kSeed);
    phrase_ = 0.0f;
    retimed_ = 0;
    toneLp_ = 0.0f;
    hfGain_ = hfTarget_ = level_ = levelTarget_ = 1.0f;
    hfStep_ = levelStep_ = 0.0f;
    rampLeft_ = 0;
    wander_.prepare (sampleRate_ / kControlBlock, kSeed + 1u);
    wander_.setRate (kWanderHz);
    wanderMod_ = wanderStep_ = 0.0f;
    tickCountdown_ = 0;
    // The onset source is kept: the engine sets it just before the slot that resets this module.
    filled_ = 0;
    warm_ = 0.0f;
    applyPan (true);
    snapNext_ = true;
}

void Doubler::applyPan (bool snap) noexcept
{
    // Constant-power pan: the original sits at -width, the double at +width. At width 1 these are
    // exactly 1 and 0, so the left side stays bit-identical to the input.
    const float theta = (1.0f - settings_.width) * 0.25f * kPi;
    const float near = std::cos (theta), far = std::sin (theta);
    if (snap)
    {
        panNear_.setCurrentAndTarget (near);
        panFar_.setCurrentAndTarget (far);
        mix_.setCurrentAndTarget (settings_.mix);
    }
    else
    {
        panNear_.setTarget (near);
        panFar_.setTarget (far);
        mix_.setTarget (settings_.mix);
    }
}

void Doubler::setSettings (const Settings& s) noexcept
{
    const float spread = finiteOr (s.spreadMs, kDefaultSpreadMs);
    settings_.spreadMs = std::clamp (spread, kMinSpreadMs, kMaxSpreadMs);
    settings_.variation = std::clamp (finiteOr (s.variation, 0.5f), 0.0f, 1.0f);
    settings_.width = std::clamp (finiteOr (s.width, 1.0f), 0.0f, 1.0f);
    settings_.mix = std::clamp (finiteOr (s.mix, 1.0f), 0.0f, 1.0f);
    targetSpread_ = static_cast<double> (settings_.spreadMs) * 1.0e-3 * sampleRate_;
    // Until audio runs after a reset, settings jump; after that they glide.
    if (snapNext_)
        spread1_ = spread2_ = targetSpread_;
    applyPan (snapNext_);
    snapNext_ = false;
}

float Doubler::gaussian() noexcept
{
    // Sum of three uniforms on [-1, 1): unit variance, bounded to +-3.
    return rng_.bipolar() + rng_.bipolar() + rng_.bipolar();
}

float Doubler::voiceDelay (const Voice& v) const noexcept
{
    return std::clamp (static_cast<float> (spread2_) * v.factor + v.drift + wanderMod_, minRead_,
                       maxRead_);
}

void Doubler::detectOnset() noexcept
{
    refractory_ = refractoryLength_;
    const std::int64_t rise = clock_ - sinceRise_;
    const std::int64_t previous = lastAttackRise_;
    lastAttackRise_ = rise;
    if (pendingCount_ == static_cast<int> (pending_.size()))
        return; // more onsets in flight than any real lead holds: this note keeps the take
    // Seen `lead` samples ahead of its arrival: act shortly before it arrives, so the stretch a
    // re-timing replays or skips lies just before this note, not further back.
    const auto lead = static_cast<std::int64_t> (lead_);
    const std::int64_t wait = std::max<std::int64_t> (0, lead - lookahead_);
    pending_[static_cast<size_t> (pendingCount_++)] = { clock_ + wait, rise + lead,
                                                        previous + lead };
}

void Doubler::retime (const PendingOnset& onset) noexcept
{
    if (fadePos_ < fadeLength_)
        return; // the previous note is still crossfading

    const float variation = settings_.variation;
    phrase_ = kPhraseRho * phrase_ + std::sqrt (1.0f - kPhraseRho * kPhraseRho) * gaussian();
    const float z =
        std::clamp (kPhraseWeight * phrase_ + kNoteWeight * gaussian(), -kZLimit, kZLimit);
    const float detuneCents = variation * kDetuneCents * rng_.bipolar();
    const float levelDb = variation * kLevelDb * rng_.bipolar();
    const float toneDb = variation * kToneDb * rng_.bipolar();

    const auto spread = static_cast<float> (spread2_);
    const float offset =
        std::clamp (spread * std::exp2 (variation * kTimingOctaves * z), minOffset_, maxOffset_);
    Voice& current = voices_[static_cast<size_t> (active_)];
    const float now = voiceDelay (current);
    const float incoming = offset + wanderMod_;

    // The change must be over before the note reaches either read position...
    const float room =
        std::min (now, incoming) + static_cast<float> (onset.arrival - clock_) - marginSamples_;
    if (room < minFade_)
        return; // detected too late to move this note cleanly; the next one will
    // ...must not start before the attack before it has passed both...
    if (static_cast<float> (clock_ - onset.previousArrival) <
        std::max (now, incoming) + attackGuard_)
        return; // a fast note just before: replaying or skipping it would be heard
    // ...and can only move the take to where the delay line holds what was played.
    if (static_cast<float> (filled_) <= incoming + 4.0f)
        return;

    const int length = std::max (1, static_cast<int> (std::min (room, maxFade_)));
    rampLeft_ = length;
    hfTarget_ = dbToGain (toneDb);
    levelTarget_ = dbToGain (levelDb);
    hfStep_ = (hfTarget_ - hfGain_) / static_cast<float> (length);
    levelStep_ = (levelTarget_ - level_) / static_cast<float> (length);
    const float slope = 1.0f - std::exp2 (detuneCents / 1200.0f);
    const float driftLimit = std::clamp (kDriftLimitRatio * offset, minDriftLimit_, maxDriftLimit_);

    if (std::abs (offset - (spread * current.factor + current.drift)) < minMove_)
    {
        // Already about there (variation near 0): no crossfade, but the note still gets its own
        // level, tone and detune.
        current.slopeTarget = slope;
        current.driftLimit = driftLimit;
        return;
    }

    Voice& next = voices_[static_cast<size_t> (active_ ^ 1)];
    next.factor = offset / spread;
    next.drift = 0.0f;
    next.slope = next.slopeTarget = slope;
    next.driftLimit = driftLimit;
    fadeLength_ = length;
    fadePos_ = 0;
    ++retimed_;
}

void Doubler::controlTick() noexcept
{
    const float depth =
        wanderSamples_ * (kWanderFloor + (1.0f - kWanderFloor) * settings_.variation);
    wanderStep_ = (depth * wander_.next() - wanderMod_) / static_cast<float> (kControlBlock);

    for (auto& v : voices_)
    {
        // Turn the detune around before the drift leaves its band (a long note's pitch then
        // swings slowly between +c and -c instead of the offset running away).
        const float ahead = v.drift + v.slope * slopeLead_; // where it would come to rest
        if ((ahead > 0.8f * v.driftLimit && v.slopeTarget > 0.0f) ||
            (ahead < -0.8f * v.driftLimit && v.slopeTarget < 0.0f))
            v.slopeTarget = -v.slopeTarget;
        v.slope += slopeCoeff_ * (v.slopeTarget - v.slope);
    }
}

void Doubler::setOnsetSource (const float* source, int leadSamples) noexcept
{
    source_ = source;
    lead_ = source != nullptr ? static_cast<float> (std::max (0, leadSamples)) : 0.0f;
}

void Doubler::processSamples (float* left, float* right, const float* source, int length) noexcept
{
    for (int i = 0; i < length; ++i)
    {
        const float l = left[i], r = right[i];

        spread1_ += glideCoeff_ * (targetSpread_ - spread1_);
        spread2_ += glideCoeff_ * (spread1_ - spread2_);

        const float h =
            detectorHp_.processSample (0, source != nullptr ? source[i] : 0.5f * (l + r));
        const float power = h * h;
        fast_ += fastCoeff_ * (power - fast_);
        slow_ += slowCoeff_ * (power - slow_);
        sinceRise_ =
            fast_ <= kRiseRatio * slow_ + kPowerFloor ? 0 : std::min (sinceRise_ + 1, 1 << 20);
        const bool above = fast_ > kOnsetRatio * slow_ + kPowerFloor;
        if (holdoff_ > 0)
        {
            // Just after a reset the detector has not heard what was already sounding: the slow
            // envelope catches up with it instead of that counting as an onset.
            --holdoff_;
            slow_ = std::max (slow_, fast_);
        }
        else if (refractory_ > 0)
        {
            --refractory_;
            if (above && !above_)
                lastAttackRise_ = clock_ - sinceRise_; // not an onset, but still an attack
        }
        else if (above)
            detectOnset();
        above_ = above;
        while (pendingCount_ > 0 && pending_[0].due <= clock_)
        {
            const PendingOnset due = pending_[0];
            std::copy (pending_.begin() + 1, pending_.begin() + pendingCount_, pending_.begin());
            --pendingCount_;
            retime (due);
        }

        line_.push (r);
        filled_ = std::min (filled_ + 1, filledLimit_);
        wanderMod_ += wanderStep_;
        for (auto& v : voices_)
            v.drift += v.slope;

        float take = line_.readCubic (voiceDelay (voices_[static_cast<size_t> (active_)]));
        if (fadePos_ < fadeLength_)
        {
            const float t = static_cast<float> (fadePos_ + 1) / static_cast<float> (fadeLength_);
            const float w = t * t * (3.0f - 2.0f * t);
            const float incoming =
                line_.readCubic (voiceDelay (voices_[static_cast<size_t> (active_ ^ 1)]));
            take += w * (incoming - take);
            if (++fadePos_ == fadeLength_)
            {
                active_ ^= 1;
                fadePos_ = fadeLength_ = 0;
            }
        }
        if (warm_ < 1.0f)
        {
            float needed = voiceDelay (voices_[static_cast<size_t> (active_)]);
            if (fadePos_ < fadeLength_)
                needed = std::max (needed, voiceDelay (voices_[static_cast<size_t> (active_ ^ 1)]));
            if (static_cast<float> (filled_) > needed + 4.0f)
                warm_ = std::min (1.0f, warm_ + warmStep_);
            take = r + warm_ * (take - r);
        }
        if (rampLeft_ > 0)
        {
            hfGain_ = --rampLeft_ > 0 ? hfGain_ + hfStep_ : hfTarget_;
            level_ = rampLeft_ > 0 ? level_ + levelStep_ : levelTarget_;
        }
        if (!primed_)
        {
            // Start the filters settled on the first sample: zero state under a note that is
            // already sounding would step.
            toneLp_ = take;
            soften_ = softenGain_ * take;
            primed_ = true;
        }
        toneLp_ += toneCoeff_ * (take - toneLp_);
        const float shaped = level_ * (toneLp_ + hfGain_ * (take - toneLp_));
        soften_ += softenCoeff_ * (softenGain_ * shaped - soften_);
        const float dbl = soften_;

        const float near = panNear_.next(), far = panFar_.next(), mix = mix_.next();
        left[i] = l + mix * (near * l + far * dbl - l);
        right[i] = r + mix * (far * l + near * dbl - r);
        ++clock_;
    }
}

void Doubler::process (const AudioView& io) noexcept
{
    // The onset source applies to this call only.
    const float* source = source_;
    source_ = nullptr;
    if (source == nullptr)
        lead_ = 0.0f;
    // Doubling is a stereo effect; a mono stream passes untouched (the engine folds to mono after
    // the post chain anyway, where the double would only comb-filter).
    if (io.numChannels() < 2)
        return;
    float* left = io.channel (0);
    float* right = io.channel (1);
    const int n = io.numFrames();
    for (int pos = 0; pos < n;)
    {
        if (tickCountdown_ == 0)
        {
            controlTick();
            tickCountdown_ = kControlBlock;
        }
        const int length = std::min (n - pos, tickCountdown_);
        processSamples (left + pos, right + pos, source != nullptr ? source + pos : nullptr,
                        length);
        pos += length;
        tickCountdown_ -= length;
    }
    lead_ = 0.0f;
    snapNext_ = false;
    fast_ = flushToZero (fast_);
    slow_ = flushToZero (slow_);
    toneLp_ = flushToZero (toneLp_);
    detectorHp_.snapToZero();
    soften_ = flushToZero (soften_);
}

} // namespace sedef::dsp
