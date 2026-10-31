// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <memory>
#include <vector>

namespace sedef::analysis
{

/// Monophonic fundamental-frequency estimator based on the McLeod Pitch Method
/// (P. McLeod, G. Wyvill, "A Smarter Way to Find Pitch", ICMC 2005): normalised square
/// difference function, key-maximum selection, parabolic interpolation.
///
/// When the window holds enough periods (about 7.5) a precision stage follows: the phase advance
/// of the fundamental partial between two Blackman-Harris sub-windows gives its frequency to a
/// small fraction of a cent. Stiff strings are slightly inharmonic, so a periodicity estimate is
/// pulled sharp by the upper partials; the fundamental is what a tuner and an intonation check
/// need. A weak fundamental is inferred from partials 2 and 3 with the stiff-string relation;
/// failing that, the periodicity estimate is returned.
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

    /// Analyses the most recent min(numFrames, windowSize()) frames. Shorter windows are fine: a
    /// pitch is reported once the window holds 2.5 periods of it and, unless the octave below is
    /// out of range, 2.5 periods of that octave too (so it cannot be an octave-high mistake).
    Result detect (const float* frames, int numFrames);

    double sampleRate() const noexcept { return sampleRate_; }
    /// Longest window analysed: eight periods of minHz, enough for the precision stage there.
    int windowSize() const noexcept { return window_; }

private:
    struct Workspace; // pffft plans and aligned buffers
    struct WorkspaceDeleter
    {
        void operator() (Workspace* w) const noexcept;
    };
    struct Peak
    {
        double lag;
        double value;
    };
    struct Partial
    {
        double hz = 0.0;
        double power = 0.0; ///< fraction of the window's power, 0..1
    };

    bool hasHarmonicSupport (const float* x, int n, double period) const noexcept;
    bool refineFundamental (const float* x, int n, double period, double& hz) const noexcept;
    Partial measurePartial (const float* x, int n, double period, int k) const noexcept;

    double sampleRate_ = 12000.0;
    float minHz_ = 25.0f, maxHz_ = 1400.0f;
    int window_ = 0;
    int maxLag_ = 0; ///< longest lag the NSDF is evaluated at (one past the longest period)
    std::vector<float> nsdf_;
    std::vector<Peak> peaks_;
    std::unique_ptr<Workspace, WorkspaceDeleter> ws_;
};

} // namespace sedef::analysis
