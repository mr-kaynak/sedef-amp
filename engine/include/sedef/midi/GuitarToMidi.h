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
    notes,       ///< note-on and note-off only: every semitone reached is a new note
    notesAndBend ///< bends and wide vibrato move the pitch bend; a held note sends none
};

/// The lowest note the player's instrument has: how far down notes are looked for, and how long
/// a note waits for a lower one that may still be on its way.
enum class LowNote : std::uint8_t
{
    automatic, ///< a six-string guitar's, widened once a lower note has been played
    e2,        ///< six strings: E2, and down to C#2 in dropped tunings
    b1,        ///< seven strings: B1, and down to G#1
    fSharp1    ///< eight strings: F#1, and down to E1
};

enum class Timing : std::uint8_t
{
    fastest, ///< each note as soon as it is sure: low strings come later than high ones
    steady   ///< every picked note a fixed lag after its pick (kSteadyLagSeconds)
};

enum class GateMode : std::uint8_t
{
    automatic, ///< over the input's own noise floor: no setting, at any input level
    manual     ///< Settings::gateDb
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
    int channel = 1; ///< 1..16
    GateMode gateMode = GateMode::automatic;
    float gateDb = -75.0f;  ///< the manual gate: notes start only above it (band RMS, dBFS)
    float dynamics = 0.75f; ///< 0: every note at kFixedVelocity, 1: the pick sets it all
    Curve curve = Curve::linear;
    LowNote lowNote = LowNote::automatic;
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
/// - **The gate.** In Auto (the default) it follows the input's own noise floor, so it needs no
///   setting at any input level. The floor is the quietest the level has been (the loudest 3 ms
///   level of each millisecond): it falls at once to anything quieter and rises
///   kNoiseRiseDbPerSecond while no note sounds; for kWarmupSeconds after reset() it is simply
///   the level. The gate sits kNoiseMarginDb over the floor, never under kAutoGateFloorDb
///   (digital silence). Manual uses Settings::gateDb as it is. Notes start only above the gate
///   and end kOffHysteresisDb under it; the tracker's detector hears picks from just under it.
/// - **The player's level.** The average attack (dB) of the picks that started notes, over about
///   the latest kPlayingNotes (each weighs the same until there are that many, then each new one
///   1/kPlayingNotes): how hard this player picks on this input. It survives reset() and
///   prepare(). Before the first note nothing says how loud the player is, so in Auto a note must
///   stand kFirstNoteMarginDb out of the noise floor (the hand on the guitar, a string ringing in
///   sympathy, stay under it). After it, a pick whose attack is under the bottom of the velocity
///   scale (below; 26.5 dB under the player's level) is not a note: a brush of the hand, a string
///   ringing in sympathy. It still counts towards the player's level, so a passage played that
///   much softer comes through after a while. A note so needs the higher of the gate and the
///   player's level less a margin, and every other level a decision compares is the note's own
///   or the player's.
/// - **A finger's touch.** A finger touching the string before it plucks it is a pick too,
///   10-60 ms ahead of the pluck, 15-30 dB under it, and sometimes with a squeak at a high
///   harmonic. From silence, a pick kTouchBelowDb under the player's level (or the first pick
///   ever) waits until kTouchWaitSeconds after it before it may start a note; the tracker
///   (MonoTracker::Options::fingerPicks) sees the pluck as a new pick, which replaces it (a pick
///   from the detector replaces any pick that started no note). No note from silence starts
///   while a louder attack is under way. MonoTracker::Options::weakFundamentals names a low
///   string plucked near the bridge, whose fundamental is weak for its first 100 ms. A pick's
///   click is never needed: fingers have none.
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
///   notes above A3 on the tracker's reading (about 16 ms; the early reading, below, often
///   names them sooner). Within kPickWindowSeconds of the pick the tracker may still
///   correct its reading; a correction must hold for kCorrectionConfirmSeconds, never goes back
///   to the note before the pick, and one down to a whole multiple of the note's period (an
///   octave, a twelfth) needs the input to repeat clearly better there than at the note's own
///   period (kCorrectionMargin, over the same stretch of signal): a string an octave apart
///   ringing under the note, or the body, can make the longer period look as good.
/// - **The early reading (Fastest).** The tracker judges a period on two of its lengths, so it
///   cannot name a low note before two long periods have passed, and the waits above come on
///   top. A picked string repeats from its pick on, though. So for every period the layer
///   keeps how well the signal since the pick's attack (the stretch, from kAttackSkipSeconds
///   after the pick) repeats one period on, a difference function as in YIN over a stretch
///   that grows from the pick instead of a window that slides, and what each period leaves
///   unexplained of it (0: the stretch repeats exactly). The shortest period that qualifies
///   starts the note, before the tracker has read it:
///   - when nothing an octave under it is a note of the range (on a six-string, up to C#3),
///     once kEarlyShare of the period (kEarlyWindowSeconds at least) has come round again,
///     leaving no more than kEarlyClean: 1.6 periods and the attack;
///   - when a lower string could be under it (a first reading can name one of a low string's
///     partials), only if it alone accounts for the stretch (kEarlyAlone, over two periods
///     and kEarlyAloneSeconds at least), which a string's partial never does;
///   - and only if no shorter period accounts for much of the stretch (kEarlyOther): two
///     strings ringing together repeat at a period neither has, and an octave under the
///     played note is a multiple of its period.
///
///   The note that sounded when the pick came may ring on under the new one, and then the
///   input repeats at its period too. When it was steady, the stretch is kept a second time,
///   less what that note would have gone on to do (its last period before the pick, repeated):
///   if that leaves clearly less (kEarlyRingsOn), the old note rings on, and what is left,
///   what the pick brought (kEarlyBrought at least), is what is read. Otherwise, while the
///   note before still rings (kEarlyRingDb), the same note and every note it is a partial of
///   wait for the tracker.
///
///   The same note picked again (the staple of a bass line) used to wait for the lowest note
///   the instrument has, since a pick no louder than what rang drowns nothing. It is read
///   against the note the tracker held at the pick: the stretch from kEarlyHeldSkipSeconds
///   after the pick (a pick on the sounding string ends what it did) repeating at that period
///   (kEarlyHeld), on its semitone, the semitones next to it clearly worse (kEarlyHeldApart),
///   not before kEarlyHeldSeconds (a fretting finger landing just after the pick must show),
///   and, with the old note taken out, not repeating at a half or a third of the period (a
///   note picked an octave or a twelfth over a ringing one). A sounding note is picked again
///   only when its periodicity broke; from silence, only by a pick that drowned what rang.
///
///   A note started this way is the tracker's to confirm. Until the tracker has had its two
///   periods (counted from the pick it saw itself), what it still holds says nothing; once it
///   names the note, the note is like any other. If it never does and the stretch, longer by
///   then, no longer repeats at the note's period (kEarlyStill), whatever the tracker names
///   next may replace the note. The early reading only starts a note for a pick the tracker's
///   detector saw (or, for the same note again, one seen as its lost period), as loud as this
///   player's picks, its level checked; Steady never uses it, and plays exactly as before.
/// - **The octave up.** After a pick, a string ringing an octave (or two) under the played note,
///   left ringing or in sympathy, makes the input repeat at its longer period as well, and the
///   tracker may name it. When the input also repeats at half that period (kOctaveUpClean, over
///   the longer period), the played note is the octave above: a string's own waveform never
///   does, its odd partials break the half period.
/// - **The range.** Settings::lowNote says how low the instrument goes: a six-string's E2 (down
///   to C#2 in dropped tunings), a seven-string's B1 (to G#1) or an eight-string's F#1 (to E1).
///   A lower range looks for longer periods, and a note above the lowest string's octave then
///   waits one period of that string before it can be sure no lower note is under it, unless the
///   note alone accounts for the signal since the pick (kAloneClean), which a cleanly played
///   note does: so the notes above cost at most a few ms, and only between the lowest string's
///   octave and about 150 Hz. Automatic (the default) is the six-string's range, exactly, until
///   a note under it has sounded for kLearnSeconds (its first one takes a third period to be
///   sure); what it learned survives reset() and prepare(), and a host saves it with the
///   session. A low string's level swings with its long period (sustainedDb()): no note ends,
///   and no bend is held back, for that.
/// - **Legato.** Without a pick, the note changes when the pitch sits within kLegatoCentre of
///   another semitone and is periodic there (kLegatoPeriodic) for kLegatoConfirmSeconds: a
///   hammer-on, pull-off or slide gives every semitone it reaches. A jump over
///   kMaxLegatoSemitones is the tracker moving to another string, not a legato note, and is
///   ignored; when the level has fallen kLegatoDropDb under its recent peak, the played note was
///   damped and the change ends the note instead. A slide's frets come faster than notes: after
///   a fret change, a further fret the same way within kSlideWindowSeconds is a note only once
///   it has been held kSlideConfirmSeconds, so a fast slide gives its first fret and the fret
///   it stops on, never a burst of short notes.
/// - **Bend (Notes + Bend).** Every note-on is preceded by a centred pitch bend, and the wheel
///   stays at exactly the centre while the note is only held: a receiver may take any wheel off
///   centre as a played bend. The bend is counted from where the note sits (its centre: the
///   reading over the note's first kBendSettleSeconds, then following the string's own slow
///   drift at kCentreCentsPerSecond, never more than kCentreMaxCents from the semitone), not
///   from equal temperament: a string a little out of tune, a fret pressed sharp, the pitch
///   sinking as the note decays are not bends. A bend starts when the pitch has been more than
///   kBendOnCents from the centre for kBendOnSeconds (or more than kBendSureCents, followed all
///   the way), then follows the pitch, at most kBendSlewCentsPerSecond, ignoring moves of the
///   reading under kBendHoldCents; it ends, exactly centred, when the pitch is back within
///   kBendOffCents for kBendOffSeconds or rests within a fret's tuning of the semitone. Only a
///   pitch that moves like a bend is followed: more than kBendJumpCents in one interval is a
///   fret taken at once or a slipped reading, and a pitch that moves while the level falls
///   (kBendFadeDb) is the hand letting go. A string bent across a semitone inside the bend
///   range stays the note that was picked; past the range the next note starts, without a gap.
///   A fret taken at once (a hammer-on, a pull-off, a slide's step) is a note in both modes:
///   nothing in the signal tells it reliably from a soft pick on the same string.
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
///   not), and no note starts from it until that is known (in Fastest, as soon as the level
///   has shown it; in Steady, 12 ms on). The next pick starts the next note.
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
///   sits kPlayingHeadroomDb over the player's level (over the note's own level for the first
///   note), so the same playing gets the same velocities at any input level (88 at the player's
///   level with the default settings), and as the player's level moves slowly, the dynamics of a
///   phrase stay in them. A legato note takes its velocity from the level.
/// - **Timing.** Fastest sends each event at the sample it was decided on, and decides a picked
///   note on the early reading when that names it. Steady sends a picked
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
    static constexpr float kBendStepCents = 1.5f;   ///< the smallest bend change sent
    static constexpr float kBendOnCents = 25.0f;    ///< a bend starts past this
    static constexpr float kBendOffCents = 10.0f;   ///< ...and ends back inside this
    static constexpr double kBendOnSeconds = 0.04;  ///< ...held this long
    static constexpr double kBendOffSeconds = 0.06; ///< ...for this long
    static constexpr float kBendSlewCentsPerSecond = 3000.0f;
    static constexpr float kCentreCentsPerSecond = 20.0f; ///< how fast a note's centre follows
    static constexpr float kCentreMaxCents = 35.0f;       ///< ...and how far from its semitone
    static constexpr float kBendJumpCents = 8.0f; ///< more per interval is a fret, not a bend
    static constexpr float kBendSureCents =
        60.0f; ///< a move this far, followed all the way, is a bend
    static constexpr float kBendHoldCents = 3.0f; ///< the bend ignores smaller moves of the reading
    static constexpr float kBendStillCents = 4.0f; ///< a pitch this still...
    static constexpr double kBendStillSeconds =
        0.15; ///< ...for this long, near the note, is the note
    static constexpr double kBendSettleSeconds = 0.03;  ///< a note's first readings set its centre
    static constexpr double kSlideWindowSeconds = 0.08; ///< frets this close in time are a slide
    static constexpr double kSlideConfirmSeconds =
        0.03;                                  ///< ...whose next note waits this long on its fret
    static constexpr float kBendFadeDb = 6.0f; ///< a level falling this fast is a hand letting go
    static constexpr float kLegatoDropDb = 10.0f;
    static constexpr float kFadedDropDb = 10.0f;
    /// The lowest note looked for at all: under an eight-string's dropped E1.
    static constexpr double kFloorHz = 39.0;
    static constexpr float kPickedClarity = 0.75f;
    static constexpr double kLowestOpenHz = 82.41; ///< the low E string in standard tuning
    /// How long a lower note may still be on its way, per LowNote: the lowest open string, the
    /// lowest note a dropped tuning reaches on it, and the lowest pitch that counts as a note.
    struct Range
    {
        double openHz, lowestHz;
        float minNoteHz, searchHz, rumbleHz;
    };
    static constexpr Range kRanges[3] = { { kLowestOpenHz, 70.0, 60.0f, 50.0f, 0.0f },
                                          { 61.74, 52.0, 50.0f, 46.0f, 30.0f },
                                          { 46.25, 41.0, 39.5f, 0.0f, 22.0f } };
    /// Automatic: a note under these (and lasting kLearnSeconds) widens the range.
    static constexpr float kLearnB1Hz = 68.0f, kLearnFSharp1Hz = 51.0f;
    static constexpr double kLearnSeconds = 0.15;
    static constexpr float kLongerPeriodic = 0.9f;
    static constexpr float kLongerMargin = 0.03f;
    static constexpr float kOwnClean = 0.96f;
    static constexpr float kAloneClean = 0.99f; ///< a note that is all there is in the signal
    static constexpr float kLeftOver = 0.01f; ///< what a period leaves unexplained, when it matters
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
    static constexpr float kVelocitySpanDb = 40.0f;
    static constexpr float kPlayingHeadroomDb = 13.5f;
    static constexpr int kPlayingNotes = 16;
    static constexpr float kDecayOffDb = 40.0f;
    static constexpr int kFixedVelocity = 100;
    static constexpr float kAutoGateFloorDb = -100.0f;
    static constexpr float kNoiseMarginDb = 20.0f;
    static constexpr float kFirstNoteMarginDb = 30.0f;
    static constexpr float kNoiseRiseDbPerSecond = 6.0f;
    static constexpr double kWarmupSeconds = 0.05;
    static constexpr double kTouchWaitSeconds = 0.06;
    static constexpr float kTouchBelowDb = 10.0f;
    // The early reading (Fastest): what a period may leave unexplained of the signal since
    // the pick, and how long a stretch must repeat. From the measurements in the design.
    static constexpr double kEarlyWindowSeconds = 0.004; ///< at least this much must repeat
    static constexpr float kEarlyShare = 0.6f;           ///< ...and this share of the period
    static constexpr float kEarlyClean = 0.05f;          ///< ...leaving no more than this
    static constexpr double kEarlyAloneSeconds = 0.005;  ///< a note a lower one could be under:
    static constexpr float kEarlyAlone = 0.02f; ///< ...this long (and a period), this clean
    static constexpr float kEarlyOther = 0.4f; ///< what every shorter period must leave unexplained
    static constexpr float kEarlyRingDb =
        20.0f; ///< the note before still rings: this near the pick
    static constexpr float kEarlyOldClean = 0.1f;   ///< a note steady enough to be taken out
    static constexpr float kEarlyRingsOn = 0.7f;    ///< it rings on: without it, this much is left
    static constexpr float kEarlyBrought = 0.25f;   ///< ...of which the pick brought at least this
    static constexpr float kEarlyHeld = 0.1f;       ///< the same note again: this clean
    static constexpr float kEarlyHeldCents = 45.0f; ///< ...this near where the note was
    static constexpr float kEarlyHeldApart = 0.2f;  ///< ...a semitone off leaving at least this
    static constexpr float kEarlyHeldPart = 0.25f;  ///< ...no note over it, the old one out
    static constexpr double kEarlyHeldSeconds = 0.012;     ///< ...not before this after the pick
    static constexpr double kEarlyHeldSkipSeconds = 0.005; ///< ...read from this after the pick
    static constexpr float kEarlyStill = 0.25f; ///< a note read early still holds: this clean

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
    /// The gate in effect (dB, the level's scale): the automatic one, or Settings::gateDb.
    float gateLevelDb() const noexcept { return gateLevelDb_; }
    /// The input's noise floor as the automatic gate tracks it (dB, the level's scale).
    float noiseFloorDb() const noexcept { return noiseDb_; }
    /// How hard this player picks on this input: the average attack (dB) of about the latest
    /// kPlayingNotes picks, -120 before the first.
    float playingLevelDb() const noexcept;
    /// The range in effect: Settings::lowNote, or what Automatic has learned so far (never
    /// LowNote::automatic). Learned ranges survive reset() and prepare().
    LowNote lowNoteInEffect() const noexcept;
    /// What Automatic has learned, whatever Settings::lowNote is now.
    LowNote learnedLowNote() const noexcept { return learned_; }
    /// Forgets what Automatic learned (a new instrument).
    void forgetLowNote() noexcept;
    /// Sets what Automatic has learned (a saved session's value).
    void setLearnedLowNote (LowNote learned) noexcept;
    /// The pitch bend last sent, in cents (0: centred).
    float bendCents() const noexcept { return bendOut_; }
    /// Note-ons sent since prepare(), and how many of them Steady sent after its lag.
    std::uint32_t notesSent() const noexcept { return notesSent_; }
    std::uint32_t lateNotes() const noexcept { return lateNotes_; }
    /// Notes still sounding at the receiver (note-on sent, note-off not yet).
    int soundingCount() const noexcept;
    /// Note-ons sent on the early reading (Fastest), before the tracker had read the note.
    std::uint32_t earlyNotes() const noexcept { return earlyNotes_; }
    /// The latest pick's date, in samples since reset() (diagnostics).
    std::int64_t pickedAt() const noexcept { return pickAt_; }
    /// The early reading of the latest pick, 0 when there is none (diagnostics).
    float earlyHz() const noexcept { return earlyHz_; }
    /// ...the share of the signal its period leaves unexplained, and the stretch that repeats (s).
    float earlyLeft() const noexcept { return earlyLeft_; }
    double earlyWindow() const noexcept { return earlyRepeat_ / rate_; }
    /// Why the latest note started and ended, and how the latest pick was seen.
    Why startWhy() const noexcept { return startWhy_; }
    Why endWhy() const noexcept { return endWhy_; }
    PickKind pickKind() const noexcept { return pickKind_; }
    /// The tracker this converter reads (diagnostics).
    const analysis::MonoTracker& tracker() const noexcept { return tracker_; }

