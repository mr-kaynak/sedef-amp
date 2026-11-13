// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Metronome.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
constexpr std::array<int, 8> kStepsPerBar { 2, 3, 4, 5, 6, 7, 9, 12 };
constexpr int kFirstEighthSignature = 4; // 6/8 onwards count eighth notes

constexpr float kMinBpm = 20.0f;
constexpr float kMaxBpm = 400.0f;
constexpr float kNormalGain = 0.55f; // about -5 dB below an accent
constexpr float kSoftGain = 0.22f;   // about -13 dB
constexpr float kEndLevel = 1.0e-4f; // -80 dB: a voice below this stops
constexpr double kFadeOutSeconds = 0.005;

struct Partial
{
    float hz = 0.0f;
    float level = 0.0f;
    float decayMs = 1.0f;
    float tailMs = 0.0f;  ///< optional slower second decay ...
    float tailMix = 0.0f; ///< ... and its share of the envelope
    float bend = 1.0f;    ///< start-frequency ratio, relaxing to hz
    float bendMs = 1.0f;
    bool square = false;
};

struct Recipe
{
    std::array<Partial, 3> partials {};
    int numPartials = 0;
    float bodyHz = 0.0f, bodyQ = 1.0f; ///< band-pass on the tonal part (0 = none)
    float noiseLevel = 0.0f, noiseHz = 1000.0f, noiseQ = 1.0f;
    float noiseAttackMs = 0.0f, noiseDecayMs = 1.0f;
    float attackMs = 0.2f;
    float accentPitch = 1.0f; ///< accents are this much higher
    float lengthMs = 200.0f;
    float level = 1.0f; ///< trims every sound to a similar accent peak
};

// Each sound is a small modal/noise recipe; nothing is sampled. Frequencies and decays are chosen
// to read clearly over a guitar; levels are trimmed to a similar short-term (50 ms) loudness,
// the 2.5 kHz clave a little lower because the ear is most sensitive there.
const std::array<Recipe, 6> kRecipes {
    // Blip: a short sine with a downward pitch flick; accents a fifth up.
    Recipe { .partials = { { { .hz = 1046.5f,
                               .level = 1.0f,
                               .decayMs = 22.0f,
                               .bend = 1.6f,
                               .bendMs = 3.0f },
                             { .hz = 2093.0f,
                               .level = 0.12f,
                               .decayMs = 8.0f,
                               .bend = 1.6f,
                               .bendMs = 3.0f } } },
             .numPartials = 2,
             .attackMs = 0.3f,
             .accentPitch = 1.5f,
             .lengthMs = 160.0f,
             .level = 0.46f },
    // Wood: three inharmonic block modes plus a band-passed noise knock.
    Recipe { .partials = { { { .hz = 1180.0f,
                               .level = 1.0f,
                               .decayMs = 26.0f,
                               .bend = 1.04f,
                               .bendMs = 2.0f },
                             { .hz = 2710.0f, .level = 0.4f, .decayMs = 12.0f },
                             { .hz = 4130.0f, .level = 0.18f, .decayMs = 6.0f } } },
             .numPartials = 3,
             .noiseLevel = 0.6f,
             .noiseHz = 2400.0f,
             .noiseQ = 1.5f,
             .noiseDecayMs = 2.5f,
             .attackMs = 0.15f,
             .accentPitch = 1.12f,
             .lengthMs = 200.0f,
             .level = 0.39f },
    // Clave: one high-Q mode near 2.5 kHz with a faint upper partial and a tiny click.
    Recipe { .partials = { { { .hz = 2500.0f, .level = 1.0f, .decayMs = 38.0f },
                             { .hz = 6900.0f, .level = 0.05f, .decayMs = 9.0f } } },
             .numPartials = 2,
             .noiseLevel = 0.25f,
             .noiseHz = 4000.0f,
             .noiseQ = 0.8f,
             .noiseDecayMs = 1.0f,
             .attackMs = 0.1f,
             .accentPitch = 1.12f,
             .lengthMs = 260.0f,
             .level = 0.33f },
    // Cowbell: two detuned soft squares through a band-pass, quick drop then a ringing tail.
    Recipe { .partials = { { { .hz = 540.0f,
                               .level = 0.8f,
                               .decayMs = 10.0f,
                               .tailMs = 110.0f,
                               .tailMix = 0.35f,
                               .square = true },
                             { .hz = 800.0f,
                               .level = 0.8f,
                               .decayMs = 10.0f,
                               .tailMs = 110.0f,
                               .tailMix = 0.35f,
                               .square = true } } },
             .numPartials = 2,
             .bodyHz = 1100.0f,
             .bodyQ = 1.2f,
             .attackMs = 0.2f,
             .accentPitch = 1.06f,
             .lengthMs = 450.0f,
             .level = 0.45f },
    // Rim: a noise crack with a ~1.7 kHz ring and a little shell body.
    Recipe { .partials = { { { .hz = 1700.0f, .level = 0.7f, .decayMs = 9.0f },
                             { .hz = 480.0f, .level = 0.35f, .decayMs = 14.0f } } },
             .numPartials = 2,
             .noiseLevel = 0.8f,
             .noiseHz = 3200.0f,
             .noiseQ = 0.9f,
             .noiseDecayMs = 3.0f,
             .attackMs = 0.1f,
             .accentPitch = 1.08f,
             .lengthMs = 150.0f,
             .level = 0.47f },
    // Shaker: band-passed noise with a soft swish attack.
    Recipe { .noiseLevel = 1.0f,
             .noiseHz = 6500.0f,
             .noiseQ = 1.1f,
             .noiseAttackMs = 7.0f,
             .noiseDecayMs = 38.0f,
             .attackMs = 0.1f,
             .accentPitch = 1.15f,
             .lengthMs = 260.0f,
             .level = 0.65f },
};

