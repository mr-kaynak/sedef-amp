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
// The tracker's detector hears picks down to this far under the gate.
constexpr float kPickUnderGateDb = 6.0f;
// The early reading looks no higher than the tracker does, and needs this much past the tracker's
// own time before the tracker's reading counts against it.
constexpr double kEarlyHighestHz = 1400.0;
constexpr double kEarlyGraceSeconds = 0.007;
constexpr double kEarlyMinGraceSeconds = 0.015;
constexpr double kEarlyLimitSeconds = 0.15; // ...and never longer than this

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
    if (static_cast<int> (s.lowNote) > static_cast<int> (LowNote::fSharp1))
        s.lowNote = LowNote::automatic;
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
    return velocityFor (std::pow (10.0f, levelDb / 20.0f), settings_, velocityTopDb (levelDb));
}

void GuitarToMidi::rememberPick (float levelDb) noexcept
{
    // An average in dB over about the latest kPlayingNotes picks: every pick weighs the same
    // until there are that many, then each new one weighs 1/kPlayingNotes. It moves slowly, so
    // the dynamics of a phrase stay in it.
    playingCount_ = std::min (playingCount_ + 1, kPlayingNotes);
    playingDb_ += (levelDb - playingDb_) / static_cast<float> (playingCount_);
}

float GuitarToMidi::playingLevelDb() const noexcept
{
    return playingCount_ > 0 ? playingDb_ : kFloorDb;
}

float GuitarToMidi::velocityTopDb (float levelDb) const noexcept
{
    // How hard this player picks on this input sets the scale: a pick at the player's level
    // sits kPlayingHeadroomDb under its top. The first pick is the player's level.
    return (playingCount_ > 0 ? playingDb_ : levelDb) + kPlayingHeadroomDb;
}

float GuitarToMidi::quietDb() const noexcept
{
    return std::max (gateLevelDb_, playingCount_ > 0 ? playingDb_ - kVelocitySpanDb : kFloorDb);
}

void GuitarToMidi::updateGate (float msLevelDb) noexcept
{
    // The noise floor: the quietest the input has been, rising slowly while no note sounds (the
    // noise may grow). During the warm-up after reset() it is simply the level: the input was
    // there before we listened, and the level rises from nothing at first.
    if (now_ <= warmupSamples_ || msLevelDb < noiseDb_)
        noiseDb_ = msLevelDb;
    else if (note_ < 0)
        noiseDb_ = std::min (msLevelDb, noiseDb_ + noiseRisePerMs_);
    gateLevelDb_ = settings_.gateMode == GateMode::manual
                       ? settings_.gateDb
                       : std::max (kAutoGateFloorDb, noiseDb_ + kNoiseMarginDb);
    // The tracker's detector hears picks down to just under the gate.
    auto options = tracker_.options();
    options.onsetFloor = std::pow (10.0f, (gateLevelDb_ - kPickUnderGateDb) / 10.0f);
    options.clickFloor = 0.1f * options.onsetFloor;
    tracker_.setOptions (options);
}

int GuitarToMidi::steadyLagSamples (double sampleRate) noexcept
{
    return samples (kSteadyLagSeconds, sampleRate);
}

void GuitarToMidi::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;
    // The tracker hears down to the lowest range's floor; applyRange() keeps its search to the
    // range in effect.
    tracker_.prepare (sampleRate, static_cast<float> (kFloorHz) - 1.0f);
    // Fingers: a touch before the pluck, and the weak fundamental and pulse-like attack of a low
    // string plucked near the bridge.
    analysis::MonoTracker::Options trackerOptions;
    trackerOptions.weakFundamentals = true;
    trackerOptions.fingerPicks = true;
    tracker_.setOptions (trackerOptions);
    hp_.c = design::highpass (sampleRate, kBandLowHz, 0.7071);
    lp_.c = design::lowpass (sampleRate, std::min (kBandHighHz, 0.4 * sampleRate), 0.7071);
    earlyLp_.c = lp_.c;
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
    while (size < static_cast<int> (2.5 / kFloorHz * rate_) + 8)
        size <<= 1;
    ring_.assign (static_cast<std::size_t> (size), 0.0f);
    earlyRing_.assign (static_cast<std::size_t> (size), 0.0f);
    ringMask_ = size - 1;
    hop_ = std::max (1, static_cast<int> (std::lround (0.0005 * rate_)));
    minWindow_ = std::max (4, static_cast<int> (std::lround (0.004 * rate_)));
    refractorySamples_ = samples (kPickRefractorySeconds, sampleRate);
    weakRefractorySamples_ = samples (0.06, sampleRate);
    weakCheckSamples_ = samples (0.012, sampleRate);
    pickLevelSamples_ = samples (0.004, sampleRate);
    lockPickGapSamples_ = samples (0.05, sampleRate);
    correctionConfirmSamples_ = samples (kCorrectionConfirmSeconds, sampleRate);
    refineSamples_ = samples (0.006, sampleRate);
    baseSamples_ = samples (0.02, sampleRate);
    clickDelaySamples_ = samples (0.0002, sampleRate);
    minCheckWindow_ = std::max (4, static_cast<int> (std::lround (kCheckWindowSeconds * rate_)));
    attackSkip_ = static_cast<int> (std::lround (kAttackSkipSeconds * rate_));
    steadySamples_ = samples (0.01, sampleRate);
    breakWindowSamples_ = samples (0.02, sampleRate);
    warmupSamples_ = samples (kWarmupSeconds, sampleRate);
    touchWaitSamples_ = samples (kTouchWaitSeconds, sampleRate);
    noiseRisePerMs_ = kNoiseRiseDbPerSecond / 1000.0f;
    bendOnTicks_ =
        std::max (1, static_cast<int> (std::lround (kBendOnSeconds / kBendIntervalSeconds)));
    bendOffTicks_ =
        std::max (1, static_cast<int> (std::lround (kBendOffSeconds / kBendIntervalSeconds)));
    bendSlewPerTick_ = kBendSlewCentsPerSecond * static_cast<float> (kBendIntervalSeconds);
    centrePerTick_ = kCentreCentsPerSecond * static_cast<float> (kBendIntervalSeconds);
    learnSamples_ = samples (kLearnSeconds, sampleRate);
    bendSettleTicks_ =
        std::max (1, static_cast<int> (std::lround (kBendSettleSeconds / kBendIntervalSeconds)));
    bendStillNeed_ =
        std::max (1, static_cast<int> (std::lround (kBendStillSeconds / kBendIntervalSeconds)));
    slideWindowSamples_ = samples (kSlideWindowSeconds, sampleRate);
    slideConfirmSamples_ = samples (kSlideConfirmSeconds, sampleRate);
    {
        const int longest = static_cast<int> (std::ceil (rate_ / kFloorHz)) + 2;
        for (auto& sums : early_)
        {
            sums.r.assign (static_cast<std::size_t> (longest + 2), 0.0);
            sums.m.assign (static_cast<std::size_t> (longest + 2), 0.0);
            sums.stretch.assign (ring_.size(), 0.0f);
        }
        earlyMinLag_ = std::max (2, static_cast<int> (std::floor (rate_ / kEarlyHighestHz)));
        earlyWindow_ = static_cast<int> (std::lround (kEarlyWindowSeconds * rate_));
        earlyAloneWindow_ = static_cast<int> (std::lround (kEarlyAloneSeconds * rate_));
        earlySkipSamples_ = samples (kAttackSkipSeconds, sampleRate);
        earlyGuardSamples_ = samples (0.0005, sampleRate);
        earlyFineLag_ = static_cast<int> (std::lround (kEarlyFineSeconds * rate_));
        earlyHeldCount_ =
            static_cast<int> (std::lround ((kEarlyHeldSeconds - kAttackSkipSeconds) * rate_));
        earlyHeldSkip_ =
            static_cast<int> (std::lround ((kEarlyHeldSkipSeconds - kAttackSkipSeconds) * rate_));
    }
    settings_ = next_;
    settingsChanged_ = false;
    reset();
    applyRange();
}

