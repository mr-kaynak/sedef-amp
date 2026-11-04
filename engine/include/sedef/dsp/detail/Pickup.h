// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace sedef::dsp::detail
{

/// The guitar Sedef assumes behind its DI, for pedals whose input loads the pickup (the
/// two-transistor fuzz, the treble booster). A DI recorded into a high-impedance input carries
/// roughly the pickup's open-circuit voltage; a pedal with a low input impedance draws current
/// through the pickup's coil instead, so its resistance and inductance shape what the pedal sees.
/// Typical single-coil values: about 6 kOhm DC resistance and 2.5 H inductance, with a 250 kOhm
/// audio-taper volume pot (about 15 % of its resistance at half rotation).
struct Pickup
{
    static constexpr double kResistance = 6.0e3;  ///< coil DC resistance, Ohm
    static constexpr double kInductance = 2.5;    ///< coil inductance, H
    static constexpr double kVolumePot = 250.0e3; ///< volume pot, Ohm
    static constexpr float kPotTaper = 3.5f;      ///< audioTaper() curve: 15 % at half rotation
};

} // namespace sedef::dsp::detail