Metronome::Settings sanitise (const Metronome::Settings& s) noexcept
{
    Metronome::Settings o = s;
    o.volumeDb = std::isfinite (s.volumeDb) ? std::clamp (s.volumeDb, -60.0f, 24.0f) : 0.0f;
    o.signature = std::clamp (s.signature, 0, static_cast<int> (kStepsPerBar.size()) - 1);
    o.sound = std::clamp (s.sound, 0, static_cast<int> (kRecipes.size()) - 1);
    o.bpm = std::isfinite (s.bpm) ? std::clamp (s.bpm, kMinBpm, kMaxBpm) : 120.0f;
    return o;
}

float decayCoeff (double ms, double fs) noexcept
{
    return static_cast<float> (std::exp (-1.0 / std::max (1.0e-3 * ms * fs, 1.0)));
}
} // namespace

int Metronome::stepsFor (int signature) noexcept
{
    return kStepsPerBar[static_cast<size_t> (
        std::clamp (signature, 0, static_cast<int> (kStepsPerBar.size()) - 1))];
}

void Metronome::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    volume_.reset (sampleRate_, 0.02);
    reset();
}

void Metronome::reset() noexcept
{
    for (auto& v : voices_)
        v.active = false;
    volume_.setCurrentAndTarget (dbToGain (settings_.volumeDb));
    running_ = false;
    startPending_ = settings_.enabled;
    now_ = 0;
    snapNext_ = true;
    currentStep_.store (-1, std::memory_order_relaxed);
}

void Metronome::setSettings (const Settings& s) noexcept
{
    settings_ = sanitise (s);
    if (snapNext_)
        volume_.setCurrentAndTarget (dbToGain (settings_.volumeDb));
    else
        volume_.setTarget (dbToGain (settings_.volumeDb));
    snapNext_ = false;

    if (!settings_.enabled)
    {
        running_ = false;
        startPending_ = false;
        currentStep_.store (-1, std::memory_order_relaxed);
        return;
    }
    if (!running_)
    {
        startPending_ = true;
        return;
    }
    const double interval = samplesPerStep();
    if (interval != interval_)
    {
        // Tempo or meter changed: measure the next step from the last one at the new spacing.
        anchor_ = lastTrigger_;
        stepsSinceAnchor_ = 1;
        interval_ = interval;
    }
}

int Metronome::currentStep() const noexcept
{
    return currentStep_.load (std::memory_order_relaxed);
}

double Metronome::samplesPerStep() const noexcept
{
    const double beatSeconds = settings_.signature >= kFirstEighthSignature ? 30.0 : 60.0;
    return sampleRate_ * beatSeconds / static_cast<double> (settings_.bpm);
}

long long Metronome::nextTrigger() const noexcept
{
    return static_cast<long long> (std::ceil (static_cast<double> (anchor_) +
                                              static_cast<double> (stepsSinceAnchor_) * interval_));
}

void Metronome::trigger (long long when, long long scheduled) noexcept
{
    const int steps = stepsFor (settings_.signature);
    if (nextStep_ >= steps)
        nextStep_ = 0; // the meter got shorter: start a new bar
    const int step = nextStep_;
    const auto state = settings_.steps[static_cast<size_t> (step)];
    if (state == Normal || state == Accent || state == Soft)
        startVoice (static_cast<StepState> (state));
    // The playhead moves through silent (Off) steps too, so the UI never appears to stall.
    currentStep_.store (step, std::memory_order_relaxed);
    lastTrigger_ = when;
    if (when != scheduled)
    {
        // Late (a tempo change made this step overdue): continue the grid from where it sounded.
        anchor_ = when;
        stepsSinceAnchor_ = 1;
    }
    else
    {
        ++stepsSinceAnchor_;
    }
    nextStep_ = step + 1 < steps ? step + 1 : 0;
}

