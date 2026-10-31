// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Drift.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
constexpr std::array<float, 21> kNoteBeats {
    1.0f / 24.0f, 1.0f / 16.0f, 1.0f / 12.0f, 3.0f / 32.0f, 1.0f / 8.0f, 1.0f / 6.0f, 3.0f / 16.0f,
    1.0f / 4.0f,  1.0f / 3.0f,  3.0f / 8.0f,  1.0f / 2.0f,  2.0f / 3.0f, 3.0f / 4.0f, 1.0f,
    4.0f / 3.0f,  3.0f / 2.0f,  2.0f,         8.0f / 3.0f,  3.0f,        4.0f,        6.0f,
};

constexpr float kMinTimeMs = 16.0f;
constexpr float kMaxTimeMs = 2500.0f;
constexpr double kTapeSeconds = 2.6;

// Time changes: a critically damped follower with a 60 ms time constant settles in ~0.3 s (the
// same response as two cascaded one-poles). Its velocity is a state clamped to 0.25x..2x playback
// speed, so limits engage and release without pitch kinks; the tape never runs backwards, and
// speeding up (where Hermite reads would alias) stays within what the record path's HF loss keeps
// below Nyquist, even when feedback replays material that was itself sped up.
constexpr double kGlideSeconds = 0.06;
constexpr double kMaxShrinkPerSample = 1.0;
constexpr double kMaxGrowPerSample = 0.75;

constexpr double kFadeSeconds = 0.02;
constexpr double kParamSeconds = 0.05;

// Head positions as fractions of the delay time, and each pattern's level per position.
//   1 head: the beat.  2 heads: straight eighths.  3 heads: triplets.
//   4 heads: swung sixteenths (the off-beat sixteenth of each eighth pushed to the triplet).
constexpr std::array<double, 5> kSlotFraction { 1.0 / 3.0, 1.0 / 2.0, 2.0 / 3.0, 5.0 / 6.0, 1.0 };
constexpr std::array<std::array<float, 5>, 4> kHeadLevels { {
    { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f },
    { 0.0f, 0.6f, 0.0f, 0.0f, 0.8f },
    { 0.5f, 0.0f, 0.6f, 0.0f, 0.62f },
    { 0.4f, 0.55f, 0.0f, 0.4f, 0.6f },
} };
constexpr size_t kLastSlot = 4;

// Feedback knob 1.0 is a loop gain just above unity so the echo can run away; the soft ceiling
// in the loop sets where the oscillation settles (around -10 dBFS on tape with neutral tone).
// When disabled the loop is held below unity so a spillover tail always dies.
constexpr float kLoopGainAtFull = 1.05f;
constexpr float kIdleLoopGain = 0.9f;
constexpr float kLoopCeiling = 0.7f;

// Tape drive: k from 0.5 (about -0.5 dB of compression at full scale, ceiling 2) to 5,
// scaled so a -10 dBFS-ish peak keeps its level.
constexpr float kDriveMinK = 0.5f;
constexpr float kDriveRange = 10.0f;
constexpr float kDriveNominal = 0.3f;

constexpr double kBassShelfHz = 220.0;
constexpr double kTrebleShelfHz = 2200.0;
constexpr double kEmphasisHz = 2500.0;
constexpr double kEmphasisDb = 6.0;
constexpr double kLowLossHz = 45.0;
constexpr double kHighLossHz = 6800.0;
constexpr double kHighLossQ = 0.6; // below Butterworth: no peak, so the loop gain bound holds

constexpr double kWowHz = 0.6;
constexpr double kFlutterHz = 6.3;
constexpr double kWowDepthSeconds = 1.6e-3;      // ~±8 cents of wow at full tape mod
constexpr double kFlutterDepthSeconds = 0.05e-3; // ~±2 cents of flutter

constexpr float kSilence = 1.0e-6f;

