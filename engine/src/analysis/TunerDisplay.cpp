// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/analysis/TunerDisplay.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::analysis
{
namespace
{
// Scatter of each estimate from one hop to the next: half the mean square difference (the
// variance of white noise), over these time constants. A long window starts out as uncertain as
// kInitialPreciseScale times the reading and earns its weight as it proves steady.
constexpr double kReadingScatterSeconds = 0.1;
constexpr double kPreciseScatterSeconds = 0.05;
constexpr double kInitialPreciseScale = 4.0;
constexpr double kVarianceFloor = 1.0e-6; ///< (0.001 cent)^2

// Smoothing: tau = clamp (hop / 2 * variance / target^2) / (1 + (error / (k sigma))^2), so noise
// is averaged down to about kTargetCents while an error well beyond the noise is followed at once.
constexpr double kTargetCents = 0.05;
constexpr double kRestMinSeconds = 0.08;
constexpr double kRestMaxSeconds = 0.5;
constexpr double kErrorSigmas = 3.0;
constexpr double kMoveTauSeconds = 0.025;
constexpr double kAttackTauSeconds = 0.035; ///< a new note's first readings converge quickly
constexpr double kRevealSeconds = 0.05;     ///< a new note shows this long after its first reading
constexpr double kMinTauSeconds = 0.01;

// A move: the reading, low-passed over a few hops, departs from the long window by more than
// kMoveSigmas times their usual disagreement (learned over the first kWarmupSeconds of every
// segment, bias included) and at least kMoveMinCents.
constexpr double kMoveMinCents = 1.5;
constexpr double kMoveSigmas = 3.0;
constexpr double kResidualSeconds = 0.02;
constexpr double kWarmupSeconds = 0.1;
constexpr double kWarmupTauSeconds = 0.04;
constexpr double kMoveVarDownSeconds = 0.4; ///< a reading that gets cleaner (a string decaying)
constexpr double kMoveVarUpSeconds = 3.0;   ///< slowly, so a gradual move is not absorbed
constexpr double kMoveHoldSeconds = 0.06;   ///< a move lasts at least this long
// The end of a move: the reading's slope (hop differences low-passed over kSlopeSeconds) stays
// below kStillCentsPerSecond, or below what the reading's scatter explains, for kStillSeconds.
constexpr double kSlopeSeconds = 0.04;
constexpr double kSlopeNoiseScale = 0.25; ///< sqrt (a / (2 - a)) of that low-pass at 5 ms hops
constexpr double kStillCentsPerSecond = 6.0;
constexpr double kStillSeconds = 0.015;
constexpr double kMaxMoveSeconds = 1.5;

constexpr double kLockMarginCents = 10.0; ///< the note changes only beyond +-(50 + this)
constexpr double kLockDwellSeconds = 0.06;
constexpr double kSlipHoldSeconds = 0.12; ///< a pitch change without a pluck must persist
constexpr double kHeldAfterSeconds = 0.08;

constexpr double kSettledWindowSeconds = 0.15;
constexpr double kSettledSpreadCents = 0.1;
constexpr double kSettledErrorCents = 0.3;

double toCents (double hz) noexcept
{
    return 1200.0 * std::log2 (hz);
}

/// One-pole smoothing coefficient for time constant tau.
double coefficient (double tau, double dt) noexcept
{
    return 1.0 - std::exp (-dt / tau);
}
} // namespace

void TunerDisplay::prepare (double hopSeconds) noexcept
{
    hop_ = hopSeconds > 0.0 ? hopSeconds : 0.005;
    reset();
}

void TunerDisplay::reset() noexcept
{
    out_ = Output {};
    phase_ = Phase::idle;
    haveValue_ = false;
    value_ = 0.0;
    note_ = 0;
    noteAway_ = pendingFor_ = invalidFor_ = 0.0;
    newNote_ = false;
    readingVar_ = preciseVar_ = 1.0;
    moveVar_ = 1.0;
    startSegment();
}

void TunerDisplay::startSegment() noexcept
{
    haveReading_ = havePrecise_ = false;
    preciseFresh_ = true;
    revealing_ = false;
    residual_ = slope_ = 0.0;
    moveWarmup_ = movingFor_ = stillFor_ = 0.0;
    moving_ = awaitingPrecise_ = false;
}

void TunerDisplay::follow (double cents, double tau) noexcept
{
    if (!haveValue_)
    {
        value_ = cents;
        haveValue_ = true;
        return;
    }
    value_ += coefficient (tau, hop_) * (cents - value_);
}

void TunerDisplay::smooth (double cents, double variance, bool moving) noexcept
{
    if (!haveValue_)
    {
        follow (cents, kMinTauSeconds);
        return;
    }
    const double var = std::max (variance, kVarianceFloor);
    const double e = cents - value_;
    const double rest = std::clamp (0.5 * hop_ * var / (kTargetCents * kTargetCents),
                                    kRestMinSeconds, kRestMaxSeconds);
    const double z = e / (kErrorSigmas * std::sqrt (var));
    double tau = rest / (1.0 + z * z);
    if (moving)
        tau = std::min (tau, kMoveTauSeconds);
    value_ += coefficient (std::max (tau, kMinTauSeconds), hop_) * e;
}

double TunerDisplay::midiOf (double cents) const noexcept
{
    return 69.0 + (cents - toCents (referenceHz_)) / 100.0;
}

void TunerDisplay::lockNote (double cents, bool force) noexcept
{
    const double midi = midiOf (cents);
    const bool outside = std::abs (midi - note_) * 100.0 > 50.0 + kLockMarginCents;
    if (force)
    {
        // A new note keeps the note shown when it is within the hysteresis of it.
        if (!out_.valid || outside)
            note_ = static_cast<int> (std::floor (midi + 0.5));
        noteAway_ = 0.0;
        return;
    }
    noteAway_ = outside ? noteAway_ + hop_ : 0.0;
    if (noteAway_ >= kLockDwellSeconds)
    {
        note_ = static_cast<int> (std::floor (midi + 0.5));
        noteAway_ = 0.0;
    }
}

void TunerDisplay::publish() noexcept
{
    out_.valid = haveValue_ && phase_ != Phase::idle;
    out_.hz = haveValue_ ? std::exp2 (value_ / 1200.0) : 0.0;
    out_.note = note_;
    out_.cents = haveValue_ ? value_ - toCents (referenceHz_) - 100.0 * (note_ - 69) : 0.0;
}

bool TunerDisplay::update (const Input& in) noexcept
{
    const double dt = hop_;
    if (in.onset)
    {
        newNote_ = true;
        startSegment();
    }
    else if (in.pitchChange && phase_ != Phase::idle)
    {
        phase_ = Phase::pending;
        pendingFor_ = 0.0;
        startSegment();
    }

    if (!in.readingValid)
    {
        invalidFor_ += dt;
        haveReading_ = false;
        if (invalidFor_ >= kHeldAfterSeconds)
        {
            out_.held = out_.valid;
            out_.settled = false;
        }
        publish();
        return false;
    }
    invalidFor_ = 0.0;
    out_.held = false;

    // Scatter of each estimate.
    const double reading = toCents (in.readingHz);
    if (haveReading_)
    {
        const double d = reading - lastReading_;
        readingVar_ += coefficient (kReadingScatterSeconds, dt) * (0.5 * d * d - readingVar_);
        slope_ += coefficient (kSlopeSeconds, dt) * (d / dt - slope_);
    }
    lastReading_ = reading;
    haveReading_ = true;
    const double precise = in.preciseValid ? toCents (in.preciseHz) : 0.0;
    if (in.preciseValid)
    {
        // A window that just (re)started is trusted only once it proves steady; one that merely
        // missed a hop keeps its record.
        if (preciseFresh_)
            preciseVar_ = kInitialPreciseScale * std::max (readingVar_, kVarianceFloor);
        else if (havePrecise_)
        {
            const double d = precise - lastPrecise_;
            preciseVar_ += coefficient (kPreciseScatterSeconds, dt) * (0.5 * d * d - preciseVar_);
        }
        preciseFresh_ = false;
        havePrecise_ = true;
        lastPrecise_ = precise;
    }

    if (newNote_)
    {
        // The first reading of a pluck: the same note as shown keeps the needle still through the
        // attack (it reads sharp); another note takes over at once.
        newNote_ = false;
        const bool same = out_.valid && haveValue_ &&
                          std::abs (midiOf (reading) - note_) * 100.0 <= 50.0 + kLockMarginCents;
        if (same)
            phase_ = Phase::waiting;
        else
        {
            revealing_ = true;
            revealIn_ = kRevealSeconds;
            candidate_ = reading;
            phase_ = Phase::attack;
        }
    }

    switch (phase_)
    {
    case Phase::idle:
        revealing_ = true;
        revealIn_ = kRevealSeconds;
        candidate_ = reading;
        phase_ = Phase::attack;
        [[fallthrough]];
    case Phase::attack:
        if (revealing_)
        {
            // The pick transient: the first readings of a new note settle out of sight, and
            // the note shown so far stands (held) meanwhile.
            candidate_ += coefficient (kAttackTauSeconds, dt) * (reading - candidate_);
            out_.held = out_.valid;
            if ((revealIn_ -= dt) > 0.0 && !in.preciseValid)
                break;
            revealing_ = false;
            out_.held = false;
            lockNote (candidate_, true);
            value_ = candidate_;
            haveValue_ = true;
        }
        if (in.preciseValid)
            phase_ = Phase::tracking;
        else
            follow (reading, kAttackTauSeconds); // short windows: they converge quickly
        break;
    case Phase::waiting:
        if (in.preciseValid)
            phase_ = Phase::tracking;
        break;
    case Phase::pending:
        if (std::abs (midiOf (reading) - note_) * 100.0 <= 50.0 + kLockMarginCents)
            phase_ = Phase::waiting; // the slip is over: back to the note shown
        else if ((pendingFor_ += dt) >= kSlipHoldSeconds)
        {
            // A different note: no hysteresis with the old one.
            out_.valid = false;
            lockNote (reading, true);
            haveValue_ = false;
            follow (reading, kAttackTauSeconds);
            phase_ = Phase::attack;
        }
        break;
    case Phase::tracking:
        break;
    }

    bool moved = false;
    if (phase_ == Phase::tracking)
    {
        // Moves, against the long window (or the value shown while it restarts).
        const double r = reading - (in.preciseValid ? precise : value_);
        residual_ += coefficient (kResidualSeconds, dt) * (r - residual_);
        const double threshold = std::max (kMoveMinCents, kMoveSigmas * std::sqrt (moveVar_));
        if (moving_)
        {
            // Follow the reading, restarting the long window, until the reading stops moving: its
            // slope within what its own scatter explains.
            movingFor_ += dt;
            const double slopeNoise = kSlopeNoiseScale * std::sqrt (2.0 * readingVar_) / dt;
            const double still = std::max (kStillCentsPerSecond, kMoveSigmas * slopeNoise);
            stillFor_ = std::abs (slope_) < still ? stillFor_ + dt : 0.0;
            if ((movingFor_ >= kMoveHoldSeconds && stillFor_ >= kStillSeconds) ||
                movingFor_ >= kMaxMoveSeconds)
            {
                moving_ = false;
                residual_ = 0.0;
            }
            moved = true;
            awaitingPrecise_ = preciseFresh_ = true;
        }
        else if (moveWarmup_ < kWarmupSeconds)
        {
            moveWarmup_ += dt;
            moveVar_ += coefficient (kWarmupTauSeconds, dt) * (r * r - moveVar_);
        }
        else if (std::abs (residual_) > threshold)
        {
            moving_ = moved = true;
            movingFor_ = stillFor_ = 0.0;
            awaitingPrecise_ = preciseFresh_ = true;
        }
        else if (r * r < moveVar_)
            moveVar_ += coefficient (kMoveVarDownSeconds, dt) * (r * r - moveVar_);
        else
            moveVar_ +=
                coefficient (kMoveVarUpSeconds, dt) * (std::min (r * r, 4.0 * moveVar_) - moveVar_);

        if (!moved && in.preciseValid)
            awaitingPrecise_ = false;
        if (moved || awaitingPrecise_)
            smooth (reading, readingVar_, true);
        else if (in.preciseValid)
        {
            // Inverse-variance blend of the two estimates.
            const double wr = 1.0 / std::max (readingVar_, kVarianceFloor);
            const double wp = 1.0 / std::max (preciseVar_, kVarianceFloor);
            smooth ((wr * reading + wp * precise) / (wr + wp), 1.0 / (wr + wp), false);
        }
        // Otherwise the long window is unavailable (the end of the decay) and the reading has not
        // moved: the value stands.
        lockNote (value_, false);
        out_.settled = in.preciseValid && !moved && !awaitingPrecise_ &&
                       in.preciseSeconds >= kSettledWindowSeconds &&
                       preciseVar_ < kSettledSpreadCents * kSettledSpreadCents &&
                       std::abs (value_ - precise) < kSettledErrorCents;
    }
    else
        out_.settled = false;

    publish();
    return moved;
}

} // namespace sedef::analysis