void Metronome::startVoice (StepState state) noexcept
{
    Voice* voice = &voices_[0];
    for (auto& v : voices_)
    {
        if (!v.active)
        {
            voice = &v;
            break;
        }
        if (v.age > voice->age)
            voice = &v; // fallback after a sudden tempo jump: reuse the oldest
    }

    const Recipe& r = kRecipes[static_cast<size_t> (settings_.sound)];
    const double fs = sampleRate_;
    const auto samples = [fs] (float ms) {
        return static_cast<int> (std::lround (1.0e-3 * ms * fs));
    };
    const float pitch = state == Accent ? r.accentPitch : 1.0f;
    const float stateGain = state == Accent ? 1.0f : state == Normal ? kNormalGain : kSoftGain;

    Voice& v = *voice;
    v.active = true;
    v.age = 0;
    v.gain = r.level * stateGain;
    v.attackStep = 1.0f / static_cast<float> (std::max (1, samples (r.attackMs)));
    v.attack = 0.0f;
    v.numPartials = r.numPartials;
    for (size_t p = 0; p < static_cast<size_t> (r.numPartials); ++p)
    {
        const Partial& src = r.partials[p];
        // Cosine phase: the first sample of every click is already non-zero, so its onset is
        // exactly the scheduled sample.
        v.phase[p] = 0.0f;
        v.increment[p] = static_cast<float> (std::min (src.hz * pitch / fs, 0.45));
        v.bend[p] = src.bend - 1.0f;
        v.bendCoeff[p] = decayCoeff (src.bendMs, fs);
        v.level[p] = src.level;
        v.fast[p] = 1.0f;
        v.fastCoeff[p] = decayCoeff (src.decayMs, fs);
        v.slow[p] = src.tailMs > 0.0f ? 1.0f : 0.0f;
        v.slowCoeff[p] = decayCoeff (src.tailMs, fs);
        v.slowMix[p] = src.tailMs > 0.0f ? src.tailMix : 0.0f;
        v.square[p] = src.square;
    }
    v.hasBody = r.bodyHz > 0.0f;
    if (v.hasBody)
    {
        v.body.setCoeffs (design::bandpass (
            fs, std::min (r.bodyHz * pitch, 0.45f * static_cast<float> (fs)), r.bodyQ));
        v.body.reset();
    }
    v.noiseGain = r.noiseLevel;
    if (v.noiseGain > 0.0f)
    {
        v.noise.setCoeffs (design::bandpass (
            fs, std::min (r.noiseHz * pitch, 0.45f * static_cast<float> (fs)), r.noiseQ));
        v.noise.reset();
        v.noiseAttack = samples (r.noiseAttackMs);
        v.noiseEnv = 1.0f;
        v.noiseCoeff = decayCoeff (r.noiseDecayMs, fs);
    }
    v.settleAge = std::max (samples (r.attackMs), v.noiseGain > 0.0f ? v.noiseAttack : 0) + 1;
    // At fast tempos a long tail fades out after three steps, so with four voices a new click
    // never has to cut one off.
    const double fadeSamples = std::max (1.0, kFadeOutSeconds * fs);
    const double maxLength = (kMaxVoices - 1) * interval_ - fadeSamples;
    v.length = static_cast<int> (
        std::max (0.0, std::min (static_cast<double> (samples (r.lengthMs)), maxLength)));
    v.fade = 1.0f;
    v.fadeStep = static_cast<float> (1.0 / fadeSamples);
}