float knobToShelfDb (float knob) noexcept
{
    // Cuts reach further than boosts: a darkening repeat is the common move, and boosts inside a
    // feedback loop quickly make it run away.
    return knob < 0.5f ? -12.0f * (1.0f - 2.0f * knob) : 6.0f * (2.0f * knob - 1.0f);
}

/// Mix law: each side is at full level from the centre outwards, so mix 0.5 is dry + full echo.
/// The 1.5 power gives finer control over quiet echo settings than a straight line.
float mixCurve (float x) noexcept
{
    const float r = std::min (1.0f, 2.0f * x);
    return r * std::sqrt (r);
}

float clampOr (float x, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (x) ? std::clamp (x, lo, hi) : fallback;
}

Drift::Settings sanitise (const Drift::Settings& s) noexcept
{
    Drift::Settings o = s;
    o.heads = std::clamp (s.heads, 0, 3);
    o.timeMs = clampOr (s.timeMs, kMinTimeMs, kMaxTimeMs, 500.0f);
    o.sync = std::clamp (s.sync, 0, 2);
    o.note = std::clamp (s.note, 0, static_cast<int> (kNoteBeats.size()) - 1);
    o.tapBpm = clampOr (s.tapBpm, 1.0f, 1000.0f, 120.0f);
    o.feedback = clampOr (s.feedback, 0.0f, 1.0f, 0.0f);
    o.tapeMod = clampOr (s.tapeMod, 0.0f, 1.0f, 0.0f);
    o.tapeDrive = clampOr (s.tapeDrive, 0.0f, 1.0f, 0.0f);
    o.bass = clampOr (s.bass, 0.0f, 1.0f, 0.5f);
    o.treble = clampOr (s.treble, 0.0f, 1.0f, 0.5f);
    o.width = clampOr (s.width, 0.0f, 1.0f, 1.0f);
    o.mix = clampOr (s.mix, 0.0f, 1.0f, 0.0f);
    return o;
}

/// Product of two first-order sections as one biquad.
BiquadCoeffs cascade (const BiquadCoeffs& p, const BiquadCoeffs& q) noexcept
{
    return { p.b0 * q.b0, p.b0 * q.b1 + p.b1 * q.b0, p.b1 * q.b1, p.a1 + q.a1, p.a1 * q.a1 };
}

/// Hermite read with the integer/fraction split done in double: at multi-second delays a float
/// position only resolves 1/64 sample, which would add audible jitter to a modulated read.
float readTape (const DelayLine& line, double delay) noexcept
{
    delay = std::clamp (delay, 1.0, static_cast<double> (line.maxDelay()));
    const int i = static_cast<int> (delay);
    const float t = static_cast<float> (delay - i);
    const float xm1 = line.read (i - 1);
    const float x0 = line.read (i);
    const float x1 = line.read (i + 1);
    const float x2 = line.read (i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}
} // namespace

float Drift::noteBeats (int noteIndex) noexcept
{
    return kNoteBeats[static_cast<size_t> (
        std::clamp (noteIndex, 0, static_cast<int> (kNoteBeats.size()) - 1))];
}

void Drift::prepare (const ProcessSpec& spec)
{
    static_assert (kSlotFraction.size() == kNumSlots);
    sampleRate_ = spec.sampleRate;
    const int capacity = static_cast<int> (std::ceil (kTapeSeconds * sampleRate_)) + 8;
    for (auto& t : tape_)
        t.prepare (capacity);
    glideRate_ = 1.0 / (kGlideSeconds * sampleRate_);

    for (auto& l : slotLevel_)
        l.reset (sampleRate_, 0.03);
    for (LinearSmoothed* s : { &inputGain_, &dryGain_ })
        s->reset (sampleRate_, kFadeSeconds);
    for (LinearSmoothed* s : { &loopGain_, &wetGain_, &sameSide_, &otherSide_, &bassDb_, &trebleDb_,
                               &drive_, &modAmount_ })
        s->reset (sampleRate_, kParamSeconds);

    wow_.prepare (sampleRate_);
    wow_.setFrequency (static_cast<float> (kWowHz));
    flutter_.prepare (sampleRate_);
    flutter_.setFrequency (static_cast<float> (kFlutterHz));
    // The random components are stepped once per control tick.
    const double tickRate = sampleRate_ / kControlBlock;
    wowDrift_.prepare (tickRate, 0x5EDEF001u);
    wowDrift_.setRate (0.9f);
    flutterNoise_.prepare (tickRate, 0x5EDEF002u);
    flutterNoise_.setRate (11.0f);
    wowSamples_ = static_cast<float> (kWowDepthSeconds * sampleRate_);
    flutterSamples_ = static_cast<float> (kFlutterDepthSeconds * sampleRate_);

    reset();
}

