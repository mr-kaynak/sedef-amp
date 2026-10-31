// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <vector>

namespace sedef::analysis
{

/// Monophonic fundamental-frequency estimator based on the McLeod Pitch Method
/// (P. McLeod, G. Wyvill, "A Smarter Way to Find Pitch", ICMC 2005): normalised square
/// difference function, key-maximum selection, parabolic interpolation.
///
/// Not real-time: may be called from an analysis thread only.
class PitchDetector
{
public:
    struct Result
    {
        bool valid = false;
        float frequencyHz = 0.0f;
        float clarity = 0.0f; ///< 0..1, NSDF peak height
    };

    /// sampleRate is the rate of the frames passed to detect(). Detectable range is
    /// [minHz, maxHz]; window must hold at least two periods of minHz.
    void prepare (double sampleRate, float minHz = 25.0f, float maxHz = 1400.0f);
    Result detect (const float* frames, int numFrames);

    double sampleRate() const noexcept { return sampleRate_; }
    int windowSize() const noexcept { return window_; }

private:
    double sampleRate_ = 12000.0;
    float minHz_ = 25.0f, maxHz_ = 1400.0f;
    int window_ = 0;
    std::vector<float> nsdf_;
};

} // namespace sedef::analysis