float Metronome::Voice::next (Random& rng) noexcept
{
    float tone = 0.0f;
    float envelope = 0.0f;
    for (size_t p = 0; p < static_cast<size_t> (numPartials); ++p)
    {
        const float c = std::cos (kTwoPi * phase[p]);
        const float osc = square[p] ? fastTanh (2.5f * c) : c;
        const float env = level[p] * (fast[p] + slowMix[p] * (slow[p] - fast[p]));
        tone += env * osc;
        envelope += env;
        phase[p] += increment[p] * (1.0f + bend[p]);
        if (phase[p] >= 1.0f)
            phase[p] -= 1.0f;
        bend[p] *= bendCoeff[p];
        fast[p] *= fastCoeff[p];
        slow[p] *= slowCoeff[p];
    }
    if (hasBody)
        tone = body.processSample (0, tone);

    if (noiseGain > 0.0f)
    {
        float env = 0.0f;
        if (age < noiseAttack)
        {
            env = static_cast<float> (age + 1) / static_cast<float> (noiseAttack);
        }
        else
        {
            env = noiseEnv;
            noiseEnv *= noiseCoeff;
        }
        tone += noiseGain * noise.processSample (0, env * rng.bipolar());
        envelope += noiseGain * env;
    }

    attack = std::min (1.0f, attack + attackStep);
    float out = gain * attack * tone;

    if (age >= length)
    {
        fade -= fadeStep;
        out *= std::max (fade, 0.0f);
        if (fade <= 0.0f)
            active = false;
    }
    if (age > settleAge && envelope < kEndLevel)
        active = false;
    ++age;
    return out;
}

void Metronome::render (const AudioView& io, int start, int end) noexcept
{
    bool anyActive = false;
    for (const auto& v : voices_)
        anyActive = anyActive || v.active;
    if (!anyActive)
    {
        volume_.skip (end - start);
        return;
    }
    float* left = io.channel (0);
    float* right = io.numChannels() > 1 ? io.channel (1) : nullptr;
    for (int i = start; i < end; ++i)
    {
        float sum = 0.0f;
        for (auto& v : voices_)
            if (v.active)
                sum += v.next (rng_);
        const float out = sum * volume_.next();
        left[i] += out;
        if (right != nullptr)
            right[i] += out;
    }
}

void Metronome::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    if (n <= 0 || io.numChannels() <= 0)
        return;
    if (startPending_)
    {
        // Start on the downbeat, at the first sample of this block.
        startPending_ = false;
        running_ = true;
        interval_ = samplesPerStep();
        anchor_ = now_;
        startedAt_ = now_;
        stepsSinceAnchor_ = 0;
        nextStep_ = 0;
    }

    for (int pos = 0; pos < n;)
    {
        int end = n;
        long long due = 0;
        bool fires = false;
        if (running_)
        {
            due = nextTrigger();
            if (due < now_ + n)
            {
                fires = true;
                end = static_cast<int> (std::max (static_cast<long long> (pos), due - now_));
            }
        }
        render (io, pos, end);
        if (fires)
            trigger (now_ + end, due);
        pos = end;
    }
    now_ += n;
}

double Metronome::barSamples() const noexcept
{
    return samplesPerStep() * static_cast<double> (stepsFor (settings_.signature));
}

int Metronome::downbeats (int frames, int* offsets, int maxOffsets) const noexcept
{
    if (frames <= 0 || !isRunning())
        return 0;
    // process()'s scheduler, on copies: the start, nextTrigger() and trigger() step for step.
    long long anchor = anchor_;
    long long steps = stepsSinceAnchor_;
    double interval = interval_;
    int next = nextStep_;
    if (startPending_)
    {
        interval = samplesPerStep();
        anchor = now_;
        steps = 0;
        next = 0;
    }
    const int perBar = stepsFor (settings_.signature);
    int count = 0;
    for (int pos = 0; pos < frames;)
    {
        const auto due = static_cast<long long> (
            std::ceil (static_cast<double> (anchor) + static_cast<double> (steps) * interval));
        if (due >= now_ + frames)
            break;
        const int end = static_cast<int> (std::max (static_cast<long long> (pos), due - now_));
        if (next >= perBar)
            next = 0;
        if (next == 0 && count < maxOffsets)
            offsets[count++] = end;
        const long long when = now_ + end;
        if (when != due)
        {
            anchor = when;
            steps = 1;
        }
        else
        {
            ++steps;
        }
        next = next + 1 < perBar ? next + 1 : 0;
        pos = end;
    }
    return count;
}

void Metronome::alignDownbeat (int offset) noexcept
{
    if (!settings_.enabled)
        return;
    const long long at = now_ + std::max (0, offset);
    bool justSounded = false;
    if (startPending_)
    {
        // Started in this block: the click begins on the loop's downbeat.
        startPending_ = false;
        running_ = true;
        interval_ = samplesPerStep();
        startedAt_ = at;
    }
    else if (running_)
    {
        justSounded = lastTrigger_ >= startedAt_ && lastTrigger_ < at &&
                      static_cast<double> (at - lastTrigger_) < 0.5 * interval_;
    }
    else
    {
        return;
    }
    anchor_ = at;
    if (justSounded)
    {
        stepsSinceAnchor_ = 1;
        nextStep_ = 1;
    }
    else
    {
        stepsSinceAnchor_ = 0;
        nextStep_ = 0;
    }
}

} // namespace sedef::dsp
