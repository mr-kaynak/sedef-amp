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
/// Not real-time: fed from the analysis thread with audio drained from the engine's ring.
class PitchTracker
{
public:
    struct Reading
    {
        bool valid = false;
        float frequencyHz = 0.0f;
        int midiNote = 0;          ///< nearest equal-tempered note for the reference
        float cents = 0.0f;        ///< deviation from midiNote, -50..+50
        float clarity = 0.0f;
        float levelDb = -120.0f;   ///< input RMS level

        bool stable = false;       ///< the stability window is full and quiet
        float stableHz = 0.0f;     ///< mean of the stable window
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
    PitchDetector detector_;
    double inputRate_ = 48000.0;
    float referenceHz_ = 440.0f;
    Reading latest_;
    std::vector<float> scratch_;
};

} // namespace sedef::analysis
