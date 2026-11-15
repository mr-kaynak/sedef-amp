// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/core/InputFold.h"
#include "sedef/record/ReampPlayer.h"

#include <cstdint>
#include <vector>

namespace sedef::record
{

/// Decoded audio frames: a file on disk (the plug-in, through JUCE's readers) or memory.
class FrameSource
{
public:
    virtual ~FrameSource() = default;
    virtual int channels() const = 0;
    virtual std::int64_t length() const = 0;
    virtual double sampleRate() const = 0;
    /// Reads `frames` frames from `start` into planar `dest` (channels() pointers). False on an
    /// error.
    virtual bool read (float* const* dest, std::int64_t start, int frames) = 0;
};

/// Frames held in memory, planar.
class MemorySource final : public FrameSource
{
public:
    MemorySource (std::vector<std::vector<float>> channels, double sampleRate)
        : data_ (std::move (channels)),
          rate_ (sampleRate)
    {
    }
    int channels() const override { return static_cast<int> (data_.size()); }
    std::int64_t length() const override
    {
        return data_.empty() ? 0 : static_cast<std::int64_t> (data_[0].size());
    }
    double sampleRate() const override { return rate_; }
    bool read (float* const* dest, std::int64_t start, int frames) override;

private:
    std::vector<std::vector<float>> data_;
    double rate_;
};

/// The reader's half of re-amp (docs/recorder.md, section 6): keeps a ReampPlayer's FIFO topped
/// up from a source at the player's rate, looping sample-exactly or ending after one pass, and
/// maps the source's channels onto the rig's input. Runs on the reader thread (any one
/// non-real-time thread).
class ReampFeeder
{
public:
    static constexpr int kReadFrames = 4096;

    enum class State
    {
        idle,     ///< nothing requested
        flushing, ///< waiting for the audio thread to empty the FIFO
        playing,  ///< play requested; filling as it drains
    };

    /// The source to play (must outlive playback; its rate must be the player's). Stops.
    void setSource (ReampPlayer& player, FrameSource* source);
    FrameSource* source() const noexcept { return source_; }

    void setLoop (ReampPlayer& player, bool loop);
    bool loop() const noexcept { return loop_; }

    /// Plays from the first frame (asks for a flush; pump() fills and starts it).
    void play (ReampPlayer& player);
    /// Fades out and stops.
    void stop (ReampPlayer& player);

    /// Moves a play along: once flushed, fills the FIFO and starts the player; afterwards keeps
    /// the FIFO full. False on a read error (playing stops).
    bool pump (ReampPlayer& player);

    State state() const noexcept { return state_; }
    /// Where the player is in the file (frames), for display.
    std::int64_t position (const ReampPlayer& player) const noexcept;
    /// The stream frame where the current play ends; -1 while looping.
    std::int64_t limit() const noexcept { return limit_; }

private:
    bool fill (ReampPlayer& player);
    bool readMapped (std::int64_t start, int frames, int channels);
    void updateLimit (ReampPlayer& player, std::int64_t consumed);

    FrameSource* source_ = nullptr;
    bool loop_ = true;
    State state_ = State::idle;
    std::uint32_t flush_ = 0;
    std::uint32_t endings_ = 0; // the player's endings() when this play started
    std::int64_t pushed_ = 0;   // stream frames pushed since the flush
    std::int64_t limit_ = -1;

    std::vector<std::vector<float>> planar_;
    std::vector<float> interleaved_;
    InputFold fold_;
};

} // namespace sedef::record
