// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/midi/GuitarToMidi.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace sedef::midi
{
namespace
{
// The level is measured in the tracker's band, so it hears what the tracker hears.
constexpr double kBandLowHz = 40.0;
constexpr double kBandHighHz = 1500.0;
constexpr double kLevelSeconds = 0.003;
constexpr float kFloorDb = -120.0f;
constexpr double kDecimatedRate = 7000.0;
// The tracker judges a period on two of its lengths, and nothing inside the first 8 ms of a pick.
constexpr double kTrackerMinSettleSeconds = 0.008;
constexpr double kTrackerMarginSeconds = 0.0005;
// The tracker's latest estimate names the held note: within this many cents, this periodic.
constexpr float kAgreeCents = 50.0f;
constexpr float kAgreeClarity = 0.8f;
constexpr double kAgreeSeconds = 0.002;
// A pick that rises this much over the level before it dominates whatever still rings.
constexpr float kDominantPickDb = 6.0f;

int samples (double seconds, double fs) noexcept
{
    return std::max (1, static_cast<int> (std::lround (seconds * fs)));
}

float powerToDb (float power) noexcept
{
    return power > 1.0e-12f ? 10.0f * std::log10 (power) : kFloorDb;
}

float amplitudeToDb (float amplitude) noexcept
{
    return amplitude > 1.0e-6f ? 20.0f * std::log10 (amplitude) : kFloorDb;
}

float midiPitch (float hz) noexcept
{
    return 69.0f + 12.0f * std::log2 (hz / 440.0f);
}

/// `high` is one of the first six partials of `low` (unison, octave, twelfth, two octaves...): a
/// string tuned to `low` rings in sympathy with `high`.
bool partialOf (int low, int high) noexcept
{
    const double ratio = std::exp2 (static_cast<double> (high - low) / 12.0);
    const double k = std::round (ratio);
    return k >= 1.0 && k <= 6.0 && std::abs (ratio - k) < 0.03 * k;
}

bool isOn (const std::array<std::array<std::uint64_t, 2>, 16>& on, int channel, int note) noexcept
{
    const auto& words = on[static_cast<std::size_t> (channel - 1)];
    return ((words[static_cast<std::size_t> (note >> 6)] >> (note & 63)) & 1u) != 0;
}

void setOn (std::array<std::array<std::uint64_t, 2>, 16>& on, int channel, int note,
            bool value) noexcept
{
    auto& word = on[static_cast<std::size_t> (channel - 1)][static_cast<std::size_t> (note >> 6)];
    const std::uint64_t bit = std::uint64_t { 1 } << (note & 63);
    word = value ? (word | bit) : (word & ~bit);
}

Settings sanitised (Settings s) noexcept
{
    s.octave = std::clamp (s.octave, -2, 1);
    s.bendRange = std::clamp (s.bendRange, 1, 24);
    s.channel = std::clamp (s.channel, 1, 16);
    s.gateDb = std::isfinite (s.gateDb) ? std::clamp (s.gateDb, -96.0f, 0.0f) : Settings {}.gateDb;
    s.dynamics = std::isfinite (s.dynamics) ? std::clamp (s.dynamics, 0.0f, 1.0f) : 0.75f;
    return s;
}
} // namespace

int GuitarToMidi::velocityFor (float level, const Settings& s, float topDb) noexcept
{
    const float db = amplitudeToDb (level);
    const float x = std::clamp ((db - (topDb - kVelocitySpanDb)) / kVelocitySpanDb, 0.0f, 1.0f);
    float y = x;
    if (s.curve == Curve::soft)
        y = std::pow (x, 0.55f);
    else if (s.curve == Curve::hard)
        y = std::pow (x, 1.8f);
    const float full = 1.0f + 126.0f * y;
    const float d = std::clamp (s.dynamics, 0.0f, 1.0f);
    const float v =
        static_cast<float> (kFixedVelocity) + d * (full - static_cast<float> (kFixedVelocity));
    return std::clamp (static_cast<int> (std::lround (v)), 1, 127);
}

int GuitarToMidi::velocityAt (float levelDb) const noexcept
{
    return velocityFor (std::pow (10.0f, levelDb / 20.0f), settings_, velocityTopDb());
}

void GuitarToMidi::rememberPick (float levelDb) noexcept
{
    pickLevels_[static_cast<std::size_t> (pickLevelPos_)] = levelDb;
    pickLevelPos_ = (pickLevelPos_ + 1) % kVelocityPicks;
    pickLevelCount_ = std::min (pickLevelCount_ + 1, kVelocityPicks);
}

float GuitarToMidi::velocityTopDb() const noexcept
{
    // How hard this player picks on this input: the loudest of the latest picks, or the second
    // loudest once there are a few (one accidental hit does not set the scale). Before any pick,
    // the scale is where a guitar at a usual recording level puts it.
    if (pickLevelCount_ == 0)
        return kVelocityTopDb;
    float first = kFloorDb, second = kFloorDb;
    for (int k = 0; k < pickLevelCount_; ++k)
    {
        const float v = pickLevels_[static_cast<std::size_t> (k)];
        if (v > first)
        {
            second = first;
            first = v;
        }
        else if (v > second)
            second = v;
    }
    return (pickLevelCount_ >= 4 ? second : first) + kVelocityHeadroomDb;
}

int GuitarToMidi::steadyLagSamples (double sampleRate) noexcept
{
    return samples (kSteadyLagSeconds, sampleRate);
}

void GuitarToMidi::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;
    tracker_.prepare (sampleRate);
    hp_.c = design::highpass (sampleRate, kBandLowHz, 0.7071);
    lp_.c = design::lowpass (sampleRate, std::min (kBandHighHz, 0.4 * sampleRate), 0.7071);
    powerCoeff_ = static_cast<float> (1.0 - std::exp (-1.0 / (kLevelSeconds * sampleRate)));
    msSamples_ = samples (0.001, sampleRate);
    muteWindowMs_ = std::clamp (static_cast<int> (std::lround (kMuteWindowSeconds * 1000.0)), 2,
                                kHistoryMs - 1);
    confirmSamples_ = samples (kConfirmSeconds, sampleRate);
    pickWindowSamples_ = samples (kPickWindowSeconds, sampleRate);
    repickMarginSamples_ = samples (kRepickMarginSeconds, sampleRate);
    lagSamples_ = steadyLagSamples (sampleRate);
    shortNoteSamples_ = samples (kShortNoteSeconds, sampleRate);
    refineToleranceSamples_ = samples (0.001, sampleRate);
    legatoLeadSamples_ = samples (kLegatoLeadSeconds, sampleRate);
    legatoConfirmSamples_ = samples (kLegatoConfirmSeconds, sampleRate);
    bendIntervalSamples_ = samples (kBendIntervalSeconds, sampleRate);
    clickHp_.c = design::highpass (sampleRate, std::min (2000.0, 0.4 * sampleRate), 0.7071);
    clickCoeff_ = static_cast<float> (1.0 - std::exp (-1.0 / (0.0003 * sampleRate)));
    {
        int size = 1;
        while (size < static_cast<int> (0.032 * sampleRate) + 8)
            size <<= 1;
        clickRing_.assign (static_cast<std::size_t> (size), 0.0f);
        clickMask_ = size - 1;
        while (size < static_cast<int> (0.064 * sampleRate))
            size <<= 1;
        inRing_.assign (static_cast<std::size_t> (size), 0.0f);
        inMask_ = size - 1;
    }
    factor_ = std::max (1, static_cast<int> (sampleRate / kDecimatedRate));
    rate_ = sampleRate / factor_;
    int size = 1;
    while (size < static_cast<int> (2.5 / kLowestHz * rate_) + 8)
        size <<= 1;
    ring_.assign (static_cast<std::size_t> (size), 0.0f);
    ringMask_ = size - 1;
    hop_ = std::max (1, static_cast<int> (std::lround (0.0005 * rate_)));
    minWindow_ = std::max (4, static_cast<int> (std::lround (0.004 * rate_)));
    refractorySamples_ = samples (kPickRefractorySeconds, sampleRate);
    weakRefractorySamples_ = samples (0.06, sampleRate);
    weakCheckSamples_ = samples (0.012, sampleRate);
    lockPickGapSamples_ = samples (0.05, sampleRate);
    correctionConfirmSamples_ = samples (kCorrectionConfirmSeconds, sampleRate);
    refineSamples_ = samples (0.006, sampleRate);
    baseSamples_ = samples (0.02, sampleRate);
    clickDelaySamples_ = samples (0.0002, sampleRate);
    minCheckWindow_ = std::max (4, static_cast<int> (std::lround (kCheckWindowSeconds * rate_)));
    attackSkip_ = static_cast<int> (std::lround (kAttackSkipSeconds * rate_));
    steadySamples_ = samples (0.01, sampleRate);
    breakWindowSamples_ = samples (0.02, sampleRate);
    settings_ = next_;
    settingsChanged_ = false;
    pickLevelCount_ = pickLevelPos_ = 0;
    reset();
}

