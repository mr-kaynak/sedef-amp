// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cassert>
#include <cstring>

namespace sedef
{

/// The engine processes at most two channels.
inline constexpr int kMaxChannels = 2;

struct ProcessSpec
{
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    int numChannels = 2;
};

/// Non-owning view over planar float audio. Cheap to copy.
class AudioView
{
public:
    AudioView() = default;

    AudioView (float* const* channels, int numChannels, int numFrames) noexcept
        : numChannels_ (numChannels),
          numFrames_ (numFrames)
    {
        assert (numChannels >= 0 && numChannels <= kMaxChannels);
        for (int c = 0; c < numChannels; ++c)
            channels_[static_cast<size_t> (c)] = channels[c];
    }

    AudioView (float* mono, int numFrames) noexcept : numChannels_ (1), numFrames_ (numFrames)
    {
        channels_[0] = mono;
    }

    float* channel (int c) const noexcept
    {
        assert (c >= 0 && c < numChannels_);
        return channels_[static_cast<size_t> (c)];
    }

    int numChannels() const noexcept { return numChannels_; }
    int numFrames() const noexcept { return numFrames_; }
    float* const* data() const noexcept { return channels_.data(); }

    AudioView subView (int start, int length) const noexcept
    {
        assert (start >= 0 && length >= 0 && start + length <= numFrames_);
        AudioView v;
        v.numChannels_ = numChannels_;
        v.numFrames_ = length;
        for (int c = 0; c < numChannels_; ++c)
            v.channels_[static_cast<size_t> (c)] = channels_[static_cast<size_t> (c)] + start;
        return v;
    }

    AudioView withChannels (int count) const noexcept
    {
        assert (count <= numChannels_);
        AudioView v = *this;
        v.numChannels_ = count;
        return v;
    }

    void clear() const noexcept
    {
        for (int c = 0; c < numChannels_; ++c)
            std::memset (channel (c), 0, sizeof (float) * static_cast<size_t> (numFrames_));
    }

    void copyFrom (const AudioView& other) const noexcept
    {
        assert (other.numFrames_ == numFrames_);
        for (int c = 0; c < numChannels_; ++c)
            std::memcpy (channel (c), other.channel (c < other.numChannels_ ? c : 0),
                         sizeof (float) * static_cast<size_t> (numFrames_));
    }

    void applyGain (float gain) const noexcept
    {
        for (int c = 0; c < numChannels_; ++c)
        {
            float* d = channel (c);
            for (int i = 0; i < numFrames_; ++i)
                d[i] *= gain;
        }
    }

private:
    std::array<float*, kMaxChannels> channels_ {};
    int numChannels_ = 0;
    int numFrames_ = 0;
};

} // namespace sedef