void Drift::reset() noexcept
{
    for (auto& t : tape_)
        t.reset();
    for (Biquad* f : { &emphasis_, &deEmphasis_, &hfLoss_, &playbackTone_, &feedbackTone_ })
        f->reset();
    mod_ = 0.0f;
    modStep_ = 0.0f;
    tickCountdown_ = 0;
    silentSamples_ = 0;
    snapDelay_ = true;
    idle_ = !settings_.enabled;
    applyTargets (true);
    snapNext_ = true;
}

void Drift::setSettings (const Settings& s) noexcept
{
    settings_ = sanitise (s);
    applyTargets (snapNext_);
    snapNext_ = false;
}

float Drift::effectiveTimeMs() const noexcept
{
    return effectiveTimeMs_.load (std::memory_order_relaxed);
}

float Drift::syncedTimeMs (const Transport& transport) const noexcept
{
    float ms = settings_.timeMs;
    if (settings_.sync != Free)
    {
        double bpm = settings_.tapBpm;
        if (settings_.sync == Host && transport.hasHostTempo && std::isfinite (transport.bpm) &&
            transport.bpm > 0.0)
            bpm = std::clamp (transport.bpm, 1.0, 1000.0);
        ms = static_cast<float> (60000.0 / bpm * noteBeats (settings_.note));
    }
    return std::clamp (ms, kMinTimeMs, kMaxTimeMs);
}

void Drift::applyTargets (bool snap) noexcept
{
    const auto set = [snap] (LinearSmoothed& s, float v) {
        if (snap)
            s.setCurrentAndTarget (v);
        else
            s.setTarget (v);
    };
    const Settings& s = settings_;
    const float bass = knobToShelfDb (s.bass);
    const float treble = knobToShelfDb (s.treble);

    float loop = kLoopGainAtFull * s.feedback;
    if (!s.enabled)
    {
        // First-order shelves peak at their shelf gain and every other loop filter is <= 0 dB, so
        // this bounds the small-signal loop gain below unity.
        const float toneMax = std::max (1.0f, dbToGain (bass)) * std::max (1.0f, dbToGain (treble));
        loop = std::min (loop, kIdleLoopGain / toneMax);
    }
    set (loopGain_, loop);
    set (inputGain_, s.enabled ? 1.0f : 0.0f);
    set (dryGain_, s.enabled ? mixCurve (1.0f - s.mix) : 1.0f);
    set (wetGain_, mixCurve (s.mix));

    // Constant-power blend from ping-pong (each loop on its own side) to centred.
    const float theta = (1.0f - s.width) * 0.25f * kPi;
    set (sameSide_, std::cos (theta));
    set (otherSide_, std::sin (theta));

    const auto& levels = kHeadLevels[static_cast<size_t> (s.heads)];
    for (size_t j = 0; j < slotLevel_.size(); ++j)
        set (slotLevel_[j], levels[j]);

    set (bassDb_, bass);
    set (trebleDb_, treble);
    set (drive_, s.tapeDrive);
    set (modAmount_, s.tapeMod * std::sqrt (s.tapeMod));

    if (snap)
    {
        updateTone();
        updateDrive();
    }
}

