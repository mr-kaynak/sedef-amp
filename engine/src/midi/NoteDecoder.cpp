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
        temper_[static_cast<std::size_t> (n)] = static_cast<float> (
            hopSeconds / std::max (static_cast<double> (model.periodShare) * period,
                                   0.001 * static_cast<double> (model.windowMs)));
    }
    lag_ = std::min (lag_, depth_ - 1);
    reset();
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

int NoteDecoder::endHop (float overQuietDb, bool attack) noexcept
{
    // Emissions.
    const float quiet =
        overQuietDb < 0.0f ? std::min (model.quietMost, -model.quietPerDb * overQuietDb) : 0.0f;
    std::array<float, kNotes> gain;
    for (int n = 0; n < kNotes; ++n)
        gain[static_cast<std::size_t> (n)] = score (left_[static_cast<std::size_t> (n)]);
    std::array<float, kNotes> emit;
    for (int n = 0; n < kNotes; ++n)
    {
        float away = 0.0f;
        for (int over : { 12, 19, 24 })
            if (n + over < kNotes && left_[static_cast<std::size_t> (n + over)] <=
                                         left_[static_cast<std::size_t> (n)] + model.sameLeft)
                away = std::max (away, gain[static_cast<std::size_t> (n + over)]);
        emit[static_cast<std::size_t> (n)] =
            temper_[static_cast<std::size_t> (n)] * (gain[static_cast<std::size_t> (n)] - away - quiet);
    }

    // The best note to come from, for a leap; every state also looks at itself, at silence and
    // at the notes a step away.
    int best = 1;
    for (int s = 2; s < kStates; ++s)
        if (delta_[static_cast<std::size_t> (s)] > delta_[static_cast<std::size_t> (best)])
            best = s;
    // A transition costs so many clean observations.
    const float unit = model.patience * score (0.0f);
    const float withAttack = unit * model.attack;
    const float move = attack ? withAttack : unit * model.leap;
    const float step = attack ? withAttack : unit * model.step;
    const float start = attack ? withAttack : unit * model.bare;
    const float endCost = score (0.0f) * model.end;
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
        consider (0, start);
        if (best != s)
            consider (best, std::abs (best - s) <= 2 ? step : move);
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
        delta_[static_cast<std::size_t> (s)] = std::max (next_[static_cast<std::size_t> (s)] - high, -1.0e6f);
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
