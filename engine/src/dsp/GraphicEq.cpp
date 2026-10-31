// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/GraphicEq.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace sedef::dsp
{
namespace
{
constexpr float kMaxGainDb = 12.0f;
constexpr float kLowCutOffHz = 20.0f;
constexpr float kLowCutMaxHz = 500.0f;
constexpr float kHighCutMinHz = 1000.0f;
constexpr float kHighCutOffHz = 20000.0f;
constexpr double kGlideSeconds = 0.05;
constexpr double kFadeSeconds = 0.02;

// Bilinear designs cramp and finally fold near Nyquist; 0.45 fs keeps the 16 kHz band and the
// cuts well-behaved even at 32 kHz. responseDb() applies the same clamp, so the UI curve agrees.
double designHz (double hz, double fs) noexcept
{
    return std::min (hz, 0.45 * fs);
}

float clampOr (float x, float lo, float hi, float fallback) noexcept
{
    return std::isfinite (x) ? std::clamp (x, lo, hi) : fallback;
}

GraphicEq::Settings sanitise (const GraphicEq::Settings& s) noexcept
{
    GraphicEq::Settings out;
    for (size_t b = 0; b < s.bandsDb.size(); ++b)
        out.bandsDb[b] = clampOr (s.bandsDb[b], -kMaxGainDb, kMaxGainDb, 0.0f);
    out.lowCutHz = clampOr (s.lowCutHz, kLowCutOffHz, kLowCutMaxHz, kLowCutOffHz);
    out.highCutHz = clampOr (s.highCutHz, kHighCutMinHz, kHighCutOffHz, kHighCutOffHz);
    return out;
}

bool lowCutOn (float hz) noexcept
{
    return hz > kLowCutOffHz;
}

bool highCutOn (float hz) noexcept
{
    return hz < kHighCutOffHz;
}

BiquadCoeffs bandCoeffs (double fs, size_t band, double gainDb) noexcept
{
    return design::peaking (fs, designHz (GraphicEq::kBandHz[band], fs), GraphicEq::kBandQ, gainDb);
}

BiquadCoeffs cutCoeffs (double fs, bool isLowCut, double hz) noexcept
{
    return isLowCut ? design::highpass (fs, designHz (hz, fs))
                    : design::lowpass (fs, designHz (hz, fs));
}

double magnitudeDb (const BiquadCoeffs& c, double hz, double fs) noexcept
{
    return 20.0 * std::log10 (std::max (design::magnitude (c, hz, fs), 1.0e-12));
}
} // namespace

float GraphicEq::responseDb (const Settings& settings, double sampleRate, float hz) noexcept
{
    const Settings s = sanitise (settings);
    double db = 0.0;
    for (size_t b = 0; b < s.bandsDb.size(); ++b)
        if (s.bandsDb[b] != 0.0f)
            db += magnitudeDb (bandCoeffs (sampleRate, b, s.bandsDb[b]), hz, sampleRate);
    if (lowCutOn (s.lowCutHz))
        db += magnitudeDb (cutCoeffs (sampleRate, true, s.lowCutHz), hz, sampleRate);
    if (highCutOn (s.highCutHz))
        db += magnitudeDb (cutCoeffs (sampleRate, false, s.highCutHz), hz, sampleRate);
    return static_cast<float> (db);
}

void GraphicEq::prepare (const ProcessSpec& spec)
{
    sampleRate_ = spec.sampleRate;
    for (auto& g : bandDb_)
        g.reset (sampleRate_, kGlideSeconds);
    for (Cut* cut : { &lowCut_, &highCut_ })
    {
        cut->log2Hz.reset (sampleRate_, kGlideSeconds);
        cut->mix.reset (sampleRate_, kFadeSeconds);
    }
    reset();
}

void GraphicEq::reset() noexcept
{
    for (auto& b : bands_)
        b.reset();
    lowCut_.filter.reset();
    highCut_.filter.reset();
    applyTarget (true);
    // The first settings after a reset are a new starting point, not an automation move.
    snapNext_ = true;
}

void GraphicEq::setSettings (const Settings& s) noexcept
{
    target_ = sanitise (s);
    applyTarget (snapNext_);
    snapNext_ = false;
}

void GraphicEq::applyTarget (bool snap) noexcept
{
    for (size_t b = 0; b < bandDb_.size(); ++b)
    {
        if (snap)
        {
            bandDb_[b].setCurrentAndTarget (target_.bandsDb[b]);
            bands_[b].snap (bandCoeffs (sampleRate_, b, target_.bandsDb[b]));
        }
        else
        {
            bandDb_[b].setTarget (target_.bandsDb[b]);
        }
    }

    const auto applyCut = [&] (Cut& cut, bool isLowCut, float hz, float offHz)
    {
        cut.wantOn = isLowCut ? lowCutOn (hz) : highCutOn (hz);
        const float targetLog2 = std::log2 (cut.wantOn ? hz : offHz);
        if (snap)
        {
            cut.log2Hz.setCurrentAndTarget (targetLog2);
            cut.mix.setCurrentAndTarget (cut.wantOn ? 1.0f : 0.0f);
            cut.running = cut.wantOn;
            cut.filter.reset();
        }
        else
        {
            if (cut.wantOn && !cut.running)
            {
                // Start at the bypass extreme, where the filter is nearly transparent, fade it in
                // and glide from there.
                cut.log2Hz.setCurrentAndTarget (std::log2 (offHz));
                cut.filter.reset();
                cut.running = true;
            }
            cut.log2Hz.setTarget (targetLog2);
            if (cut.wantOn)
                cut.mix.setTarget (1.0f);
            // Turning off fades out only once the glide has reached the extreme (updateCut).
        }
        cut.filter.snap (cutCoeffs (sampleRate_, isLowCut, std::exp2 (cut.log2Hz.current())));
    };
    applyCut (lowCut_, true, target_.lowCutHz, kLowCutOffHz);
    applyCut (highCut_, false, target_.highCutHz, kHighCutOffHz);

    updateBands (0);
}

bool GraphicEq::isSmoothing() const noexcept
{
    for (const auto& g : bandDb_)
        if (g.isSmoothing())
            return true;
    for (const Cut* cut : { &lowCut_, &highCut_ })
        if (cut->running && (cut->log2Hz.isSmoothing() || cut->mix.isSmoothing() || !cut->wantOn))
            return true;
    return false;
}

void GraphicEq::updateBands (int advance) noexcept
{
    numActiveBands_ = 0;
    for (size_t b = 0; b < bandDb_.size(); ++b)
    {
        auto& gain = bandDb_[b];
        if (gain.isSmoothing())
        {
            gain.skip (advance);
            bands_[b].retarget (bandCoeffs (sampleRate_, b, gain.current()));
        }
        // A 0 dB peaking filter is the identity and its state settles to exactly zero, so a band
        // that has reached 0 dB can drop out without a discontinuity.
        if (gain.current() != 0.0f || gain.isSmoothing())
            activeBands_[static_cast<size_t> (numActiveBands_++)] = static_cast<int> (b);
        else
            bands_[b].reset();
    }
}

void GraphicEq::updateCut (Cut& cut, bool isLowCut, int advance) noexcept
{
    if (!cut.running)
        return;
    if (cut.log2Hz.isSmoothing())
    {
        cut.log2Hz.skip (advance);
        cut.filter.retarget (cutCoeffs (sampleRate_, isLowCut, std::exp2 (cut.log2Hz.current())));
    }
    if (!cut.wantOn && !cut.log2Hz.isSmoothing())
        cut.mix.setTarget (0.0f);
}

void GraphicEq::Section::run (const AudioView& io, int start, int length,
                              const float* fade) noexcept
{
    const int channels = io.numChannels();
    if (!ramping && fade == nullptr)
    {
        const float b0 = coeffs.b0, b1 = coeffs.b1, b2 = coeffs.b2, a1 = coeffs.a1, a2 = coeffs.a2;
        int ch = 0;
        if (channels == 2)
        {
            float* l = io.channel (0) + start;
            float* r = io.channel (1) + start;
            float l1 = state[0][0], l2 = state[0][1], r1 = state[1][0], r2 = state[1][1];
            for (int i = 0; i < length; ++i)
            {
                const float xl = l[i], xr = r[i];
                const float yl = b0 * xl + l1;
                const float yr = b0 * xr + r1;
                l1 = b1 * xl - a1 * yl + l2;
                r1 = b1 * xr - a1 * yr + r2;
                l2 = b2 * xl - a2 * yl;
                r2 = b2 * xr - a2 * yr;
                l[i] = yl;
                r[i] = yr;
            }
            state[0] = { l1, l2 };
            state[1] = { r1, r2 };
            ch = 2;
        }
        for (; ch < channels; ++ch)
        {
            float* d = io.channel (ch) + start;
            auto& z = state[static_cast<size_t> (ch)];
            float z1 = z[0], z2 = z[1];
            for (int i = 0; i < length; ++i)
            {
                const float x = d[i];
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                d[i] = y;
            }
            z = { z1, z2 };
        }
        return;
    }

    const float step = 1.0f / static_cast<float> (length);
    for (int ch = 0; ch < channels; ++ch)
    {
        float* d = io.channel (ch) + start;
        auto& z = state[static_cast<size_t> (ch)];
        float z1 = z[0], z2 = z[1];
        BiquadCoeffs k = coeffs;
        for (int i = 0; i < length; ++i)
        {
            if (ramping)
            {
                const float t = static_cast<float> (i + 1) * step;
                k = { lerp (from.b0, to.b0, t), lerp (from.b1, to.b1, t), lerp (from.b2, to.b2, t),
                      lerp (from.a1, to.a1, t), lerp (from.a2, to.a2, t) };
            }
            const float x = d[i];
            const float y = k.b0 * x + z1;
            z1 = k.b1 * x - k.a1 * y + z2;
            z2 = k.b2 * x - k.a2 * y;
            d[i] = fade != nullptr ? x + fade[i] * (y - x) : y;
        }
        z = { z1, z2 };
    }
    // Land exactly on the target (lerp at t = 1 may be an ulp off; a band reaching 0 dB must be
    // the exact identity to drop out cleanly).
    if (ramping)
        snap (to);
}

void GraphicEq::processBlock (const AudioView& io, int start, int length) noexcept
{
    const auto prepareFade = [length] (Cut& cut,
                                       std::array<float, kControlBlock>& gains) -> const float*
    {
        if (!cut.running || (!cut.mix.isSmoothing() && cut.mix.current() >= 1.0f))
            return nullptr;
        assert (length <= kControlBlock);
        for (int i = 0; i < length; ++i)
            gains[static_cast<size_t> (i)] = cut.mix.next();
        return gains.data();
    };
    const float* lowFade = prepareFade (lowCut_, lowMix_);
    const float* highFade = prepareFade (highCut_, highMix_);

    if (lowCut_.running)
        lowCut_.filter.run (io, start, length, lowFade);
    for (int k = 0; k < numActiveBands_; ++k)
        bands_[static_cast<size_t> (activeBands_[static_cast<size_t> (k)])].run (io, start, length,
                                                                                 nullptr);
    if (highCut_.running)
        highCut_.filter.run (io, start, length, highFade);

    for (Cut* cut : { &lowCut_, &highCut_ })
    {
        if (cut->running && !cut->wantOn && !cut->mix.isSmoothing() && cut->mix.current() <= 0.0f)
        {
            cut->running = false;
            cut->filter.reset();
        }
    }
}

void GraphicEq::process (const AudioView& io) noexcept
{
    const int n = io.numFrames();
    if (n <= 0)
        return;
    if (!isSmoothing())
    {
        // Steady state: coefficients are current and no cut is fading, so run the block in one go.
        processBlock (io, 0, n);
        return;
    }
    for (int start = 0; start < n; start += kControlBlock)
    {
        const int length = std::min (kControlBlock, n - start);
        updateBands (length);
        updateCut (lowCut_, true, length);
        updateCut (highCut_, false, length);
        processBlock (io, start, length);
    }
}

} // namespace sedef::dsp