void GuitarToMidi::reset() noexcept
{
    tracker_.reset();
    hp_.s0 = hp_.s1 = lp_.s0 = lp_.s1 = 0.0f;
    clickHp_.s0 = clickHp_.s1 = 0.0f;
    clickPower_ = 0.0f;
    std::fill (clickRing_.begin(), clickRing_.end(), 0.0f);
    std::fill (inRing_.begin(), inRing_.end(), 0.0f);
    refineHz_ = 0.0f;
    pickRefined_ = true;
    power_ = 0.0f;
    levelDb_ = kFloorDb;
    history_.fill (kFloorDb);
    historyPos_ = 0;
    msCount_ = 0;
    msMax_ = kFloorDb;
    std::fill (ring_.begin(), ring_.end(), 0.0f);
    ringWrite_ = 0;
    phase_ = 0;
    hopCount_ = 0;
    periodicity_ = 0.0f;
    periodicHz_ = 0.0f;
    periodicAt_ = periodicSince_ = 0;
    leastPeriodicity_ = 1.0f;
    lastPickAt_ = previousPickAt_ = 0;
    pickWeak_ = false;
    pickChecking_ = false;
    preHeldDb_ = kFloorDb;
    prevNote_ = lastNote_ = -1;
    lowCheckedAt_ = 0;
    lowCheckedHz_ = 0.0f;
    lowerCached_ = false;
    now_ = 0;
    pickCount_ = 0;
    pickAt_ = 0;
    pickFresh_ = false;
    trackerMoved_ = false;
    pickHz_ = 0.0f;
    preLevelDb_ = kFloorDb;
    agreeSamples_ = 0;
    pitch_ = 0.0f;
    note_ = -1;
    noteHz_ = 0.0f;
    heardHz_ = 0.0f;
    noteLevelDb_ = kFloorDb;
    octaveAt_ = 0;
    octaveHz_ = 0.0f;
    octaveRatio_ = 1.0f;
    candidate_ = -1;
    candidateSamples_ = 0;
    noteDue_ = 0;
    bendSent_ = kBendCentre;
    bendCents_ = 0.0f;
    lastBendAt_ = 0;
    queued_ = 0;
    bridgeNote_ = -1;
    on_ = {};
    channelsUsed_ = 0;
}

void GuitarToMidi::setSettings (const Settings& s) noexcept
{
    const auto clean = sanitised (s);
    const auto& from = settingsChanged_ ? next_ : settings_;
    if (clean.octave != from.octave || clean.mode != from.mode ||
        clean.bendRange != from.bendRange || clean.timing != from.timing ||
        clean.channel != from.channel || clean.gateDb != from.gateDb ||
        clean.dynamics != from.dynamics || clean.curve != from.curve)
    {
        next_ = clean;
        settingsChanged_ = true;
    }
}

int GuitarToMidi::soundingCount() const noexcept
{
    int n = 0;
    for (const auto& words : on_)
        for (auto w : words)
            n += std::popcount (w);
    return n;
}

// -------------------------------------------------------------------------------------------------

void GuitarToMidi::applySettings (EventList& out, int offset) noexcept
{
    settingsChanged_ = false;
    const Settings old = settings_;
    settings_ = next_;
    if (old.timing != settings_.timing)
    {
        // What waits goes out now, in order; the rest follows the new timing.
        for (int k = 0; k < queued_; ++k)
            emit (out, queue_[static_cast<std::size_t> (k)].event.withOffset (offset));
        queued_ = 0;
    }
    if (note_ >= 0 && (old.channel != settings_.channel || old.octave != settings_.octave))
    {
        endNote (now_, Why::other, true);
        emitDue (out, offset);
        pickFresh_ = false;
    }
    if (old.mode == Mode::notesAndBend && settings_.mode == Mode::notes && bendSent_ != kBendCentre)
    {
        schedule (now_, Event::pitchBend (bendChannel_, kBendCentre, 0));
        bendSent_ = kBendCentre;
        emitDue (out, offset);
    }
}

