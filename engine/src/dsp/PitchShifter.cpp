// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/PitchShifter.h"

#include "sedef/core/Biquad.h"
#include "sedef/core/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sedef::dsp
{
namespace
{
// Times in ms. docs/dsp/transpose.md explains how they trade splice rate against latency.
constexpr double kCorrelationMs = 12.0;    // compared segment, about one period of a low E
constexpr double kLookAheadMs = 3.0;       // part of that segment after the splice point
constexpr double kSearchMs = 16.0;         // span of candidate splice offsets (> low D period)
constexpr double kJumpMinMs = 8.0;         // shortest jump for small shifts...
constexpr double kJumpPerOctaveMs = 10.0;  // ...longer for big shifts to keep the splice rate down
constexpr double kSpliceFadeMs = 40.0;     // longest splice crossfade
constexpr double kTransitionFadeMs = 30.0; // crossfade when the semitone setting changes
constexpr double kHistoryMs = 160.0;
constexpr double kSearchRate = 12000.0; // decimated rate for the coarse search
constexpr int kMargin = 4;              // interpolation and rounding headroom, samples

/// Catmull-Rom (cubic Hermite) through p[0..3], evaluated at t in [0, 1] between p[1] and p[2].
inline float hermite (const float* p, float t) noexcept
{
    const float c1 = 0.5f * (p[2] - p[0]);
    const float c2 = p[0] - 2.5f * p[1] + 2.0f * p[2] - 0.5f * p[3];
    const float c3 = 0.5f * (p[3] - p[0]) + 1.5f * (p[1] - p[2]);
    return ((c3 * t + c2) * t + c1) * t + p[1];
}

/// Circular buffer written twice in a row, so every window of up to `size` samples is
/// contiguous (no wrap handling in the correlation loops).
struct MirrorBuffer
{
    std::vector<float> data;
    std::uint32_t size = 1;
    std::uint32_t mask = 0;

    void prepare (int minSize)
    {
        size = 1;
        while (size < static_cast<std::uint32_t> (minSize))
            size <<= 1;
        mask = size - 1;
        data.assign (2 * size + 4, 0.0f);
    }

    void clear() noexcept { std::fill (data.begin(), data.end(), 0.0f); }

    void write (std::uint32_t index, float x) noexcept
    {
        const std::uint32_t i = index & mask;
        data[i] = x;
        data[i + size] = x;
    }

    const float* at (std::uint32_t index) const noexcept { return data.data() + (index & mask); }
};

struct DotEnergy
{
    float dot, energy;
};

/// Sum a[i] b[i] and sum b[i]^2. Eight partial sums keep the loop vectorisable without
/// relaxed floating-point rules.
inline DotEnergy dotEnergy (const float* a, const float* b, int n) noexcept
{
    std::array<float, 8> d {}, e {};
    int i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; ++k)
        {
            d[static_cast<size_t> (k)] += a[i + k] * b[i + k];
            e[static_cast<size_t> (k)] += b[i + k] * b[i + k];
        }
    float dot = 0.0f, energy = 0.0f;
    for (int k = 0; k < 8; ++k)
    {
        dot += d[static_cast<size_t> (k)];
        energy += e[static_cast<size_t> (k)];
    }
    for (; i < n; ++i)
    {
        dot += a[i] * b[i];
        energy += b[i] * b[i];
    }
    return { dot, energy };
}

inline float normalisedCorrelation (float dot, float refEnergy, float energy) noexcept
{
    return dot / std::sqrt (refEnergy * energy + 1.0e-30f);
}
} // namespace

struct PitchShifter::Impl
{
    /// Everything that depends on the shift amount, in samples.
    struct Shift
    {
        int semitones = 0;
        double rate = 1.0;     // read speed; the pitch ratio
        double jumpMin = 0.0;  // smallest splice jump
        double lowDelay = 0.0; // downward: a new head starts no closer than this
        double trigger = 0.0;  // downward: splice at delay >= trigger; upward: at delay <= trigger
        int fade = 1;          // splice crossfade length

        bool dry() const noexcept { return semitones == 0; }
    };

    /// A read head: how far behind the newest input sample it reads, and how fast it moves.
    struct Head
    {
        double delay = 0.0;
        Shift shift;
    };

    struct Match
    {
        double delay;
        float rho;
    };

