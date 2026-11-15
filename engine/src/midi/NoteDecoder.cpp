// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/midi/NoteDecoder.h"

#include <algorithm>
#include <cmath>

namespace sedef::midi
{

float NoteDecoder::score (float left) const noexcept
{
    return std::log ((1.0f - model.outlier) * std::exp (-left / model.left) / model.left +
                     model.outlier);
}

void NoteDecoder::prepare (double hopSeconds, double longestLagSeconds)
{
    hopSeconds_ = hopSeconds;
    depth_ = std::max (1, static_cast<int> (std::ceil (longestLagSeconds / hopSeconds))) + 1;
    from_.assign (static_cast<std::size_t> (depth_ * kStates), 0);
    // The tracker judges a period over a window of one period (model.windowMs at least): hops
    // inside one window are one observation, so each counts for its share of it.
    for (int n = 0; n < kNotes; ++n)
    {
        const double period = 1.0 / (440.0 * std::exp2 ((n + kLowestNote - 69) / 12.0));
        attackSpan_[static_cast<std::size_t> (n)] =
            static_cast<float> (static_cast<double> (model.attackSeconds) + 2.0 * period);
        const double window = std::clamp (static_cast<double> (model.periodShare) * period,
                                          0.001 * static_cast<double> (model.windowMs),
                                          0.001 * static_cast<double> (model.longestWindowMs));
        window_[static_cast<std::size_t> (n)] = static_cast<float> (window);
        temper_[static_cast<std::size_t> (n)] = static_cast<float> (hopSeconds / window);
    }
    lag_ = std::min (lag_, depth_ - 1);
    setPatience (model.patience, budget_);
    reset();
}

void NoteDecoder::setPatience (float patience, double budgetSeconds) noexcept
{
    model.patience = patience;
    budget_ = budgetSeconds;
    for (int n = 0; n < kNotes; ++n)
    {
        float p = patience;
        if (budgetSeconds > 0.0)
        {
            // What the budget leaves once the note could be read at all: two of its periods
            // (attackSpan_ holds them, and the attack) and the tracker's own margin.
            const double read = static_cast<double> (attackSpan_[static_cast<std::size_t> (n)]) -
                                static_cast<double> (model.attackSeconds) +
                                static_cast<double> (model.readSeconds);
            const double fits = (budgetSeconds - read) /
                                static_cast<double> (window_[static_cast<std::size_t> (n)]);
            p = std::clamp (static_cast<float> (fits), std::min (1.0f, patience), patience);
        }
        patience_[static_cast<std::size_t> (n)] = p;
    }
}

float NoteDecoder::patienceOf (int note) const noexcept
{
    return patience_[static_cast<std::size_t> (std::clamp (note - kLowestNote, 0, kNotes - 1))];
}

void NoteDecoder::setLag (double seconds) noexcept
{
    lag_ = std::clamp (static_cast<int> (std::lround (seconds / hopSeconds_)), 0, depth_ - 1);
}

void NoteDecoder::reset() noexcept
{
    delta_.fill (-1.0e6f);
    delta_[0] = 0.0f;
    std::fill (from_.begin(), from_.end(), std::uint8_t { 0 });
    head_ = 0;
    now_ = kSilence;
    margin_ = 0.0f;
    left_.fill (1.0f);
}

void NoteDecoder::beginHop() noexcept
{
    left_.fill (1.0f);
}

void NoteDecoder::addPeriod (float pitch, float value) noexcept
{
    const int n = static_cast<int> (std::lround (pitch)) - kLowestNote;
    if (n < 0 || n >= kNotes || !(value > 0.0f))
        return;
    auto& left = left_[static_cast<std::size_t> (n)];
    left = std::min (left, 1.0f - std::min (value, 1.0f));
}

double NoteDecoder::attackSpan (int note) const noexcept
{
    return static_cast<double> (
        attackSpan_[static_cast<std::size_t> (std::clamp (note - kLowestNote, 0, kNotes - 1))]);
}

int NoteDecoder::endHop (float overQuietDb, double sinceAttack) noexcept
{
    // Emissions.
    const float quiet =
        overQuietDb < 0.0f ? std::min (model.quietMost, -model.quietPerDb * overQuietDb) : 0.0f;
    std::array<float, kNotes> gain;
    // (Most notes have no period this hop: their evidence is the same number.)
    const float none = score (1.0f);
    for (int n = 0; n < kNotes; ++n)
        gain[static_cast<std::size_t> (n)] = left_[static_cast<std::size_t> (n)] < 1.0f
                                                 ? score (left_[static_cast<std::size_t> (n)])
                                                 : none;
    std::array<float, kNotes> emit;
    for (int n = 0; n < kNotes; ++n)
    {
        float away = 0.0f;
        for (int over : { 12, 19, 24 })
            if (n + over < kNotes && left_[static_cast<std::size_t> (n + over)] <=
                                         left_[static_cast<std::size_t> (n)] + model.sameLeft)
                away = std::max (away, gain[static_cast<std::size_t> (n + over)]);
        emit[static_cast<std::size_t> (n)] = temper_[static_cast<std::size_t> (n)] *
                                             (gain[static_cast<std::size_t> (n)] - away - quiet);
    }

    // The best note to come from, for a leap; every state also looks at itself, at silence and
    // at the notes a step away.
    int best = 1;
    for (int s = 2; s < kStates; ++s)
        if (delta_[static_cast<std::size_t> (s)] > delta_[static_cast<std::size_t> (best)])
            best = s;
    // A transition costs so many clean observations.
    const float clean = score (0.0f);
    const float endCost = clean * model.end;
    const auto since = static_cast<float> (sinceAttack);
    auto* from = from_.data() + static_cast<std::size_t> (head_ * kStates);

    // Silence: stays, or a note ends.
    {
        float value = delta_[0];
        int source = 0;
        if (delta_[static_cast<std::size_t> (best)] - endCost > value)
        {
            value = delta_[static_cast<std::size_t> (best)] - endCost;
            source = best;
        }
        next_[0] = value;
        from[0] = static_cast<std::uint8_t> (source);
    }
    for (int s = 1; s < kStates; ++s)
    {
        float value = delta_[static_cast<std::size_t> (s)];
        int source = s;
        const auto consider = [&] (int other, float cost) {
            const float v = delta_[static_cast<std::size_t> (other)] - cost;
            if (v > value)
            {
                value = v;
                source = other;
            }
        };
        const bool attack = since >= 0.0f && since <= attackSpan_[static_cast<std::size_t> (s - 1)];
        const float unit = patience_[static_cast<std::size_t> (s - 1)] * clean;
        const float withAttack = unit * model.attack;
        const float step = attack ? withAttack : unit * model.step;
        consider (0, attack ? withAttack : unit * model.bare);
        if (best != s)
            consider (best,
                      std::abs (best - s) <= 2 ? step : (attack ? withAttack : unit * model.leap));
        for (int d : { -2, -1, 1, 2 })
            if (s + d >= 1 && s + d < kStates)
                consider (s + d, step);
        next_[static_cast<std::size_t> (s)] = value + emit[static_cast<std::size_t> (s - 1)];
        from[s] = static_cast<std::uint8_t> (source);
    }

    // Keep the scores near zero, and find the best state and its lead.
    int top = 0;
    for (int s = 1; s < kStates; ++s)
        if (next_[static_cast<std::size_t> (s)] > next_[static_cast<std::size_t> (top)])
            top = s;
    float second = -1.0e9f;
    for (int s = 0; s < kStates; ++s)
        if (s != top)
            second = std::max (second, next_[static_cast<std::size_t> (s)]);
    const float high = next_[static_cast<std::size_t> (top)];
    for (int s = 0; s < kStates; ++s)
        delta_[static_cast<std::size_t> (s)] =
            std::max (next_[static_cast<std::size_t> (s)] - high, -1.0e6f);
    margin_ = high - second;
    now_ = top == 0 ? kSilence : top - 1 + kLowestNote;

    // The state `lag` hops ago on the best path to now.
    int state = top;
    int at = head_;
    for (int k = 0; k < lag_; ++k)
    {
        state = from_[static_cast<std::size_t> (at * kStates + state)];
        at = at == 0 ? depth_ - 1 : at - 1;
    }
    head_ = head_ + 1 < depth_ ? head_ + 1 : 0;
    return state == 0 ? kSilence : state - 1 + kLowestNote;
}

} // namespace sedef::midi