void Drift::updateTone() noexcept
{
    const double fs = sampleRate_;
    const auto tone = cascade (design::lowShelf1 (fs, kBassShelfHz, bassDb_.current()),
                               design::highShelf1 (fs, kTrebleShelfHz, trebleDb_.current()));
    playbackTone_.setCoeffs (tone);
    feedbackTone_.setCoeffs (tone);
    emphasis_.setCoeffs (design::highShelf1 (fs, kEmphasisHz, kEmphasisDb));
    deEmphasis_.setCoeffs (cascade (design::highShelf1 (fs, kEmphasisHz, -kEmphasisDb),
                                    design::highpass1 (fs, kLowLossHz)));
    hfLoss_.setCoeffs (design::lowpass (fs, std::min (kHighLossHz, 0.4 * fs), kHighLossQ));
}

void Drift::updateDrive() noexcept
{
    driveK_ = kDriveMinK * std::pow (kDriveRange, drive_.current());
    driveGain_ = kDriveNominal / fastTanh (driveK_ * kDriveNominal);
    // The loop divides by the drive's small-signal slope so drive does not move the point where
    // the echo starts to run away.
    driveInvSlope_ = 1.0f / (driveK_ * driveGain_);
}

void Drift::wake() noexcept
{
    idle_ = false;
    silentSamples_ = 0;
    snapDelay_ = true;
    // The tape is silent, so the voicing can jump straight to its targets; only the input into
    // the tape and the dry level fade.
    for (LinearSmoothed* s : { &loopGain_, &wetGain_, &sameSide_, &otherSide_, &bassDb_, &trebleDb_,
                               &drive_, &modAmount_ })
        s->setCurrentAndTarget (s->target());
    for (auto& l : slotLevel_)
        l.setCurrentAndTarget (l.target());
    updateTone();
    updateDrive();
}

void Drift::goIdle() noexcept
{
    idle_ = true;
    for (Biquad* f : { &emphasis_, &deEmphasis_, &hfLoss_, &playbackTone_, &feedbackTone_ })
        f->reset();
}

void Drift::controlTick() noexcept
{
    if (bassDb_.isSmoothing() || trebleDb_.isSmoothing())
    {
        bassDb_.skip (kControlBlock);
        trebleDb_.skip (kControlBlock);
        updateTone();
    }
    if (drive_.isSmoothing())
    {
        drive_.skip (kControlBlock);
        updateDrive();
    }
    modAmount_.skip (kControlBlock);

    wow_.advance (kControlBlock);
    flutter_.advance (kControlBlock);
    const float wow = 0.8f * Lfo::sine (wow_.phase()) + 0.35f * wowDrift_.next();
    const float flutter = 0.6f * Lfo::sine (flutter_.phase()) + 0.6f * flutterNoise_.next();
    const float next = modAmount_.current() * (wowSamples_ * wow + flutterSamples_ * flutter);
    modStep_ = (next - mod_) / static_cast<float> (kControlBlock);

    for (Biquad* f : { &emphasis_, &deEmphasis_, &hfLoss_, &playbackTone_, &feedbackTone_ })
        f->snapToZero();
}