void GuitarToMidi::process (const float* in, int numFrames, EventList& out) noexcept
{
    if (settingsChanged_)
        applySettings (out, 0);

    for (int i = 0; i < numFrames; ++i)
    {
        const float x = in[i];
        ++now_;
        tracker_.push (x);

        const float click = clickHp_.process (x);
        clickPower_ += clickCoeff_ * (click * click - clickPower_);
        clickRing_[static_cast<std::size_t> (now_ & clickMask_)] = clickPower_;
        inRing_[static_cast<std::size_t> (now_ & inMask_)] = x;
        const float band = lp_.process (hp_.process (x));
        power_ += powerCoeff_ * (band * band - power_);
        levelDb_ = powerToDb (power_);
        msMax_ = std::max (msMax_, levelDb_);
        if (++phase_ >= factor_)
        {
            phase_ = 0;
            ring_[static_cast<std::size_t> (ringWrite_)] = band;
            ringWrite_ = (ringWrite_ + 1) & ringMask_;
            if (++hopCount_ >= hop_)
            {
                hopCount_ = 0;
                watchPeriodicity();
            }
        }
        if (++msCount_ >= msSamples_)
        {
            history_[static_cast<std::size_t> (historyPos_)] = msMax_;
            historyPos_ = historyPos_ + 1 < kHistoryMs ? historyPos_ + 1 : 0;
            // The sounding note's attack is the loudest it has been: a pick dated a little early
            // (the finger touching the string first) has not reached it yet.
            if (note_ >= 0)
                noteLevelDb_ = std::max (noteLevelDb_, msMax_);
            msCount_ = 0;
            msMax_ = kFloorDb;
        }

        if (tracker_.pickCount() != pickCount_)
        {
            pickCount_ = tracker_.pickCount();
            registerPick (now_ - std::max (0, tracker_.samplesSincePick()), PickKind::detector);
        }
        const auto ev = tracker_.event();
        if (ev == analysis::MonoTracker::Event::noteOn ||
            ev == analysis::MonoTracker::Event::noteChange)
            trackerMoved_ = true;
        // The tracker locking from silence with the level on the rise is a pick its detector
        // missed (a soft pick under what still rang): it came about one settling time earlier.
        // So is the tracker changing its reading with the level risen from under the gate: it
        // can hold a reading through a short silence.
        const bool fromSilence = ev == analysis::MonoTracker::Event::noteOn ||
                                 (ev == analysis::MonoTracker::Event::noteChange &&
                                  levelAgo (kLockRiseMs) < settings_.gateDb);
        if (fromSilence && now_ - lastPickAt_ > lockPickGapSamples_ &&
            levelDb_ >= levelAgo (kLockRiseMs) + kLockRiseDb)
        {
            const double settle =
                std::max (2.0 / static_cast<double> (tracker_.hz()) + kTrackerMarginSeconds,
                          kTrackerMinSettleSeconds);
            registerPick (now_ - samples (settle, sampleRate_), PickKind::lock);
        }

        const float hz = tracker_.hz();
        const float estimate = tracker_.estimateHz();
        const bool agree = tracker_.voiced() && hz > 0.0f && estimate > 0.0f &&
                           tracker_.clarity() >= kAgreeClarity &&
                           std::abs (1200.0f * std::log2 (estimate / hz)) < kAgreeCents;
        agreeSamples_ = agree ? agreeSamples_ + 1 : 0;

        decide();
        emitDue (out, i);
    }

    hp_.s0 = std::abs (hp_.s0) < 1.0e-20f ? 0.0f : hp_.s0;
    hp_.s1 = std::abs (hp_.s1) < 1.0e-20f ? 0.0f : hp_.s1;
    lp_.s0 = std::abs (lp_.s0) < 1.0e-20f ? 0.0f : lp_.s0;
    lp_.s1 = std::abs (lp_.s1) < 1.0e-20f ? 0.0f : lp_.s1;
    clickHp_.s0 = std::abs (clickHp_.s0) < 1.0e-20f ? 0.0f : clickHp_.s0;
    clickHp_.s1 = std::abs (clickHp_.s1) < 1.0e-20f ? 0.0f : clickHp_.s1;
    clickPower_ = clickPower_ < 1.0e-20f ? 0.0f : clickPower_;
    power_ = power_ < 1.0e-20f ? 0.0f : power_;
}

void GuitarToMidi::registerPick (std::int64_t at, PickKind kind) noexcept
{
    const bool weak = kind == PickKind::weak;
    // The tracker's detector and the periodicity watch can both see one pick: the first counts.
    // A break of periodicity alone (weak) also follows the end of whatever the pick replaced,
    // so it waits longer after any pick.
    // A pick the detector (or a lock) sees while a break of periodicity still waits for its level
    // check replaces it: the break was the previous note's end, a mute just before this pick.
    const bool replacesWeak = pickWeak_ && !weak;
    if (lastPickAt_ > 0 &&
        at - lastPickAt_ < (weak ? weakRefractorySamples_ : refractorySamples_) && !replacesWeak)
        return;
    if (!replacesWeak)
        previousPickAt_ = lastPickAt_;
    lastPickAt_ = at;
    pickKind_ = kind;
    pickAt_ = at;
    pickRefined_ = false;
    refineHz_ = weak ? periodicHz_ : 0.0f;
    pickWeak_ = weak;
    pickWeakSource_ = weak;
    leastPeriodicity_ = weak ? periodicity_ : 1.0f;
    prevNote_ = note_ >= 0 ? note_ : lastNote_;
    pickFresh_ = true;
    trackerMoved_ = false;
    pickHz_ = tracker_.voiced() ? tracker_.hz() : 0.0f;
    // The level just before the rise: the quietest of the three milliseconds before it.
    const int ago = static_cast<int> (std::min<std::int64_t> (now_ - at, 1 << 20));
    const int msAgo = std::clamp (ago / std::max (1, msSamples_) + 1, 1, kHistoryMs - 4);
    float before = levelAgo (msAgo);
    for (int k = msAgo + 1; k < msAgo + 3; ++k)
        before = std::min (before, levelAgo (k));
    preLevelDb_ = before;
    float held = kFloorDb;
    for (int k = msAgo; k < std::min (msAgo + 16, kHistoryMs); ++k)
        held = std::max (held, levelAgo (k));
    preHeldDb_ = held;
    // On a string faded far under the last note's attack (sounding, or just ended by the
    // fretting hand moving on), a pick must show it brought energy before it counts: the move
    // breaks the period, or taps the fret, too.
    pickChecking_ = weak || preHeldDb_ < noteLevelDb_ - kFadedDropDb;
}

float GuitarToMidi::periodicityAt (float hz, int window) const noexcept
{
    const double period = rate_ / static_cast<double> (hz);
    const int lag = static_cast<int> (period);
    const auto frac = static_cast<float> (period - lag);
    const int n = std::max ({ static_cast<int> (std::ceil (period)), minWindow_, window });
    if (lag < 1 || n + lag + 2 > ringMask_)
        return 1.0f;
    double r = 0.0, m = 0.0;
    for (int k = 0; k < n; ++k)
    {
        const float x = ringAt (k);
        const float y = (1.0f - frac) * ringAt (k + lag) + frac * ringAt (k + lag + 1);
        r += static_cast<double> (x) * y;
        m += static_cast<double> (x) * x + static_cast<double> (y) * y;
    }
    return m > 1.0e-14 ? static_cast<float> (2.0 * r / m) : 0.0f;
}

void GuitarToMidi::watchPeriodicity() noexcept
{
    // While a note sounds, how periodic the band is at the note's own period, over one period (at
    // least 4 ms): 1 for a string that keeps vibrating. A pick on it starts a new waveform, which
    // breaks that at once, even when the new pick is softer than what it replaces and the
    // tracker's level-based detector stays silent. The period is the one the note had while it
    // was clearly steady, so a pick that bends the tracker's reading for a moment cannot hide
    // itself; a steady note moves it along (vibrato, bends).
    if (note_ < 0 || !tracker_.voiced() || tracker_.hz() <= 0.0f)
    {
        periodicity_ = 0.0f;
        periodicSince_ = 0;
        periodicHz_ = 0.0f;
        return;
    }
    const float hz = tracker_.hz();
    if (periodicHz_ <= 0.0f || periodicityAt (hz) >= kPeriodicAnchor)
        periodicHz_ = hz;
    periodicity_ = periodicityAt (periodicHz_);
    leastPeriodicity_ = std::min (leastPeriodicity_, periodicity_);
    if (periodicity_ >= kPeriodicHigh)
    {
        periodicAt_ = now_;
        if (periodicSince_ == 0)
            periodicSince_ = now_;
        return;
    }
    const bool steady = periodicSince_ > 0 && periodicAt_ - periodicSince_ >= steadySamples_;
    if (periodicity_ < kPeriodicBreak && steady && now_ - periodicAt_ < breakWindowSamples_ &&
        levelDb_ >= settings_.gateDb)
        registerPick (periodicAt_, PickKind::weak);
    if (periodicity_ < kPeriodicBreak)
        periodicSince_ = 0;
}

