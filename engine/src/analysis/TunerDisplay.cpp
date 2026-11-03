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
// The reading's noise without its moves, from second differences (a steady turn cancels out; white
// noise gives 6 sigma^2): a hop counts for at most this many times the noise (3 sigma), and none
// while the pitch moves. A step or a turn then neither makes a long window that starts after it
// look uncertain nor passes for noise when the turn is still under way.
constexpr double kNoiseClip = 9.0;
/// Where that noise estimate starts: (0.1 cent)^2. The clip lets it grow by 40 % a hop, so a noisy
/// reading (hum) is known within 0.1 s; a high start would make a clean string's first long window
/// look uncertain.
constexpr double kNoisePrior = 0.01;

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
// segment, bias included, from hops where the reading is not trending) and at least kMoveMinCents.
// The first hop of the long window seeds that disagreement, so a move is looked for from the
// second hop on.
constexpr double kMoveMinCents = 1.5;
constexpr double kMoveSigmas = 3.0;
// A steady turn is a move as soon as it is clear: the reading's slope beyond kTrendScale times the
// end-of-move level (a real string's wobble of a cent stays below it), one way, for kTrendSeconds,
// and the reading ahead of the long window that way by at least kTrendCents (the long window lags
// a turn by half its length).
constexpr double kTrendScale = 2.0;
constexpr double kTrendSeconds = 0.03;
constexpr double kTrendCents = 0.75;
constexpr double kResidualSeconds = 0.02;
constexpr double kWarmupSeconds = 0.1;
constexpr double kWarmupTauSeconds = 0.04;
constexpr double kMoveVarDownSeconds = 0.4; ///< a reading that gets cleaner (a string decaying)
constexpr double kMoveVarUpSeconds = 3.0;   ///< slowly, so a gradual move is not absorbed
constexpr double kMoveHoldSeconds = 0.06;   ///< a move lasts at least this long
// The end of a move: the reading's slope (hop differences low-passed over kSlopeSeconds) stays
// below kStillCentsPerSecond, or below what the reading's noise explains, for kStillSeconds.
constexpr double kSlopeSeconds = 0.02;
constexpr double kSlopeNoiseScale = 0.35; ///< sqrt (a / (2 - a)) of that low-pass at 5 ms hops
constexpr double kStillCentsPerSecond = 6.0;
constexpr double kStillSeconds = 0.015;
constexpr double kMaxMoveSeconds = 1.5;

constexpr double kLockMarginCents = 10.0; ///< the note changes beyond +-(50 + this)...
constexpr double kLockDwellSeconds = 0.06;
constexpr double kLockEdgeSeconds = 0.25; ///< ...or once closer to another note for this long
constexpr double kSlipHoldSeconds = 0.12; ///< a pitch change without a pluck must persist
constexpr double kHeldAfterSeconds = 0.08;
/// A re-pluck of the note shown keeps the needle still this long (the tracker's attack skip: the
/// string reads sharp until then), then follows the reading until the long window is ready.
constexpr double kWaitSeconds = 0.15;

constexpr double kSettledWindowSeconds = 0.15;
constexpr double kSettledSpreadCents = 0.1;
constexpr double kSettledErrorCents = 0.3;

double toCents (double hz) noexcept
{
    return 1200.0 * std::log2 (hz);
}

bool usable (double hz) noexcept
{
    return hz > 0.0 && std::isfinite (hz);
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
    noteAway_ = edgeAway_ = pendingFor_ = invalidFor_ = sinceOnset_ = 0.0;
    newNote_ = false;
    readingVar_ = preciseVar_ = 1.0;
    readingNoise_ = kNoisePrior;
    startSegment();
}

void TunerDisplay::setReferenceHz (double a4Hz) noexcept
{
    const double ref = usable (a4Hz) ? a4Hz : 440.0;
    if (ref == referenceHz_)
        return;
    referenceHz_ = ref;
    if (haveValue_)
    {
        note_ = nearestNote (value_);
        noteAway_ = edgeAway_ = 0.0;
        publish();
    }
}

