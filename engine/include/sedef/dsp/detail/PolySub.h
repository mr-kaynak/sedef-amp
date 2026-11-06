// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Biquad.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sedef::dsp::detail
{

/// Polyphonic octave-down voices (Trench's Poly mode): every partial of the input, from every
/// string of a chord, is resynthesised at half and at a quarter of its frequency.
///
/// - **Filter bank.** The input is band-limited and decimated to about 4 kHz, then split by a
///   bank of complex band-pass filters (three cascaded one-pole resonators per band): 14 Hz wide
///   from 50 Hz, then a 60th of the centre frequency, up to 1.2 kHz. Each band's output is the
///   partial near its centre as an analytic signal.
/// - **One band per partial.** A partial is a peak of the bank's power across bands, and only
///   the band at the peak (its "owner") is voiced. When the peak moves to the next band (vibrato,
///   a bend, a beat), the owner hands its phase and level over, so one partial stays one
///   continuous voice. Silent bands copy the phase of their stronger neighbour, so a new owner
///   starts where the old one was.
/// - **Phase division.** The owner's phase is followed step by step and its steps are halved
///   (and quartered): the voice is phase-continuous and exactly an octave (two octaves) down. Its
///   level is the band's, corrected for where the partial sits on the band's slope.
/// - **Resolution on demand.** Narrow bands resolve the partials of a chord but are slow, so the
///   bank widens four times on a pick (and on a sudden drop, such as a palm mute) and narrows back
///   over about 30 ms: notes speak within about 10 ms, sustained chords are resolved.
///
/// Why not halve every band (the classic octave-down filter bank): each band would pick either of
/// the two square roots of its phase, and bands sharing a partial then cancel; two partials in
/// one band turn into intermodulation. One owner per partial removes the ambiguity, and narrow
/// bands keep a second partial far down in the owner band. See docs/dsp/trench.md.
///
/// Allocation happens in prepare() only; everything else is real-time safe.
class PolySub
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;

    /// Feeds one input sample and returns the voices for it, one and two octaves down. Each sub
    /// partial has the amplitude of the partial it came from.
    void process (float x, float& sub1, float& sub2) noexcept;

    /// Partials being voiced (owners), for tests.
    int partials() const noexcept;
    /// The bank's decimation factor and band count, for tests.
    int decimation() const noexcept { return factor_; }
    int bands() const noexcept { return static_cast<int> (centre_.size()); }

private:
    /// One biquad section with its own coefficients and state.
    struct Section
    {
        BiquadCoeffs c;
        float s0 = 0.0f, s1 = 0.0f;

        float process (float x) noexcept
        {
            const float y = c.b0 * x + s0;
            s0 = c.b1 * x - c.a1 * y + s1;
            s1 = c.b2 * x - c.a2 * y;
            return y;
        }

        void clear() noexcept { s0 = s1 = 0.0f; }
    };

    static constexpr int kOrder = 3; ///< one-pole resonators per band

    void step (float x) noexcept; ///< one sample of the decimated bank
    void setWidth (float factor) noexcept;
    void assignOwners() noexcept;
    void flushDenormals() noexcept;

    double sampleRate_ = 48000.0, bankRate_ = 4000.0;
    int factor_ = 12, phase_ = 0;

    // Resampling.
    std::array<Section, 3> antiAlias_ {};
    std::array<Section, 2> smooth1_ {}, smooth2_ {};
    float previous1_ = 0.0f, previous2_ = 0.0f, current1_ = 0.0f, current2_ = 0.0f;

    // Bands (structure of arrays, so the filter loops vectorise).
    std::vector<float> centre_; ///< Hz
    /// pi * per-pole bandwidth / bank rate: a pole's radius is exp (-damping * width factor).
    std::vector<float> damping_;
    std::vector<float> cosW_, sinW_;     ///< e^{i w_c}
    std::vector<float> zeroRe_, zeroIm_; ///< gain of the zero at -f_c, normalised at +f_c
    std::vector<float> poleRe_, poleIm_, gain_, radius_;
    std::array<std::vector<float>, kOrder> re_, im_;
    std::vector<float> lastRe_, lastIm_, power_;
    std::vector<float> halfRe_, halfIm_, quarterRe_, quarterIm_, fade_;
    std::vector<std::uint8_t> owner_;
    float input_ = 0.0f; ///< previous decimated input, for the zero

    // Pick (and drop) detector, and the bandwidth it sets.
    float fast_ = 0.0f, slow_ = 0.0f, fastCoeff_ = 0.0f, slowCoeff_ = 0.0f;
    bool armed_ = true, dropArmed_ = true;
    int refractory_ = 0, refractorySteps_ = 0;
    float widen_ = 1.0f, width_ = 1.0f, narrowCoeff_ = 0.0f;
    float fadeStep_ = 0.0f;
    float floor_ = 0.0f; ///< band power under which no band is voiced
    int steps_ = 0;      ///< bank steps, modulo the update and flush periods
};

} // namespace sedef::dsp::detail