bool GuitarToMidi::refineByPeriod() noexcept
{
    // A pick seen as a break of periodicity: the first sample where the input stops repeating
    // the period it had (its difference from one period earlier rises 9 dB over that
    // difference's level before).
    const double period = sampleRate_ / static_cast<double> (refineHz_);
    const auto whole = static_cast<std::int64_t> (period);
    const auto frac = static_cast<float> (period - static_cast<double> (whole));
    const std::int64_t from =
        std::max (pickAt_ - refineSamples_, now_ - inMask_ + whole + 2 + baseSamples_);
    const std::int64_t to = std::min (now_, pickAt_ + refineSamples_);
    if (to <= from)
        return false;
    const auto at = [this] (std::int64_t k) {
        return inRing_[static_cast<std::size_t> (k & inMask_)];
    };
    float power = 0.0f, base = 0.0f;
    int count = 0;
    const std::int64_t warm = from - baseSamples_;
    for (std::int64_t k = warm; k <= to; ++k)
    {
        const float past = at (k - whole) + frac * (at (k - whole - 1) - at (k - whole));
        const float r = at (k) - past;
        power += clickCoeff_ * (r * r - power);
        if (k < from)
        {
            if (k >= warm + baseSamples_ / 4)
            {
                base += power;
                ++count;
            }
            continue;
        }
        if (count > 0 && power > 8.0f * std::max (base / static_cast<float> (count), 1.0e-12f))
        {
            pickAt_ = std::max (from, k - clickDelaySamples_);
            lastPickAt_ = pickAt_;
            return true;
        }
    }
    return false;
}

void GuitarToMidi::refinePick() noexcept
{
    if (refineHz_ > 0.0f && refineByPeriod())
    {
        pickRefined_ = true;
        return;
    }
    // The detectors date a pick to a few milliseconds. The pick's click (the input above 2 kHz,
    // over 0.3 ms) dates it to a fraction of one: the first sample within 6 ms of the estimate
    // where the click's power stands 9 dB over its level before. Steady's constant latency
    // depends on it.
    pickRefined_ = true;
    const auto span = static_cast<std::int64_t> (clickMask_);
    const std::int64_t from = std::max (pickAt_ - refineSamples_, now_ - span + baseSamples_ + 1);
    double base = 0.0;
    int count = 0;
    for (std::int64_t k = from - baseSamples_; k < from - refineSamples_ / 3; ++k, ++count)
        base += clickRing_[static_cast<std::size_t> (k & clickMask_)];
    if (count <= 0)
        return;
    const auto threshold = static_cast<float> (std::max (8.0 * base / count, 1.0e-10));
    const std::int64_t to = std::min (now_, pickAt_ + refineSamples_);
    for (std::int64_t k = from; k <= to; ++k)
        if (clickRing_[static_cast<std::size_t> (k & clickMask_)] > threshold)
        {
            // Back to where it started to rise (the smoothing's delay).
            pickAt_ = std::max (from, k - clickDelaySamples_);
            lastPickAt_ = pickAt_;
            return;
        }
}

float GuitarToMidi::pickLevelDb() const noexcept
{
    // The pick's level: the loudest millisecond from 3 to 12 ms after it (so neither its click
    // nor what it replaced counts), or as much of that as has passed.
    const int ago = static_cast<int> (
        std::min<std::int64_t> ((now_ - pickAt_) / std::max (1, msSamples_), kHistoryMs - 1));
    float peak = kFloorDb;
    for (int k = std::max (0, ago - 12); k <= std::max (0, ago - 3); ++k)
        peak = std::max (peak, levelAgo (k));
    return ago < 3 ? levelDb_ : peak;
}

float GuitarToMidi::attackLevelDb() const noexcept
{
    // A note's attack, when it starts: the loudest millisecond from 3 ms after its pick until now.
    // A pick dated early (a finger touching the string before it plucks) has not reached the
    // attack 12 ms on.
    const int ago = static_cast<int> (
        std::min<std::int64_t> ((now_ - pickAt_) / std::max (1, msSamples_), kHistoryMs - 1));
    float peak = levelDb_;
    for (int k = 0; k <= ago - 3; ++k)
        peak = std::max (peak, levelAgo (k));
    return std::max (peak, pickLevelDb());
}

float GuitarToMidi::levelAgo (int ms) const noexcept
{
    const int k = ((historyPos_ - 1 - ms) % kHistoryMs + kHistoryMs) % kHistoryMs;
    return history_[static_cast<std::size_t> (k)];
}

bool GuitarToMidi::muted() const noexcept
{
    // Against the note's own peak, from 3 ms after its pick: a louder string damped as this note
    // was picked is not this note being muted.
    const auto since = now_ - (note_ >= 0 ? noteStart_ : pickAt_);
    const int ms = static_cast<int> (std::min<std::int64_t> (
        since / std::max (1, msSamples_) - 3, static_cast<std::int64_t> (muteWindowMs_)));
    float peak = levelDb_;
    for (int k = 0; k < ms; ++k)
        peak = std::max (peak, levelAgo (k));
    return levelDb_ < peak - kMuteDropDb;
}

bool GuitarToMidi::pitchSettled() const noexcept
{
    if (!pickFresh_ || trackerMoved_)
        return true;
    // The tracker held a note when the pick came and has not moved since: it is the same note
    // once the tracker had time to name another (two of its periods), and its estimate keeps
    // naming the held one. If the pick did not drown what still rang, a lower note could still be
    // on its way: wait for the lowest one.
    if (pickHz_ <= 0.0f || !tracker_.voiced())
        return false;
    const auto since = static_cast<double> (now_ - pickAt_) / sampleRate_;
    const double own = std::max (2.0 / static_cast<double> (tracker_.hz()) + kTrackerMarginSeconds,
                                 kTrackerMinSettleSeconds) +
                       kRepickMarginSeconds;
    // The lowest note a pick may bring (drop D's open string, with room) needs two of its
    // periods before the tracker can name it.
    const double lowest = 2.0 / kLowestHz + kTrackerMarginSeconds + kRepickMarginSeconds;
    const bool dominant = pickLevelDb() >= preLevelDb_ + kDominantPickDb;
    const bool agreed = agreeSamples_ >= samples (kAgreeSeconds, sampleRate_);
    return agreed && since >= (dominant ? own : lowest);
}

float GuitarToMidi::recentPeakDb() const noexcept
{
    float peak = levelDb_;
    for (int k = 0; k < muteWindowMs_; ++k)
        peak = std::max (peak, levelAgo (k));
    return peak;
}

