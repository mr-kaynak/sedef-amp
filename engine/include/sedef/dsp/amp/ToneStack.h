// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

#include <array>

/// Passive three-knob ("FMV") tone stack after D. T. Yeh and J. O. Smith, "Discretization of the
/// '59 Fender Bassman Tone Stack", DAFx-06. The circuit is solved symbolically once: H(s) is a
/// third-order rational function whose coefficients are polynomials in the component values and
/// the pot positions, so a knob move only costs re-evaluating those polynomials and one bilinear
/// transform. Internal to the amp section; exposed for tests.
///
///   Vin ─C1─ A ─(1-t)R1─ Vout ─t·R1─ B ─l·R2─ C ─(1-m)R3─ W ─m·R3─ gnd
///   Vin ─R4─ D ─C2─ B
///            D ─C3─ W
namespace sedef::dsp::amp
{

/// Component values. The pots are wired as in the paper: treble as a divider (output on its
/// wiper), bass as a variable resistor, mid as a divider whose wiper takes the mid capacitor.
struct ToneStackComponents
{
    double c1, c2, c3; ///< treble, bass and mid capacitors (F)
    double r1, r2, r3; ///< treble, bass and mid pots (ohm)
    double r4;         ///< slope resistor (ohm)
};

/// Late-1950s American tweed bass amplifier values (the circuit analysed by Yeh & Smith).
inline constexpr ToneStackComponents kTweedStack { 250e-12, 20e-9, 20e-9, 250e3, 1e6, 25e3, 56e3 };
/// British-style values derived from the same topology: bigger treble cap, smaller slope resistor.
inline constexpr ToneStackComponents kBritishStack {
    470e-12, 22e-9, 22e-9, 220e3, 1e6, 25e3, 33e3
};

/// H(s) = (b1 s + b2 s^2 + b3 s^3) / (1 + a1 s + a2 s^2 + a3 s^3). b[0] is always 0 (no DC path).
struct ToneStackAnalog
{
    std::array<double, 4> b {}, a {};
};

/// Normalised third-order digital coefficients (a[0] == 1).
struct ToneStackCoeffs
{
    std::array<double, 4> b { 1.0, 0.0, 0.0, 0.0 }, a { 1.0, 0.0, 0.0, 0.0 };
};

/// Pot positions in 0..1 after tapering: t = treble wiper (1 = towards C1), m = mid wiper
/// (1 = top), l = fraction of the bass pot in circuit.
ToneStackAnalog toneStackAnalog (const ToneStackComponents& c, double t, double m,
                                 double l) noexcept;

/// Bilinear transform without pre-warping. At the oversampled rates the stack runs at
/// (>= 176.4 kHz) the warp stays below 0.1 dB up to 10 kHz.
ToneStackCoeffs toneStackDigital (const ToneStackAnalog& h, double sampleRate) noexcept;

/// Log ("audio") taper used for the bass pot: 10 % of the resistance at mid rotation.
double logTaper (double knob) noexcept;

/// Direct-form-II-transposed third-order section in double precision: the stack's poles sit at a
/// few hertz, very close to z = 1 at oversampled rates, where float coefficients are too coarse.
class ToneStackFilter
{
public:
    void setCoeffs (const ToneStackCoeffs& c) noexcept { c_ = c; }
    const ToneStackCoeffs& coeffs() const noexcept { return c_; }
    void reset() noexcept { state_ = {}; }

    float processSample (int channel, float in) noexcept
    {
        auto& s = state_[static_cast<size_t> (channel)];
        const double x = in;
        const double y = c_.b[0] * x + s[0];
        s[0] = c_.b[1] * x - c_.a[1] * y + s[1];
        s[1] = c_.b[2] * x - c_.a[2] * y + s[2];
        s[2] = c_.b[3] * x - c_.a[3] * y;
        return static_cast<float> (y);
    }

    /// Flushes denormal-range state (call once per block).
    void snapToZero() noexcept;

private:
    ToneStackCoeffs c_;
    std::array<std::array<double, 3>, kMaxChannels> state_ {};
};

} // namespace sedef::dsp::amp
