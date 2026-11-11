// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/Biquad.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sedef::analysis
{

/// Real-time monophonic note tracker: which note is sounding, how sure it is, and the note's
/// fundamental as an analytic (complex) signal whose phase advances exactly one turn per period.
///
/// Built for octave dividers (Trench's Mono mode) and meant to be reusable for guitar-to-MIDI. Two
/// parts work together:
///
/// - **Period estimator.** The input is band-limited and decimated to about 7–8 kHz. A normalised
///   square difference function (NSDF, McLeod & Wyvill 2005) is kept up to date incrementally for
///   every lag, over a window of one candidate period (at least 4 ms), so a note can be read after
///   about two of its periods. Every 0.5 ms the key maxima are picked as in MPM, with an extra
///   check against octave-high errors (a strong 2nd or 3rd harmonic): a peak at twice or three
///   times the chosen lag that is clearly more periodic wins.
/// - **Note logic with hysteresis.** A note locks once two estimates in a row agree and are
///   periodic enough. While locked it follows small moves (vibrato, bends, glides), survives
///   short stretches of poor estimates (pick noise, a ringing neighbour) for a hold time, changes
///   to a new note only when the new period has been seen consistently (longer for octave jumps,
///   the classic estimator error), and lets go after the hold.
/// - **Fundamental.** Two cascaded complex one-pole resonators, steered to the note, pass its
///   fundamental and attenuate the 2nd harmonic by about 25 dB. Their output is the note's
///   analytic fundamental: its phase is what an octave divider halves.
///
/// Allocation happens in prepare() only; everything else is real-time safe.
class MonoTracker
{
public:
    static constexpr float kDefaultMinHz = 50.0f;
    static constexpr float kDefaultMaxHz = 1400.0f;

    void prepare (double sampleRate, float minHz = kDefaultMinHz, float maxHz = kDefaultMaxHz);
    void reset() noexcept;

    /// Behaviours a caller may turn on. All are off by default, and with all off the tracker
    /// behaves exactly as it always has (an octave divider's tracking does not move).
    struct Options
    {
        /// Notes whose fundamental is very weak are still notes. A low string plucked near the
        /// bridge and heard by a bridge pickup can carry only -30 to -40 dB of its energy at the
        /// fundamental for the first 100 ms or so, under the share that otherwise marks a
        /// period as a "virtual fundamental" (two notes repeating together at a period no note
        /// has). Such a period is accepted when its 7th harmonic still carries energy: no pair of
        /// notes in a ratio of small numbers (an octave, fifth, fourth, third or sixth) has any
        /// there. Applies to notes up to a seventh of the anti-alias band (about 210 Hz). A
        /// period with under 1 % of its energy at its fundamental is then judged 2.5 ms later
        /// than others, once the attack no longer leaks into its fundamental.
        bool weakFundamentals = false;
        /// Pick detection for fingers. For 60 ms after a pick only a pick 6 dB louder than its
        /// peak counts, even inside the refractory time, dated from where its climb began: a
        /// finger touching the string just before it plucks it does not hide the pluck, and the
        /// attack of a low note plucked near the bridge (whose 2 ms power swings by 10-20 dB
        /// with every period while the 40 ms power catches up) is not picked again and again.
        bool fingerPicks = false;
        /// The quietest a pick may be: its 2 ms power in the band (linear, 1 is full scale),
        /// and its click's above 2 kHz. A caller that tracks the input's noise floor can keep
        /// them just over it.
        float onsetFloor = 1.0e-7f; ///< -70 dBFS
        float clickFloor = 1.0e-8f; ///< -80 dBFS
        /// The lowest note looked for, when above the floor given to prepare() (0: that floor).
        /// A caller prepared for an extended range can keep the search to a guitar's own range,
        /// where periods longer than any of its notes are never candidates.
        float lowestHz = 0.0f;
        /// The rumble high-pass ahead of everything, when not the usual 40 Hz (0: 40 Hz). A string
        /// under a guitar's low E needs it lower to keep its fundamental.
        float rumbleHz = 0.0f;
    };
    /// Real-time safe; takes effect from the next sample.
    void setOptions (const Options& options) noexcept;
    const Options& options() const noexcept { return options_; }

    /// Feeds one input sample.
    void push (float x) noexcept;

    /// A note is locked (it may be quiet: level gating is the caller's job).
    bool voiced() const noexcept { return locked_; }
    /// The locked note's fundamental, 0 when none: measured on the fundamental itself once the
    /// resonators have settled on it (to a fraction of a cent), the estimator's value before.
    float hz() const noexcept
    {
        return locked_ ? (partialSettled_ ? partialHz_ : lockedHz_) : 0.0f;
    }
    /// The locked note as the estimator follows it, 0 when none: it moves with the string at once
    /// and without a break (hz() changes its source once a note holds still), which is what a
    /// pitch bend follows. A few cents less exact than hz() on a steady note.
    float followedHz() const noexcept { return locked_ ? lockedHz_ : 0.0f; }
    /// Periodicity of the latest estimate, 0..1 (the NSDF peak).
    float clarity() const noexcept { return clarity_; }
    /// The latest raw estimate, 0 when nothing periodic was found.
    float estimateHz() const noexcept { return estimateHz_; }
    /// Increments on every lock and on every change of note.
    std::uint32_t noteId() const noexcept { return noteId_; }

    /// The steered resonators' output for the latest sample: the fundamental, analytic.
    float fundamentalRe() const noexcept { return zRe2_; }
    float fundamentalIm() const noexcept { return zIm2_; }

    /// What the latest push() changed. A note-to-MIDI layer places the event at the sample whose
    /// push() returned it (its offset in the block) and reads the pick from samplesSincePick().
    enum class Event : std::uint8_t
    {
        none,
        noteOn,     ///< a note locked while none was held
        noteChange, ///< a different note took over: the old one ends and the new one starts here.
                    ///< A legato step may instead be followed like a bend (hz() moves to it
                    ///< either way); a layer that wants one note per semitone quantises hz().
        noteOff,    ///< the held note was let go (it faded, or stopped being periodic)
    };
    Event event() const noexcept { return event_; }
    /// Input samples since the latest pick (dated back to where its rise began), -1 before any:
    /// how far before a note-on its pick was.
    int samplesSincePick() const noexcept;
    /// Level of the latest pick, for velocity: the highest 2 ms RMS of the band-limited input
    /// (40 Hz to 1.5 kHz) over the 10 ms after it, as a linear amplitude. 0 before any pick.
    float pickLevel() const noexcept;
    /// Increments on every pick (reset() starts it again from 0): a layer that compares it from
    /// one sample to the next sees each pick once, on the push that detected it.
    std::uint32_t pickCount() const noexcept { return pickCount_; }

private:
    struct KeyMax
    {
        float lag;
        float value;
    };

    void updateLags (float x) noexcept;
    void estimate() noexcept;
    void track() noexcept;
    void steer (float hz) noexcept;
    void measurePartial() noexcept;
    float fundamentalShare (float lag) const noexcept;
    /// Share of the last two periods' energy at the given harmonic of the period.
    float partialShare (float lag, int harmonic) const noexcept;
    /// The period's own frequency sounds: it is a note, not a virtual fundamental.
    bool fundamentalSounds (float lag) const noexcept;

    Options options_;

    // Rates and ranges.
    double sampleRate_ = 48000.0;
    double rate_ = 8000.0; ///< decimated rate
    int factor_ = 6, phase_ = 0;
    int minLag_ = 2, maxLag_ = 160;
    int searchLag_ = 160; ///< the longest lag estimate() looks at (Options::lowestHz)
    int minWindow_ = 32;
    int hop_ = 4, hopCount_ = 0;
    float minHz_ = kDefaultMinHz, maxHz_ = kDefaultMaxHz;

    // Conditioning: rumble out, anti-alias for the decimated estimator.
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
    };
    Section hp_, aa1_, aa2_, pick_;

    // Decimated history (mirrored, so lags read contiguously) and the running sums per lag.
    std::vector<float> history_;
    int historySize_ = 0, write_ = 0;
    std::vector<double> r_, m_;
    std::vector<float> nsdf_;
    std::array<KeyMax, 48> keys_ {};

    // Estimate and note logic.
    float estimateHz_ = 0.0f, clarity_ = 0.0f;
    float heldHz_ = 0.0f, heldClarity_ = 0.0f; ///< the key maximum of the locked note, if any
    bool estimateWeak_ = false; ///< the estimate's fundamental is weak (Options::weakFundamentals)

    // Picks.
    float fastPower_ = 0.0f, slowPower_ = 0.0f, fastCoeff_ = 0.0f, slowCoeff_ = 0.0f;
    float clickFast_ = 0.0f, clickSlow_ = 0.0f, clickFastCoeff_ = 0.0f, clickSlowCoeff_ = 0.0f;
    int refractory_ = 0, refractorySamples_ = 0;
    int onsetHops_ = 0, onsetWindowHops_ = 0;
    int samplesSinceOnset_ = 0; ///< decimated samples since the last pick
    bool picked_ = false;       ///< a pick has been seen since the last reset
    std::uint32_t pickCount_ = 0;
    float pickPeak_ = 0.0f; ///< highest fast power since the last pick, within its window
    int pickWindow_ = 0, pickWindowSamples_ = 0; ///< decimated samples left / in the window
    Event event_ = Event::none;
    int rising_ = 0; ///< decimated samples the power has been rising
    int climb_ = 0;  ///< decimated samples the 2 ms power has been climbing without a break
    float candidateHz_ = 0.0f;
    int candidateHops_ = 0;
    bool locked_ = false;
    float lockedHz_ = 0.0f;
    int holdHops_ = 0, maxHoldHops_ = 60;
    int lockHops_ = 2, switchHops_ = 2, octaveSwitchHops_ = 16;
    std::uint32_t noteId_ = 0;

    // Steered resonators.
    float steerHz_ = 0.0f, steerFraction_ = 0.0f;
    int hopsSinceNote_ = 0; ///< since the last lock or change of note
    float pRe_ = 0.0f, pIm_ = 0.0f, gain_ = 0.0f;
    float zRe1_ = 0.0f, zIm1_ = 0.0f, zRe2_ = 0.0f, zIm2_ = 0.0f;
    float turnRe_ = 0.0f, turnIm_ = 0.0f; ///< the output's turn over the current hop
    float partialHz_ = 0.0f;              ///< the partial the resonators pass, measured
    bool partialSettled_ = false;         ///< ... and the resonators follow it
};

} // namespace sedef::analysis