float GuitarToMidi::shortPeriodicity (int lag, int window) const noexcept
{
    double r = 0.0, m = 0.0;
    for (int k = 0; k < window; ++k)
    {
        const double x = ringAt (k), y = ringAt (k + lag);
        r += x * y;
        m += x * x + y * y;
    }
    return m > 1.0e-14 ? static_cast<float> (2.0 * r / m) : 0.0f;
}

bool GuitarToMidi::lowerNotePossible (float hz) const noexcept
{
    // In the first periods of a low note the tracker can name one of its partials, whose short
    // period settles first. A note heard soon after a pick is checked against every longer period
    // the signal since the pick can already show (at least 2 ms of it): if one repeats clearly
    // better, a lower note is on its way. The lowest open string's period must fit before
    // anything above its octave counts.
    const double since = static_cast<double> (now_ - pickAt_) / sampleRate_;
    if (since >= 2.0 / kLowestHz + kTrackerMarginSeconds + kRepickMarginSeconds)
        return false; // the tracker would have named it by now
    // The attack's first milliseconds (the pick's click, what it replaced) repeat at no period.
    const int available = static_cast<int> ((now_ - pickAt_) / factor_) - attackSkip_;
    const int window = minCheckWindow_;
    const auto candidateLag = static_cast<int> (std::lround (rate_ / static_cast<double> (hz)));
    if (static_cast<double> (hz) >= 1.9 * kLowestOpenHz &&
        available < static_cast<int> (rate_ / kLowestOpenHz) + window)
        return true; // too early to tell
    if (now_ - lowCheckedAt_ < hop_ * factor_ && lowCheckedHz_ == hz)
        return lowerCached_;
    lowCheckedAt_ = now_;
    lowCheckedHz_ = hz;
    lowerCached_ = false;
    const int longest =
        std::min ({ available - window, static_cast<int> (rate_ / kLowestHz) + 1, ringMask_ / 2 });
    if (candidateLag < 1 || longest <= candidateLag + 1)
        return false;
    const int w0 = std::min (std::max (candidateLag, window), available - candidateLag);
    const float own = w0 >= window ? shortPeriodicity (candidateLag, w0) : 0.0f;
    for (int lag = candidateLag + std::max (2, candidateLag / 4); lag <= longest; ++lag)
    {
        const int w = std::min (lag, available - lag);
        if (w < window)
            break;
        const float p = shortPeriodicity (lag, w);
        if (own < kOwnClean && p >= kLongerPeriodic && p > own + kLongerMargin)
        {
            lowerCached_ = true;
            break;
        }
    }
    return lowerCached_;
}

bool GuitarToMidi::harmonicSlip (float hz, bool correction) const noexcept
{
    // The tracker naming a whole multiple of the sounding note's period (an octave, a twelfth
    // or two octaves down) while the input repeats at the sounding note's own period as well is
    // the estimator's classic slip, not a new note: a lower note really played breaks the
    // sounding note's period. The other way (a fraction of the period), only a correction must
    // repeat as well as the note it corrects; a new pick an octave up over a ringing string is
    // as often a new note as not.
    if (noteHz_ <= 0.0f || hz <= 0.0f)
        return false;
    const float ratio = noteHz_ / hz;
    const float r = ratio >= 1.0f ? ratio : 1.0f / ratio;
    const float k = std::round (r);
    if (k < 2.0f || std::abs (r - k) > kHarmonicTolerance * k)
        return false;
    // Both over the same stretch of signal: the longer period.
    const int window =
        static_cast<int> (std::ceil (rate_ / static_cast<double> (std::min (hz, noteHz_))));
    const float own = periodicityAt (noteHz_, window);
    const float other = periodicityAt (hz, window);
    if (ratio > 1.0f)
        return !(own < kOwnClean && other > own + (correction ? kCorrectionMargin : kLongerMargin));
    // Up: unless the input repeats cleanly at the shorter period too (octaveChecked()).
    return correction && other < own - kLongerMargin && other < kOctaveUpClean;
}

std::int64_t GuitarToMidi::fundamentalOnset (float hz, std::int64_t around) noexcept
{
    // Where the input's component at hz starts: its amplitude over one of its periods (the input
    // turned to baseband at hz, summed over a period: the other partials cancel), from 30 ms
    // before `around` to now. The onset is where that amplitude first passes half way from its
    // level before to its highest since, less half a period (the sum's own rise). It needs the
    // note to rise 12 dB out of what was there: a pick over silence or over another note.
    const auto period = static_cast<int> (std::lround (sampleRate_ / static_cast<double> (hz)));
    if (period < 8 || period > kDemodSize)
        return -1;
    const std::int64_t from =
        std::max (around - samples (0.03, sampleRate_), now_ - inMask_ + period + 2);
    if (now_ - from < 3 * period)
        return -1;
    const double w = -kTwoPiD * static_cast<double> (hz) / sampleRate_;
    const double rotRe = std::cos (w), rotIm = std::sin (w);
    double phRe = 1.0, phIm = 0.0;
    double sumRe = 0.0, sumIm = 0.0;
    double ampBase = 1.0e30, ampPeak = 0.0;
    std::int64_t peakAt = from;
    const auto count = static_cast<int> (
        std::min<std::int64_t> (now_ - from + 1, static_cast<std::int64_t> (demodAmp_.size())));
    for (int i = 0; i < count; ++i)
    {
        const std::int64_t k = from + i;
        const double x = inRing_[static_cast<std::size_t> (k & inMask_)];
        const auto re = static_cast<float> (x * phRe);
        const auto im = static_cast<float> (x * phIm);
        const double nextRe = phRe * rotRe - phIm * rotIm;
        phIm = phRe * rotIm + phIm * rotRe;
        phRe = nextRe;
        const auto slot = static_cast<std::size_t> (i % period);
        if (i >= period)
        {
            sumRe -= demodRe_[slot];
            sumIm -= demodIm_[slot];
        }
        demodRe_[slot] = re;
        demodIm_[slot] = im;
        sumRe += re;
        sumIm += im;
        const double amp = std::sqrt (sumRe * sumRe + sumIm * sumIm);
        demodAmp_[static_cast<std::size_t> (i)] = static_cast<float> (amp);
        if (i >= period && amp > ampPeak)
        {
            ampPeak = amp;
            peakAt = k;
        }
    }
    for (int i = period; i < count && from + i <= peakAt; ++i)
        ampBase = std::min (ampBase, static_cast<double> (demodAmp_[static_cast<std::size_t> (i)]));
    if (ampPeak < 4.0 * ampBase || ampPeak <= 0.0)
        return -1;
    const double half = ampBase + 0.5 * (ampPeak - ampBase);
    // The last time it sat near its level before, then the first crossing after it.
    int start = period;
    for (int i = period; i < count && from + i <= peakAt; ++i)
        if (demodAmp_[static_cast<std::size_t> (i)] <= ampBase + 0.1 * (ampPeak - ampBase))
            start = i;
    for (int i = start; i < count && from + i <= peakAt; ++i)
        if (demodAmp_[static_cast<std::size_t> (i)] >= half)
            return from + i - period / 2;
    return -1;
}