    /// The velocity a pick of this level (linear RMS) gets with these settings, on a scale whose
    /// top is `topDb`.
    static int velocityFor (float level, const Settings& s, float topDb) noexcept;
    /// The top of the velocity scale for a pick of this level (dB) now: kPlayingHeadroomDb over
    /// the player's level, or over this pick when it is the first.
    float velocityTopDb (float levelDb) const noexcept;
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
    void sendBend (float cents, std::int64_t due) noexcept;
    /// Notes + Bend: follows the sounding note's pitch, once per kBendIntervalSeconds.
    void followBend() noexcept;
    /// Notes + Bend: the string was bent to fret `q`, inside the bend's range.
    void bendToFret (int q, float heard, float recentDb) noexcept;
    bool bendReaches (int q) const noexcept;
    /// A fret further on from the latest fret change, the same way and soon after: a slide.
    bool slideStep (int q) const noexcept;
    void applyRange() noexcept;
    /// The automatic range has not heard a string as low as `hz` yet.
    bool unheardLow (float hz) const noexcept;
    /// The string's pitch for the bend, in cents from the semitone of `note`.
    float bendCentsFrom (int note) const noexcept;
    void schedule (std::int64_t due, const Event& e) noexcept;
    /// The latest queued note-on (or note-off) of this note, -1 if none.
    int findQueued (bool on, int channel, int note) const noexcept;
    void unqueue (int index) noexcept;
    void emitDue (EventList& out, int offset) noexcept;
    void emit (EventList& out, const Event& e) noexcept;
    void endAll (EventList& out, int offset, bool everyChannel) noexcept;
    bool muted() const noexcept;
    /// The level of a string whose period is longer than `coveredSeconds`: see the definition.
    float sustainedDb (double coveredSeconds = 1.0 / kLowestOpenHz) const noexcept;
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
    /// The early reading: keeps, for every period, how well the signal since the pick repeats.
    void earlyPush() noexcept;
    void earlyBegin() noexcept;
    void earlyAdd (int ago) noexcept;
    void earlyEstimate() noexcept;
    /// The stretch says the note the tracker held at the pick was picked again.
    bool earlyHeldNote (float heldPeriod) noexcept;
    /// The note the early reading names for the latest pick, when it may start one: -1 if none.
    int earlyNoteFor (float& hz) const noexcept;
    bool earlyStillRepeats (float hz) const noexcept;
    /// The note just started was started on the early reading, at `hz`.
    void startedEarly (float hz) noexcept;
    float pickLevelDb() const noexcept;
    int velocityAt (float levelDb) const noexcept;
    void rememberPick (float levelDb) noexcept;
    /// Once a millisecond: the noise floor, the gate in effect and the tracker's pick floor.
    void updateGate (float msLevelDb) noexcept;
    /// Under this, the input is silent for this player: the gate, or far under how they pick.
    float quietDb() const noexcept;
    float attackLevelDb() const noexcept;
    float attackBeforeDb() const noexcept;
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
    int pickLevelSamples_ = 192;
    int correctionConfirmSamples_ = 384, touchWaitSamples_ = 1440;

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
    /// The average attack of the latest picks that started notes (or were too soft to), over
    /// about kPlayingNotes of them, kept across reset() and prepare(): how this player picks on
    /// this input.
    float playingDb_ = -120.0f;
    int playingCount_ = 0;
    // The gate.
    float noiseDb_ = kAutoGateFloorDb, gateLevelDb_ = kAutoGateFloorDb, noiseRisePerMs_ = 0.006f;
    std::int64_t warmupSamples_ = 2400;
    std::int64_t noteDue_ = 0;   ///< when the sounding note's note-on goes out
    std::int64_t noteStart_ = 0; ///< the sounding note's pick (or legato move)

