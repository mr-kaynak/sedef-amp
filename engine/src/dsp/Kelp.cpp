// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/Kelp.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::dsp
{
namespace
{
// The clipping stage, with the component values of the published circuit analyses (Yeh, Abel &
// Smith, DAFx-07): non-inverting op-amp, R4 + C3 from the inverting input to ground, the drive
// pot in series with R6 across the feedback path, Cf and two silicon diodes in parallel with it.
constexpr double kR4 = 4.7e3;
constexpr double kC3 = 47.0e-9;     // with R4: the 720 Hz high-pass of the feedback network
constexpr double kR6 = 51.0e3;      // feedback resistance at drive 0
constexpr double kRDrive = 500.0e3; // audio-taper pot
constexpr double kCf = 51.0e-12;
constexpr double kDriveTaper = 4.0; // ~12 % of the track at half rotation, like an A-taper pot

// 1N4148 / 1N914 SPICE model: saturation current and emission coefficient; thermal voltage at
// 27 degrees C. The calibration is 1.0 full scale = 1 V at the jack (docs/dsp/amps.md, sec. 4).
constexpr double kIs = 2.52e-9;
constexpr double kNVt = 1.752 * 0.025852;

// Tone stage: R + C passive low-pass (1 k, 220 nF), then an active treble control, modelled as a
// first-order shelf (1 + s/wz) / (1 + s/wp) with its pole at 5 kHz and its zero moving from 5 kHz
// (all the way dark: only the low-pass is left) down to 560 Hz (bright: the low-pass is undone
// and a little treble lift remains). That shelf is exactly 1 + (G - 1) HP(wp), G = wp / wz, so it
// runs as a fixed high-pass blended in with a smoothed gain: turning the knob never touches a
// filter coefficient.
constexpr double kToneLpHz = 723.4;
constexpr double kShelfPoleHz = 5000.0;
constexpr float kShelfZeroBrightHz = 560.0f;

constexpr double kInputHpHz = 20.0;
constexpr double kOutputHpHz = 10.0;
constexpr float kLevelRangeDb = 40.0f; // level: -20..+20 dB

constexpr double kGainRampSeconds = 0.03;
constexpr double kFilterSmoothingSeconds = 0.02;
constexpr int kHalleySteps = 2;

/// Output trim (dB): measured makeup for the reference tone (see docs/dsp/kelp.md), so that level
/// 0.5 is close to unity loudness across the whole drive range.
float trimDb (float drive) noexcept
{
    return -4.11f - 1.26f * drive;
}

double curveA (double rf) noexcept
{
    return 2.0 * kIs * rf / kNVt;
}

/// sinh(v) and cosh(v) - 1 for v >= 0, accurate near zero.
inline void sinhCoshm1 (double v, double& sh, double& chm1) noexcept
{
    if (v > 0.5)
    {
        const double e = std::exp (v);
        const double ei = 1.0 / e;
        sh = 0.5 * (e - ei);
        chm1 = 0.5 * (e + ei) - 1.0;
        return;
    }
    const double em1 = std::expm1 (v);
    const double e = em1 + 1.0;
    sh = em1 * (em1 + 2.0) / (2.0 * e);
    chm1 = em1 * em1 / (2.0 * e);
}

struct DiodeSolution
{
    double v = 0.0;    ///< root, >= 0
    double sh = 0.0;   ///< sinh v
    double chm1 = 0.0; ///< cosh v - 1
};

/// Root of g(v) = v + a sinh v - t for t >= 0. g is increasing and convex on v >= 0, and both t
/// and asinh(t / a) bound the root from above. Two Halley steps from the smaller bound, in float,
/// land within ~4e-6 (relative residual) over t = 1e-6 .. 1e6 and the whole drive range; one
/// Newton step in double then squares that error, so the antiderivative the anti-aliasing
/// differentiates is accurate to ~1e-11. sinh and cosh at the final root come from
/// the last evaluation by Taylor expansion.
DiodeSolution solveDiodes (double t, double a) noexcept
{
    t = std::min (t, 1.0e12);
    const auto tf = static_cast<float> (t);
    const auto af = static_cast<float> (a);
    float vf = std::min (tf, std::asinh (tf / af));
    for (int k = 0; k < kHalleySteps; ++k)
    {
        const float e = std::exp (vf);
        const float sh = 0.5f * (e - 1.0f / e);
        const float gp = 1.0f + af * 0.5f * (e + 1.0f / e);
        const float g = vf + af * sh - tf;
        vf -= 2.0f * g * gp / (2.0f * gp * gp - g * af * sh);
    }

    DiodeSolution s;
    const double v = std::max (0.0, static_cast<double> (vf));
    double sh = 0.0, chm1 = 0.0;
    sinhCoshm1 (v, sh, chm1);
    const double ch = chm1 + 1.0;
    const double d = -(v + a * sh - t) / (1.0 + a * ch);
    s.v = v + d;
    s.sh = sh + d * (ch + 0.5 * d * sh);
    s.chm1 = chm1 + d * (sh + 0.5 * d * ch);
    return s;
}

/// F(w) = integral of v dw: with dw = (1 + a cosh v) dv, v^2 / 2 + a (v sinh v - cosh v + 1).
double antiderivativeAt (const DiodeSolution& s, double a) noexcept
{
    return 0.5 * s.v * s.v + a * (s.v * s.sh - s.chm1);
}
} // namespace

// -------------------------------------------------------------------------------------------------

double Kelp::FeedbackDiodes::solve (double w, double a) noexcept
{
    const double v = solveDiodes (std::abs (w), a).v;
    return w < 0.0 ? -v : v;
}

double Kelp::FeedbackDiodes::antiderivative (double w, double a) noexcept
{
    return antiderivativeAt (solveDiodes (std::abs (w), a), a);
}

void Kelp::FeedbackDiodes::setA (double a) noexcept
{
    a_ = a;
    f1_ = antiderivative (x1_, a_);
}

float Kelp::FeedbackDiodes::process (float in) noexcept
{
    // First-order antiderivative anti-aliasing (Parker et al., DAFx-16; Bilbao et al., IEEE SPL
    // 2017), as detail::Adaa1, with the curve's own parameter.
    const double x = in;
    const double fx = antiderivative (x, a_);
    const double dx = x - x1_;
    const double y = std::abs (dx) > 1.0e-6 * (1.0 + std::abs (x)) ? (fx - f1_) / dx
                                                                   : solve (0.5 * (x + x1_), a_);
    x1_ = x;
    f1_ = fx;
    return static_cast<float> (y);
}

double Kelp::feedbackResistance (float drive) noexcept
{
    return kR6 + kRDrive * static_cast<double> (audioTaper (drive, kDriveTaper));
}

// -------------------------------------------------------------------------------------------------

void Kelp::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    numChannels_ = std::clamp (spec.numChannels, 1, kMaxChannels);
    maxBlockSize_ = std::max (spec.maxBlockSize, 1);

    oversampler_.prepare (maxBlockSize_, numChannels_, Oversampler::factorLog2For (sampleRate_));
    latency_ = oversampler_.latencySamples();

    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        const auto c = static_cast<size_t> (ch);
        cleanDelay_[c].prepare (std::max (latency_, 1));
        drive_[c].assign (ch < numChannels_ ? static_cast<size_t> (maxBlockSize_) : 0, 0.0f);
        drivePtrs_[c] = drive_[c].data();
    }
    curveA_.assign (static_cast<size_t> (maxBlockSize_ / kControlBlock + 2), 0.0f);

    driveGain_.reset (sampleRate_, kGainRampSeconds);
    outputGain_.reset (sampleRate_, kGainRampSeconds);
    treble_.reset (sampleRate_, kGainRampSeconds);
    driveKnob_.reset (sampleRate_, kFilterSmoothingSeconds);

    inputHp_.setCoeffs (design::highpass1 (sampleRate_, kInputHpHz));
    feedbackHp_.setCoeffs (design::highpass1 (sampleRate_, 1.0 / (kTwoPiD * kR4 * kC3)));
    toneLp_.setCoeffs (design::lowpass1 (sampleRate_, kToneLpHz));
    toneHp_.setCoeffs (design::highpass1 (sampleRate_, kShelfPoleHz));
    dcBlock_.setCoeffs (design::highpass1 (sampleRate_, kOutputHpHz));

    applySettings (true);
    reset();
}

