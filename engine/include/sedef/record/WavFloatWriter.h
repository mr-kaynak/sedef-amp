// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sedef::record
{

/// Where a file's bytes go. Random access, because a WAV header's sizes are written last.
class ByteSink
{
public:
    virtual ~ByteSink() = default;
    /// False on an error (disk full, file gone).
    virtual bool writeAt (std::uint64_t offset, const void* data, std::size_t bytes) = 0;
    /// Hands what was written to the OS (crash safety). False on an error.
    virtual bool flush() { return true; }
};

/// A ByteSink in memory: the tests' file.
class MemorySink final : public ByteSink
{
public:
    bool writeAt (std::uint64_t offset, const void* data, std::size_t bytes) override;
    std::vector<unsigned char> bytes;
    /// Writes beyond this many bytes fail (a full disk); 0 means no limit.
    std::uint64_t limit = 0;
};

/// One LIST/INFO entry: a four-character id (ISFT, ICRD, INAM, ICMT) and its text.
struct InfoField
{
    std::array<char, 4> id {};
    std::string text;
};

/// Writes a 32-bit float WAV file (docs/recorder.md, section 4): RIFF, an 18-byte `fmt ` chunk
/// with WAVE_FORMAT_IEEE_FLOAT, `fact`, `data`, and a LIST/INFO chunk after the samples.
/// Frames go to any position: a gap before them becomes silence, and frames written again
/// replace what was there. Not real time (the recorder's writer thread).
class WavFloatWriter
{
public:
    /// Bytes before the first sample.
    static constexpr std::uint64_t kHeaderBytes = 58;
    /// Room kept at the end for the LIST chunk, so RIFF's 32-bit sizes always hold.
    static constexpr std::uint64_t kInfoReserve = 4096;

    /// The most frames a file with `channels` can hold (the 4 GB limit of RIFF).
    static std::int64_t maxFrames (int channels) noexcept;

    WavFloatWriter (ByteSink& sink, int channels, double sampleRate);

    /// Writes the header for an empty file.
    bool begin();
    /// Writes `frames` interleaved frames starting at frame `position`. False on a sink error or
    /// if the file would pass maxFrames().
    bool write (std::int64_t position, const float* interleaved, int frames);
    /// Pads the file with silence to `frames` long (nothing if it is that long already).
    bool extendTo (std::int64_t frames);
    /// Brings the RIFF, fact and data sizes up to date and flushes.
    bool updateHeader();
    /// Appends the LIST/INFO chunk and completes the header. Nothing may be written afterwards.
    bool finish (const std::vector<InfoField>& info);

    std::int64_t frames() const noexcept { return frames_; }
    int channels() const noexcept { return channels_; }

private:
    bool writeSilence (std::int64_t from, std::int64_t to);
    std::uint64_t dataBytes() const noexcept;

    ByteSink& sink_;
    int channels_;
    std::uint32_t rate_;
    std::int64_t frames_ = 0; // one past the highest frame written
    bool finished_ = false;
    std::vector<float> zeros_;
};

/// Reads back what WavFloatWriter writes (the tests, and the recorder's own checks): the
/// header's fields, the INFO entries and the samples, deinterleaved.
struct WavFloatFile
{
    int formatTag = 0;
    int channels = 0;
    std::uint32_t sampleRate = 0;
    int bitsPerSample = 0;
    std::uint32_t factFrames = 0;
    std::uint32_t riffSize = 0;
    std::vector<std::vector<float>> samples;
    std::vector<InfoField> info;

    std::string infoText (const char* id) const;
    /// False if the bytes are not a 32-bit float WAV in this layout.
    static bool parse (const std::vector<unsigned char>& bytes, WavFloatFile& out);
};

} // namespace sedef::record