std::int64_t GuitarToMidi::clickNear (std::int64_t from, std::int64_t to) const noexcept
{
    // The first pick click (the input above 2 kHz rising 9 dB over its level in the 6 ms before)
    // between two times, dated as refinePick() dates it; -1 when there is none or it is out of
    // reach.
    const auto reach = static_cast<std::int64_t> (clickMask_);
    const auto baseSpan = static_cast<std::int64_t> (refineSamples_);
    if (from - baseSpan <= now_ - reach)
        return -1;
    double base = 0.0;
    for (std::int64_t k = from - baseSpan; k < from; ++k)
        base += clickRing_[static_cast<std::size_t> (k & clickMask_)];
    const auto threshold =
        static_cast<float> (std::max (8.0 * base / static_cast<double> (baseSpan), 1.0e-10));
    for (std::int64_t k = from; k <= std::min (now_, to); ++k)
        if (clickRing_[static_cast<std::size_t> (k & clickMask_)] > threshold)
            return std::max (from, k - clickDelaySamples_);
    return -1;
}

float GuitarToMidi::octaveChecked (float hz) noexcept
{
    // Once per hop of the decimated analysis, or when the reading moves.
    if (now_ - octaveAt_ < hop_ * factor_ && std::abs (hz - octaveHz_) <= 0.005f * hz)
        return octaveHz_ > 0.0f ? hz * octaveRatio_ : hz;
    octaveAt_ = now_;
    octaveHz_ = hz;
    // Over two of the longer periods: steadier than one, so the answer does not flicker.
    const int window = 2 * static_cast<int> (std::ceil (rate_ / static_cast<double> (hz)));
    float up = hz;
    while (static_cast<double> (2.0f * up) <= rate_ / 8.0 &&
           periodicityAt (2.0f * up, window) >= kOctaveUpClean)
        up *= 2.0f;
    octaveRatio_ = up / hz;
    return up;
}

int GuitarToMidi::quantise (float p) const noexcept
{
    return std::clamp (static_cast<int> (std::lround (p)), 0, 127);
}

bool GuitarToMidi::pickedNoteReady (float hz) const noexcept
{
    // A note heard after a pick counts once the tracker could have judged it on two of its own
    // periods since the pick (when the tracker missed the pick, its estimate still straddles it),
    // it is not under the lowest note a guitar plays, and no lower note can still be coming.
    if (hz < kMinNoteHz || periodicityAt (hz) < kPickedClarity)
        return false;
    const double since = static_cast<double> (now_ - pickAt_) / sampleRate_;
    if (since <
        std::max (2.0 / static_cast<double> (hz) + kTrackerMarginSeconds, kTrackerMinSettleSeconds))
        return false;
    return !lowerNotePossible (hz);
}

void GuitarToMidi::decide() noexcept
{
    const float hz = tracker_.hz();
    const bool voiced = tracker_.voiced() && hz > 0.0f;
    pitch_ = voiced ? midiPitch (hz) + 12.0f * static_cast<float> (settings_.octave) : 0.0f;
    if (!pickRefined_ && now_ - pickAt_ >= refineSamples_)
        refinePick();
    // A pick seen only as a break of periodicity must bring a level: when the played note is
    // damped while another string rings on, the waveform changes too, but the level falls. On a
    // string faded far under the sounding note's attack, any pick must bring energy: the
    // fretting hand moving on to the next fret (or off it) breaks the period, and can tap the
    // fret, but leaves the level where it was.
    if (pickChecking_ && now_ - pickAt_ >= weakCheckSamples_)
    {
        pickChecking_ = false;
        const float level = pickLevelDb();
        const bool fretting =
            level < noteLevelDb_ - kFadedDropDb && level < preHeldDb_ + kLockRiseDb;
        const bool damped = pickWeak_ && level < preLevelDb_ - kWeakPickDropDb;
        pickWeak_ = false;
        if (damped || fretting)
        {
            pickFresh_ = false;
            lastPickAt_ = pickAt_ = previousPickAt_;
        }
    }
    const auto sincePick = now_ - pickAt_;
    const bool recentPick = pickFresh_ && sincePick <= pickWindowSamples_;
    if (pickFresh_ && !recentPick)
        pickFresh_ = false;
    // A change soon after a pick is judged as picked even once that pick has started a note: the
    // tracker may still correct it, and the correction must not count as legato.
    const bool pickedChange = lastPickAt_ > 0 && sincePick <= pickWindowSamples_;
    // After a pick, a string ringing an octave under the played note (left ringing, or in
    // sympathy) makes the input repeat at its longer period too, and the tracker may name it.
    // When the input repeats cleanly at half that period as well, the played note is the octave
    // above: a string's own waveform never does (its odd partials break the half period).
    const float heard = voiced && pickedChange ? octaveChecked (hz) : hz;
    heardHz_ = voiced ? heard : 0.0f;
    const float heardPitch = voiced ? pitch_ + 12.0f * std::log2 (heard / hz) : 0.0f;
    const int q = voiced ? quantise (heardPitch) : -1;

    if (note_ < 0)
    {
        // From silence a note needs a pick behind it (its level checked, when it may be the
        // fretting hand), a level over the gate and a settled pitch. A mute breaks the period
        // too: when the level has fallen far under the note before a pick seen only that way,
        // and what is heard now is a string that rings in sympathy with that note (it shares
        // its partials), the break was the mute and the string only rang on under it.
        const bool sympathy = pickWeakSource_ && prevNote_ >= 0 && q >= 0 &&
                              levelDb_ < preLevelDb_ - kWeakPickDropDb && partialOf (q, prevNote_);
        if (!voiced || !recentPick || pickChecking_ || levelDb_ < settings_.gateDb || muted() ||
            sympathy || !pitchSettled())
        {
            candidate_ = -1;
            return;
        }
        candidateSamples_ = q == candidate_ ? candidateSamples_ + 1 : 1;
        candidate_ = q;
        if (candidateSamples_ < confirmSamples_ || !pickedNoteReady (heard))
            return;
        // Once there is a pick to go by, a pick far under how this player picks (under the
        // bottom of the velocity scale) is not a note: a finger touching a string, the hand on
        // the guitar. It still counts towards how this player picks, so a passage played that
        // much softer comes through after a while. Before the first pick only the gate stands:
        // nothing says how loud this input is yet.
        const float level = attackLevelDb();
        if (pickLevelCount_ > 0 && level < velocityTopDb() - kVelocitySpanDb)
        {
            rememberPick (level);
            pickFresh_ = false;
            candidate_ = -1;
            return;
        }
        startNote (q, velocityAt (level), pickAt_, Why::pick);
        pickFresh_ = false;
        candidate_ = -1;
        return;
    }

    // A sounding note ends when the tracker hears nothing, the level falls under the gate or
    // kDecayOffDb under the note's attack (it is no longer heard, however loud the input), or it
    // drops faster than any string decays (a mute).
    const bool underGate = levelDb_ < settings_.gateDb - kOffHysteresisDb;
    const bool decayed = levelDb_ < noteLevelDb_ - kDecayOffDb;
    if (!voiced || underGate || decayed || muted())
    {
        endNote (now_, !voiced     ? Why::unvoiced
                       : underGate ? Why::gate
                       : decayed   ? Why::decayed
                                   : Why::mute);
        candidate_ = -1;
        return;
    }

    if (q == note_)
        noteHz_ = heard; // follows vibrato and bends, for the slip check
    const float away = std::abs (heardPitch - static_cast<float> (note_));
    if (q != note_ && away > 0.5f + kHysteresisSemitones)
    {
        // A legato candidate must sit on its semitone, not glide through it: the tracker follows a
        // whole-step hammer-on through the semitone between within a couple of milliseconds.
        const bool clean = std::abs (heardPitch - static_cast<float> (q)) < kLegatoCentre &&
                           periodicity_ >= kLegatoPeriodic;
        if (q != candidate_)
        {
            candidate_ = q;
            candidateSamples_ = 0;
        }
        if (pickedChange || clean)
            ++candidateSamples_;
        if (pickedChange)
        {
            // A correction of the note this pick started must hold longer than a new note: the
            // strings still ringing under a new note can pull the tracker around for a moment. A
            // pick whose level is still to be checked starts nothing yet.
            if (candidateSamples_ < (pickFresh_ ? confirmSamples_ : correctionConfirmSamples_) ||
                pickChecking_ || !pickedNoteReady (heard))
                return;
            // A second change after one pick corrects the tracker's first reading, but never back
            // to the note before the pick, whose string may simply still ring.
            if ((!pickFresh_ && q == prevNote_) || harmonicSlip (heard, !pickFresh_))
            {
                candidate_ = -1;
                return;
            }
            const int velocity = velocityAt (pickLevelDb());
            const bool wasFresh = pickFresh_;
            endNote (pickAt_, Why::replaced);
            startNote (q, velocity, pickAt_, wasFresh ? Why::pick : Why::correction);
            pickFresh_ = false;
            candidate_ = -1;
            return;
        }
        if (candidateSamples_ < (std::abs (q - note_) == 1 ? 4 : 1) * legatoConfirmSamples_)
            return;
        // No pick. A level that fell means the note was damped and another string rings on: the
        // note ends. So does a string faded far under the note's attack: the fretting hand moved
        // on before the next pick. A leap no fretting hand makes without picking (an octave flip
        // of the tracker on a note and its partial) is ignored.
        const float recent = recentPeakDb();
        if (levelDb_ < recent - kLegatoDropDb || recent < noteLevelDb_ - kFadedDropDb)
        {
            endNote (now_, Why::damped);
            candidate_ = -1;
            return;
        }
        if (std::abs (q - note_) > kMaxLegatoSemitones)
        {
            candidate_ = -1;
            return;
        }
        const auto reference = now_ - legatoLeadSamples_;
        endNote (reference, Why::replaced);
        startNote (q, velocityAt (levelDb_), reference, Why::legato);
        candidate_ = -1;
        return;
    }
    candidate_ = -1;

    // The same note picked again: a new note once the tracker has had time to hear another, and
    // only if the pick shows in the signal as a new waveform (the note's periodicity broke under
    // kRetriggerPeriodic). A rise of the level is no proof: pick noise, a pick scraping a damped
    // string or strings beating raise it too, and leave the note's waveform as it was.
    const bool attacked = leastPeriodicity_ < kRetriggerPeriodic;
    // And only once the input repeats at the note's own period again: a new note picked on
    // another string, with this one still ringing under it, leaves the tracker on the old note
    // for a moment, but the input then repeats at the new note's period, not the old one's.
    if (recentPick && !pickChecking_ && q == note_ && pitchSettled() && attacked &&
        periodicityAt (heard) >= kPeriodicHigh)
    {
        const int velocity = velocityAt (pickLevelDb());
        endNote (pickAt_, Why::replaced);
        startNote (q, velocity, pickAt_, Why::repick);
        pickFresh_ = false;
        return;
    }

    if (settings_.mode == Mode::notesAndBend)
    {
        const float cents = 100.0f * (pitch_ - static_cast<float> (note_));
        if (now_ - lastBendAt_ >= bendIntervalSamples_ &&
            std::abs (cents - bendCents_) >= kBendDeadbandCents)
            sendBend (pitch_ - static_cast<float> (note_), now_);
    }
}

