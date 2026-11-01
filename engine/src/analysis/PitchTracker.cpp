// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/analysis/PitchTracker.h"

#include "sedef/core/Biquad.h"
#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace sedef::analysis
{
namespace
{
constexpr double kTargetRate = 12000.0;  ///< analysis rate before rounding the decimation factor
constexpr double kAntiAliasCutoff = 0.3; ///< fraction of the analysis rate (8th-order Butterworth)
constexpr double kDcCutoffHz = 15.0;     ///< removes interface DC offset; below the lowest note
constexpr float kMinHz = 25.0f;          ///< below a 5-string bass low B
constexpr float kMaxHz = 1400.0f;        ///< above the 24th fret of the high E string
constexpr double kHopSeconds = 0.005;

constexpr double kGateCloseDb = -60.0; ///< readings are invalid below this input level
constexpr double kGateOpenDb = -57.0;  ///< hysteresis so a decaying tail does not re-trigger
constexpr double kOnsetRatio = 4.0;    ///< +6 dB of high-passed energy over the recent maximum
constexpr double kOnsetRefractorySeconds = 0.08;
constexpr double kAttackSkipSeconds = 0.15; ///< strings read sharp while the pluck decays

constexpr double kWindowPeriods = 8.0; ///< enough for the detector's precision stage
constexpr double kMinWindowSeconds = 0.04;
constexpr double kMinDetectSeconds = 0.01;

constexpr double kSettledClarity = 0.8; ///< per settled detection (the detector gates readings)
constexpr double kStableClarity = 0.9;  ///< mean over the stability window
constexpr double kStableStdCents = 0.5;
constexpr double kStableSpanSeconds = 0.4;
constexpr double kMinStableFill = 0.75; ///< fraction of the span's hops that must be present
constexpr double kSettleToleranceCents = 30.0;
constexpr double kPitchChangeCents = 50.0;
constexpr double kValidAgreementCents = 50.0;
constexpr int kHoldHops = 3;             ///< bridge single failed detections
constexpr double kHumMaxLevelDb = -40.0; ///< quieter mains-frequency pitches are taken as hum

double centsBetween (double hz, double refHz) noexcept
{
    return 1200.0 * std::log2 (hz / refHz);
}

/// Mains hum and its rectifier harmonic. None is within 20 cents of an equal-tempered note
/// (A = 440), so only a badly detuned, quiet string can be mistaken for it.
bool isMainsHum (double hz) noexcept
{
    for (double mains : { 50.0, 60.0, 100.0, 120.0 })
        if (std::abs (centsBetween (hz, mains)) < 20.0)
            return true;
    return false;
}
} // namespace

void PitchTracker::prepare (double inputSampleRate)
{
    inputRate_ = inputSampleRate > 0.0 ? inputSampleRate : 48000.0;
    factor_ = std::max (1, static_cast<int> (std::lround (inputRate_ / kTargetRate)));
    rate_ = inputRate_ / factor_;
    hop_ = std::max (1, static_cast<int> (std::lround (kHopSeconds * rate_)));

    // 8th-order Butterworth as four RBJ sections with the Butterworth pole Qs.
    for (int k = 0; k < kAntiAliasSections; ++k)
    {
        const double q = 1.0 / (2.0 * std::cos ((2 * k + 1) * kPiD / (4 * kAntiAliasSections)));
        const auto c = design::lowpass (inputRate_, kAntiAliasCutoff * rate_, q);
        antiAlias_[k] = { c.b0, c.b1, c.b2, c.a1, c.a2 };
    }
    const auto dc = design::highpass1 (rate_, kDcCutoffHz);
    dcBlock_ = { dc.b0, dc.b1, dc.b2, dc.a1, dc.a2 };

    detector_.prepare (rate_, kMinHz, kMaxHz);
    history_.assign (2 * static_cast<size_t> (detector_.windowSize()), 0.0f);
    const auto spanHops = static_cast<size_t> (std::ceil (kStableSpanSeconds * rate_ / hop_));
    settled_.assign (spanHops + 4, Settled {});
    prepared_ = true;
    reset();
}

void PitchTracker::reset()
{
    for (auto& s : antiAlias_)
        s.s1 = s.s2 = 0.0;
    dcBlock_.s1 = dcBlock_.s2 = 0.0;
    phase_ = 0;
    historyFill_ = 0;
    pos_ = 0;
    hopFill_ = 0;
    inputSq_ = fluxSq_ = 0.0;
    prev_ = 0.0f;
    std::fill (std::begin (levelHops_), std::end (levelHops_), 0.0);
    std::fill (std::begin (fluxHops_), std::end (fluxHops_), 0.0);
    hopCount_ = 0;
    gateOpen_ = false;
    startSegment (0, false);
    // The onset counter keeps counting so a UI comparing ids never mistakes a new pluck for an old.
    latest_ = Reading {};
    latest_.onsetId = onsetCounter_;
}

void PitchTracker::push (const float* samples, int numSamples)
{
    if (!prepared_)
        prepare (inputRate_);
    if (samples == nullptr)
        return;
    for (int i = 0; i < numSamples; ++i)
    {
        double x = samples[i];
        if (!std::isfinite (x))
            x = 0.0;
        inputSq_ += x * x;
        for (auto& s : antiAlias_)
            x = s.process (x);
        if (++phase_ >= factor_)
        {
            phase_ = 0;
            pushDecimated (static_cast<float> (x));
        }
    }
}

void PitchTracker::pushDecimated (float x)
{
    const auto y = static_cast<float> (dcBlock_.process (x));
    if (historyFill_ == static_cast<int> (history_.size()))
    {
        // Keep one full window; the buffer is twice that, so this runs once per window.
        const int keep = detector_.windowSize();
        std::memmove (history_.data(), history_.data() + historyFill_ - keep,
                      sizeof (float) * static_cast<size_t> (keep));
        historyFill_ = keep;
    }
    history_[static_cast<size_t> (historyFill_++)] = y;
    ++pos_;

    // First difference: emphasises the fresh upper partials of a pluck over a decaying note.
    const float diff = y - prev_;
    prev_ = y;
    fluxSq_ += static_cast<double> (diff) * diff;

    if (++hopFill_ >= hop_)
    {
        hopFill_ = 0;
        runHop();
    }
}

int PitchTracker::windowFor (double hz) const noexcept
{
    const double frames = std::max (kWindowPeriods * rate_ / hz, kMinWindowSeconds * rate_);
    return std::min (static_cast<int> (std::ceil (frames)), detector_.windowSize());
}

double PitchTracker::median() const noexcept
{
    if (recentCount_ == 0)
        return 0.0;
    // At most five values: an insertion sort (std::sort's heap paths trip GCC's -Warray-bounds).
    const int n = std::min (recentCount_, kMedianSize);
    double v[kMedianSize] {};
    for (int i = 0; i < n; ++i)
    {
        const double x = recent_[i];
        int j = i;
        for (; j > 0 && v[j - 1] > x; --j)
            v[j] = v[j - 1];
        v[j] = x;
    }
    return v[(n - 1) / 2]; // lower median: never a mean of two octaves
}

void PitchTracker::startSegment (std::int64_t start, bool countOnset)
{
    segmentStart_ = std::max<std::int64_t> (start, 0);
    settleFrom_ =
        segmentStart_ + static_cast<std::int64_t> (std::lround (kAttackSkipSeconds * rate_));
    lastOnset_ = pos_;
    if (countOnset)
        ++onsetCounter_;

    segmentHz_ = 0.0;
    recentCount_ = recentHead_ = 0;
    lastAccept_ = std::numeric_limits<std::int64_t>::min() / 2;
    lastAcceptedHz_ = lastClarity_ = 0.0;
    settledCount_ = settledHead_ = 0;
    settledRefHz_ = 0.0;
    stableCount_ = 0;

    latest_.valid = latest_.stable = false;
    latest_.stableHz = latest_.stableStdCents = 0.0f;
    latest_.stableCount = 0;
    latest_.onsetId = onsetCounter_;
}

void PitchTracker::runHop()
{
    // Keep the filters' decay into silence out of the denormal range.
    const auto flush = [] (Section& s) {
        s.s1 = std::abs (s.s1) < 1.0e-30 ? 0.0 : s.s1;
        s.s2 = std::abs (s.s2) < 1.0e-30 ? 0.0 : s.s2;
    };
    for (auto& s : antiAlias_)
        flush (s);
    flush (dcBlock_);

    levelHops_[hopCount_ % kLevelHops] = inputSq_ / (static_cast<double> (hop_) * factor_);
    fluxHops_[hopCount_ % kFluxHops] = fluxSq_ / hop_;
    inputSq_ = fluxSq_ = 0.0;
    ++hopCount_;

    const int levelCount = std::min (hopCount_, kLevelHops);
    double meanSquare = 0.0;
    for (int i = 0; i < levelCount; ++i)
        meanSquare += levelHops_[i];
    meanSquare /= levelCount;
    const double levelDb = 10.0 * std::log10 (std::max (meanSquare, 1.0e-12));
    const bool gate = levelDb >= (gateOpen_ ? kGateCloseDb : kGateOpenDb);

    // Onset: a note emerging from silence, or the high-passed energy jumping over everything in
    // the 10..45 ms before (the maximum covers a full period, so a note's own waveform cannot).
    bool onset = false;
    if (gate && !gateOpen_)
        onset = true;
    else if (gate && pos_ - lastOnset_ >= std::lround (kOnsetRefractorySeconds * rate_))
    {
        double ref = 0.0;
        for (int k = 2; k < kFluxHops && k < hopCount_; ++k)
            ref = std::max (ref, fluxHops_[(hopCount_ - 1 - k) % kFluxHops]);
        onset = fluxHops_[(hopCount_ - 1) % kFluxHops] > kOnsetRatio * ref;
    }
    gateOpen_ = gate;
    if (onset)
        startSegment (pos_, true);

    Reading r = latest_;
    r.levelDb = static_cast<float> (levelDb);
    r.onsetId = onsetCounter_;
    if (!gate)
    {
        r.valid = r.stable = false;
        r.clarity = 0.0f;
        latest_ = r;
        return;
    }

    // Reading: a detection on the frames since the segment start, up to the note's short window,
    // so the needle follows a turning peg.
    const int target = segmentHz_ > 0.0 ? windowFor (segmentHz_) : detector_.windowSize();
    const int n =
        static_cast<int> (std::min<std::int64_t> ({ pos_ - segmentStart_, target, historyFill_ }));
    PitchDetector::Result det;
    if (n >= kMinDetectSeconds * rate_)
        det = detector_.detect (history_.data() + historyFill_ - n, n);

    // Hum on an idle input is periodic too; it must not read (or settle) as a note.
    if (det.valid && !(levelDb < kHumMaxLevelDb && isMainsHum (det.frequencyHz)))
    {
        const double hz = det.frequencyHz;
        recent_[recentHead_] = hz;
        recentHead_ = (recentHead_ + 1) % kMedianSize;
        recentCount_ = std::min (recentCount_ + 1, kMedianSize);
        if (segmentHz_ > 0.0 && recentCount_ >= 3 &&
            std::abs (centsBetween (median(), segmentHz_)) > kPitchChangeCents)
        {
            // The note changed without an audible onset (legato, slide, or another string plucked
            // softly while the last one rings): new segment from about where the new pitch took
            // over the median. It only counts as a pluck when clear, so two strings ringing at
            // similar levels cannot raise onsets.
            startSegment (pos_ - 3 * hop_, det.clarity >= kSettledClarity);
            r = latest_;
            r.levelDb = static_cast<float> (levelDb);
            recent_[0] = hz;
            recentHead_ = recentCount_ = 1;
        }
        lastAccept_ = pos_;
        lastAcceptedHz_ = hz;
        lastClarity_ = det.clarity;
        segmentHz_ = median();
    }

    const double med = median();
    r.valid = recentCount_ >= 2 && pos_ - lastAccept_ <= kHoldHops * hop_ &&
              std::abs (centsBetween (lastAcceptedHz_, med)) <= kValidAgreementCents;
    r.frequencyHz = static_cast<float> (med);
    toNote (r.frequencyHz, referenceHz_, r.midiNote, r.cents);
    r.clarity = static_cast<float> (r.valid ? lastClarity_ : det.clarity);

    // Settled: a second detection over the frames after the attack skip, up to the longest window.
    // Its long sub-windows average noise and reject interference near the fundamental (hum) that
    // the short reading window cannot. The skip also grows to half the time since the onset, which
    // keeps the tail of a slow attack glide out whatever its time constant.
    const auto settledStart = std::max (settleFrom_, segmentStart_ + (pos_ - segmentStart_) / 2);
    if (r.valid && pos_ - settledStart >= windowFor (segmentHz_))
    {
        const int np = static_cast<int> (
            std::min<std::int64_t> ({ pos_ - settledStart, detector_.windowSize(), historyFill_ }));
        const auto precise = detector_.detect (history_.data() + historyFill_ - np, np);
        if (precise.valid && precise.clarity >= kSettledClarity &&
            std::abs (centsBetween (precise.frequencyHz, segmentHz_)) <= kSettleToleranceCents)
        {
            if (settledRefHz_ <= 0.0)
                settledRefHz_ = precise.frequencyHz;
            const auto capacity = static_cast<int> (settled_.size());
            const int slot = (settledHead_ + settledCount_) % capacity;
            settled_[static_cast<size_t> (slot)] = {
                pos_, centsBetween (precise.frequencyHz, settledRefHz_), precise.clarity
            };
            if (settledCount_ < capacity)
                ++settledCount_;
            else
                settledHead_ = (settledHead_ + 1) % capacity;
            ++stableCount_;
        }
    }

    // Stability window: settled detections of the last kStableSpanSeconds.
    const auto span = static_cast<std::int64_t> (std::lround (kStableSpanSeconds * rate_));
    const auto capacity = static_cast<int> (settled_.size());
    while (settledCount_ > 0 && settled_[static_cast<size_t> (settledHead_)].pos <= pos_ - span)
    {
        settledHead_ = (settledHead_ + 1) % capacity;
        --settledCount_;
    }
    bool full = false, quiet = false;
    if (settledCount_ > 0)
    {
        double sum = 0.0, sumClarity = 0.0;
        for (int i = 0; i < settledCount_; ++i)
        {
            const auto& e = settled_[static_cast<size_t> ((settledHead_ + i) % capacity)];
            sum += e.cents;
            sumClarity += e.clarity;
        }
        const double mean = sum / settledCount_;
        double var = 0.0;
        for (int i = 0; i < settledCount_; ++i)
        {
            const double dev =
                settled_[static_cast<size_t> ((settledHead_ + i) % capacity)].cents - mean;
            var += dev * dev;
        }
        const double stdCents = settledCount_ > 1 ? std::sqrt (var / (settledCount_ - 1)) : 0.0;
        r.stableHz = static_cast<float> (settledRefHz_ * std::exp2 (mean / 1200.0));
        r.stableStdCents = static_cast<float> (stdCents);

        const auto oldest = settled_[static_cast<size_t> (settledHead_)].pos;
        full = oldest <= pos_ - span + hop_ &&
               settledCount_ >= kMinStableFill * static_cast<double> (span) / hop_;
        quiet = sumClarity / settledCount_ >= kStableClarity && stdCents < kStableStdCents;
    }
    r.stableCount = stableCount_;
    r.stable = r.valid && full && quiet;
    latest_ = r;
}

void PitchTracker::toNote (float hz, float a4Hz, int& midiNote, float& cents) noexcept
{
    if (!(hz > 0.0f) || !(a4Hz > 0.0f))
    {
        midiNote = 0;
        cents = 0.0f;
        return;
    }
    const double note = 69.0 + 12.0 * std::log2 (static_cast<double> (hz) / a4Hz);
    const double nearest = std::floor (note + 0.5); // [-50, +50): exactly +50 rounds up
    midiNote = static_cast<int> (nearest);
    cents = static_cast<float> (100.0 * (note - nearest));
}

} // namespace sedef::analysis
