// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/analysis/MonoTracker.h"
#include "sedef/core/Biquad.h"
#include "sedef/midi/MidiEvent.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sedef::midi
{

enum class Mode : std::uint8_t
{
    notes,       ///< note-on and note-off only
    notesAndBend ///< plus pitch bend for what lies under a semitone (vibrato, slight bends)
};

enum class Timing : std::uint8_t
{
    fastest, ///< each note as soon as it is sure: low strings come later than high ones
    steady   ///< every picked note a fixed lag after its pick (kSteadyLagSeconds)
};

enum class Curve : std::uint8_t
{
    soft,   ///< a light pick already plays loud
    linear, ///< velocity follows the pick level in dB
    hard    ///< loud notes need a hard pick
};

/// Why the latest note started or ended (diagnostics: render tools and tests read it).
enum class Why : std::uint8_t
{
    none,
    pick,       ///< started from silence by a pick
    correction, ///< started by the tracker correcting the note a pick started
    legato,     ///< started without a pick (hammer-on, pull-off, slide)
    repick,     ///< the same note picked again
    replaced,   ///< ended for the next note
    unvoiced,   ///< ended: the tracker heard no note
    gate,       ///< ended: the level fell under the gate
    mute,       ///< ended: the level fell faster than a string decays
    damped,     ///< ended: no pick, another pitch, the level fallen (the note was damped)
    decayed,    ///< ended: the level fell kDecayOffDb under the note's attack
    other       ///< ended by a settings change or allNotesOff()
};

/// How the latest pick was seen.
enum class PickKind : std::uint8_t
{
    none,
    detector, ///< MonoTracker's pick detector
    weak,     ///< the sounding note losing its period
    lock      ///< the tracker locking with the level on the rise
};

struct Settings
{
    int octave = -1; ///< -2..+1 octaves; a bass sounds an octave under a guitar
    Mode mode = Mode::notes;
    int bendRange = 2; ///< semitones; must match the instrument's
    Timing timing = Timing::fastest;
    int channel = 1;        ///< 1..16
    float gateDb = -75.0f;  ///< notes start only above this level (band RMS, dBFS)
    float dynamics = 0.75f; ///< 0: every note at kFixedVelocity, 1: the pick sets it all
    Curve curve = Curve::linear;
};

/// Monophonic guitar DI to MIDI notes, in real time.
///
/// The pitch comes from analysis::MonoTracker; everything that turns it into notes is here.
/// - **Level.** The input's power in the tracker's band (40 Hz to 1.5 kHz) over about 3 ms, in
///   dBFS, with the highest value of each of the last 64 milliseconds kept.
/// - **Picks.** A note starts from silence only with a pick behind it (within
///   kPickWindowSeconds), so a ringing string or a squeak never starts one. Three things count
///   as a pick: MonoTracker's detector; the note losing its own period (its periodicity, measured
///   at a decimated rate, falls under kPeriodicBreak within 20 ms of being steady), which is how a
///   soft pick over a ringing note or the same note picked again shows; and the tracker locking
///   (or changing its reading after the level was under the gate) with the level kLockRiseDb over
///   where it was kLockRiseMs before. A pick seen only as lost periodicity counts when the level
///   3-12 ms on is no more than kWeakPickDropDb under the level before it (a mute also breaks the
///   period); and when, by the time its note would start, the level has fallen that far and the
///   note heard is one the note before was a partial of, the break was a mute and that string
///   only rang on in sympathy. Picks closer than kPickRefractorySeconds are one pick. Each pick
///   is then dated to the sample from the signal: by its click (the power above 2 kHz rising
///   9 dB), or, for a lost period, by where the input stops repeating the period it had.
/// - **The player's level.** The levels of the latest kVelocityPicks picks that started notes say
///   how hard this player picks on this input; they survive reset() (not prepare()). Once there is
///   one, a pick whose attack (the loudest millisecond from 3 ms after it until its note would
///   start) is under the bottom of the velocity scale (below) is not a note: a finger touching a
///   string, the hand on the guitar. It is still remembered, so a passage played that much softer
///   comes through after a while. The gate (Settings::gateDb) only needs to keep the input's own
///   noise out: its default sits under the softest picks of an input whose peaks are at -30 dBFS.
/// - **Quantisation with hysteresis.** The tracked pitch (a fractional MIDI note, after the
///   octave transpose) changes the note only once it is more than kHysteresisSemitones past the
///   half-way point to the next semitone and has stayed there for kConfirmSeconds. Vibrato and a
///   slightly sharp fret keep the note.
/// - **A picked note** goes out once the tracker's pitch has settled and the note is sure: its
///   own period is periodic (kPickedClarity) over the signal since the pick, at least two of its
///   periods (8 ms or more) have passed, and no lower note can be under it. Right after a pick
///   the tracker can name a partial of a low note, whose short period settles first; so a note
///   above the low E's second partial waits until one period of the low E has passed after the
///   attack (kAttackSkipSeconds), and is held back while a longer period is clearly more
///   periodic than its own (kLongerPeriodic, kLongerMargin). That wait is the latency floor of
///   notes above A3 (about 16 ms). Within kPickWindowSeconds of the pick the tracker may still
///   correct its reading; a correction must hold for kCorrectionConfirmSeconds, never goes back
///   to the note before the pick, and one down to a whole multiple of the note's period (an
///   octave, a twelfth) needs the input to repeat clearly better there than at the note's own
///   period (kCorrectionMargin, over the same stretch of signal): a string an octave apart
///   ringing under the note, or the body, can make the longer period look as good.
/// - **The octave up.** After a pick, a string ringing an octave (or two) under the played note,
///   left ringing or in sympathy, makes the input repeat at its longer period as well, and the
///   tracker may name it. When the input also repeats at half that period (kOctaveUpClean, over
///   the longer period), the played note is the octave above: a string's own waveform never
///   does, its odd partials break the half period.
/// - **Legato.** Without a pick, the note changes when the pitch sits within kLegatoCentre of
///   another semitone and is periodic there (kLegatoPeriodic) for kLegatoConfirmSeconds: a
///   hammer-on, pull-off or slide gives every semitone it reaches. A jump over
///   kMaxLegatoSemitones is the tracker moving to another string, not a legato note, and is
///   ignored; when the level has fallen kLegatoDropDb under its recent peak, the played note was
///   damped and the change ends the note instead.
/// - **The fretting hand on a fading string.** A player moves to the next fret, or lifts off it,
///   while the string still rings, tens of milliseconds before picking the next note: the
///   ringing string takes the new pitch (or the open string's) with no energy added. A legato
///   note keeps the energy its pick gave the string; when the loudest of the last
///   kMuteWindowSeconds is kFadedDropDb under the sounding note's attack (the loudest it has been
///   since its pick, or since the pick of the legato run it belongs to), the
///   string has faded, and a change of pitch on it without a pick ends the note instead of
///   starting one. The move can also break the note's periodicity, or tap the fret hard enough
///   for the tracker's detector: on a faded string, a pick counts only when its level (3-12 ms
///   on) comes back within kFadedDropDb of that attack or rises kLockRiseDb over the loudest
///   millisecond of the 16 before it (a pick puts energy into the string, a fretting hand does
///   not), and no note starts from it until that is known. The next pick starts the next note.
/// - **The same note picked again** ends the old note and starts a new one when the pick shows
///   in the signal as a new waveform (the note's periodicity dips under kRetriggerPeriodic) and
///   the input repeats at the note's own period again. A rise of the level is no proof (pick
///   noise, a pick scraping a damped string, strings beating), and a new note picked on another
///   string over this one leaves the tracker on this one for a moment, but repeats at its own
///   period.
/// - **Note-off from the level,** not from the tracker, which rides out poor estimates for a long
///   time. The note ends when the level falls kOffHysteresisDb under the gate or kDecayOffDb
///   under the note's attack (it is no longer heard, however loud the input), or kMuteDropDb
///   under its highest point of the last kMuteWindowSeconds (a mute: no string decays that fast),
///   or when the tracker no longer hears a note. The mute is judged against the note's own
///   level, from 3 ms after its pick: a louder string damped as the note was picked is not the
///   note being muted.
/// - **Velocity** from the pick's level (the loudest millisecond 3-12 ms after it, or its attack
///   for a note from silence): kVelocitySpanDb of picking strength up to the top of the scale map
///   to 1..127 through the curve, then blend towards kFixedVelocity as the dynamics fall. The top
///   sits kVelocityHeadroomDb over the player's loud picks (the second loudest of the latest
///   kVelocityPicks, the loudest while there are fewer than four), so the same playing gets the
///   same velocities at any input level; before the first pick it is kVelocityTopDb, where a
///   guitar at a usual recording level puts it. A legato note takes its velocity from the
///   level.
/// - **Timing.** Fastest sends each event at the sample it was decided on. Steady sends a picked
///   note's note-on kSteadyLagSeconds after its pick, and every other event that long after it
///   was decided, so all notes have the same latency and a recording can be moved earlier by one
///   amount. A note sure only after the lag is still sent, at once, and counted as late. Steady
///   also sees glitches coming: a note that would sound shorter than kShortNoteSeconds while its
///   note-on still waits is taken back, and the note it interrupted sounds on if it comes back
///   before its own note-off went out.
/// - **No stuck notes.** Every note-on sent is remembered until its note-off is sent;
///   allNotesOff() and panic() end all of them. A change of channel or octave ends the sounding
///   note first. Legato order is note-off, then note-on, at the same sample.
///
/// prepare() allocates (the tracker's and the analysis buffers); everything else is real-time
/// safe.
class GuitarToMidi
{
public:
    // Thresholds, chosen from the measurements in docs/design/guitar-to-midi.md.
    static constexpr float kHysteresisSemitones = 0.25f;
    static constexpr double kConfirmSeconds = 0.002;
    static constexpr double kCorrectionConfirmSeconds = 0.008;

    static constexpr double kPickWindowSeconds = 0.1;
    static constexpr double kRepickMarginSeconds = 0.004;
    static constexpr float kOffHysteresisDb = 8.0f;
    static constexpr float kMuteDropDb = 18.0f;
    static constexpr double kMuteWindowSeconds = 0.04;
    static constexpr double kSteadyLagSeconds = 0.038;
    static constexpr double kShortNoteSeconds = 0.04;
    static constexpr double kLegatoLeadSeconds = 0.004;
    static constexpr double kBendIntervalSeconds = 0.002;
    static constexpr float kBendDeadbandCents = 1.5f;
    static constexpr float kLegatoDropDb = 10.0f;
    static constexpr float kFadedDropDb = 10.0f;
    static constexpr double kLowestHz = 70.0;
    static constexpr float kMinNoteHz = 60.0f;
    static constexpr float kPickedClarity = 0.75f;
    static constexpr double kLowestOpenHz = 82.41; ///< the low E string in standard tuning
    static constexpr float kLongerPeriodic = 0.9f;
    static constexpr float kLongerMargin = 0.03f;
    static constexpr float kOwnClean = 0.96f;
    static constexpr float kOctaveUpClean = 0.8f;
    static constexpr double kAttackSkipSeconds = 0.0025;
    static constexpr double kCheckWindowSeconds = 0.0015;
    static constexpr float kLegatoCentre = 0.4f;
    static constexpr float kLegatoPeriodic = 0.85f;
    static constexpr double kLegatoConfirmSeconds = 0.003;
    static constexpr int kMaxLegatoSemitones = 7;
    static constexpr float kPeriodicHigh = 0.9f;
    static constexpr float kPeriodicAnchor = 0.97f;
    static constexpr float kWeakPickDropDb = 10.0f;
    static constexpr double kPickRefractorySeconds = 0.04;
    static constexpr float kHarmonicTolerance = 0.03f;
    static constexpr float kCorrectionMargin = 0.25f;
    static constexpr float kRetriggerPeriodic = 0.75f;
    static constexpr int kLockRiseMs = 30;
    static constexpr float kLockRiseDb = 3.0f;
    static constexpr float kPeriodicBreak = 0.7f;
    static constexpr float kVelocityTopDb = -10.0f;
    static constexpr float kVelocitySpanDb = 40.0f;
    static constexpr float kVelocityHeadroomDb = 10.0f;
    static constexpr int kVelocityPicks = 32;
    static constexpr float kDecayOffDb = 40.0f;
    static constexpr int kFixedVelocity = 100;

    void prepare (double sampleRate);
    /// Forgets the signal and every pending event, without sending anything: call allNotesOff()
    /// first if notes may be sounding.
    void reset() noexcept;
    /// Takes effect at the start of the next process(). A new channel or octave ends the sounding
    /// note there; a new timing sends the events still waiting at once.
    void setSettings (const Settings& s) noexcept;
    const Settings& settings() const noexcept { return settings_; }

    /// Feeds a block of the guitar's mono input and appends what it decided to `out`, with
    /// offsets in [0, numFrames).
    void process (const float* in, int numFrames, EventList& out) noexcept;

    /// Ends everything sent so far, at `offset`: a note-off for every sounding note, then All
    /// Notes Off and a centred bend on every channel used. Drops the events still waiting and
    /// forgets the note being played, so the next note needs a new pick.
    void allNotesOff (EventList& out, int offset) noexcept;
    /// allNotesOff() on all 16 channels.
    void panic (EventList& out, int offset) noexcept;

    // Telemetry (read from the audio thread, or after process()).
    /// The note being played (decided; in Steady its note-on may still be waiting), -1 if none.
    int note() const noexcept { return note_; }
    /// The tracked pitch as a fractional MIDI note after the octave transpose, 0 when none.
    float pitch() const noexcept { return pitch_; }
    float levelDb() const noexcept { return levelDb_; }
    int lastVelocity() const noexcept { return lastVelocity_; }
    /// Note-ons sent since prepare(), and how many of them Steady sent after its lag.
    std::uint32_t notesSent() const noexcept { return notesSent_; }
    std::uint32_t lateNotes() const noexcept { return lateNotes_; }
    /// Notes still sounding at the receiver (note-on sent, note-off not yet).
    int soundingCount() const noexcept;
    /// Why the latest note started and ended, and how the latest pick was seen.
    Why startWhy() const noexcept { return startWhy_; }
    Why endWhy() const noexcept { return endWhy_; }
    PickKind pickKind() const noexcept { return pickKind_; }

    /// The velocity a pick of this level (linear RMS) gets with these settings, on a scale whose
    /// top is `topDb` (by default where it sits before any pick: kVelocityTopDb).
    static int velocityFor (float level, const Settings& s, float topDb = kVelocityTopDb) noexcept;
    /// The top of the velocity scale now: kVelocityHeadroomDb over the player's loud picks.
    float velocityTopDb() const noexcept;
    /// Steady's lag in samples at this rate.
    static int steadyLagSamples (double sampleRate) noexcept;

private:
    struct Pending
    {
        std::int64_t due = 0;
        Event event;
    };
    static constexpr int kQueueSize = 64;
    static constexpr int kHistoryMs = 64;

    void applySettings (EventList& out, int offset) noexcept;
    void decide() noexcept;
    void startNote (int note, int velocity, std::int64_t reference, Why why) noexcept;
    /// Ends the sounding note: at `reference` plus Steady's lag, or now with `immediately`.
    void endNote (std::int64_t reference, Why why, bool immediately = false) noexcept;
    float levelAgo (int ms) const noexcept;
    void sendBend (float deviation, std::int64_t due) noexcept;
    void schedule (std::int64_t due, const Event& e) noexcept;
    /// The latest queued note-on (or note-off) of this note, -1 if none.
    int findQueued (bool on, int channel, int note) const noexcept;
    void unqueue (int index) noexcept;
    void emitDue (EventList& out, int offset) noexcept;
    void emit (EventList& out, const Event& e) noexcept;
    void endAll (EventList& out, int offset, bool everyChannel) noexcept;
    bool muted() const noexcept;
    bool pitchSettled() const noexcept;
    bool pickedNoteReady (float hz) const noexcept;
    bool harmonicSlip (float hz, bool correction) const noexcept;
    /// A note at `hz` heard soon after a pick may be a partial of a lower note still on its way.
    bool lowerNotePossible (float hz) const noexcept;
    float shortPeriodicity (int lag, int window) const noexcept;
    float recentPeakDb() const noexcept;
    void registerPick (std::int64_t at, PickKind kind) noexcept;
    void refinePick() noexcept;
    bool refineByPeriod() noexcept;
    void watchPeriodicity() noexcept;
    /// How periodic the band is at this frequency's period, over one period (or `window`
    /// decimated samples if longer): 1 for a perfect repeat.
    float periodicityAt (float hz, int window = 0) const noexcept;
    float pickLevelDb() const noexcept;
    int velocityAt (float levelDb) const noexcept;
    void rememberPick (float levelDb) noexcept;
    float attackLevelDb() const noexcept;
    float ringAt (int ago) const noexcept
    {
        return ring_[static_cast<std::size_t> ((ringWrite_ - 1 - ago) & ringMask_)];
    }
    int quantise (float p) const noexcept;
    /// `hz`, or the octave above it (or two) when the input repeats cleanly at that period too.
    float octaveChecked (float hz) noexcept;
    /// Where the input's component at `hz` starts rising, near `around`; -1 when unclear.
    std::int64_t fundamentalOnset (float hz, std::int64_t around) noexcept;
    std::int64_t clickNear (std::int64_t from, std::int64_t to) const noexcept;

    double sampleRate_ = 48000.0;
    Settings settings_, next_;
    bool settingsChanged_ = false;
    analysis::MonoTracker tracker_;

    // Level: band-limited power, its dB value and the highest value of each recent millisecond.
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
    Section hp_, lp_, clickHp_;
    float clickPower_ = 0.0f, clickCoeff_ = 0.0f;
    std::vector<float> clickRing_; ///< the click's power at every sample of the last 32 ms
    int clickMask_ = 0;
    std::vector<float> inRing_; ///< the input of the last 64 ms or more
    std::int64_t inMask_ = 0;
    float refineHz_ = 0.0f;
    bool pickRefined_ = true;
    int refineSamples_ = 288, baseSamples_ = 960, clickDelaySamples_ = 10;
    float power_ = 0.0f, powerCoeff_ = 0.0f;
    float levelDb_ = -120.0f;
    std::array<float, kHistoryMs> history_ {};
    int historyPos_ = 0, msSamples_ = 48, msCount_ = 0;
    float msMax_ = -120.0f;
    int muteWindowMs_ = 40;

    // The band at a decimated rate (about 7 kHz), for the periodicity measures.
    std::vector<float> ring_;
    int ringMask_ = 0, ringWrite_ = 0, factor_ = 6, phase_ = 0;
    double rate_ = 8000.0;
    // Periodicity at the note's period, every hop.
    int hop_ = 4, hopCount_ = 0, minWindow_ = 32;
    float periodicity_ = 0.0f, periodicHz_ = 0.0f;
    std::int64_t periodicAt_ = 0, periodicSince_ = 0; ///< last periodic hop, start of that run
    float leastPeriodicity_ = 1.0f; ///< the lowest periodicity since the latest pick
    int steadySamples_ = 480, breakWindowSamples_ = 960, refractorySamples_ = 1200;
    int weakRefractorySamples_ = 2880, weakCheckSamples_ = 576, lockPickGapSamples_ = 2400;
    int correctionConfirmSamples_ = 384;

    // Clock and timing constants (samples).
    std::int64_t now_ = 0;
    int confirmSamples_ = 96, pickWindowSamples_ = 4800;
    int repickMarginSamples_ = 192, lagSamples_ = 1632, legatoLeadSamples_ = 192;
    int bendIntervalSamples_ = 96, legatoConfirmSamples_ = 144;
    mutable std::int64_t lowCheckedAt_ = 0;
    mutable float lowCheckedHz_ = 0.0f;
    mutable bool lowerCached_ = false;
    int minCheckWindow_ = 16, attackSkip_ = 24;

    // Picks.
    std::uint32_t pickCount_ = 0;
    std::int64_t pickAt_ = 0, lastPickAt_ = 0, previousPickAt_ = 0;
    bool pickWeak_ = false; ///< seen only as a break of periodicity, its level not checked yet
    bool pickChecking_ =
        false; ///< the pick's level is not checked yet (weak, or on a faded string)
    float preHeldDb_ = -120.0f;   ///< the loudest millisecond of the 16 before the pick
    bool pickWeakSource_ = false; ///< the latest pick was seen only as a break of periodicity
    int prevNote_ = -1;           ///< the note before the latest pick
    int lastNote_ = -1;           ///< the latest note ended
    bool pickFresh_ = false;      ///< a pick not yet used by a note
    bool trackerMoved_ = false;   ///< the tracker locked or changed note since that pick
    float pickHz_ = 0.0f;         ///< what the tracker held when the pick came (0: nothing)
    float preLevelDb_ = -120.0f;  ///< the level just before the pick's rise
    int agreeSamples_ = 0;        ///< the tracker's estimate has named its note this long

    // The note.
    float pitch_ = 0.0f;
    int note_ = -1, noteChannel_ = 1;
    float noteHz_ = 0.0f;  ///< the tracker's reading when the sounding note started
    float heardHz_ = 0.0f; ///< the tracker's reading, an octave up or two when the input says so
    float noteLevelDb_ = -120.0f; ///< the sounding note's attack: the loudest it has been
    int candidate_ = -1, candidateSamples_ = 0;
    int lastVelocity_ = 0;
    /// The levels of the latest kVelocityPicks picks that started notes (or were too soft to),
    /// kept across reset(): they say how this player picks on this input.
    std::array<float, kVelocityPicks> pickLevels_ {};
    int pickLevelCount_ = 0, pickLevelPos_ = 0;
    std::int64_t noteDue_ = 0;   ///< when the sounding note's note-on goes out
    std::int64_t noteStart_ = 0; ///< the sounding note's pick (or legato move)

    // Bend.
    int bendSent_ = kBendCentre, bendChannel_ = 1;
    float bendCents_ = 0.0f;
    std::int64_t lastBendAt_ = 0;

    // Events waiting for their time (Steady), in time order.
    std::array<Pending, kQueueSize> queue_ {};
    int queued_ = 0;
    int shortNoteSamples_ = 1920, refineToleranceSamples_ = 48;
    std::int64_t octaveAt_ = 0;
    float octaveHz_ = 0.0f, octaveRatio_ = 1.0f; ///< octaveChecked()'s latest answer
    static constexpr int kDemodSize = 2048;
    std::array<float, kDemodSize> demodRe_ {}, demodIm_ {};
    std::array<float, 8192> demodAmp_ {};
    int bridgeNote_ = -1, bridgeChannel_ = 1; ///< a note a dropped glitch interrupted (Steady)

    // What the receiver holds: notes on per channel, and the channels used since the last flush.
    std::array<std::array<std::uint64_t, 2>, 16> on_ {};
    std::uint16_t channelsUsed_ = 0;

    std::uint32_t notesSent_ = 0, lateNotes_ = 0;
    Why startWhy_ = Why::none, endWhy_ = Why::none;
    PickKind pickKind_ = PickKind::none;
};

} // namespace sedef::midi
