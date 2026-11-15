// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace sedef::midi
{

/// Which note is sounding, weighed over time: a Viterbi decoder over the states {silence, one
/// state per semitone}, fed once per analysis hop with how well the input repeats at each
/// note's period. docs/design/guitar-to-midi.md ("Real playing": "The decoder") has the model
/// and where every number comes from.
///
/// - **Observation.** For each note n, d_n in [0, 1]: the share of the input one period of n
///   leaves unexplained (1 - the NSDF at that period; 1 when the input has no period there).
///   A sounding note leaves what does not repeat (noise, drift): d ~ Exponential (mean
///   Model::left), with a share Model::outlier of hops where anything can be read (an attack,
///   a transition). With no note at n, d is anything: uniform. So a hop's evidence for n is the
///   log likelihood ratio L(d) = ln ((1 - outlier) e^(-d / left) / left + outlier): +2.2 for a
///   clean period, -2.3 for none. Silence scores 0.
/// - **One observation.** The tracker judges a period over a window, so hops inside one window
///   repeat each other: each counts for its share of the window (Model::periodShare of the
///   note's period, between Model::windowMs and Model::longestWindowMs: half a period of a
///   guitar's low E). A clean period so earns one observation's worth, L(0), per window.
/// - **A period's multiples repeat too.** A note an octave or a twelfth over n makes the input
///   repeat at n's period as well. When such a note explains the input as well as n does
///   (within Model::sameLeft), the evidence is that note's, and n is given none.
/// - **Level.** Under the quietest a note can be (the caller's), every note loses
///   Model::quietPerDb per dB, up to Model::quietMost.
/// - **Transitions** cost a number of clean observations, times Model::patience: starting a
///   note with an attack behind it (Model::attack: a pick or a rise of the level no longer
///   ago than Model::attackSeconds and two of the note's periods, the time the tracker needs
///   to show a period at all), without one (bare); moving one or two
///   semitones without an attack (step: a hammer-on, a pull-off, a slide), any other move
///   without one (leap). Ending costs Model::end. A note that cannot earn its start back is
///   never on the best path: how short a note may be, and how long a new note is doubted,
///   follow from the patience alone. It is the one setting: the wait for a note with an attack
///   is patience x one window. With a budget (a fixed lag the caller sends its notes at), a
///   low note's patience is cut to what the lag leaves after the note's own two periods.
/// - **Lag.** endHop() answers for the hop `lag` ago: the state the best path to now passes
///   through then. With no lag it is the best state now.
///
/// prepare() allocates; everything else is real-time safe.
class NoteDecoder
{
public:
    static constexpr int kLowestNote = 24, kHighestNote = 96;
    static constexpr int kNotes = kHighestNote - kLowestNote + 1, kStates = kNotes + 1;
    static constexpr int kSilence = -1;

    struct Model
    {
        float left = 0.1f, outlier = 0.1f, sameLeft = 0.08f;
        float quietPerDb = 0.35f, quietMost = 3.0f;
        float windowMs = 4.0f, periodShare = 0.5f, longestWindowMs = 6.0f;
        float attack = 1.0f, bare = 2.0f, step = 2.0f, leap = 3.0f, end = 3.0f;
        float patience = 1.0f;
        float attackSeconds = 0.012f; ///< how long an attack lasts
        float readSeconds = 0.005f;   ///< what a tracker takes to read a note, beyond two periods
    };
    Model model;

    void prepare (double hopSeconds, double longestLagSeconds);
    /// The lag in effect, at most what prepare() was given. Forgets nothing.
    void setLag (double seconds) noexcept;
    /// How many clean observations a transition costs, as a multiple of the model's counts.
    /// `budgetSeconds` (0: none): the time the caller can wait for a note after its attack. A
    /// note's patience is then no more than fits in the budget once the note has had the two
    /// periods and Model::readSeconds a tracker needs to read it, and never under 1.
    void setPatience (float patience, double budgetSeconds = 0.0) noexcept;
    /// The patience in effect for this note.
    float patienceOf (int note) const noexcept;
    double lagSeconds() const noexcept { return lag_ * hopSeconds_; }
    void reset() noexcept;

    /// One hop: beginHop(), addPeriod() for every period the input repeats at, endHop().
    void beginHop() noexcept;
    /// `pitch`: the period as a fractional MIDI note; `value`: how well the input repeats there.
    void addPeriod (float pitch, float value) noexcept;
    /// `overQuietDb`: the level over the quietest a note can be; `sinceAttack`: how long ago
    /// the latest pick or rise of the level was, in seconds (negative: none yet). An attack is
    /// behind a note for as long as the note needs to show: Model::attackSeconds and two of
    /// its periods. Returns the note `lag` ago (kSilence: none).
    int endHop (float overQuietDb, double sinceAttack) noexcept;
    /// How long after an attack a note at this pitch still starts with the attack behind it.
    double attackSpan (int note) const noexcept;
    /// The best state now, whatever the lag.
    int now() const noexcept { return now_; }
    /// How far the best path is ahead of the runner-up state now, in nats.
    float margin() const noexcept { return margin_; }

private:
    float score (float left) const noexcept;

    double hopSeconds_ = 0.0005;
    int lag_ = 0, depth_ = 1, head_ = 0;
    std::array<float, kNotes> temper_ {};
    std::array<float, kNotes> attackSpan_ {}; ///< seconds
    std::array<float, kNotes> window_ {};     ///< seconds: one observation
    std::array<float, kNotes> patience_ {};
    double budget_ = 0.0;
    std::array<float, kNotes> left_ {};
    std::array<float, kStates> delta_ {}, next_ {};
    std::vector<std::uint8_t> from_; ///< depth_ hops of kStates back-pointers
    int now_ = kSilence;
    float margin_ = 0.0f;
};

} // namespace sedef::midi
