// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/AudioView.h"

#include <memory>

namespace sedef::dsp
{

/// Clean amp: two triode stages, Fender-style tone stack, push-pull power amp.
struct LagoonSettings
{
    float volume = 0.5f; ///< preamp drive
    float bass = 0.5f;
    float cut = 0.5f;    ///< subtle high cut
    float master = 0.5f; ///< power amp drive
    float output = 0.5f; ///< tone-neutral level
};

/// Crunch amp: cascaded stages, Marshall-style tone stack, three treble voicings.
struct ReefSettings
{
    int voice = 0; ///< 0 Normal, 1 Thick, 2 Thicker
    float gain = 0.5f, bass = 0.5f, mid = 0.5f, treble = 0.5f;
    float presence = 0.5f; ///< preamp high-frequency attenuation
    float master = 0.5f, output = 0.5f;
};

/// High-gain lead amp with bright switch and mid "shift" (moves the mid centre frequency).
struct AbyssSettings
{
    bool bright = true;
    float gain = 0.5f, bass = 0.5f, mid = 0.5f, treble = 0.5f;
    bool shift = false;
    float presence = 0.5f; ///< power-amp high shelf boost
    float master = 0.5f, output = 0.5f;
};

/// Hosts the three amp models; exactly one is audible. Switching models crossfades. Runs the
/// nonlinear stages oversampled (see ADR-0008) and reports the resulting latency.
class AmpSection
{
public:
    enum Model
    {
        Lagoon = 0,
        Reef = 1,
        Abyss = 2
    };

    struct Settings
    {
        int model = Reef;
        LagoonSettings lagoon;
        ReefSettings reef;
        AbyssSettings abyss;
    };

    AmpSection();
    ~AmpSection();
    AmpSection (const AmpSection&) = delete;
    AmpSection& operator= (const AmpSection&) = delete;

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    void process (const AudioView& io) noexcept;
    int latencySamples() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sedef::dsp
