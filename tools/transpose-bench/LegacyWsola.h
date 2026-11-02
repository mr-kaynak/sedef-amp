// SPDX-License-Identifier: AGPL-3.0-or-later
// The 0.2.0 transpose (WSOLA-style delay-line shifter), kept unchanged as the baseline for
// tools/transpose-bench. Not part of the engine.
#pragma once

#include "sedef/core/AudioView.h"

#include <memory>

namespace sedef::bench
{

/// Polyphonic +-12 semitone pitch shifter for alternate tunings (e.g. play in Eb with standard
/// tuning). Time-domain overlap-add with correlation-based splice points (WSOLA-style), so chords
/// survive. At 0 semitones it is a true bypass.
class LegacyWsola
{
public:
    struct Settings
    {
        int semitones = 0; ///< -12..+12
    };

    /// The 0.2.0 splice timing by default; the bench sweeps it.
    struct Tuning
    {
        double jumpMinMs = 8.0;
        double spliceFadeMs = 40.0;
        double searchMs = 16.0;
    };
    void setTuning (const Tuning& t) noexcept { tuning_ = t; }

    void prepare (const ProcessSpec& spec);
    void reset() noexcept;
    void setSettings (const Settings& s) noexcept;
    /// Mono or stereo in place.
    void process (const AudioView& io) noexcept;

private:
    struct Impl;
    struct ImplDeleter
    {
        void operator() (Impl* impl) const noexcept;
    };
    std::unique_ptr<Impl, ImplDeleter> impl_;
    Settings settings_;
    Tuning tuning_;
};

} // namespace sedef::bench