void GuitarToMidi::startNote (int note, int velocity, std::int64_t reference, Why why) noexcept
{
    startWhy_ = why;
    noteStart_ = reference;
    // A legato note keeps the energy the pick gave the string (or what it added to it).
    noteLevelDb_ = why == Why::legato ? std::max (noteLevelDb_, recentPeakDb())
                   : why == Why::pick ? attackLevelDb()
                                      : pickLevelDb();
    // A pick that starts a note tells how hard this player picks (a correction reuses its pick).
    if (why == Why::pick || why == Why::repick)
        rememberPick (noteLevelDb_);
    // Steady dates a picked note from the signal once more, now that its pitch is known: where its
    // fundamental rises out of what was there (and the click there, if there is one). A pick
    // dated to the previous note's mute, or a soft pick dated by a break of periodicity, moves to
    // where the new note starts.
    if (settings_.timing == Timing::steady && why == Why::pick && heardHz_ > 0.0f)
    {
        // The onset only says where to look; the click there dates the pick.
        const auto onset = fundamentalOnset (heardHz_, reference);
        const auto click = onset > 0 ? clickNear (onset - 8 * refineToleranceSamples_,
                                                  onset + 3 * refineToleranceSamples_)
                                     : -1;
        if (click > 0 && std::abs (click - reference) > refineToleranceSamples_)
        {
            reference = click;
            noteStart_ = reference;
        }
    }
    std::int64_t due = now_;
    if (settings_.timing == Timing::steady)
    {
        due = reference + lagSamples_;
        if (due < now_)
        {
            due = now_;
            ++lateNotes_;
        }
    }
    // Steady: the note a dropped glitch interrupted comes back while its note-off still waits.
    // It never ended at the receiver: take the note-off back and let it sound on.
    if (bridgeNote_ == note && bridgeChannel_ == settings_.channel)
    {
        const int off = findQueued (false, bridgeChannel_, note);
        bridgeNote_ = -1;
        if (off >= 0 && queue_[static_cast<std::size_t> (off)].due >= now_)
        {
            unqueue (off);
            note_ = note;
            noteHz_ = heardHz_;
            periodicHz_ = 0.0f;
            noteChannel_ = settings_.channel;
            noteDue_ = now_;
            if (settings_.mode == Mode::notesAndBend)
                sendBend (pitch_ - static_cast<float> (note), now_);
            return;
        }
    }
    bridgeNote_ = -1;
    // Steady dates the note-on from its pick, which can come before what ended the note before
    // it was decided (a mute, the fretting hand): that note-off goes no later than this note-on,
    // so one note sounds at a time.
    if (settings_.timing == Timing::steady)
        for (int k = queued_ - 1; k >= 0; --k)
        {
            const auto p = queue_[static_cast<std::size_t> (k)];
            if (p.event.isNoteOff() && p.due > due &&
                findQueued (true, p.event.channel(), p.event.note()) < 0)
            {
                unqueue (k);
                schedule (due, p.event);
            }
        }
    note_ = note;
    noteHz_ = heardHz_;
    periodicHz_ = 0.0f;
    noteChannel_ = settings_.channel;
    noteDue_ = due;
    lastVelocity_ = velocity;
    if (settings_.mode == Mode::notesAndBend)
        sendBend (pitch_ - static_cast<float> (note), due);
    schedule (due, Event::noteOn (noteChannel_, note, velocity, 0));
    ++notesSent_;
}