    double fs = 48000.0;
    int decimation = 4;
    int corrLen = 1, corrLenDecimated = 1, lookAhead = 1, search = 1;
    double transitionFade = 1.0;

    std::array<MirrorBuffer, kMaxChannels> audio;
    MirrorBuffer mono, decimated;
    Biquad decimationFilter;
    std::uint32_t write = 0, decimatedWrite = 0;
    int decimationPhase = 0;

    Head cur, next;
    bool fading = false;
    int fadePos = 0, fadeLen = 1;
    float rho = 0.0f;
    int target = 0;
    std::vector<float> scores; // fine-search correlation per candidate shift

    void prepare (const ProcessSpec& spec)
    {
        fs = spec.sampleRate;
        auto samples = [&] (double ms)
        {
            return static_cast<int> (std::lround (ms * 0.001 * fs));
        };
        decimation = std::max (1, static_cast<int> (std::lround (fs / kSearchRate)));
        corrLen = std::max (8, samples (kCorrelationMs));
        corrLenDecimated = std::max (8, corrLen / decimation);
        lookAhead = samples (kLookAheadMs);
        search = std::max (2 * decimation, samples (kSearchMs));
        transitionFade = std::max (1.0, kTransitionFadeMs * 0.001 * fs);

        const int history = samples (kHistoryMs);
        for (auto& b : audio)
            b.prepare (history);
        mono.prepare (history);
        decimated.prepare (history / decimation + 8);
        scores.assign (static_cast<size_t> (2 * decimation + 3), 0.0f);

        if (decimation > 1)
            decimationFilter.setCoeffs (design::lowpass (fs, 0.4 * fs / decimation));
        else
            decimationFilter.setCoeffs (BiquadCoeffs {});
    }

    void reset() noexcept
    {
        for (auto& b : audio)
            b.clear();
        mono.clear();
        decimated.clear();
        decimationFilter.reset();
        write = decimatedWrite = 0;
        decimationPhase = 0;

        cur = Head {};
        cur.shift = makeShift (target);
        if (!cur.shift.dry())
            cur.delay = cur.shift.rate < 1.0 ? cur.shift.lowDelay + 0.5 * search
                                             : cur.shift.trigger + cur.shift.jumpMin + 0.5 * search;
        next = cur;
        fading = false;
        fadePos = 0;
    }

    Shift makeShift (int semitones) const noexcept
    {
        Shift s;
        s.semitones = semitones;
        if (semitones == 0)
            return s;

        s.rate = std::exp2 (semitones / 12.0);
        const double octaves = std::abs (semitones) / 12.0;
        s.jumpMin = (kJumpMinMs + kJumpPerOctaveMs * octaves) * 0.001 * fs;
        s.lowDelay = static_cast<double> (lookAhead + decimation + kMargin);
        // A head drifts by |1 - rate| samples per sample, so a jump of J lasts J / |1 - rate|.
        const double interval = s.jumpMin / std::abs (1.0 - s.rate);
        s.fade =
            std::max (1, static_cast<int> (std::min (kSpliceFadeMs * 0.001 * fs, 0.8 * interval)));
        if (s.rate < 1.0)
            s.trigger = s.lowDelay + search + s.jumpMin;
        else // the old head keeps approaching the write point during the fade
            s.trigger = std::max (s.lowDelay, (s.rate - 1.0) * s.fade + kMargin + 2.0);
        return s;
    }

    void push (const AudioView& io, int numChannels, int i) noexcept
    {
        ++write;
        float sum = 0.0f;
        for (int c = 0; c < numChannels; ++c)
        {
            const float x = io.channel (c)[i];
            audio[static_cast<size_t> (c)].write (write, x);
            sum += x;
        }
        const float m = numChannels > 1 ? 0.5f * sum : sum;
        mono.write (write, m);
        const float lp = decimationFilter.processSample (0, m);
        if (++decimationPhase >= decimation)
        {
            decimationPhase = 0;
            ++decimatedWrite;
            decimated.write (decimatedWrite, lp);
        }
    }

    /// Input delayed by a fractional delay >= 1 (the newest sample has delay 0).
    float read (int channel, double delay) const noexcept
    {
        delay = std::max (delay, 1.0);
        const auto k = static_cast<std::uint32_t> (delay);
        const float t = 1.0f - static_cast<float> (delay - static_cast<double> (k));
        return hermite (audio[static_cast<size_t> (channel)].at (write - k - 2u), t);
    }

