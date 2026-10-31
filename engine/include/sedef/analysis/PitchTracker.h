// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/analysis/PitchDetector.h"

#include <cstdint>
#include <vector>

namespace sedef::analysis
{

/// Turns a raw audio stream into stable tuner readings and precise intonation measurements.
///
/// Pipeline: anti-alias + decimate to ~12 kHz -> sliding window -> PitchDetector every hop ->
/// onset detection -> skip the pluck attack (strings read sharp for ~100-200 ms) -> median
/// filter -> stability window with running statistics.
///
/// Two detections run per hop. The reading's window starts at the latest onset and grows to eight
/// periods of the note (at least 40 ms): the first reading needs only a few periods and the
/// needle follows a turning peg. The settled detection uses up to 320 ms that lie entirely after
/// the attack skip (150 ms, growing to half the time since the onset); `stable` and `stableHz`
/// summarise the last 0.4 s of those, so they follow the slow flat drift of a decaying string.
/// See docs/dsp/tuner.md.
///
/// Not real-time: fed from the analysis thread with audio drained from the engine's ring.
class PitchTracker
{
public:
    struct Reading
    {
        bool valid = false;
        float frequencyHz = 0.0f;
        int midiNote = 0;   ///< nearest equal-tempered note for the reference
        float cents = 0.0f; ///< deviation from midiNote, -50..+50
        float clarity = 0.0f;
        float levelDb = -120.0f; ///< input RMS level

        bool stable = false;   ///< the stability window is full and quiet
        float stableHz = 0.0f; ///< mean of the stable window
        float stableStdCents = 0.0f;
        int stableCount = 0;       ///< detections in the current settled segment
        std::uint32_t onsetId = 0; ///< increments on every detected pluck
    };

    void prepare (double inputSampleRate);
    void reset();
    void setReferenceHz (float a4Hz) noexcept { referenceHz_ = a4Hz; }

    /// Feeds input-rate samples; updates latest().
    void push (const float* samples, int numSamples);
    Reading latest() const noexcept { return latest_; }

    /// Frequency -> (midi note, cents) for a given A4 reference.
    static void toNote (float hz, float a4Hz, int& midiNote, float& cents) noexcept;

private:
    // Implementation detail; free to change.
    struct Section ///< transposed direct form II biquad in double precision
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double s1 = 0.0, s2 = 0.0;

        double process (double x) noexcept
        {
            const double y = b0 * x + s1;
            s1 = b1 * x - a1 * y + s2;
            s2 = b2 * x - a2 * y;
            return y;
        }
    };
    struct Settled
    {
        std::int64_t pos;
        double cents; ///< relative to settledRefHz_
        double clarity;
    };

    static constexpr int kAntiAliasSections = 4;
    static constexpr int kLevelHops = 10;
    static constexpr int kFluxHops = 10;
    static constexpr int kMedianSize = 5;

    void pushDecimated (float x);
    void runHop();
    void startSegment (std::int64_t start, bool countOnset);
    int windowFor (double hz) const noexcept;
    double median() const noexcept;

    PitchDetector detector_;
    double inputRate_ = 48000.0;
    float referenceHz_ = 440.0f;
    Reading latest_;
    bool prepared_ = false;

    // Decimation.
    Section antiAlias_[kAntiAliasSections];
    Section dcBlock_;
    int factor_ = 4, phase_ = 0;
    double rate_ = 12000.0; ///< analysis (decimated) rate
    int hop_ = 60;

    // Decimated history, contiguous so windows can be handed to the detector directly.
    std::vector<float> history_;
    int historyFill_ = 0;
    std::int64_t pos_ = 0; ///< decimated samples received

    // Per-hop accumulators and short histories.
    int hopFill_ = 0;
    double inputSq_ = 0.0, fluxSq_ = 0.0;
    float prev_ = 0.0f;
    double levelHops_[kLevelHops] {};
    double fluxHops_[kFluxHops] {};
    int hopCount_ = 0;

    // Segment (one note from its onset).
    std::uint32_t onsetCounter_ = 0;
    bool gateOpen_ = false;
    std::int64_t segmentStart_ = 0, settleFrom_ = 0, lastOnset_ = 0, lastAccept_ = 0;
    double segmentHz_ = 0.0;
    double recent_[kMedianSize] {};
    int recentCount_ = 0, recentHead_ = 0;
    double lastAcceptedHz_ = 0.0, lastClarity_ = 0.0;

    // Settled detections of the stability window (ring).
    std::vector<Settled> settled_;
    int settledHead_ = 0, settledCount_ = 0;
    double settledRefHz_ = 0.0;
    int stableCount_ = 0;
};

} // namespace sedef::analysis
