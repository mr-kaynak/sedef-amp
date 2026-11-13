// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/record/ReampFeeder.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace sedef::record
{

bool MemorySource::read (float* const* dest, std::int64_t start, int frames)
{
    if (start < 0 || frames < 0 || start + frames > length())
        return false;
    for (std::size_t c = 0; c < data_.size(); ++c)
        std::memcpy (dest[c], data_[c].data() + start,
                     static_cast<std::size_t> (frames) * sizeof (float));
    return true;
}

void ReampFeeder::setSource (ReampPlayer& player, FrameSource* source)
{
    stop (player);
    source_ = source;
    pushed_ = 0;
    limit_ = -1;
}

void ReampFeeder::play (ReampPlayer& player)
{
    if (source_ == nullptr || source_->length() <= 0)
        return;
    const int sourceChannels = std::max (1, source_->channels());
    planar_.assign (static_cast<std::size_t> (sourceChannels),
                    std::vector<float> (static_cast<std::size_t> (kReadFrames)));
    interleaved_.assign (static_cast<std::size_t> (kReadFrames) * 2, 0.0f);
    fold_.prepare (player.sampleRate());

    flush_ = player.requestFlush();
    state_ = State::flushing;
    pushed_ = 0;
    limit_ = loop_ ? -1 : source_->length();
}

void ReampFeeder::stop (ReampPlayer& player)
{
    if (state_ != State::idle)
        player.requestStop();
    state_ = State::idle;
}

void ReampFeeder::setLoop (ReampPlayer& player, bool loop)
{
    if (loop == loop_)
        return;
    loop_ = loop;
    if (source_ == nullptr || state_ == State::idle)
        return;
    updateLimit (player, state_ == State::flushing ? 0 : player.consumed());
}

void ReampFeeder::updateLimit (ReampPlayer& player, std::int64_t consumed)
{
    const std::int64_t n = source_->length();
    // Loop off: the pass being played is the last (docs/recorder.md, section 6).
    limit_ = loop_ ? -1 : (consumed / n + 1) * n;
    if (state_ == State::playing)
        player.setEndAt (limit_);
}

std::int64_t ReampFeeder::position (const ReampPlayer& player) const noexcept
{
    if (source_ == nullptr || source_->length() <= 0)
        return 0;
    return player.consumed() % source_->length();
}

bool ReampFeeder::readMapped (std::int64_t start, int frames, int channels)
{
    const int sourceChannels = static_cast<int> (planar_.size());
    std::array<float*, 8> ptrs {};
    for (int c = 0; c < sourceChannels && c < 8; ++c)
        ptrs[static_cast<std::size_t> (c)] = planar_[static_cast<std::size_t> (c)].data();
    if (sourceChannels > 8 || !source_->read (ptrs.data(), start, frames))
        return false;

    const auto at = [&] (int i, int c) -> float& {
        return interleaved_[static_cast<std::size_t> (i * channels + c)];
    };
    const auto& first = planar_[0];
    if (channels == 1 && sourceChannels >= 2)
    {
        // A stereo file on a mono input: folded the way the rig folds a stereo input.
        fold_.process (planar_[0].data(), planar_[1].data(), interleaved_.data(), frames);
    }
    else
    {
        for (int c = 0; c < channels; ++c)
        {
            const auto& from = c < sourceChannels ? planar_[static_cast<std::size_t> (c)] : first;
            for (int i = 0; i < frames; ++i)
                at (i, c) = from[static_cast<std::size_t> (i)];
        }
    }
    return true;
}

bool ReampFeeder::fill (ReampPlayer& player)
{
    const std::int64_t n = source_->length();
    const int channels = player.channels();
    auto& fifo = player.fifo();
    for (;;)
    {
        const auto room = static_cast<std::int64_t> (fifo.freeFrames());
        if (room == 0 || (limit_ >= 0 && pushed_ >= limit_))
            return true;
        const std::int64_t pos = pushed_ % n; // the seam: frame n - 1, then frame 0
        std::int64_t count = std::min<std::int64_t> ({ room, kReadFrames, n - pos });
        if (limit_ >= 0)
            count = std::min (count, limit_ - pushed_);
        if (!readMapped (pos, static_cast<int> (count), channels))
            return false;
        fifo.push (interleaved_.data(), static_cast<std::size_t> (count));
        pushed_ += count;
    }
}

bool ReampFeeder::pump (ReampPlayer& player)
{
    if (source_ == nullptr || state_ == State::idle)
        return true;

    if (state_ == State::flushing)
    {
        if (!player.flushed (flush_))
            return true;
        if (!fill (player))
        {
            state_ = State::idle;
            return false;
        }
        // Full (or the whole pass): start it.
        endings_ = player.endings();
        player.setEndAt (limit_);
        player.requestPlay (flush_);
        state_ = State::playing;
        return true;
    }

    if (player.endings() != endings_)
    {
        state_ = State::idle; // it played to its end
        return true;
    }
    if (!fill (player))
    {
        stop (player);
        return false;
    }
    return true;
}

} // namespace sedef::record