    bool needsSplice() const noexcept
    {
        const auto& s = cur.shift;
        if (s.dry())
            return false;
        return s.rate < 1.0 ? cur.delay >= s.trigger : cur.delay <= s.trigger;
    }

    /// Picks the new head's delay in [newLo, newHi] whose upcoming waveform best matches the old
    /// head's (maximum normalised cross-correlation of the mono signal). Coarse search on the
    /// decimated copy, then refinement at full rate with parabolic interpolation.
    Match findMatch (double oldDelay, double newLo, double newHi) noexcept
    {
        const double mid = 0.5 * (newLo + newHi);
        // How far past the splice point both segments can look without reading unwritten input.
        const double available = std::min (oldDelay, newLo) - (decimation + kMargin);
        const double ahead = std::min (static_cast<double> (lookAhead), available);
        // Shift J moves a segment J samples later in time: new delay = old delay - J.
        const int refNewest = static_cast<int> (std::ceil (oldDelay - ahead));
        const int jLo = static_cast<int> (std::ceil (oldDelay - newHi));
        const int jHi = static_cast<int> (std::floor (oldDelay - newLo));
        if (jHi < jLo)
            return { mid, 0.0f };

        // Coarse: candidate shifts m * decimation on the decimated signal.
        const int d = decimation;
        const int len = corrLenDecimated;
        const int qRef = std::max (0, static_cast<int> (std::ceil (
                                          static_cast<double> (refNewest - decimationPhase) / d)));
        const float* ref =
            decimated.at (decimatedWrite - static_cast<std::uint32_t> (qRef + len - 1));
        const float refEnergy = dotEnergy (ref, ref, len).energy;
        if (refEnergy < 1.0e-10f * static_cast<float> (len))
            return { mid, 1.0f }; // silence: any splice point is as good as another

        const int mLo = static_cast<int> (std::ceil (static_cast<double> (jLo) / d));
        const int mHi =
            std::min (qRef, static_cast<int> (std::floor (static_cast<double> (jHi) / d)));
        int fineLo = jLo, fineHi = jHi;
        if (mLo <= mHi)
        {
            float best = -2.0f;
            int bestM = mLo;
            for (int m = mLo; m <= mHi; ++m)
            {
                const float* cand =
                    decimated.at (decimatedWrite - static_cast<std::uint32_t> (qRef - m + len - 1));
                const auto [dot, energy] = dotEnergy (ref, cand, len);
                const float r = normalisedCorrelation (dot, refEnergy, energy);
                if (r > best)
                {
                    best = r;
                    bestM = m;
                }
            }
            fineLo = std::max (jLo, bestM * d - d);
            fineHi = std::min (jHi, bestM * d + d);
        }

        // Fine: every integer shift around the coarse winner, full rate.
        const int n = corrLen;
        const float* refFull = mono.at (write - static_cast<std::uint32_t> (refNewest + n - 1));
        const float refFullEnergy = dotEnergy (refFull, refFull, n).energy;
        auto score = [&] (int j)
        {
            const float* cand =
                mono.at (write - static_cast<std::uint32_t> (refNewest - j + n - 1));
            const auto [dot, energy] = dotEnergy (refFull, cand, n);
            return normalisedCorrelation (dot, refFullEnergy, energy);
        };

        const int count = std::min (fineHi - fineLo + 1, static_cast<int> (scores.size()));
        int best = 0;
        for (int k = 0; k < count; ++k)
        {
            scores[static_cast<size_t> (k)] = score (fineLo + k);
            if (scores[static_cast<size_t> (k)] > scores[static_cast<size_t> (best)])
                best = k;
        }

        double shift = fineLo + best;
        if (best > 0 && best + 1 < count)
        {
            const float before = scores[static_cast<size_t> (best - 1)];
            const float peak = scores[static_cast<size_t> (best)];
            const float after = scores[static_cast<size_t> (best + 1)];
            const float curvature = before - 2.0f * peak + after;
            if (curvature < 0.0f)
                shift += std::clamp (0.5 * (before - after) / curvature, -0.5, 0.5);
        }

        const double delay = std::clamp (oldDelay - shift, newLo - 1.0, newHi + 1.0);
        return { delay, std::clamp (scores[static_cast<size_t> (best)], 0.0f, 1.0f) };
    }