void TunerDisplay::startSegment() noexcept
{
    haveReading_ = haveDiff_ = havePrecise_ = false;
    preciseFresh_ = true;
    revealing_ = false;
    residual_ = slope_ = trendFor_ = 0.0;
    trendDirection_ = 0;
    moveVar_ = 1.0;
    moveWarmup_ = movingFor_ = stillFor_ = 0.0;
    moving_ = awaitingPrecise_ = settledSinceMove_ = false;
}

void TunerDisplay::startTracking (double precise) noexcept
{
    phase_ = Phase::tracking;
    nameFrom (precise);
}

void TunerDisplay::nameFrom (double precise) noexcept
{
    // The attack or a move named the note (or the hysteresis kept the last one); the long window
    // knows which one the pitch is closest to.
    if (std::abs (midiOf (precise) - note_) > 0.5)
    {
        note_ = nearestNote (precise);
        noteAway_ = edgeAway_ = 0.0;
    }
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

int TunerDisplay::nearestNote (double cents) const noexcept
{
    return static_cast<int> (std::floor (midiOf (cents) + 0.5));
}

void TunerDisplay::lockNote (double cents, bool force) noexcept
{
    const double away = std::abs (midiOf (cents) - note_) * 100.0;
    const bool outside = away > 50.0 + kLockMarginCents;
    if (force)
    {
        // A new note keeps the note shown when it is within the hysteresis of it.
        if (!out_.valid || outside)
            note_ = nearestNote (cents);
        noteAway_ = edgeAway_ = 0.0;
        return;
    }
    noteAway_ = outside ? noteAway_ + hop_ : 0.0;
    edgeAway_ = away > 50.0 ? edgeAway_ + hop_ : 0.0;
    if (noteAway_ >= kLockDwellSeconds || edgeAway_ >= kLockEdgeSeconds)
    {
        note_ = nearestNote (cents);
        noteAway_ = edgeAway_ = 0.0;
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
    // PitchTracker passes only detections; anything else counts as no reading.
    const bool readingValid = in.readingValid && usable (in.readingHz);
    const bool preciseValid = readingValid && in.preciseValid && usable (in.preciseHz);
    sinceOnset_ += dt;
    if (in.onset)
    {
        newNote_ = true;
        sinceOnset_ = 0.0;
        startSegment();
    }
    else if (in.pitchChange && phase_ != Phase::idle)
    {
        phase_ = Phase::pending;
        pendingFor_ = 0.0;
        startSegment();
    }

    if (!readingValid)
    {
        invalidFor_ += dt;
        haveReading_ = haveDiff_ = false;
        if (invalidFor_ >= kHeldAfterSeconds)
        {
            // The last value stands, as precise as it was.
            out_.held = out_.valid;
            out_.settled = out_.held && settledSinceMove_;
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
        const double a = coefficient (kReadingScatterSeconds, dt);
        readingVar_ += a * (0.5 * d * d - readingVar_);
        if (haveDiff_ && !moving_)
        {
            const double dd = (d - lastDiff_) * (d - lastDiff_) / 6.0;
            readingNoise_ +=
                a * (std::min (dd, kNoiseClip * std::max (readingNoise_, kVarianceFloor)) -
                     readingNoise_);
        }
        lastDiff_ = d;
        haveDiff_ = true;
        slope_ += coefficient (kSlopeSeconds, dt) * (d / dt - slope_);
    }
    lastReading_ = reading;
    haveReading_ = true;
    const double precise = preciseValid ? toCents (in.preciseHz) : 0.0;
    if (preciseValid)
    {
        // A window that just (re)started is trusted only once it proves steady; one that merely
        // missed a hop keeps its record.
        if (preciseFresh_)
            preciseVar_ = kInitialPreciseScale * std::max (readingNoise_, kVarianceFloor);
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
            if ((revealIn_ -= dt) > 0.0 && !preciseValid)
                break;
            revealing_ = false;
            out_.held = false;
            lockNote (candidate_, true);
            value_ = candidate_;
            haveValue_ = true;
        }
        if (preciseValid)
            startTracking (precise);
        else
            follow (reading, kAttackTauSeconds); // short windows: they converge quickly
        break;
    case Phase::waiting:
        if (preciseValid)
            startTracking (precise);
        else if (sinceOnset_ >= kWaitSeconds)
            smooth (reading, readingNoise_, false); // the attack is over; a peg may be turning
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
        const double r = reading - (preciseValid ? precise : value_);
        if (!moving_ && moveWarmup_ == 0.0)
        {
            // The long window's first hop: the two windows' disagreement to begin with (another
            // string ringing pulls the short one) is not a move.
            residual_ = r;
            moveVar_ = r * r + readingNoise_;
        }
        else
            residual_ += coefficient (kResidualSeconds, dt) * (r - residual_);
        const double threshold = std::max (kMoveMinCents, kMoveSigmas * std::sqrt (moveVar_));
        const double slopeNoise = kSlopeNoiseScale * std::sqrt (2.0 * readingNoise_) / dt;
        const double still = std::max (kStillCentsPerSecond, kMoveSigmas * slopeNoise);
        const double trend = kTrendScale * still;
        const int direction = slope_ > trend ? 1 : (slope_ < -trend ? -1 : 0);
        trendFor_ = direction != 0 && direction == trendDirection_ ? trendFor_ + dt : 0.0;
        trendDirection_ = direction;
        const bool trending = trendFor_ >= kTrendSeconds;
        if (moving_)
        {
            // Follow the reading until it stops moving: its slope within what its own scatter
            // explains. The long window restarts while it moves, so it begins where the pitch has
            // come to rest.
            movingFor_ += dt;
            const bool stillNow = std::abs (slope_) < still;
            stillFor_ = stillNow ? stillFor_ + dt : 0.0;
            if ((movingFor_ >= kMoveHoldSeconds && stillFor_ >= kStillSeconds) ||
                movingFor_ >= kMaxMoveSeconds)
            {
                moving_ = false;
                residual_ = 0.0;
            }
            if (!stillNow)
                moved = preciseFresh_ = true;
            awaitingPrecise_ = true;
        }
        else if (std::abs (residual_) > threshold ||
                 (trending && direction * residual_ > kTrendCents))
        {
            moving_ = moved = true;
            movingFor_ = stillFor_ = 0.0;
            awaitingPrecise_ = preciseFresh_ = true;
            settledSinceMove_ = false;
        }
        else if (moveWarmup_ < kWarmupSeconds)
        {
            // Learn the usual disagreement from hops where the reading is not trending (a turn or
            // a step under way is not the usual; a string's wobble is).
            if (std::abs (slope_) < trend)
                moveVar_ += coefficient (kWarmupTauSeconds, dt) * (r * r - moveVar_);
            moveWarmup_ += dt;
        }
        else if (r * r < moveVar_)
            moveVar_ += coefficient (kMoveVarDownSeconds, dt) * (r * r - moveVar_);
        else
            moveVar_ +=
                coefficient (kMoveVarUpSeconds, dt) * (std::min (r * r, 4.0 * moveVar_) - moveVar_);

        if (!moving_ && preciseValid && awaitingPrecise_)
        {
            // The long window is back after a move.
            awaitingPrecise_ = false;
            nameFrom (precise);
        }
        if (moving_ || awaitingPrecise_)
            smooth (reading, readingVar_, true);
        else if (preciseValid)
        {
            // Inverse-variance blend of the two estimates.
            const double wr = 1.0 / std::max (readingVar_, kVarianceFloor);
            const double wp = 1.0 / std::max (preciseVar_, kVarianceFloor);
            smooth ((wr * reading + wp * precise) / (wr + wp), 1.0 / (wr + wp), false);
        }
        // Otherwise the long window is unavailable (the end of the decay) and the reading has not
        // moved: the value stands.
        lockNote (value_, false);
        out_.settled = preciseValid && !moving_ && !awaitingPrecise_ && !trending &&
                       in.preciseSeconds >= kSettledWindowSeconds &&
                       preciseVar_ < kSettledSpreadCents * kSettledSpreadCents &&
                       std::abs (value_ - precise) < kSettledErrorCents;
        settledSinceMove_ = settledSinceMove_ || out_.settled;
    }
    else
        out_.settled = false;

    publish();
    return moved;
}

} // namespace sedef::analysis