void Kelp::reset() noexcept
{
    oversampler_.reset();
    for (auto& d : cleanDelay_)
        d.reset();
    for (auto& d : diodes_)
    {
        d.reset();
        d.setA (curveA (feedbackResistance (driveKnob_.current())));
    }
    for (auto* f : { &inputHp_, &feedbackHp_, &feedbackLp_, &toneLp_, &toneHp_, &dcBlock_ })
        f->reset();
    primed_ = false;
}

void Kelp::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    applySettings (!primed_);
    primed_ = true;
}

int Kelp::latencySamples() const noexcept
{
    return latency_;
}

void Kelp::applySettings (bool snap) noexcept
{
    const float drive = std::clamp (settings_.drive, 0.0f, 1.0f);
    const float tone = std::clamp (settings_.tone, 0.0f, 1.0f);
    const float level = std::clamp (settings_.level, 0.0f, 1.0f);

    // Volts at the inverting input -> what Rf alone would develop across the feedback path, in
    // units of the diodes' n Vt.
    const auto gain = static_cast<float> (feedbackResistance (drive) / kR4 / kNVt);
    const float output = dbToGain (trimDb (drive) + kLevelRangeDb * (level - 0.5f));
    const float treble = static_cast<float> (kShelfPoleHz) /
                             mapExp (tone, static_cast<float> (kShelfPoleHz), kShelfZeroBrightHz) -
                         1.0f;

    if (snap)
    {
        driveGain_.setCurrentAndTarget (gain);
        outputGain_.setCurrentAndTarget (output);
        treble_.setCurrentAndTarget (treble);
        driveKnob_.setCurrentAndTarget (drive);
        updateDrive (drive);
        for (auto& d : diodes_)
            d.setA (curveA (feedbackResistance (drive)));
    }
    else
    {
        driveGain_.setTarget (gain);
        outputGain_.setTarget (output);
        treble_.setTarget (treble);
        driveKnob_.setTarget (drive);
    }
}