    // Bend (Notes + Bend).
    int bendSent_ = kBendCentre, bendChannel_ = 1;
    int sentNote_ = -1;       ///< the MIDI note sounding at the receiver for note_
    float bendOut_ = 0.0f;    ///< cents sent, from sentNote_
    float bendGlide_ = 0.0f;  ///< where the bend is on its way to its target (slew limited)
    float bendSeen_ = 0.0f;   ///< the latest deviation taken as a continuous move
    float bendTarget_ = 0.0f; ///< where the bend is to go
    float bendStill_ = 0.0f;  ///< the deviation the pitch has been resting at
    int bendStillTicks_ = 0, bendStillNeed_ = 75;
    int bendSettling_ = 0, bendSettleTicks_ = 15;
    std::int64_t stepAt_ = 0; ///< the latest fret change without a pick
    int stepDirection_ = 0, slideWindowSamples_ = 3840, slideConfirmSamples_ = 1440;
    float bendCentre_ = 0.0f; ///< where note_ sits, in cents from its semitone
    bool bendOn_ = false;     ///< the pitch has left the note's centre: the bend follows it
    int bendOutside_ = 0, bendInside_ = 0; ///< ticks past kBendOnCents / inside kBendOffCents
    int bendOnTicks_ = 8, bendOffTicks_ = 30;
    float bendSlewPerTick_ = 6.0f, centrePerTick_ = 0.1f;
    std::int64_t lastBendAt_ = 0;

