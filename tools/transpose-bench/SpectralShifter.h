// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

#include <memory>

namespace sedef::bench
{

/// Transpose candidate (tools/transpose-bench, not part of the engine): a mono phase-vocoder
/// pitch shifter with a fixed latency. docs/dsp/transpose.md has its measurements.
///
/// The analysis window is long (frequency resolution for low strings and chords) and asymmetric,
/// so only its short tail is resynthesised: the latency is the synthesis window, not the FFT.
/// Spectral peaks are moved with their whole region of influence and one phase rotation each
/// (Laroche & Dolson 1999, identity phase locking); regions whose energy centre crosses the
/// synthesis point are re-phased so the attack stays coherent (after Roebel 2003).
class SpectralShifter
{
public:
    struct Config
    {
        double analysisMs = 85.0; ///< FFT length (the nearest power of two)
        double synthesisMs = 8.0; ///< synthesis window; the latency is one sample less
        int overlap = 4;          ///< synthesis windows per hop
        bool asymmetric = true;   ///< false: symmetric Hann windows, latency = FFT length
        bool transientReset = true;
        double onsetCentre = 0.25; ///< attack: energy centre this far into the frame's future half
        double onsetRise = 1.5;    ///< and region energy grown by this factor in one hop
        int peakNeighbours = 2;    ///< a peak exceeds this many bins on each side
        bool clipRegions = true;   ///< shifted regions do not overlap (downward shifts)
        double peakFloorDb = -90.0;
        double glideMs = 25.0; ///< pitch-ratio glide when the ratio changes
    };

    SpectralShifter();
    ~SpectralShifter();
    SpectralShifter (SpectralShifter&&) noexcept;
    SpectralShifter& operator= (SpectralShifter&&) noexcept;

    void prepare (double sampleRate, const Config& config);
    void reset() noexcept;
    /// Pitch ratio (output / input frequency). Changes glide over Config::glideMs.
    void setRatio (double ratio, bool glide = true) noexcept;
    int latencySamples() const noexcept;
    /// Feeds input without computing output (cheap); keeps the history current while unused.
    void push (const float* in, int n) noexcept;
    /// In place.
    void process (float* io, int n) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sedef::bench