int GuitarToMidi::findQueued (bool on, int channel, int note) const noexcept
{
    for (int k = queued_ - 1; k >= 0; --k)
    {
        const auto& e = queue_[static_cast<std::size_t> (k)].event;
        if ((on ? e.isNoteOn() : e.isNoteOff()) && e.channel() == channel && e.note() == note)
            return k;
    }
    return -1;
}

void GuitarToMidi::unqueue (int index) noexcept
{
    for (int k = index; k + 1 < queued_; ++k)
        queue_[static_cast<std::size_t> (k)] = queue_[static_cast<std::size_t> (k + 1)];
    --queued_;
}

void GuitarToMidi::endNote (std::int64_t reference, Why why, bool immediately) noexcept
{
    if (note_ < 0)
        return;
    endWhy_ = why;
    lastNote_ = note_;
    std::int64_t due = now_;
    if (settings_.timing == Timing::steady && !immediately)
        due = std::max (reference + lagSamples_, now_);
    // Never before its own note-on.
    due = std::max (due, noteDue_);
    // Steady sees a glitch coming: a note whose note-on still waits and that would sound
    // shorter than kShortNoteSeconds is taken back, note-on and all. When it interrupted a note
    // whose note-off still waits too, that note may come back (startNote()).
    if (settings_.timing == Timing::steady && !immediately && noteDue_ >= now_ &&
        due - noteDue_ < shortNoteSamples_)
    {
        const int on = findQueued (true, noteChannel_, note_);
        if (on >= 0 && queue_[static_cast<std::size_t> (on)].due == noteDue_)
        {
            unqueue (on);
            --notesSent_;
            // Its bend goes too; the next note sends its own.
            for (int k = queued_ - 1; k >= 0; --k)
            {
                const auto& p = queue_[static_cast<std::size_t> (k)];
                if (p.event.isPitchBend() && p.due == noteDue_ && p.event.channel() == noteChannel_)
                {
                    unqueue (k);
                    bendSent_ = -1;
                    break;
                }
            }
            bridgeNote_ = -1;
            for (int k = 0; k < queued_; ++k)
            {
                const auto& p = queue_[static_cast<std::size_t> (k)];
                if (p.event.isNoteOff() && p.due == noteDue_ && p.event.channel() == noteChannel_)
                {
                    bridgeNote_ = p.event.note();
                    bridgeChannel_ = noteChannel_;
                }
            }
            lastNote_ = bridgeNote_ >= 0 ? bridgeNote_ : lastNote_;
            note_ = -1;
            return;
        }
    }
    schedule (due, Event::noteOff (noteChannel_, note_, 0));
    note_ = -1;
}

void GuitarToMidi::sendBend (float deviation, std::int64_t due) noexcept
{
    const int range = std::max (1, settings_.bendRange);
    const float share = std::clamp (deviation / static_cast<float> (range), -1.0f, 1.0f);
    const int value =
        std::clamp (kBendCentre + static_cast<int> (std::lround (share * 8192.0f)), 0, 16383);
    bendCents_ = 100.0f * deviation;
    lastBendAt_ = now_;
    if (value == bendSent_ && settings_.channel == bendChannel_)
        return;
    bendSent_ = value;
    bendChannel_ = settings_.channel;
    if (settings_.timing == Timing::steady && due == now_)
        due = now_ + lagSamples_;
    schedule (due, Event::pitchBend (bendChannel_, value, 0));
}

void GuitarToMidi::schedule (std::int64_t due, const Event& e) noexcept
{
    if (queued_ >= kQueueSize)
    {
        // Full (it never is in practice): make room by dropping the oldest bend, or else by
        // sending the earliest event late rather than losing a note-off.
        int drop = -1;
        for (int k = 0; k < queued_ && drop < 0; ++k)
            if (queue_[static_cast<std::size_t> (k)].event.isPitchBend())
                drop = k;
        if (drop < 0)
            drop = 0;
        for (int k = drop; k + 1 < queued_; ++k)
            queue_[static_cast<std::size_t> (k)] = queue_[static_cast<std::size_t> (k + 1)];
        --queued_;
    }
    // Insert after every event due at the same time or earlier: ties keep their order.
    int at = queued_;
    while (at > 0 && queue_[static_cast<std::size_t> (at - 1)].due > due)
    {
        queue_[static_cast<std::size_t> (at)] = queue_[static_cast<std::size_t> (at - 1)];
        --at;
    }
    queue_[static_cast<std::size_t> (at)] = { due, e };
    ++queued_;
}

void GuitarToMidi::emitDue (EventList& out, int offset) noexcept
{
    int sent = 0;
    while (sent < queued_ && queue_[static_cast<std::size_t> (sent)].due <= now_)
    {
        emit (out, queue_[static_cast<std::size_t> (sent)].event.withOffset (offset));
        ++sent;
    }
    if (sent == 0)
        return;
    for (int k = sent; k < queued_; ++k)
        queue_[static_cast<std::size_t> (k - sent)] = queue_[static_cast<std::size_t> (k)];
    queued_ -= sent;
}

void GuitarToMidi::emit (EventList& out, const Event& e) noexcept
{
    const int ch = e.channel();
    if (e.isNoteOn())
    {
        // A note-on for a note already sounding at the receiver ends it first.
        if (isOn (on_, ch, e.note()))
            out.push (Event::noteOff (ch, e.note(), e.offset));
        setOn (on_, ch, e.note(), true);
    }
    else if (e.isNoteOff())
    {
        if (!isOn (on_, ch, e.note()))
            return;
        setOn (on_, ch, e.note(), false);
    }
    channelsUsed_ = static_cast<std::uint16_t> (channelsUsed_ | (1u << (ch - 1)));
    out.push (e);
}

void GuitarToMidi::endAll (EventList& out, int offset, bool everyChannel) noexcept
{
    queued_ = 0;
    note_ = -1;
    bridgeNote_ = -1;
    candidate_ = -1;
    pickFresh_ = false;
    for (int ch = 1; ch <= 16; ++ch)
        for (int n = 0; n < 128; ++n)
            if (isOn (on_, ch, n))
                emit (out, Event::noteOff (ch, n, offset));
    const auto used = static_cast<std::uint16_t> (channelsUsed_ | (1u << (settings_.channel - 1)));
    for (int ch = 1; ch <= 16; ++ch)
        if (everyChannel || (used & (1u << (ch - 1))) != 0)
        {
            out.push (Event::controller (ch, kAllNotesOff, 0, offset));
            out.push (Event::pitchBend (ch, kBendCentre, offset));
        }
    bendSent_ = kBendCentre;
    bendCents_ = 0.0f;
    channelsUsed_ = 0;
}

void GuitarToMidi::allNotesOff (EventList& out, int offset) noexcept
{
    endAll (out, offset, false);
}

void GuitarToMidi::panic (EventList& out, int offset) noexcept
{
    endAll (out, offset, true);
}

} // namespace sedef::midi