void GuitarToMidi::reset() noexcept
{
    tracker_.reset();
    noiseDb_ = kAutoGateFloorDb;
    gateLevelDb_ = settings_.gateMode == GateMode::manual ? settings_.gateDb
                                                          : kAutoGateFloorDb + kNoiseMarginDb;
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
    std::fill (earlyRing_.begin(), earlyRing_.end(), 0.0f);
    earlyLp_.s0 = earlyLp_.s1 = 0.0f;
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
    sentNote_ = -1;
    bendOut_ = bendGlide_ = bendSeen_ = bendCentre_ = bendTarget_ = bendStill_ = 0.0f;
    bendOn_ = false;
    bendOutside_ = bendInside_ = bendStillTicks_ = 0;
    stepAt_ = 0;
    stepDirection_ = 0;
    noteLearned_ = false;
    lastBendAt_ = 0;
    queued_ = 0;
    bridgeNote_ = -1;
    on_ = {};
    channelsUsed_ = 0;
    earlyPickAt_ = -1;
    earlyCount_ = 0;
    earlyHz_ = 0.0f;
    earlyOld_ = earlyRest_ = earlyHeld_ = false;
    earlyNote_ = earlyUnread_ = earlyDoubt_ = earlyStart_ = false;
    earlyLimit_ = 0;
}