    void startSplice() noexcept
    {
        const auto& s = cur.shift;
        const double lo = s.rate < 1.0 ? s.lowDelay : cur.delay + s.jumpMin;
        const Match m = findMatch (cur.delay, lo, lo + search);
        next.shift = s;
        next.delay = m.delay;
        rho = m.rho;
        beginFade (s.fade);
    }

    void startTransition() noexcept
    {
        const Shift s = makeShift (target);
        double fade = transitionFade;
        // An upward head must not overtake the write point before the fade ends.
        if (!cur.shift.dry() && cur.shift.rate > 1.0)
            fade = std::min (fade, (cur.delay - kMargin - 2.0) / (cur.shift.rate - 1.0));

        next.shift = s;
        next.delay = 0.0;
        if (!s.dry())
        {
            const double lo =
                s.rate < 1.0 ? s.lowDelay : s.trigger + std::max (s.jumpMin, (s.rate - 1.0) * fade);
            next.delay = findMatch (cur.delay, lo, lo + search).delay;
        }
        // Different pitches do not add coherently: equal-power fade.
        rho = 0.0f;
        beginFade (static_cast<int> (fade));
    }

    void beginFade (int length) noexcept
    {
        fadeLen = std::max (1, length);
        fadePos = 0;
        fading = true;
    }

    void process (const AudioView& io) noexcept
    {
        const int n = io.numFrames();
        const int numChannels = std::min (io.numChannels(), kMaxChannels);
        if (n <= 0 || numChannels <= 0)
            return;

        // True bypass: leave the audio untouched but keep the history for a later change.
        if (cur.shift.dry() && !fading && target == 0)
        {
            for (int i = 0; i < n; ++i)
                push (io, numChannels, i);
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            push (io, numChannels, i);
            if (!fading)
            {
                if (target != cur.shift.semitones)
                    startTransition();
                else if (needsSplice())
                    startSplice();
            }

            if (!fading)
            {
                if (cur.shift.dry())
                    continue;
                for (int c = 0; c < numChannels; ++c)
                    io.channel (c)[i] = read (c, cur.delay);
                cur.delay += 1.0 - cur.shift.rate;
                continue;
            }

            // Hann (raised-cosine) crossfade. The gains sum to one for a perfect match (rho = 1)
            // and are rescaled towards equal power as the match gets worse.
            const float t = (static_cast<float> (fadePos) + 0.5f) / static_cast<float> (fadeLen);
            const float cosine = std::cos (kPi * t);
            const float gOld = 0.5f + 0.5f * cosine;
            const float gNew = 0.5f - 0.5f * cosine;
            const float norm =
                1.0f / std::sqrt (gOld * gOld + gNew * gNew + 2.0f * rho * gOld * gNew);
            for (int c = 0; c < numChannels; ++c)
            {
                float& y = io.channel (c)[i];
                const float a = cur.shift.dry() ? y : read (c, cur.delay);
                const float b = next.shift.dry() ? y : read (c, next.delay);
                y = norm * (gOld * a + gNew * b);
            }
            cur.delay += 1.0 - cur.shift.rate;
            next.delay += 1.0 - next.shift.rate;
            if (++fadePos >= fadeLen)
            {
                cur = next;
                fading = false;
            }
        }
    }
};

void PitchShifter::ImplDeleter::operator() (Impl* impl) const noexcept
{
    delete impl;
}

void PitchShifter::prepare (const ProcessSpec& spec)
{
    if (impl_ == nullptr)
        impl_.reset (new Impl);
    impl_->prepare (spec);
    impl_->target = std::clamp (settings_.semitones, -12, 12);
    impl_->reset();
}

void PitchShifter::reset() noexcept
{
    if (impl_ != nullptr)
        impl_->reset();
}

void PitchShifter::setSettings (const Settings& s) noexcept
{
    settings_ = s;
    if (impl_ != nullptr)
        impl_->target = std::clamp (s.semitones, -12, 12);
}

void PitchShifter::process (const AudioView& io) noexcept
{
    if (impl_ != nullptr)
        impl_->process (io);
}

} // namespace sedef::dsp
