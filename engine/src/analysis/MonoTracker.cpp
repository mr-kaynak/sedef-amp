// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/analysis/MonoTracker.h"

#include "sedef/core/Math.h"

#include <algorithm>
#include <cmath>

namespace sedef::analysis
{
namespace
{
// Estimator.
constexpr double kTargetRate = 7000.0;  // decimated rate is fs / floor (fs / 7 kHz): 7-8 kHz
constexpr double kRumbleHz = 40.0;      // high-pass ahead of everything
constexpr double kAntiAliasHz = 1500.0; // 4th-order Butterworth ahead of the decimation
constexpr double kMinWindowSeconds = 0.004;
constexpr double kHopSeconds = 0.0005;
constexpr float kKeyThreshold = 0.9f;  // MPM: the first key maximum within 90 % of the highest
constexpr float kOctaveMargin = 0.03f; // a multiple of the lag wins if this much more periodic
constexpr double kSilenceEnergy = 1.0e-12;
// Picks: 2 ms power 6 dB over the 40 ms power, then 25 ms before the next. For 40 ms after one,
// the locked note has no say against a new estimate.
constexpr double kFastPowerSeconds = 0.002;
constexpr double kSlowPowerSeconds = 0.04;
constexpr float kOnsetRatio = 4.0f;
constexpr float kRiseRatio = 1.5f;
constexpr double kClickHz = 2000.0;
constexpr double kRefractorySeconds = 0.025;
constexpr double kOnsetWindowSeconds = 0.04;
constexpr double kPickLevelSeconds = 0.01;     // a pick's level is its peak over this long
constexpr double kCleanMarginSeconds = 0.0005; // the detector fires just after the pick starts
constexpr double kAttackSeconds = 0.008;       // and nothing is judged inside the attack itself
// A period whose own frequency carries less than this share of the energy (-30 dB) is a virtual
// fundamental. A bright bridge pickup's low E still has about -17 dB there.
constexpr float kMinFundamentalShare = 0.001f;
// Options::weakFundamentals: a period with at least -45 dB at its fundamental and -17 dB at its
// 7th harmonic is a note. A virtual fundamental has about -50 dB at both (at most -40 dB and
// -19 dB in the attack); a low string plucked near the bridge, -40 to -30 dB at the fundamental
// and -10 dB at the 7th harmonic.
constexpr float kWeakFundamentalShare = 3.0e-5f;
constexpr float kSeventhShare = 0.02f;
constexpr int kWitnessHarmonic = 7;
// ...and a period with under 1 % of its energy at its fundamental is judged 2.5 ms later than
// others: until then the attack at the edge of its two-period window leaks into that bin (a
// virtual fundamental can show 0.3 % there at the usual time, and 0.006 % 2 ms on).
constexpr float kCleanShare = 0.01f;
constexpr double kWeakMarginSeconds = 0.003;
// Options::fingerPicks: for this long after a pick, only a pick this much louder than its peak.
constexpr double kFingerWindowSeconds = 0.06;
constexpr float kLouderPick = 4.0f; // 6 dB

// Note logic.
constexpr float kLockClarity = 0.8f;     // periodic enough to lock or to change note
constexpr float kHoldClarity = 0.5f;     // periodic enough to keep following the locked note
constexpr float kPickedClarity = 0.7f;   // periodic enough to take over just after a pick
constexpr float kSameNoteCents = 100.0f; // moves within a semitone per hop are followed
constexpr float kAgreeCents = 50.0f;     // two estimates of the same new note
constexpr float kOctaveToleranceCents = 60.0f;
constexpr double kHoldSeconds = 0.03;
constexpr double kSwitchSeconds = 0.002;
constexpr double kOctaveSwitchSeconds = 0.008;
constexpr float kFollow = 0.35f; // per-hop smoothing of the locked frequency
// How much more periodic another note must be: at least 0.05, and 40 % of what the locked note
// lacks (0.76 needs 0.86, 0.95 needs 0.97).
constexpr float kChallengeMargin = 0.05f;
constexpr float kChallengeShare = 0.4f;

// Steered resonators: bandwidth per pole as a fraction of the note. Wide when a note starts, so
// they settle within about a period, then narrow over three periods, so a neighbouring string
// that still rings (a fourth or a fifth away) is held 15-30 dB down.
constexpr float kWideBandwidth = 0.5f;
constexpr float kNarrowBandwidth = 0.2f;
constexpr float kNarrowingPeriods = 3.0f;
constexpr float kMinResonatorHz = 10.0f;
constexpr float kPartialPeriods = 4.0f; // smoothing of the measured partial
// While a note sustains, the centre moves only for a real change of pitch (a bend): every move
// turns the output's phase a little, so estimator jitter must not move it.
constexpr float kSteerDeadbandCents = 35.0f;
constexpr float kReportCents = 20.0f;

float centsBetween (float a, float b) noexcept
{
    return 1200.0f * std::log2 (a / b);
}
} // namespace

void MonoTracker::prepare (double sampleRate, float minHz, float maxHz)
{
    sampleRate_ = sampleRate;
    factor_ = std::max (1, static_cast<int> (sampleRate / kTargetRate));
    rate_ = sampleRate / factor_;
    maxHz_ = std::min (maxHz, static_cast<float> (0.2 * rate_));
    minHz_ = std::clamp (minHz, 20.0f, 0.5f * maxHz_);
    minLag_ = std::max (2, static_cast<int> (std::floor (rate_ / maxHz_)));
    maxLag_ = static_cast<int> (std::ceil (rate_ / minHz_)) + 2;
    minWindow_ = std::max (4, static_cast<int> (std::lround (kMinWindowSeconds * rate_)));
    hop_ = std::max (1, static_cast<int> (std::lround (kHopSeconds * rate_)));
    const double hopSeconds = hop_ / rate_;
    maxHoldHops_ = static_cast<int> (std::lround (kHoldSeconds / hopSeconds));
    octaveSwitchHops_ = static_cast<int> (std::lround (kOctaveSwitchSeconds / hopSeconds));
    fastCoeff_ = static_cast<float> (1.0 - std::exp (-1.0 / (kFastPowerSeconds * rate_)));
    clickFastCoeff_ = static_cast<float> (1.0 - std::exp (-1.0 / (kFastPowerSeconds * sampleRate)));
    clickSlowCoeff_ = static_cast<float> (1.0 - std::exp (-1.0 / (kSlowPowerSeconds * sampleRate)));
    pick_.c = design::highpass (sampleRate, std::min (kClickHz, 0.4 * sampleRate), 0.7071);
    slowCoeff_ = static_cast<float> (1.0 - std::exp (-1.0 / (kSlowPowerSeconds * rate_)));
    refractorySamples_ = static_cast<int> (std::lround (kRefractorySeconds * rate_));
    onsetWindowHops_ = static_cast<int> (std::lround (kOnsetWindowSeconds / hopSeconds));
    pickWindowSamples_ = static_cast<int> (std::lround (kPickLevelSeconds * rate_));
    lockHops_ = 2;
    switchHops_ = std::max (2, static_cast<int> (std::lround (kSwitchSeconds / hopSeconds)));

    // The longest read is the longest window plus the longest lag behind the newest sample.
    historySize_ = 2 * std::max (maxLag_, minWindow_) + 8;
    history_.assign (static_cast<size_t> (2 * historySize_), 0.0f);
    r_.assign (static_cast<size_t> (maxLag_ + 2), 0.0);
    m_.assign (static_cast<size_t> (maxLag_ + 2), 0.0);
    nsdf_.assign (static_cast<size_t> (maxLag_ + 2), 0.0f);
    setOptions (options_);

    hp_.c = design::highpass (sampleRate, kRumbleHz, 0.5);
    const double aa = std::min (kAntiAliasHz, 0.2 * rate_);
    aa1_.c = design::lowpass (sampleRate, aa, 0.5411961);
    aa2_.c = design::lowpass (sampleRate, aa, 1.3065630);
    reset();
}

void MonoTracker::setOptions (const Options& options) noexcept
{
    if (options.rumbleHz != options_.rumbleHz)
        hp_.c = design::highpass (sampleRate_,
                                  options.rumbleHz > 0.0f ? options.rumbleHz : kRumbleHz, 0.5);
    options_ = options;
    searchLag_ =
        options.lowestHz > 0.0f
            ? std::min (
                  maxLag_,
                  static_cast<int> (std::ceil (rate_ / static_cast<double> (options.lowestHz))) + 2)
            : maxLag_;
}

void MonoTracker::reset() noexcept
{
    for (auto* s : { &hp_, &aa1_, &aa2_, &pick_ })
        s->s0 = s->s1 = 0.0f;
    std::fill (history_.begin(), history_.end(), 0.0f);
    std::fill (r_.begin(), r_.end(), 0.0);
    std::fill (m_.begin(), m_.end(), 0.0);
    std::fill (nsdf_.begin(), nsdf_.end(), 0.0f);
    write_ = 0;
    phase_ = 0;
    hopCount_ = 0;
    estimateHz_ = clarity_ = 0.0f;
    estimateWeak_ = false;
    candidateHz_ = 0.0f;
    candidateHops_ = 0;
    locked_ = false;
    lockedHz_ = 0.0f;
    holdHops_ = 0;
    steerHz_ = 0.0f;
    steerFraction_ = 0.0f;
    hopsSinceNote_ = 0;
    partialHz_ = 0.0f;
    partialSettled_ = false;
    turnRe_ = turnIm_ = 0.0f;
    fastPower_ = slowPower_ = 0.0f;
    clickFast_ = clickSlow_ = 0.0f;
    refractory_ = 0;
    onsetHops_ = 0;
    rising_ = 0;
    climb_ = 0;
    samplesSinceOnset_ = 1 << 24;
    picked_ = false;
    pickCount_ = 0;
    pickPeak_ = 0.0f;
    pickWindow_ = 0;
    event_ = Event::none;
    zRe1_ = zIm1_ = zRe2_ = zIm2_ = 0.0f;
    steer (200.0f);
}

void MonoTracker::steer (float hz) noexcept
{
    // Narrow from wide to narrow over the first periods of the note.
    const float periods =
        static_cast<float> (hopsSinceNote_) * static_cast<float> (hop_ / rate_) * hz;
    const float share = std::clamp (periods / kNarrowingPeriods, 0.0f, 1.0f);
    const float fraction = kWideBandwidth + share * (kNarrowBandwidth - kWideBandwidth);
    if (hz <= 0.0f || (std::abs (hz - steerHz_) < 0.0005f * steerHz_ &&
                       std::abs (fraction - steerFraction_) < 0.01f))
        return;
    steerHz_ = hz;
    steerFraction_ = fraction;
    const double bandwidth = std::max (kMinResonatorHz, fraction * hz);
    const double r = std::exp (-kPiD * bandwidth / sampleRate_);
    const double w = kTwoPiD * hz / sampleRate_;
    pRe_ = static_cast<float> (r * std::cos (w));
    pIm_ = static_cast<float> (r * std::sin (w));
    gain_ = static_cast<float> (1.0 - r);
}

int MonoTracker::samplesSincePick() const noexcept
{
    return picked_ ? samplesSinceOnset_ * factor_ + phase_ : -1;
}

float MonoTracker::pickLevel() const noexcept
{
    return std::sqrt (pickPeak_);
}

void MonoTracker::push (float x) noexcept
{
    event_ = Event::none;
    const float band = aa2_.process (aa1_.process (hp_.process (x)));
    // The pick's click, above the strings' strongest partials: it shows a new pick even when the
    // low band hardly rises (a note picked over a louder ringing string).
    const float click = pick_.process (x);
    clickFast_ += clickFastCoeff_ * (click * click - clickFast_);
    clickSlow_ += clickSlowCoeff_ * (click * click - clickSlow_);

    // Two complex one-poles, unity gain and zero phase at the steered frequency.
    const float lastRe = zRe2_, lastIm = zIm2_;
    const float re1 = pRe_ * zRe1_ - pIm_ * zIm1_ + gain_ * band;
    const float im1 = pRe_ * zIm1_ + pIm_ * zRe1_;
    zRe1_ = re1;
    zIm1_ = im1;
    const float re2 = pRe_ * zRe2_ - pIm_ * zIm2_ + gain_ * re1;
    const float im2 = pRe_ * zIm2_ + pIm_ * zRe2_ + gain_ * im1;
    zRe2_ = re2;
    zIm2_ = im2;
    // The output's turn per sample, summed over the hop: z[n] conj (z[n-1]).
    turnRe_ += re2 * lastRe + im2 * lastIm;
    turnIm_ += im2 * lastRe - re2 * lastIm;

    if (++phase_ < factor_)
        return;
    phase_ = 0;
    updateLags (band);
    if (++hopCount_ < hop_)
        return;
    hopCount_ = 0;
    measurePartial();
    estimate();
    track();

    // Silence decays every state towards zero; keep it out of the denormal range.
    for (auto* s : { &hp_, &aa1_, &aa2_, &pick_ })
    {
        s->s0 = std::abs (s->s0) < 1.0e-20f ? 0.0f : s->s0;
        s->s1 = std::abs (s->s1) < 1.0e-20f ? 0.0f : s->s1;
    }
    for (auto* v : { &zRe1_, &zIm1_, &zRe2_, &zIm2_, &clickFast_, &clickSlow_ })
        *v = std::abs (*v) < 1.0e-20f ? 0.0f : *v;
}

void MonoTracker::measurePartial() noexcept
{
    // The resonators' own output says where the partial they pass is, to a fraction of a cent:
    // its turn per sample, smoothed over a few periods (which also averages out the beating with
    // a neighbouring string). It is reported, never fed back to the steering: moving a narrow
    // resonator's centre turns its output's phase, and a loop through that phase rings.
    const float turn = std::atan2 (turnIm_, turnRe_);
    const bool valid = turnRe_ * turnRe_ + turnIm_ * turnIm_ > 1.0e-30f;
    turnRe_ = turnIm_ = 0.0f;
    if (!valid || steerHz_ <= 0.0f)
        return;
    const auto hz = static_cast<float> (turn * sampleRate_ / kTwoPiD);
    // A slip of the phase (the output passing near zero) is not a frequency.
    if (std::abs (hz - steerHz_) > 0.1f * steerHz_)
        return;
    const float hopSeconds = static_cast<float> (hop_ / rate_);
    const float k = 1.0f - std::exp (-hopSeconds * steerHz_ / kPartialPeriods);
    partialHz_ += k * (hz - partialHz_);
}

void MonoTracker::updateLags (float x) noexcept
{
    history_[static_cast<size_t> (write_)] = x;
    history_[static_cast<size_t> (write_ + historySize_)] = x;
    // h[-k] is the sample k decimated steps ago.
    const float* h = history_.data() + write_ + historySize_;
    write_ = write_ + 1 < historySize_ ? write_ + 1 : 0;

    // Picks: the short-term power jumps 6 dB over the longer-term one.
    const float power = x * x;
    const float lastFast = fastPower_;
    fastPower_ += fastCoeff_ * (power - fastPower_);
    climb_ = fastPower_ > lastFast ? climb_ + 1 : 0;
    slowPower_ += slowCoeff_ * (power - slowPower_);
    if (samplesSinceOnset_ < 1 << 24)
        ++samplesSinceOnset_;
    // How long the power has been rising: a pick is dated from where its rise began, not from
    // where it crossed the detection ratio a few milliseconds later.
    const bool lowRising = fastPower_ > kRiseRatio * slowPower_;
    const bool clickRising = clickFast_ > kRiseRatio * clickSlow_;
    rising_ = lowRising || clickRising ? rising_ + 1 : 0;
    const bool lowPick = fastPower_ > kOnsetRatio * slowPower_ && fastPower_ > options_.onsetFloor;
    const bool clickPick = clickFast_ > kOnsetRatio * clickSlow_ &&
                           clickFast_ > options_.clickFloor && fastPower_ > options_.onsetFloor;
    bool pick = false;
    int since = rising_;
    if (options_.fingerPicks && picked_ &&
        static_cast<double> (samplesSinceOnset_) < kFingerWindowSeconds * rate_)
    {
        if (refractory_ > 0)
            --refractory_;
        if (pickWindow_ == 0 && fastPower_ > kLouderPick * pickPeak_ &&
            fastPower_ > kOnsetRatio * slowPower_ && fastPower_ > options_.onsetFloor)
        {
            // Dated from where this climb of the power began.
            pick = true;
            since = climb_;
        }
    }
    else if (refractory_ > 0)
        --refractory_;
    else
        pick = lowPick || clickPick;
    if (pick)
    {
        onsetHops_ = onsetWindowHops_;
        refractory_ = refractorySamples_;
        samplesSinceOnset_ = since;
        picked_ = true;
        ++pickCount_;
        pickPeak_ = 0.0f;
        pickWindow_ = pickWindowSamples_;
    }
    if (pickWindow_ > 0)
    {
        --pickWindow_;
        pickPeak_ = std::max (pickPeak_, fastPower_);
    }

    const double x0 = x;
    const double e0 = x0 * x0;
    for (int lag = 1; lag <= maxLag_; ++lag)
    {
        // The window is one lag long (at least minWindow_): a period is judged on one period.
        const int window = std::max (lag, minWindow_);
        const double a = h[-lag];
        const double b = h[-window];
        const double c = h[-window - lag];
        r_[static_cast<size_t> (lag)] += x0 * a - b * c;
        m_[static_cast<size_t> (lag)] += e0 + a * a - b * b - c * c;
    }
}

void MonoTracker::estimate() noexcept
{
    for (int lag = 1; lag <= searchLag_; ++lag)
    {
        const double m = m_[static_cast<size_t> (lag)];
        nsdf_[static_cast<size_t> (lag)] =
            m > kSilenceEnergy ? static_cast<float> (2.0 * r_[static_cast<size_t> (lag)] / m)
                               : 0.0f;
    }

    // Key maxima (MPM): the highest point of every positive lobe after the first negative-going
    // zero crossing (the lobe around lag 0 is skipped), placed between lags by a parabola. The
    // peaks of a bright note are sharp, so the grid alone would favour multiples of the period
    // that happen to fall closer to whole lags.
    int count = 0;
    bool pastZero = false, inLobe = false;
    int best = 0;
    const auto addKey = [this, &count] (int lag) {
        if (lag < minLag_ || lag >= searchLag_ || count >= static_cast<int> (keys_.size()))
            return;
        const float y0 = nsdf_[static_cast<size_t> (lag - 1)];
        const float y1 = nsdf_[static_cast<size_t> (lag)];
        const float y2 = nsdf_[static_cast<size_t> (lag + 1)];
        const float denom = y0 - 2.0f * y1 + y2;
        float shift = 0.0f, peak = y1;
        if (denom < 0.0f)
        {
            shift = std::clamp (0.5f * (y0 - y2) / denom, -0.5f, 0.5f);
            peak = y1 - 0.25f * (y0 - y2) * shift;
        }
        keys_[static_cast<size_t> (count++)] = { static_cast<float> (lag) + shift, peak };
    };
    for (int lag = 1; lag < searchLag_; ++lag)
    {
        const float v = nsdf_[static_cast<size_t> (lag)];
        if (!pastZero)
        {
            pastZero = v <= 0.0f;
            continue;
        }
        if (v > 0.0f)
        {
            if (!inLobe)
            {
                inLobe = true;
                best = lag;
            }
            else if (v > nsdf_[static_cast<size_t> (best)])
            {
                best = lag;
            }
        }
        else if (inLobe)
        {
            inLobe = false;
            addKey (best);
        }
    }
    if (inLobe && best < searchLag_ - 1)
        addKey (best);

    heldHz_ = 0.0f;
    heldClarity_ = 0.0f;
    if (count == 0)
    {
        estimateHz_ = 0.0f;
        clarity_ = 0.0f;
        return;
    }

    // Continuity: while a note is locked, the most periodic key maximum within a semitone of it
    // is that note, whatever the others say, as long as its fundamental still sounds. (After a
    // jump up an octave the new note repeats at the old period too, but the old fundamental is
    // gone.)
    if (locked_)
    {
        int held = -1;
        for (int k = 0; k < count; ++k)
        {
            const auto& key = keys_[static_cast<size_t> (k)];
            const auto hz = static_cast<float> (rate_) / key.lag;
            if (std::abs (centsBetween (hz, lockedHz_)) < kSameNoteCents &&
                (held < 0 || key.value > keys_[static_cast<size_t> (held)].value))
                held = k;
        }
        if (held >= 0 && fundamentalSounds (keys_[static_cast<size_t> (held)].lag))
        {
            heldHz_ = static_cast<float> (rate_) / keys_[static_cast<size_t> (held)].lag;
            heldClarity_ = keys_[static_cast<size_t> (held)].value;
        }
    }

    float highest = 0.0f;
    for (int k = 0; k < count; ++k)
        highest = std::max (highest, keys_[static_cast<size_t> (k)].value);
    int chosen = 0;
    while (keys_[static_cast<size_t> (chosen)].value < kKeyThreshold * highest)
        ++chosen;

    // Octave-high guard: a strong 2nd (or 3rd) harmonic makes half (a third of) the period look
    // periodic too. Twice (three times) that lag is then clearly more periodic still.
    const KeyMax first = keys_[static_cast<size_t> (chosen)];
    for (int multiple = 2; multiple <= 3; ++multiple)
    {
        const float target = first.lag * static_cast<float> (multiple);
        for (int k = chosen + 1; k < count; ++k)
        {
            const auto& key = keys_[static_cast<size_t> (k)];
            if (std::abs (key.lag - target) <= 0.08f * target &&
                key.value > keys_[static_cast<size_t> (chosen)].value + kOctaveMargin)
                chosen = k;
        }
    }

    // Virtual fundamentals: two notes a fifth or a fourth apart (a ringing string and the new
    // note) repeat together at a period no note has. Nothing sounds at that fundamental, so when
    // the chosen period has no energy at its own frequency, the most periodic of its halves,
    // thirds and quarters that does is the note.
    if (!fundamentalSounds (keys_[static_cast<size_t> (chosen)].lag))
    {
        const float lag = keys_[static_cast<size_t> (chosen)].lag;
        int part = -1;
        for (int k = 0; k < chosen; ++k)
        {
            const auto& key = keys_[static_cast<size_t> (k)];
            bool divides = false;
            for (int parts = 2; parts <= 4; ++parts)
                divides =
                    divides || std::abs (key.lag * static_cast<float> (parts) - lag) <= 0.04f * lag;
            if (divides && key.value >= kHoldClarity &&
                (part < 0 || key.value > keys_[static_cast<size_t> (part)].value) &&
                fundamentalSounds (key.lag))
                part = k;
        }
        if (part < 0)
        {
            estimateHz_ = 0.0f;
            clarity_ = 0.0f;
            return;
        }
        chosen = part;
    }

    const auto& key = keys_[static_cast<size_t> (chosen)];
    estimateHz_ = static_cast<float> (rate_) / key.lag;
    clarity_ = std::clamp (key.value, 0.0f, 1.0f);
    estimateWeak_ = options_.weakFundamentals && fundamentalShare (key.lag) < kCleanShare;
}

bool MonoTracker::fundamentalSounds (float lag) const noexcept
{
    const float share = fundamentalShare (lag);
    if (share >= kMinFundamentalShare)
        return true;
    return options_.weakFundamentals && share >= kWeakFundamentalShare &&
           kWitnessHarmonic * rate_ / static_cast<double> (lag) <= kAntiAliasHz &&
           partialShare (lag, kWitnessHarmonic) >= kSeventhShare;
}

float MonoTracker::fundamentalShare (float lag) const noexcept
{
    return partialShare (lag, 1);
}

float MonoTracker::partialShare (float lag, int harmonic) const noexcept
{
    // The share of the last two periods' energy at a harmonic of the period (one DFT bin over
    // exactly two periods, so every other harmonic of the period, and of half of it, falls on
    // another bin and leaks nothing).
    const int n = std::min (static_cast<int> (std::lround (2.0f * lag)), historySize_ - 2);
    const float* h = history_.data() + (write_ == 0 ? historySize_ - 1 : write_ - 1) + historySize_;
    const double w = kTwoPiD * harmonic / static_cast<double> (lag);
    const double c = std::cos (w), sn = std::sin (w);
    double re = 0.0, im = 0.0, energy = 0.0;
    double rotRe = 1.0, rotIm = 0.0;
    for (int k = 0; k < n; ++k)
    {
        const double x = h[-k];
        re += x * rotRe;
        im += x * rotIm;
        energy += x * x;
        const double nextRe = rotRe * c - rotIm * sn;
        rotIm = rotRe * sn + rotIm * c;
        rotRe = nextRe;
    }
    return energy > kSilenceEnergy ? static_cast<float> (2.0 * (re * re + im * im) / (n * energy))
                                   : 0.0f;
}

void MonoTracker::track() noexcept
{
    if (onsetHops_ > 0)
        --onsetHops_;
    if (hopsSinceNote_ < 1 << 20)
        ++hopsSinceNote_;
    // A period is judged on two of its lengths of signal. Until that much has passed since the
    // last pick, the window still holds the attack, whose step leaks energy into every frequency
    // (and so hides a virtual fundamental), and for a long period what came before the pick. The
    // estimate can be locked onto or switched to only after it, and never inside the first 8 ms
    // of the attack, whose short periods can look periodic.
    const bool found = estimateHz_ > 0.0f;
    const double settleMargin = estimateWeak_ ? kWeakMarginSeconds : kCleanMarginSeconds;
    const bool settled =
        found && static_cast<double> (samplesSinceOnset_) >=
                     std::max (2.0 / estimateHz_ + settleMargin, kAttackSeconds) * rate_;
    // Right after a pick, a new note may be less periodic than usual (the old string still
    // rings under it): it takes over at a lower bar.
    const float bar = locked_ && onsetHops_ > 0 ? kPickedClarity : kLockClarity;
    const bool periodic = found && settled && clarity_ >= bar;
    const bool usable = found && clarity_ >= kHoldClarity;

    const auto countCandidate = [this, periodic] {
        if (periodic && candidateHz_ > 0.0f &&
            std::abs (centsBetween (estimateHz_, candidateHz_)) < kAgreeCents)
        {
            ++candidateHops_;
        }
        else
        {
            candidateHz_ = estimateHz_;
            candidateHops_ = periodic ? 1 : 0;
        }
        if (periodic)
            candidateHz_ = estimateHz_;
    };

    if (!locked_)
    {
        hopsSinceNote_ = 0; // wide while searching
        countCandidate();
        if (candidateHops_ >= lockHops_)
        {
            locked_ = true;
            event_ = Event::noteOn;
            lockedHz_ = estimateHz_;
            holdHops_ = 0;
            candidateHops_ = 0;
            hopsSinceNote_ = 0;
            partialHz_ = lockedHz_;
            partialSettled_ = false;
            ++noteId_;
        }
        // Steer ahead of the lock, so the resonators have settled when it comes.
        if (usable)
            steer (locked_ ? lockedHz_ : estimateHz_);
        return;
    }

    // A different note takes over only when it is clearly more periodic than the locked one, and
    // stays so: longer for an octave, the estimator's typical slip.
    const float cents = found ? centsBetween (estimateHz_, lockedHz_) : 0.0f;
    // Right after a pick the estimate is not weighed against the locked note: a new note picked
    // while the old string rings must win even though the mix still repeats at the old period.
    const float margin = std::max (kChallengeMargin, kChallengeShare * (1.0f - heldClarity_));
    const bool challenger = periodic && std::abs (cents) >= kSameNoteCents &&
                            (onsetHops_ > 0 || clarity_ > heldClarity_ + margin);
    if (challenger)
    {
        countCandidate();
        // A jump to a harmonic of the note (or the note to a harmonic of it) while the note is
        // still clearly there is the estimator's classic slip, not a new note, until it has
        // lasted.
        bool harmonic = false;
        for (float ratio : { 2.0f, 3.0f, 4.0f })
            harmonic = harmonic || std::abs (std::abs (cents) - 1200.0f * std::log2 (ratio)) <
                                       kOctaveToleranceCents;
        harmonic = harmonic && heldClarity_ >= kLockClarity;
        if (candidateHops_ >= (harmonic ? octaveSwitchHops_ : switchHops_))
        {
            lockedHz_ = estimateHz_;
            holdHops_ = 0;
            candidateHops_ = 0;
            hopsSinceNote_ = 0;
            partialHz_ = lockedHz_;
            partialSettled_ = false;
            ++noteId_;
            event_ = Event::noteChange;
            steer (lockedHz_);
            return;
        }
    }
    else
    {
        candidateHops_ = 0;
    }

    if (heldClarity_ >= kHoldClarity)
    {
        // The same note: follow vibrato, bends and glides.
        lockedHz_ *= std::exp2 (kFollow * centsBetween (heldHz_, lockedHz_) / 1200.0f);
        holdHops_ = 0;
    }
    else if (++holdHops_ > maxHoldHops_)
    {
        locked_ = false;
        candidateHops_ = 0;
        event_ = Event::noteOff;
    }
    // While the resonators narrow onto a new note they follow the estimate; then they stay put
    // unless the note moves away by more than the dead band.
    const float settledPeriods =
        static_cast<float> (hopsSinceNote_) * static_cast<float> (hop_ / rate_) * lockedHz_;
    const bool narrowing = settledPeriods < kNarrowingPeriods;
    if (narrowing || std::abs (centsBetween (lockedHz_, steerHz_)) > kSteerDeadbandCents)
        steer (lockedHz_);
    // The measurement is smoothed, so it is reported only while the note holds still (a bend
    // or vibrato in progress is reported from the estimate).
    partialSettled_ = locked_ && settledPeriods > kNarrowingPeriods + kPartialPeriods &&
                      std::abs (centsBetween (partialHz_, lockedHz_)) < kReportCents;
}

} // namespace sedef::analysis