void GuitarToMidi::setSettings (const Settings& s) noexcept
{
    const auto clean = sanitised (s);
    const auto& from = settingsChanged_ ? next_ : settings_;
    if (clean.octave != from.octave || clean.mode != from.mode ||
        clean.bendRange != from.bendRange || clean.timing != from.timing ||
        clean.channel != from.channel || clean.gateMode != from.gateMode ||
        clean.gateDb != from.gateDb || clean.dynamics != from.dynamics ||
        clean.curve != from.curve || clean.lowNote != from.lowNote)
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
    if (old.lowNote != settings_.lowNote)
        applyRange();
    // A note bent to another fret is not the note the receiver holds: without the bend, or with
    // another range, it would jump.
    const bool bendChanged = old.mode != settings_.mode || old.bendRange != settings_.bendRange;
    if (note_ >= 0 && (old.channel != settings_.channel || old.octave != settings_.octave ||
                       (bendChanged && (note_ != sentNote_ || bendSent_ != kBendCentre))))
    {
        endNote (now_, Why::other, true);
        emitDue (out, offset);
        pickFresh_ = false;
    }
    if (bendChanged && bendSent_ != kBendCentre)
    {
        schedule (now_, Event::pitchBend (bendChannel_, kBendCentre, 0));
        bendSent_ = kBendCentre;
        bendOut_ = bendGlide_ = bendTarget_ = 0.0f;
        bendOn_ = false;
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
        // The early reading reads between the decimated samples: its band is cut once more, so
        // that what the decimation folds down stays far under what a period may leave.
        const float earlyBand = earlyLp_.process (band);
        power_ += powerCoeff_ * (band * band - power_);
        levelDb_ = powerToDb (power_);
        msMax_ = std::max (msMax_, levelDb_);
        if (++phase_ >= factor_)
        {
            phase_ = 0;
            ring_[static_cast<std::size_t> (ringWrite_)] = band;
            earlyRing_[static_cast<std::size_t> (ringWrite_)] = earlyBand;
            ringWrite_ = (ringWrite_ + 1) & ringMask_;
            earlyPush();
            if (++hopCount_ >= hop_)
            {
                hopCount_ = 0;
                earlyEstimate();
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
            updateGate (msMax_);
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
        const bool fromSilence =
            ev == analysis::MonoTracker::Event::noteOn ||
            (ev == analysis::MonoTracker::Event::noteChange && levelAgo (kLockRiseMs) < quietDb());
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
    earlyLp_.s0 = std::abs (earlyLp_.s0) < 1.0e-20f ? 0.0f : earlyLp_.s0;
    earlyLp_.s1 = std::abs (earlyLp_.s1) < 1.0e-20f ? 0.0f : earlyLp_.s1;
    clickHp_.s0 = std::abs (clickHp_.s0) < 1.0e-20f ? 0.0f : clickHp_.s0;
    clickHp_.s1 = std::abs (clickHp_.s1) < 1.0e-20f ? 0.0f : clickHp_.s1;
    clickPower_ = clickPower_ < 1.0e-20f ? 0.0f : clickPower_;
    power_ = power_ < 1.0e-20f ? 0.0f : power_;
}

void GuitarToMidi::registerPick (std::int64_t at, PickKind kind) noexcept
{
    // Right after reset() a rise is only the input starting to be heard.
    if (now_ <= warmupSamples_)
        return;
    const bool weak = kind == PickKind::weak;
    // The tracker's detector and the periodicity watch can both see one pick: the first counts.
    // A break of periodicity alone (weak) also follows the end of whatever the pick replaced,
    // so it waits longer after any pick.
    // A pick the detector (or a lock) sees while a break of periodicity still waits for its level
    // check replaces it: the break was the previous note's end, a mute just before this pick.
    const bool replacesWeak = pickWeak_ && !weak;
    // From silence, a pick that started no note (it waits, or was too soft) may have been a
    // finger touching the string: a pick the detector sees soon after it (it hears one that soon
    // only when it is much louder) is the pluck.
    const bool replacesTouch = kind == PickKind::detector && note_ < 0;
    if (lastPickAt_ > 0 &&
        at - lastPickAt_ < (weak ? weakRefractorySamples_ : refractorySamples_) && !replacesWeak &&
        !replacesTouch)
        return;
    if (!replacesWeak && !replacesTouch)
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
    // (A note started early is watched at its own period until the tracker reads it.)
    if (note_ < 0 || (!earlyNote_ && (!tracker_.voiced() || tracker_.hz() <= 0.0f)))
    {
        periodicity_ = 0.0f;
        periodicSince_ = 0;
        periodicHz_ = 0.0f;
        return;
    }
    const float hz = earlyNote_ ? noteHz_ : tracker_.hz();
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
        levelDb_ >= gateLevelDb_)
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
    const auto threshold = static_cast<float> (std::max (8.0 * base / count, 1.0e-20));
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

void GuitarToMidi::earlyPush() noexcept
{
    // The early reading (Fastest). For every period it keeps how well the signal since the
    // latest pick's attack repeats one period on: r and m, summed over every pair of samples a
    // period apart inside that stretch (a difference function as in de Cheveigne and Kawahara's
    // YIN, over a window that grows from the pick instead of sliding). The tracker's window is
    // one period wide and so needs two periods of the note; this one starts at the attack, so a
    // period shows as soon as the first few milliseconds after the attack have come round again.
    const bool active = settings_.timing == Timing::fastest && lastPickAt_ > 0 &&
                        now_ - pickAt_ <= earlySpanSamples_;
    if (!active)
    {
        earlyHz_ = 0.0f;
        earlyPickAt_ = -1;
        return;
    }
    if (earlyPickAt_ != pickAt_)
    {
        earlyBegin();
        return;
    }
    if (now_ >= pickAt_ + earlySkipSamples_)
        earlyAdd (0);
}

void GuitarToMidi::earlyBegin() noexcept
{
    // A new pick, or the same one dated again: count afresh from the samples kept.
    earlyPickAt_ = pickAt_;
    earlyHz_ = 0.0f;
    earlyCount_ = 0;
    for (auto& sums : early_)
    {
        sums.energy = 0.0;
        std::fill (sums.r.begin(), sums.r.end(), 0.0);
        std::fill (sums.m.begin(), sums.m.end(), 0.0);
    }
    // The note that sounded when the pick came may ring on under the new one. While it does,
    // the input repeats at its period too, and nothing read from the mix is the new note
    // alone. So the stretch is kept twice: as it is, and less what the old note would have gone
    // on to do (its last period before the pick, repeated). If the old note does ring on,
    // taking it out leaves less (earlyEstimate() then reads what is left: what the pick
    // brought); if the pick ended it, as a pick on the same string does, taking it out adds a
    // ghost of it, and the stretch as it is is the new note. It needs a note that was steady:
    // the input just before the pick must repeat at the tracker's period.
    earlyOld_ = false;
    earlyOldEnd_ = pickAt_ - earlyGuardSamples_;
    if (pickHz_ > 0.0f && earlyOldEnd_ < now_)
    {
        const double period = rate_ / static_cast<double> (pickHz_);
        const int ago = static_cast<int> ((now_ - earlyOldEnd_) / factor_);
        const int window = static_cast<int> (std::ceil (period));
        const int reach = earlySpanSamples_ / factor_ + 2 * window + 8;
        if (window >= earlyMinLag_ && reach < ringMask_ && ago + 2 * window + 4 < ringMask_)
        {
            const int whole = static_cast<int> (period);
            const auto frac = static_cast<float> (period - whole);
            double diff = 0.0, energy = 0.0;
            for (int k = 0; k < window; ++k)
            {
                const float x = earlyAt (ago + k);
                const float y = (1.0f - frac) * earlyAt (ago + k + whole) +
                                frac * earlyAt (ago + k + whole + 1);
                diff += static_cast<double> (x - y) * (x - y);
                energy += static_cast<double> (x) * x + static_cast<double> (y) * y;
            }
            earlyOld_ = energy > 1.0e-14 && diff <= static_cast<double> (kEarlyOldClean) * energy;
            earlyOldPeriod_ = period;
        }
    }
    const std::int64_t from = pickAt_ + earlySkipSamples_;
    const auto have = static_cast<int> (std::min<std::int64_t> (
        now_ >= from ? (now_ - from) / factor_ + 1 : 0, static_cast<std::int64_t> (ringMask_ / 2)));
    for (int index = 0; index < have; ++index)
        earlyAdd (have - 1 - index);
}

void GuitarToMidi::earlyAdd (int ago) noexcept
{
    // The sample `ago` decimated steps back is the next one of the stretch.
    if (earlyCount_ >= static_cast<int> (early_[0].stretch.size()))
        return;
    const float x = earlyAt (ago);
    float rest = x;
    if (earlyOld_)
    {
        // Less the old note, as it was one or more of its periods earlier, before the pick.
        const double since = static_cast<double> (now_ - earlyOldEnd_) / factor_ - ago;
        const double back = ago + (std::floor (since / earlyOldPeriod_) + 1.0) * earlyOldPeriod_;
        const int whole = static_cast<int> (back);
        if (whole + 1 < ringMask_)
        {
            const auto frac = static_cast<float> (back - whole);
            rest -= (1.0f - frac) * earlyAt (whole) + frac * earlyAt (whole + 1);
        }
    }
    const int longest = std::min (earlyCount_, earlyLagLimit_);
    const float both[2] = { x, rest };
    for (int which = 0; which < (earlyOld_ ? 2 : 1); ++which)
    {
        auto& sums = early_[static_cast<std::size_t> (which)];
        const double v = both[which];
        sums.stretch[static_cast<std::size_t> (earlyCount_)] = both[which];
        sums.energy += v * v;
        const float* past = sums.stretch.data() + earlyCount_;
        for (int lag = 1; lag <= longest; ++lag)
        {
            const double y = past[-lag];
            sums.r[static_cast<std::size_t> (lag)] += v * y;
            sums.m[static_cast<std::size_t> (lag)] += v * v + y * y;
        }
    }
    ++earlyCount_;
}

void GuitarToMidi::earlyEstimate() noexcept
{
    earlyHz_ = 0.0f;
    earlyHeld_ = false;
    earlyRest_ = false;
    if (earlyPickAt_ != pickAt_ || earlyPickAt_ < 0)
        return;
    const int n = earlyCount_;
    const int top = std::min (earlyLagLimit_, n - 2);
    if (top <= earlyMinLag_ + 1)
        return;
    // The old note rings on when taking it out leaves clearly less; then what is left is read.
    earlyRest_ =
        earlyOld_ && early_[1].energy < static_cast<double> (kEarlyRingsOn) * early_[0].energy;
    const auto& sums = early_[earlyRest_ ? 1u : 0u];
    // What a period leaves unexplained: 0 when the stretch repeats exactly, 1 when it does not
    // repeat at all.
    const auto left = [&sums] (int lag) {
        const double m = sums.m[static_cast<std::size_t> (lag)];
        return m > 1.0e-14
                   ? static_cast<float> (1.0 - 2.0 * sums.r[static_cast<std::size_t> (lag)] / m)
                   : 2.0f;
    };
    // Every dip, shortest period first, placed between lags by a parabola. The first that
    // qualifies is the note.
    bool shorter = false; // a shorter period accounts for much of the signal
    // What is left of a ringing note taken out must be something the pick brought: noise over
    // a ringing note brings little.
    const bool brought =
        !earlyRest_ || early_[1].energy >= static_cast<double> (kEarlyBrought) * early_[0].energy;
    // The note the tracker held when the pick came: the same note picked again, or the fretting
    // hand already on the next fret of a string that still rang. Read from the stretch as it
    // is: a pick on the string that sounded ends what it did.
    const float heldPeriod =
        pickHz_ > 0.0f && !earlyRest_ ? static_cast<float> (rate_) / pickHz_ : 0.0f;
    const int heldFrom =
        heldPeriod > 0.0f ? static_cast<int> (heldPeriod * std::exp2 (-kEarlyHeldCents / 1200.0f))
                          : 0;
    float before = left (earlyMinLag_), at = left (earlyMinLag_ + 1);
    for (int lag = earlyMinLag_ + 1; lag < top; ++lag)
    {
        if (lag == heldFrom && !shorter && earlyHeldNote (heldPeriod))
            return;
        const float after = left (lag + 1);
        if (at <= before && at <= after)
        {
            const float denom = before - 2.0f * at + after;
            float shift = 0.0f, value = at;
            if (denom > 0.0f)
            {
                shift = std::clamp (0.5f * (before - after) / denom, -0.5f, 0.5f);
                value = std::max (0.0f, at - 0.25f * (before - after) * shift);
            }
            float period = static_cast<float> (lag) + shift;
            // A short period is a few samples of the decimated band: between two whole lags
            // even a perfect repeat leaves something. Such a dip is measured again between the
            // lags.
            if (lag < earlyFineLag_ && value <= kEarlyFine)
                value = earlyFineLeft (sums.stretch.data(), n, lag, period);
            const auto hz = static_cast<float> (rate_) / period;
            // Nothing an octave under this period is a note of the range: the attack's first
            // kEarlyWindowSeconds repeating one period on is the string's pulse come round.
            // Otherwise a lower string may be on its way, named by one of its partials: only a
            // period that alone accounts for the signal counts (kEarlyAlone, over a period or
            // more), which a string's partial never does.
            const bool lowest = 2.0 * static_cast<double> (period) > rate_ / lowestHz_;
            const int window = n - lag;
            // And the two windows compared must hold at least kEarlyDensity of the stretch's
            // energy their length would: a string that gives one pulse a period (plucked near
            // the bridge, heard near it) has, until its pulse has come round twice, all its energy
            // between the windows, and what is compared is the quiet between the pulses.
            const bool representative =
                sums.m[static_cast<std::size_t> (lag)] * n >=
                static_cast<double> (kEarlyDensity) * 2.0 * window * sums.energy;
            const bool clean =
                representative &&
                (lowest ? value <= kEarlyClean &&
                              window >=
                                  std::max (earlyWindow_, static_cast<int> (kEarlyShare * period))
                        : value <= kEarlyAlone && window >= std::max (lag, earlyAloneWindow_));
            // And no shorter period may account for much of the signal: two strings ringing
            // together repeat at a period neither has, an octave under the played note is a
            // multiple of its period, and a few milliseconds of a low string can look like
            // themselves a little later than one period on.
            const bool note = hz >= minNoteHz_ && !unheardLow (hz) && !shorter && brought;
            // Then two checks that cost a little more, made only for a period that would
            // otherwise be named. Half the period must leave what a string's octave above
            // leaves (twice the share of its odd partials, over any stretch): something slow,
            // such as the band's own ring after a step or a mute, repeats at every short period,
            // and at half of one even better. And a period of kEarlyFineSeconds or more must
            // name the same note when only the later half of the two windows is read: what the
            // attack and the band's answer to it leave in the first milliseconds can move a low
            // note's dip by more than half a semitone, where the string itself moves it nowhere.
            // (A shorter period is read again between the lags instead, and on the strings that
            // start in phase it reads right without the check: the design, "Why the early reading
            // can be trusted, and when".)
            const auto halfLeaves = [&] {
                const float half = 0.5f * period;
                const auto under = static_cast<int> (half);
                const float frac = half - static_cast<float> (under);
                return under >= 1 &&
                       (1.0f - frac) * left (under) + frac * left (under + 1) > kEarlyOther;
            };
            const auto placed = [&] {
                if (lag < earlyFineLag_)
                    return true;
                const float late = earlyLatePeriod (sums.stretch.data(), n, period);
                return late > 0.0f && std::lround (midiPitch (static_cast<float> (rate_) / late)) ==
                                          std::lround (midiPitch (hz));
            };
            if (clean && note && halfLeaves() && placed())
            {
                earlyHz_ = hz;
                earlyLeft_ = value;
                earlyRepeat_ = window;
                return;
            }
            shorter = shorter || value <= kEarlyOther;
        }
        before = at;
        at = after;
    }
}

bool GuitarToMidi::earlyStillRepeats (float hz) const noexcept
{
    // The stretch since the pick, as read when the note started, still repeats at the note's
    // period (true when the stretch is no longer kept: nothing says otherwise).
    if (earlyPickAt_ != pickAt_ || earlyPickAt_ < 0 || hz <= 0.0f)
        return true;
    const auto& sums = early_[earlyRest_ ? 1u : 0u];
    const auto lag = static_cast<int> (std::lround (rate_ / static_cast<double> (hz)));
    if (lag - 1 < earlyMinLag_ || lag + 1 > std::min (earlyLagLimit_, earlyCount_ - 2))
        return true;
    float least = 2.0f;
    for (int k = lag - 1; k <= lag + 1; ++k)
    {
        const double m = sums.m[static_cast<std::size_t> (k)];
        if (m > 1.0e-14)
            least = std::min (
                least, static_cast<float> (1.0 - 2.0 * sums.r[static_cast<std::size_t> (k)] / m));
    }
    return least <= kEarlyStill;
}

float GuitarToMidi::earlyFineLeft (const float* x, int n, int lag, float& period) const noexcept
{
    // What the periods a quarter of a sample apart around `lag` leave unexplained, the stretch
    // read between its samples by a cubic through four of them; the least of them, and the
    // period placed by a parabola through it and its neighbours.
    constexpr int kSteps = 5; // -0.5, -0.25, 0, 0.25, 0.5
    float values[kSteps] = {};
    for (int step = 0; step < kSteps; ++step)
    {
        const float back = static_cast<float> (lag) + 0.25f * static_cast<float> (step - 2);
        const int whole = static_cast<int> (std::floor (back));
        const float f = back - static_cast<float> (whole);
        // Lagrange weights for the samples at whole - 1, whole, whole + 1, whole + 2 back.
        const float w0 = -f * (f - 1.0f) * (f - 2.0f) / 6.0f;
        const float w1 = (f + 1.0f) * (f - 1.0f) * (f - 2.0f) / 2.0f;
        const float w2 = -(f + 1.0f) * f * (f - 2.0f) / 2.0f;
        const float w3 = (f + 1.0f) * f * (f - 1.0f) / 6.0f;
        double diff = 0.0, energy = 0.0;
        for (int i = whole + 2; i < n; ++i)
        {
            const float a = x[i];
            const float b = w0 * x[i - whole + 1] + w1 * x[i - whole] + w2 * x[i - whole - 1] +
                            w3 * x[i - whole - 2];
            diff += static_cast<double> (a - b) * (a - b);
            energy += static_cast<double> (a) * a + static_cast<double> (b) * b;
        }
        values[step] = energy > 1.0e-14 ? static_cast<float> (diff / energy) : 2.0f;
    }
    int best = 0;
    for (int step = 1; step < kSteps; ++step)
        if (values[step] < values[best])
            best = step;
    float shift = 0.0f;
    if (best > 0 && best < kSteps - 1)
    {
        const float denom = values[best - 1] - 2.0f * values[best] + values[best + 1];
        if (denom > 0.0f)
            shift = std::clamp (0.5f * (values[best - 1] - values[best + 1]) / denom, -0.5f, 0.5f);
    }
    period = static_cast<float> (lag) + 0.25f * (static_cast<float> (best - 2) + shift);
    return values[best];
}

float GuitarToMidi::earlyLatePeriod (const float* x, int n, float period) const noexcept
{
    // The later half of the two windows only (samples from `skip` on), every whole lag within a
    // semitone of `period`: the deepest dip inside that span, placed by a parabola.
    const float semitone = std::exp2 (1.0f / 12.0f);
    const int first = static_cast<int> (period / semitone) - 1;
    const int last = static_cast<int> (period * semitone) + 2;
    const int skip = (n - static_cast<int> (period)) / 2;
    if (first < earlyMinLag_ || last - first + 1 > 64 || n - skip - last < 8)
        return 0.0f;
    std::array<float, 64> values {};
    for (int lag = first; lag <= last; ++lag)
    {
        double diff = 0.0, energy = 0.0;
        for (int i = skip + lag; i < n; ++i)
        {
            const float a = x[i], b = x[i - lag];
            diff += static_cast<double> (a - b) * (a - b);
            energy += static_cast<double> (a) * a + static_cast<double> (b) * b;
        }
        values[static_cast<std::size_t> (lag - first)] =
            energy > 1.0e-14 ? static_cast<float> (diff / energy) : 2.0f;
    }
    int best = -1;
    for (int k = 1; k < last - first; ++k)
    {
        const auto at = values[static_cast<std::size_t> (k)];
        if (at <= values[static_cast<std::size_t> (k - 1)] &&
            at <= values[static_cast<std::size_t> (k + 1)] &&
            (best < 0 || at < values[static_cast<std::size_t> (best)]))
            best = k;
    }
    if (best < 0)
        return 0.0f;
    const float y0 = values[static_cast<std::size_t> (best - 1)];
    const float y1 = values[static_cast<std::size_t> (best)];
    const float y2 = values[static_cast<std::size_t> (best + 1)];
    const float denom = y0 - 2.0f * y1 + y2;
    const float shift = denom > 0.0f ? std::clamp (0.5f * (y0 - y2) / denom, -0.5f, 0.5f) : 0.0f;
    return static_cast<float> (first + best) + shift;
}

bool GuitarToMidi::earlyHeldNote (float heldPeriod) noexcept
{
    // The note the string already sounded needs less than a note out of nowhere: the stretch
    // repeating at its period, near enough and on the same semitone (the tracker's reading from
    // before the pick may be of a string on its way to another fret), says the pick started
    // that note again. The stretch is read from kEarlyHeldSkipSeconds after the pick: what the
    // old note still did under the pick is gone by then. And not before kEarlyHeldSeconds: a
    // fretting finger that lands just after the pick must have had time to show.
    const auto& x = early_[0].stretch;
    const int n = earlyCount_;
    const float span = std::exp2 (kEarlyHeldCents / 1200.0f);
    const int first = static_cast<int> (heldPeriod / span) - 1;
    const int last = static_cast<int> (heldPeriod * span) + 2;
    const int need = std::max (earlyWindow_, static_cast<int> (kEarlyShare * heldPeriod));
    if (n < earlyHeldCount_ || first < earlyMinLag_ || last > earlyLagLimit_ ||
        n - earlyHeldSkip_ - last < need)
        return false;
    std::array<float, 16> values {};
    const int count = last - first + 1;
    if (count > static_cast<int> (values.size()))
        return false;
    const auto left = [&x, n, this] (int lag) {
        double diff = 0.0, energy = 0.0;
        for (int i = earlyHeldSkip_ + lag; i < n; ++i)
        {
            const float a = x[static_cast<std::size_t> (i)];
            const float b = x[static_cast<std::size_t> (i - lag)];
            diff += static_cast<double> (a - b) * (a - b);
            energy += static_cast<double> (a) * a + static_cast<double> (b) * b;
        }
        return energy > 1.0e-14 ? static_cast<float> (diff / energy) : 2.0f;
    };
    for (int k = 0; k < count; ++k)
        values[static_cast<std::size_t> (k)] = left (first + k);
    // A dip inside the span, not its edge (a note a semitone away slopes down towards it).
    int best = 1;
    for (int k = 1; k + 1 < count; ++k)
        if (values[static_cast<std::size_t> (k)] < values[static_cast<std::size_t> (best)])
            best = k;
    const float y0 = values[static_cast<std::size_t> (best - 1)];
    const float y1 = values[static_cast<std::size_t> (best)];
    const float y2 = values[static_cast<std::size_t> (best + 1)];
    if (y1 > y0 || y1 > y2 || y1 > kEarlyHeld)
        return false;
    const float denom = y0 - 2.0f * y1 + y2;
    const float shift = denom > 0.0f ? std::clamp (0.5f * (y0 - y2) / denom, -0.5f, 0.5f) : 0.0f;
    const float period = static_cast<float> (first + best) + shift;
    const auto hz = static_cast<float> (rate_) / period;
    if (hz < minNoteHz_ || unheardLow (hz) ||
        std::abs (1200.0f * std::log2 (period / heldPeriod)) > kEarlyHeldCents ||
        std::lround (midiPitch (hz)) != std::lround (midiPitch (pickHz_)))
        return false;
    // And the stretch must tell this note from the semitones next to it: a few milliseconds
    // of a nearly pure tone repeat almost as well a semitone off, and a string whose fret
    // changed since the pick repeats no better at the old note than at the new.
    const float semitone = std::exp2 (1.0f / 12.0f);
    const int under = static_cast<int> (std::lround (period * semitone));
    const int over = static_cast<int> (std::lround (period / semitone));
    if (over < earlyMinLag_ || n - earlyHeldSkip_ - under < earlyWindow_ ||
        std::min (left (under), left (over)) < kEarlyHeldApart)
        return false;
    // A note picked an octave or a twelfth over the old one, which rings on, repeats at the
    // old note's period too. Then what is left without the old note repeats at a half or a
    // third of that period, which a string picked again never does.
    if (earlyOld_)
    {
        const auto& rest = early_[1];
        for (int part = 2; part <= 4; ++part)
        {
            const auto lag = static_cast<int> (std::lround (period / static_cast<float> (part)));
            for (int k = lag - 1; k <= lag + 1; ++k)
            {
                const double m = k >= earlyMinLag_ ? rest.m[static_cast<std::size_t> (k)] : 0.0;
                if (m > 1.0e-14 &&
                    1.0 - 2.0 * rest.r[static_cast<std::size_t> (k)] / m <= kEarlyHeldPart)
                    return false;
            }
        }
    }
    earlyHz_ = pickHz_;
    earlyHeld_ = true;
    earlyLeft_ = y1;
    earlyRepeat_ = n - earlyHeldSkip_ - (first + best);
    return true;
}

int GuitarToMidi::earlyNoteFor (float& hz) const noexcept
{
    // Only for a pick the tracker's detector saw, its level checked, in Fastest. A pick seen
    // as the sounding note losing its period (a pick no louder than what rang) may be the
    // fretting hand taking another fret: it counts only for the same note again.
    const bool seen =
        pickKind_ == PickKind::detector || (pickKind_ == PickKind::weak && earlyHeld_);
    if (earlyHz_ <= 0.0f || !pickFresh_ || pickChecking_ || !seen || earlyPickAt_ != pickAt_ ||
        now_ - pickAt_ > pickWindowSamples_)
        return -1;
    hz = earlyHz_;
    const int q = quantise (midiPitch (hz) + 12.0f * static_cast<float> (settings_.octave));
    // The note before the pick may still ring under the new one, and the signal then repeats
    // at its period too, and at every period it is a partial of. Read from what a ringing note
    // left when taken out, the reading is of the new note alone. Otherwise, while the note
    // before still rings (it was not steady enough to take out, or too quiet to show), the
    // same note again and every note it is a partial of wait for the tracker, unless the pick
    // drowned what rang.
    const bool rings = !earlyRest_ && preLevelDb_ > pickLevelDb() - kEarlyRingDb;
    if (rings && !earlyOld_ && earlyHeld_)
        return -1;
    if (rings && !earlyHeld_ && prevNote_ >= 0 && partialOf (q, prevNote_))
        return -1;
    return q;
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

float GuitarToMidi::attackBeforeDb() const noexcept
{
    // The loudest millisecond from 3 ms after the pick until the one before this.
    const int ago = static_cast<int> (
        std::min<std::int64_t> ((now_ - pickAt_) / std::max (1, msSamples_), kHistoryMs - 1));
    float peak = kFloorDb;
    for (int k = 0; k <= ago - 3; ++k)
        peak = std::max (peak, levelAgo (k));
    return ago < 3 ? levelDb_ : peak;
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
    return sustainedDb() < peak - kMuteDropDb;
}

float GuitarToMidi::sustainedDb (double coveredSeconds) const noexcept
{
    // The 3 ms level of a low string swings with every period (10-20 dB, plucked near the bridge
    // with the fingers): a string under the low E is as loud as the loudest millisecond of its
    // last period, less what `coveredSeconds` of it the plain level stands for already.
    const float hz = note_ >= 0 && noteHz_ > 0.0f ? noteHz_ : tracker_.hz();
    if (hz <= 0.0f)
        return levelDb_;
    const int back = std::clamp (
        static_cast<int> (std::ceil (1000.0 * (1.0 / static_cast<double> (hz) - coveredSeconds))),
        0, kHistoryMs / 2);
    float level = levelDb_;
    for (int k = 0; k < back; ++k)
        level = std::max (level, levelAgo (k));
    return level;
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
    const double lowest = 2.0 / lowestHz_ + kTrackerMarginSeconds + kRepickMarginSeconds;
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
    if (since >= 2.0 / lowestHz_ + kTrackerMarginSeconds + kRepickMarginSeconds)
        return false; // the tracker would have named it by now
    // The attack's first milliseconds (the pick's click, what it replaced) repeat at no period.
    const int available = static_cast<int> ((now_ - pickAt_) / factor_) - attackSkip_;
    const int window = minCheckWindow_;
    const auto candidateLag = static_cast<int> (std::lround (rate_ / static_cast<double> (hz)));
    // A six-string guitar's wait, in every range: one period of the low E.
    if (static_cast<double> (hz) >= 1.9 * kLowestOpenHz &&
        available < static_cast<int> (rate_ / kLowestOpenHz) + window)
        return true; // too early to tell
    if (now_ - lowCheckedAt_ < hop_ * factor_ && lowCheckedHz_ == hz)
        return lowerCached_;
    lowCheckedAt_ = now_;
    lowCheckedHz_ = hz;
    lowerCached_ = false;
    const bool lowRange = openHz_ < kLowestOpenHz;
    if (lowRange && static_cast<double> (hz) >= 1.9 * openHz_ &&
        available < static_cast<int> (rate_ / openHz_) + 2 * window)
    {
        // A range under a guitar's: one period of its lowest string (a first reading can name
        // any of a low string's partials, and not exactly). Unless the note alone accounts for
        // the signal since the pick: all of it (at least two periods, and 8 ms) repeats at the
        // note's period to within kAloneClean, so nothing else sounds, and nothing lower can be
        // on its way. (A low string whose 4th partial is its strongest still leaves 3 % of
        // itself outside that period.)
        const int span = available - candidateLag;
        const int least = std::max (candidateLag, static_cast<int> (std::lround (0.008 * rate_)));
        lowerCached_ = candidateLag < 1 || span < least || periodicityAt (hz, span) < kAloneClean;
        return lowerCached_;
    }
    const int longest =
        std::min ({ available - window, static_cast<int> (rate_ / lowestHz_) + 1, ringMask_ / 2 });
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
        bool lower =
            !lowRange && own < kOwnClean && p >= kLongerPeriodic && p > own + kLongerMargin;
        if (lowRange && p >= kLongerPeriodic)
        {
            // In a low range, a string whose strongest partial was named first repeats nearly
            // as well at that partial's period as at its own. Over the same stretch, with the
            // note's period taken exactly: the longer period leaves less than half of what the
            // note's own leaves unexplained, and that is more than 1 % of the signal.
            const float left = 1.0f - periodicityAt (hz, w);
            lower = left > kLeftOver && 1.0f - p < 0.5f * left;
        }
        if (lower)
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
        static_cast<float> (std::max (8.0 * base / static_cast<double> (baseSpan), 1.0e-20));
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
    if (hz < minNoteHz_ || periodicityAt (hz) < kPickedClarity)
        return false;
    const double since = static_cast<double> (now_ - pickAt_) / sampleRate_;
    // The automatic range, before it has heard a string under a guitar's: a note down there
    // must hold for a third period (two strings ringing together repeat at such a period for a
    // moment). The first one comes a period late, once; then the range knows.
    const double periods = unheardLow (hz) ? 3.0 : 2.0;
    if (since < std::max (periods / static_cast<double> (hz) + kTrackerMarginSeconds,
                          kTrackerMinSettleSeconds))
        return false;
    // In a range under a guitar's, a low string picked again takes two of its long periods to
    // be named again, and the tracker reads one thing after another meanwhile. A note above the
    // lowest string's octave must account for the signal since the pick (up to one period of
    // that string), not only for its own last period.
    if (openHz_ < kLowestOpenHz && static_cast<double> (hz) >= 1.9 * openHz_)
    {
        const auto lag = static_cast<int> (std::lround (rate_ / static_cast<double> (hz)));
        const int available = static_cast<int> ((now_ - pickAt_) / factor_) - attackSkip_;
        const int span = std::min (available - lag, static_cast<int> (rate_ / openHz_));
        if (lag < 1 || span < lag || shortPeriodicity (lag, span) < kPickedClarity)
            return false;
    }
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
    if (pickChecking_ &&
        now_ - pickAt_ >=
            (settings_.timing == Timing::fastest ? pickLevelSamples_ : weakCheckSamples_))
    {
        // The pick's level is its loudest millisecond from 3 to 12 ms on, which only grows as
        // those pass: in Fastest, a pick that has shown its energy already (a note from
        // silence, a pick well over a faded string) need not wait for the rest.
        const float level = pickLevelDb();
        const bool fretting =
            level < noteLevelDb_ - kFadedDropDb && level < preHeldDb_ + kLockRiseDb;
        const bool damped = pickWeak_ && level < preLevelDb_ - kWeakPickDropDb;
        if (now_ - pickAt_ >= weakCheckSamples_ || (!damped && !fretting))
        {
            pickChecking_ = false;
            pickWeak_ = false;
            if (damped || fretting)
            {
                pickFresh_ = false;
                lastPickAt_ = pickAt_ = previousPickAt_;
            }
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

    // The early reading (Fastest): the note the signal since the pick already shows, before the
    // tracker has had its two periods.
    float earlyHz = 0.0f;
    const int earlyQ = settings_.timing == Timing::fastest ? earlyNoteFor (earlyHz) : -1;

    if (note_ < 0)
    {
        // From silence a note needs a pick behind it (its level checked, when it may be the
        // fretting hand), a level over the gate and a settled pitch. A mute breaks the period
        // too: when the level has fallen far under the note before a pick seen only that way,
        // and what is heard now is a string that rings in sympathy with that note (it shares
        // its partials), the break was the mute and the string only rang on under it.
        // The early reading starts a note only for a pick as loud as this player's picks (a
        // softer one may be a finger's touch, whose push sounds the string's own note: it
        // waits for the tracker and the rules below), and the note the string already had
        // only for a pick that drowned what rang (the note just ended may still be all there
        // is to hear).
        const bool early = earlyQ >= 0 && levelDb_ >= gateLevelDb_ && !muted() &&
                           playingCount_ > 0 && attackLevelDb() >= playingDb_ - kTouchBelowDb &&
                           (!earlyHeld_ || pickLevelDb() >= preLevelDb_ + kDominantPickDb);
        int start = earlyQ;
        float startHz = earlyHz;
        if (!early)
        {
            const bool sympathy = pickWeakSource_ && prevNote_ >= 0 && q >= 0 &&
                                  levelDb_ < preLevelDb_ - kWeakPickDropDb &&
                                  partialOf (q, prevNote_);
            if (!voiced || !recentPick || pickChecking_ || levelDb_ < gateLevelDb_ || muted() ||
                sympathy || !pitchSettled())
            {
                candidate_ = -1;
                return;
            }
            candidateSamples_ = q == candidate_ ? candidateSamples_ + 1 : 1;
            candidate_ = q;
            if (candidateSamples_ < confirmSamples_ || !pickedNoteReady (heard))
                return;
            start = q;
            startHz = heard;
        }
        // Before the first note nothing says how loud this player is: a note must stand
        // kFirstNoteMarginDb out of the input's noise (the hand on the guitar, a string ringing
        // in sympathy, stay under it).
        const float level = attackLevelDb();
        if (playingCount_ == 0 && settings_.gateMode == GateMode::automatic &&
            level < noiseDb_ + kFirstNoteMarginDb)
        {
            pickFresh_ = false;
            candidate_ = -1;
            return;
        }
        // After it, a pick far under how this player picks (under the bottom of the velocity
        // scale) is not a note: a brush of the hand, a string ringing in sympathy. It still
        // counts towards how this player picks, so a passage played that much softer comes
        // through after a while.
        if (playingCount_ > 0 && level < velocityTopDb (level) - kVelocitySpanDb)
        {
            rememberPick (level);
            pickFresh_ = false;
            candidate_ = -1;
            return;
        }
        // A finger touching the string before it plucks it is a pick too, 10-60 ms ahead of the
        // pluck and 15-30 dB under it. From silence, a pick kTouchBelowDb under how this player
        // picks, or the first one (nothing says yet how they pick), waits until kTouchWaitSeconds
        // after it: a louder pick by then replaces it. And a louder attack starting right now is
        // a pick about to be seen: no note starts in the middle of it.
        if ((playingCount_ == 0 || level < playingDb_ - kTouchBelowDb) &&
            now_ - pickAt_ < touchWaitSamples_)
            return;
        if (levelDb_ > attackBeforeDb() + kDominantPickDb)
            return;
        stepDirection_ = 0;
        heardHz_ = startHz;
        earlyStart_ = early;
        startNote (start, velocityAt (level), pickAt_, Why::pick);
        if (early)
            startedEarly (startHz);
        pickFresh_ = false;
        candidate_ = -1;
        return;
    }

    // A new note picked over the sounding one, on the early reading. The same note again needs
    // the pick to show as a new waveform (the note's periodicity broke): a pick's noise over a
    // sounding note leaves the note as it was.
    const bool again = earlyQ == note_;
    if (earlyQ >= 0 && !bendReaches (earlyQ) &&
        (!again || (earlyHeld_ && leastPeriodicity_ < kRetriggerPeriodic)))
    {
        const int velocity = velocityAt (pickLevelDb());
        stepDirection_ = 0;
        stepAt_ = now_;
        endNote (pickAt_, Why::replaced);
        heardHz_ = earlyHz;
        earlyStart_ = true;
        startNote (earlyQ, velocity, pickAt_, again ? Why::repick : Why::pick);
        startedEarly (earlyHz);
        pickFresh_ = false;
        candidate_ = -1;
        return;
    }
    // A note started early: until the tracker has had time to read it, what the tracker still
    // holds (nothing, the note before, a partial on its way to the note) says nothing about it.
    // The tracker counts its two periods from the latest pick it saw itself, which may be later
    // than the one this note is dated from (the swings of a low string's attack).
    const int trackerSince = tracker_.samplesSincePick();
    const bool timeUp = now_ >= earlyLimit_ || trackerSince < 0 || trackerSince >= earlyGrace_;
    // The tracker names the note: after the pick, or still, once it has had its time.
    const bool read = q == note_ && (trackerMoved_ || timeUp);
    if (earlyNote_ && (read || timeUp))
    {
        // The tracker reads it too (or has had its time): from here on the note is like any
        // other, and its attack, complete by now, tells how hard this player picks.
        earlyNote_ = false;
        rememberPick (noteLevelDb_);
        if (read)
        {
            periodicHz_ = 0.0f;
            bendSettling_ = bendSettleTicks_;
        }
        // If the tracker has not named it, and the stretch, twice as long by now, no longer
        // repeats at its period, the early reading was wrong: whatever the tracker names next
        // may replace it, the note before the pick too.
        earlyDoubt_ = !read && !earlyStillRepeats (noteHz_);
    }
    if (read)
        earlyUnread_ = earlyDoubt_ = false;

    // A sounding note ends when the tracker hears nothing, the level falls under the gate or
    // kDecayOffDb under the note's attack (it is no longer heard, however loud the input), or it
    // drops faster than any string decays (a mute).
    const bool underGate = levelDb_ < gateLevelDb_ - kOffHysteresisDb;
    const bool decayed = levelDb_ < noteLevelDb_ - kDecayOffDb;
    // (A note started early that the tracker has not named yet is not one it has lost: the
    // tracker may have let go of the note before, and lock onto this one a moment later.)
    const bool unheard = !voiced && !earlyNote_ && !(earlyUnread_ && now_ < earlyLimit_);
    if (unheard || underGate || decayed || muted())
    {
        endNote (now_, unheard     ? Why::unvoiced
                       : underGate ? Why::gate
                       : decayed   ? Why::decayed
                                   : Why::mute);
        candidate_ = -1;
        return;
    }
    if (earlyNote_)
    {
        candidate_ = -1;
        return;
    }

    if (q == note_)
        noteHz_ = heard; // follows vibrato and bends, for the slip check
    // Automatic range: a note this low that has lasted is one of this instrument's strings.
    if (!noteLearned_ && now_ - noteStart_ >= learnSamples_ && q == note_)
    {
        noteLearned_ = true;
        if (settings_.lowNote == LowNote::automatic && periodicity_ >= kPeriodicHigh)
        {
            const auto before = learned_;
            if (noteHz_ < kLearnFSharp1Hz)
                learned_ = LowNote::fSharp1;
            else if (noteHz_ < kLearnB1Hz && learned_ == LowNote::e2)
                learned_ = LowNote::b1;
            if (learned_ != before)
                applyRange();
        }
    }
    if (settings_.mode == Mode::notesAndBend)
        followBend();
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
            if ((!pickFresh_ && q == prevNote_ && !earlyDoubt_) ||
                harmonicSlip (heard, !pickFresh_))
            {
                candidate_ = -1;
                return;
            }
            // A pick seen only as the note losing its period, that added no energy, is the
            // string taking another fret: a hammer-on, a pull-off, a step of a slide.
            const bool fretChange = pickWeakSource_ &&
                                    std::abs (q - note_) <= kMaxLegatoSemitones &&
                                    pickLevelDb() < preHeldDb_ + kLockRiseDb;
            if (bendReaches (q))
            {
                bendToFret (q, heard, recentPeakDb());
                pickFresh_ = false;
                candidate_ = -1;
                return;
            }
            // A slide's frets come faster than notes: the next note waits until one is held.
            if (fretChange && slideStep (q) && candidateSamples_ < slideConfirmSamples_)
                return;
            stepDirection_ = fretChange ? (q > note_ ? 1 : -1) : 0;
            stepAt_ = now_;
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
        if (sustainedDb() < recent - kLegatoDropDb || recent < noteLevelDb_ - kFadedDropDb)
        {
            endNote (now_, Why::damped);
            candidate_ = -1;
            return;
        }
        // (Nor a note under any string the automatic range has heard picked.)
        if (std::abs (q - note_) > kMaxLegatoSemitones || unheardLow (heard))
        {
            candidate_ = -1;
            return;
        }
        // Notes + Bend: a string bent across the semitone, inside the bend's range, is still
        // the note that was picked; past the range, the next note starts.
        if (bendReaches (q))
        {
            bendToFret (q, heard, recent);
            candidate_ = -1;
            return;
        }
        // A slide's frets come faster than notes: the next note waits until one is held.
        if (slideStep (q) && candidateSamples_ < slideConfirmSamples_)
            return;
        stepDirection_ = q > note_ ? 1 : -1;
        stepAt_ = now_;
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
        stepDirection_ = 0;
        endNote (pickAt_, Why::replaced);
        startNote (q, velocity, pickAt_, Why::repick);
        pickFresh_ = false;
        return;
    }
}

float GuitarToMidi::bendCentsFrom (int note) const noexcept
{
    // The tracker's reading as its estimator follows it: it moves with the string without a
    // break (hz() changes its source when a note holds still, a step of some cents).
    const float hz = tracker_.followedHz();
    if (hz <= 0.0f)
        return 0.0f;
    return 100.0f * (midiPitch (hz) + 12.0f * static_cast<float> (settings_.octave) -
                     static_cast<float> (note));
}

bool GuitarToMidi::unheardLow (float hz) const noexcept
{
    return settings_.lowNote == LowNote::automatic && hz < kRanges[0].minNoteHz &&
           (learned_ == LowNote::e2 || (learned_ == LowNote::b1 && hz < kRanges[1].minNoteHz));
}

bool GuitarToMidi::bendReaches (int q) const noexcept
{
    // The bend followed the pitch all the way to this fret: the string was bent there. A fret
    // taken at once (a hammer-on, a pull-off, a slide's step) cannot be told from a soft pick on
    // the same string by anything the signal shows reliably, and a picked note that came out as
    // a bend would have lost its attack: those are notes, as in Notes mode.
    if (settings_.mode != Mode::notesAndBend || sentNote_ < 0 ||
        std::abs (q - sentNote_) > settings_.bendRange)
        return false;
    return std::abs (bendCentsFrom (note_) - bendCentre_ - bendSeen_) <= kBendJumpCents;
}

bool GuitarToMidi::slideStep (int q) const noexcept
{
    return stepDirection_ != 0 && (q > note_ ? 1 : -1) == stepDirection_ &&
           now_ - stepAt_ < slideWindowSamples_;
}

void GuitarToMidi::bendToFret (int q, float heard, float recentDb) noexcept
{
    // The bend carries on from where it is: only the fret it is counted from changes.
    startWhy_ = Why::legato;
    note_ = q;
    noteHz_ = heard;
    periodicHz_ = 0.0f;
    noteLevelDb_ = std::max (noteLevelDb_, recentDb);
    bendSeen_ = bendStill_ = bendCentsFrom (note_) - bendCentre_;
    bendOutside_ = bendInside_ = bendStillTicks_ = 0;
    bendOn_ = true;
    bendTarget_ = 100.0f * static_cast<float> (note_ - sentNote_) + bendSeen_;
}

void GuitarToMidi::followBend() noexcept
{
    if (now_ - lastBendAt_ < bendIntervalSamples_)
        return;
    lastBendAt_ = now_;
    // How far the pitch is from where this note sits. A move of more than kBendJumpCents in one
    // interval is the string taking another fret (or the tracker's reading slipping): the legato
    // rules decide what that is, and the bend waits where it was. And a pitch that moves while
    // the level falls is the hand letting go of the string, or moving on to the next fret before
    // the next pick: no bend starts there.
    const float raw = bendCentsFrom (note_);
    if (bendSettling_ > 0)
    {
        // A note's first kBendSettleSeconds: the reading may still be on its way from where the
        // string was (the fret before, the attack), so where it is now is the note's centre.
        --bendSettling_;
        bendCentre_ = std::clamp (raw, -kCentreMaxCents, kCentreMaxCents);
        bendSeen_ = bendStill_ = 0.0f;
        bendOutside_ = bendStillTicks_ = 0;
        return;
    }
    const float d = raw - bendCentre_;
    const bool lettingGo = !bendOn_ && sustainedDb (0.0) < recentPeakDb() - kBendFadeDb;
    if (lettingGo)
        bendOutside_ = 0;
    else if (std::abs (d - bendSeen_) <= kBendJumpCents || std::abs (d) <= kBendOnCents)
    {
        const bool followed = std::abs (d - bendSeen_) <= kBendJumpCents;
        bendSeen_ = d;
        if (std::abs (d - bendStill_) <= kBendStillCents)
            ++bendStillTicks_;
        else
        {
            bendStill_ = d;
            bendStillTicks_ = 0;
        }
        if (!bendOn_)
        {
            if (std::abs (d) > kBendOnCents)
            {
                // Past the dead band for kBendOnSeconds, or so far past it, followed all the
                // way, that it can only be a bend.
                if (++bendOutside_ >= bendOnTicks_ || (followed && std::abs (d) > kBendSureCents))
                {
                    bendOn_ = true;
                    bendInside_ = 0;
                    bendStillTicks_ = 0;
                }
            }
            else
            {
                // A held note: its centre follows the string's own slow drift (the pitch sinking
                // as the note decays, a fret pressed a little sharp), so none of it is bend.
                bendOutside_ = 0;
                bendCentre_ =
                    std::clamp (bendCentre_ + std::clamp (d, -centrePerTick_, centrePerTick_),
                                -kCentreMaxCents, kCentreMaxCents);
            }
        }
        else
        {
            // Back at the note: inside kBendOffCents for kBendOffSeconds...
            bendInside_ = std::abs (d) < kBendOffCents ? bendInside_ + 1 : 0;
            // ...or the pitch has come to rest within a fret's tuning of its semitone: that is
            // where the note sits (a string let down a little off where it was, a note whose
            // first reading was off), and its centre from now on. A bend held rests further out.
            const bool resting = bendStillTicks_ >= bendStillNeed_ && note_ == sentNote_ &&
                                 std::abs (raw) <= kCentreMaxCents;
            if (bendInside_ >= bendOffTicks_ || resting)
            {
                bendOn_ = false;
                bendOutside_ = 0;
                if (resting)
                {
                    bendCentre_ = raw;
                    bendSeen_ = bendStill_ = 0.0f;
                }
            }
        }
    }
    const float limit = 100.0f * static_cast<float> (settings_.bendRange);
    const float fret = 100.0f * static_cast<float> (note_ - sentNote_);
    const float want = std::clamp (fret + (bendOn_ ? bendSeen_ : 0.0f), -limit, limit);
    // The reading's own small moves are not the string's.
    if (!bendOn_ || std::abs (want - bendTarget_) >= kBendHoldCents)
        bendTarget_ = want;
    if (bendTarget_ == bendGlide_)
        return;
    bendGlide_ += std::clamp (bendTarget_ - bendGlide_, -bendSlewPerTick_, bendSlewPerTick_);
    if (std::abs (bendGlide_ - bendOut_) >= kBendStepCents || bendGlide_ == bendTarget_)
        sendBend (bendGlide_, now_);
}

void GuitarToMidi::applyRange() noexcept
{
    const auto& r = kRanges[static_cast<int> (lowNoteInEffect()) - 1];
    openHz_ = r.openHz;
    lowestHz_ = r.lowestHz;
    // Automatic hears any note down to the floor, whatever it has learned so far (that is how
    // it learns); a chosen range hears its own notes only.
    const bool automatic = settings_.lowNote == LowNote::automatic;
    minNoteHz_ = automatic ? kRanges[2].minNoteHz : r.minNoteHz;
    auto options = tracker_.options();
    options.lowestHz = automatic ? 0.0f : r.searchHz;
    options.rumbleHz = r.rumbleHz;
    tracker_.setOptions (options);
    // The early reading looks down to the range's lowest note, for two of its periods.
    earlyLagLimit_ = std::min (static_cast<int> (std::ceil (rate_ / lowestHz_)) + 2,
                               static_cast<int> (early_[0].r.size()) - 2);
    earlySpanSamples_ = earlySkipSamples_ + samples (2.0 / lowestHz_ + 0.01, sampleRate_);
    earlyPickAt_ = -1;
}

LowNote GuitarToMidi::lowNoteInEffect() const noexcept
{
    return settings_.lowNote == LowNote::automatic ? learned_ : settings_.lowNote;
}

void GuitarToMidi::forgetLowNote() noexcept
{
    setLearnedLowNote (LowNote::e2);
}

void GuitarToMidi::setLearnedLowNote (LowNote learned) noexcept
{
    learned_ = learned == LowNote::automatic ? LowNote::e2 : learned;
    applyRange();
}

void GuitarToMidi::startNote (int note, int velocity, std::int64_t reference, Why why) noexcept
{
    startWhy_ = why;
    noteStart_ = reference;
    noteLearned_ = false;
    earlyNote_ = earlyUnread_ = false;
    earlyDoubt_ = false;
    // A legato note keeps the energy the pick gave the string (or what it added to it).
    noteLevelDb_ = why == Why::legato ? std::max (noteLevelDb_, recentPeakDb())
                   : why == Why::pick ? attackLevelDb()
                                      : pickLevelDb();
    // A pick that starts a note tells how hard this player picks (a correction reuses its pick).
    // A note started early has not reached its attack yet: it tells once the tracker reads it.
    if ((why == Why::pick || why == Why::repick) && !earlyStart_)
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
            note_ = sentNote_ = note;
            noteHz_ = heardHz_;
            periodicHz_ = 0.0f;
            noteChannel_ = settings_.channel;
            noteDue_ = now_;
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
    note_ = sentNote_ = note;
    noteHz_ = heardHz_;
    periodicHz_ = 0.0f;
    noteChannel_ = settings_.channel;
    noteDue_ = due;
    lastVelocity_ = velocity;
    if (settings_.mode == Mode::notesAndBend)
    {
        // Every note starts with the bend centred: what the note before left of its bend (still
        // waiting, in Steady) is dropped, and the note's own centre is where it sits now.
        for (int k = queued_ - 1; k >= 0; --k)
        {
            const auto& p = queue_[static_cast<std::size_t> (k)];
            if (p.event.isPitchBend() && p.due >= due)
            {
                unqueue (k);
                bendSent_ = -1;
            }
        }
        bendOn_ = false;
        bendOutside_ = bendInside_ = bendStillTicks_ = 0;
        bendSettling_ = bendSettleTicks_;
        bendGlide_ = bendSeen_ = bendTarget_ = bendStill_ = 0.0f;
        bendCentre_ = std::clamp (bendCentsFrom (note), -kCentreMaxCents, kCentreMaxCents);
        // Sent with every note-on, not only after a bend: a receiver whose wheel was left off
        // centre by anything else starts this note in tune too.
        bendSent_ = -1;
        sendBend (0.0f, due);
    }
    schedule (due, Event::noteOn (noteChannel_, note, velocity, 0));
    ++notesSent_;
}

void GuitarToMidi::startedEarly (float hz) noexcept
{
    // The tracker judges a period on two of its lengths, a weak fundamental a little later, and
    // changes its reading only once the new one has held: until then it has no say.
    earlyNote_ = earlyUnread_ = true;
    earlyStart_ = false;
    ++earlyNotes_;
    earlyGrace_ = samples (
        std::max (2.0 / static_cast<double> (hz) + kEarlyGraceSeconds, kEarlyMinGraceSeconds),
        sampleRate_);
    earlyLimit_ = now_ + samples (kEarlyLimitSeconds, sampleRate_);
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
    earlyNote_ = earlyUnread_ = false;
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
        const int on = findQueued (true, noteChannel_, sentNote_);
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
            note_ = sentNote_ = -1;
            return;
        }
    }
    schedule (due, Event::noteOff (noteChannel_, sentNote_, 0));
    note_ = sentNote_ = -1;
}

void GuitarToMidi::sendBend (float cents, std::int64_t due) noexcept
{
    const int range = std::max (1, settings_.bendRange);
    const float share = std::clamp (cents / (100.0f * static_cast<float> (range)), -1.0f, 1.0f);
    const int value =
        std::clamp (kBendCentre + static_cast<int> (std::lround (share * 8192.0f)), 0, 16383);
    bendOut_ = cents;
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
    note_ = sentNote_ = -1;
    bridgeNote_ = -1;
    candidate_ = -1;
    pickFresh_ = false;
    earlyNote_ = earlyUnread_ = false;
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
    bendOut_ = bendGlide_ = bendTarget_ = 0.0f;
    bendOn_ = false;
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