void Drift::processSamples (const AudioView& io, int start, int length) noexcept
{
    const bool stereo = io.numChannels() > 1;
    float* left = io.channel (0) + start;
    float* right = stereo ? io.channel (1) + start : nullptr;

    // Early heads that are silent and staying silent are not read; the last head always is,
    // because it carries the feedback.
    std::array<size_t, kNumSlots> heads {};
    size_t numHeads = 0;
    for (size_t j = 0; j < kLastSlot; ++j)
        if (slotLevel_[j].current() != 0.0f || slotLevel_[j].isSmoothing())
            heads[numHeads++] = j;

    const auto record = [this] (int loop, float x) noexcept {
        float y = emphasis_.processSample (loop, x);
        y = driveGain_ * fastTanh (driveK_ * y);
        y = deEmphasis_.processSample (loop, y);
        return hfLoss_.processSample (loop, y);
    };

    for (int i = 0; i < length; ++i)
    {
        const double accel =
            glideRate_ * (glideRate_ * (targetDelay_ - delay_) - 2.0 * delayVelocity_);
        delayVelocity_ =
            std::clamp (delayVelocity_ + accel, -kMaxShrinkPerSample, kMaxGrowPerSample);
        delay_ += delayVelocity_;
        mod_ += modStep_;

        // Taps are read before this sample is written, hence the -1.
        const double offset = static_cast<double> (mod_) - 1.0;
        float wetA = 0.0f, wetB = 0.0f;
        for (size_t k = 0; k < numHeads; ++k)
        {
            const size_t j = heads[k];
            const double d = kSlotFraction[j] * delay_ + offset;
            const float level = slotLevel_[j].next();
            wetA += level * readTape (tape_[0], d);
            wetB += level * readTape (tape_[1], d);
        }
        const float lastA = readTape (tape_[0], delay_ + offset);
        const float lastB = readTape (tape_[1], delay_ + offset);
        const float lastLevel = slotLevel_[kLastSlot].next();
        wetA = playbackTone_.processSample (0, wetA + lastLevel * lastA);
        wetB = playbackTone_.processSample (1, wetB + lastLevel * lastB);

        const float loop = loopGain_.next();
        const auto feedback = [&] (int line, float last) noexcept {
            const float x = loop * feedbackTone_.processSample (line, last);
            return kLoopCeiling * fastTanh (x * (1.0f / kLoopCeiling)) * driveInvSlope_;
        };
        const float fbA = feedback (0, lastA);
        const float fbB = feedback (1, lastB);

        const float input = stereo ? 0.5f * (left[i] + right[i]) : left[i];
        const float recA = record (0, inputGain_.next() * input + fbB);
        const float recB = record (1, fbA);
        tape_[0].push (recA);
        tape_[1].push (recB);
        if (std::abs (recA) > kSilence || std::abs (recB) > kSilence)
            silentSamples_ = 0;
        else
            ++silentSamples_;

        const float dry = dryGain_.next();
        const float wet = wetGain_.next();
        const float same = sameSide_.next();
        const float other = otherSide_.next();
        if (stereo)
        {
            left[i] = dry * left[i] + wet * (same * wetA + other * wetB);
            right[i] = dry * right[i] + wet * (same * wetB + other * wetA);
        }
        else
        {
            left[i] = dry * left[i] + wet * 0.5f * (same + other) * (wetA + wetB);
        }
    }
}

void Drift::process (const AudioView& io, const Transport& transport) noexcept
{
    const float timeMs = syncedTimeMs (transport);
    effectiveTimeMs_.store (timeMs, std::memory_order_relaxed);
    targetDelay_ = static_cast<double> (timeMs) * 0.001 * sampleRate_;

    const int n = io.numFrames();
    if (n <= 0 || io.numChannels() <= 0)
        return;
    if (idle_)
    {
        if (!settings_.enabled)
            return; // tail has died: the dry signal passes untouched
        wake();
    }
    if (snapDelay_)
    {
        delay_ = targetDelay_;
        delayVelocity_ = 0.0;
        snapDelay_ = false;
    }

    for (int pos = 0; pos < n;)
    {
        if (tickCountdown_ == 0)
        {
            controlTick();
            tickCountdown_ = kControlBlock;
        }
        const int length = std::min (n - pos, tickCountdown_);
        processSamples (io, pos, length);
        pos += length;
        tickCountdown_ -= length;
    }

    // Idle only once nothing audible is left anywhere on the tape: a later, longer delay time
    // would otherwise replay stale material.
    if (!settings_.enabled && !inputGain_.isSmoothing() && !dryGain_.isSmoothing() &&
        silentSamples_ > tape_[0].maxDelay())
        goIdle();
}

} // namespace sedef::dsp