    // Range.
    double openHz_ = kLowestOpenHz, lowestHz_ = 70.0;
    float minNoteHz_ = 60.0f;
    LowNote learned_ = LowNote::e2; ///< what Automatic has heard; kept across reset()
    bool noteLearned_ = false;      ///< the sounding note has been counted
    int learnSamples_ = 7200;

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

    // The early reading.
    /// For every period, how the stretch since the pick repeats one period on: the stretch as
    /// it is [0], and with the note that sounded before the pick taken out [1].
    struct EarlySums
    {
        std::vector<double> r, m;
        std::vector<float> stretch;
        double energy = 0.0;
    };
    std::array<EarlySums, 2> early_;
    bool earlyOld_ = false;        ///< a steady note sounded at the pick: it can be taken out
    bool earlyRest_ = false;       ///< ...and rings on: the reading is of what is left without it
    double earlyOldPeriod_ = 0.0;  ///< its period, in decimated samples
    std::int64_t earlyOldEnd_ = 0; ///< where what it did before the pick ends
    int earlyGuardSamples_ = 24, earlyHeldCount_ = 76, earlyHeldSkip_ = 20;
    std::int64_t earlyPickAt_ = -1; ///< the pick the sums are counted from
    int earlyCount_ = 0;            ///< decimated samples since that pick's attack
    int earlyLagLimit_ = 0, earlyMinLag_ = 4, earlyWindow_ = 32, earlyAloneWindow_ = 40;
    int earlySpanSamples_ = 2880, earlySkipSamples_ = 120;
    float earlyHz_ = 0.0f, earlyLeft_ = 0.0f;
    bool earlyHeld_ = false; ///< the early reading is the note the tracker held at the pick
    int earlyRepeat_ = 0;
    bool earlyNote_ = false;   ///< the sounding note started early; the tracker has not read it yet
    bool earlyUnread_ = false; ///< the tracker has not named the sounding note since its pick
    bool earlyDoubt_ = false; ///< ...and never did, and the stretch stopped repeating at its period
    bool earlyStart_ = false; ///< startNote() is starting a note on the early reading
    std::int64_t earlyLimit_ = 0;
    int earlyGrace_ = 0; ///< the tracker has no say until this long after its own latest pick

    std::uint32_t notesSent_ = 0, lateNotes_ = 0, earlyNotes_ = 0;
    Why startWhy_ = Why::none, endWhy_ = Why::none;
    PickKind pickKind_ = PickKind::none;
};

} // namespace sedef::midi