void Kelp::updateDrive (float drive) noexcept
{
    // Cf across Rf: the clipping path's bandwidth falls from ~61 kHz to ~5.7 kHz with the drive.
    feedbackLp_.setCoeffs (
        design::lowpass1 (sampleRate_, 1.0 / (kTwoPiD * feedbackResistance (drive) * kCf)));
}

void Kelp::process (const AudioView& io) noexcept
{
    // The scratch buffers hold maxBlockSize frames; split anything larger.
    if (maxBlockSize_ <= 0)
        return; // not prepared
    for (int start = 0; start < io.numFrames(); start += maxBlockSize_)
        processBlock (io.subView (start, std::min (maxBlockSize_, io.numFrames() - start)));
}

void Kelp::processBlock (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    const int channels = std::min (io.numChannels(), numChannels_);
    if (n == 0 || channels == 0)
        return;

    // 1. Base rate: input coupling, then the clean term (delayed to line up with the clipper, left
    //    in io) and the feedback path's input, in n Vt units.
    for (int start = 0, block = 0; start < n; start += kControlBlock, ++block)
    {
        const int end = std::min (n, start + kControlBlock);
        if (driveKnob_.isSmoothing())
            updateDrive (driveKnob_.skip (end - start));
        curveA_[static_cast<size_t> (block)] =
            static_cast<float> (curveA (feedbackResistance (driveKnob_.current())));

        for (int i = start; i < end; ++i)
        {
            const float g = driveGain_.next();
            for (int ch = 0; ch < channels; ++ch)
            {
                const auto c = static_cast<size_t> (ch);
                const float x = inputHp_.processSample (ch, io.channel (ch)[i]);
                cleanDelay_[c].push (x);
                io.channel (ch)[i] = cleanDelay_[c].read (latency_);
                drive_[c][static_cast<size_t> (i)] =
                    feedbackLp_.processSample (ch, g * feedbackHp_.processSample (ch, x));
            }
        }
    }

    // 2. Oversampled and anti-aliased: the voltage the diodes leave across the feedback path.
    const AudioView drive (drivePtrs_.data(), channels, n);
    const AudioView up = oversampler_.upsample (drive);
    const int log2Factor = oversampler_.factor() == 4 ? 2 : oversampler_.factor() == 2 ? 1 : 0;
    for (int ch = 0; ch < channels; ++ch)
    {
        auto& diodes = diodes_[static_cast<size_t> (ch)];
        float* d = up.channel (ch);
        for (int i = 0; i < up.numFrames(); ++i)
        {
            const double a = curveA_[static_cast<size_t> ((i >> log2Factor) / kControlBlock)];
            if (a != diodes.a())
                diodes.setA (a);
            d[i] = static_cast<float> (kNVt) * diodes.process (d[i]);
        }
    }
    oversampler_.downsample (drive);

    // 3. Base rate: the op-amp's output is its input plus the feedback voltage; then the tone stage
    //    and the volume.
    for (int i = 0; i < n; ++i)
    {
        const float out = outputGain_.next();
        const float treble = treble_.next();
        for (int ch = 0; ch < channels; ++ch)
        {
            float& x = io.channel (ch)[i];
            float y = x + drive_[static_cast<size_t> (ch)][static_cast<size_t> (i)];
            y = toneLp_.processSample (ch, y);
            y += treble * toneHp_.processSample (ch, y);
            y = dcBlock_.processSample (ch, y);
            x = out * y;
        }
    }
}

} // namespace sedef::dsp
