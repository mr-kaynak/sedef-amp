// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace sedef::analysis
{

/// Turns the tracker's per-hop estimates into what a tuner needle should show: calm on a
/// sustained note, prompt while a peg turns, the same note from the pick to the end of the decay,
/// and the last value held when the note dies. See docs/dsp/tuner.md, "Display".
///
/// Fed once per hop by PitchTracker with two estimates of the pitch:
/// - the short-window reading: fast, but it swings with hum and noise and is pulled by another
///   string still ringing;
/// - a long-window detection over the frames since the pitch last moved: precise once it is long,
///   and without lag while the pitch holds.
/// Each is weighted by the inverse of its observed scatter, and the result is smoothed by a
/// first-order filter whose time constant grows with the measurement noise and shrinks with the
/// error measured in units of that noise. The note name is locked with hysteresis. When the
/// reading departs from the long window by more than its usual disagreement (a peg turn), the
/// tracker starts the long window again from there.
class TunerDisplay
{
public:
    struct Input
    {
        bool onset = false;       ///< an audible pluck started a new segment this hop
        bool pitchChange = false; ///< the segment restarted on a pitch change without a pluck
        bool readingValid = false;
        double readingHz = 0.0; ///< short-window reading (median of five)
        bool preciseValid = false;
        double preciseHz = 0.0;      ///< long-window detection since the last move
        double preciseSeconds = 0.0; ///< its window length
    };

    struct Output
    {
        bool valid = false;   ///< a note is shown (also while held)
        bool held = false;    ///< the reading has ended; the last value stands
        bool settled = false; ///< from a long window and converged: good to a tenth of a cent
        double hz = 0.0;
        int note = 0;       ///< midi note shown, for the reference
        double cents = 0.0; ///< hz relative to note
    };

    void prepare (double hopSeconds) noexcept;
    void reset() noexcept;
    void setReferenceHz (double a4Hz) noexcept { referenceHz_ = a4Hz > 0.0 ? a4Hz : 440.0; }

    /// One hop. Returns true when the reading has moved away from the long window: the tracker
    /// then restarts the long window at the current position.
    bool update (const Input& in) noexcept;
    const Output& output() const noexcept { return out_; }

private:
    enum class Phase
    {
        idle,     ///< nothing to show yet
        attack,   ///< a new note: following the reading until the long window is ready
        waiting,  ///< a re-pluck of the note shown: keep it still through the attack
        tracking, ///< the long window is available
        pending,  ///< a pitch change without a pluck: hold until it persists (slips, legato)
    };

    void startSegment() noexcept;
    /// One-pole approach to `cents` with time constant `tau` (or straight there when empty).
    void follow (double cents, double tau) noexcept;
    /// The approach whose time constant follows the measurement's noise and the error.
    void smooth (double cents, double variance, bool moving) noexcept;
    double midiOf (double cents) const noexcept;
    void lockNote (double cents, bool force) noexcept;
    void publish() noexcept;

    double hop_ = 0.005;
    double referenceHz_ = 440.0;
    Output out_;

    Phase phase_ = Phase::idle;
    bool haveValue_ = false;
    double value_ = 0.0; ///< pitch shown, cents re 1 Hz
    int note_ = 0;
    double noteAway_ = 0.0;   ///< seconds the pitch has been outside the locked note
    double pendingFor_ = 0.0; ///< seconds a pitch change has persisted
    double invalidFor_ = 0.0; ///< seconds without a valid reading
    bool newNote_ = false;    ///< a pluck, and no reading of it yet
    bool revealing_ = false;  ///< a new note's first readings, not shown yet
    double revealIn_ = 0.0;
    double candidate_ = 0.0;

    // Scatter of the two estimates from one hop to the next (cents^2).
    bool haveReading_ = false, havePrecise_ = false, preciseFresh_ = true;
    double lastReading_ = 0.0, lastPrecise_ = 0.0;
    double readingVar_ = 1.0, preciseVar_ = 1.0;

    // Moves: the reading's low-passed departure from the long window, against its usual spread.
    double residual_ = 0.0;
    double moveVar_ = 1.0;    ///< usual (reading - long window)^2, bias and noise included
    double moveWarmup_ = 0.0; ///< seconds of the segment's learning of moveVar_
    bool moving_ = false;     ///< following the reading while the pitch moves
    double movingFor_ = 0.0, stillFor_ = 0.0;
    double slope_ = 0.0;           ///< of the reading, cents per second
    bool awaitingPrecise_ = false; ///< the move is over and the long window is still too short
};

} // namespace sedef::analysis
