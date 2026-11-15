// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/record/WavFloatWriter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace sedef::record
{

static_assert (std::endian::native == std::endian::little,
               "WAV samples are written straight from memory: little-endian hosts only");

namespace
{
constexpr std::uint16_t kIeeeFloat = 3;
constexpr std::uint64_t kRiffSizeAt = 4;
constexpr std::uint64_t kFactFramesAt = 46;
constexpr std::uint64_t kDataSizeAt = 54;

void put16 (unsigned char* p, std::uint32_t v)
{
    p[0] = static_cast<unsigned char> (v & 0xff);
    p[1] = static_cast<unsigned char> ((v >> 8) & 0xff);
}

void put32 (unsigned char* p, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = static_cast<unsigned char> ((v >> (8 * i)) & 0xff);
}

std::uint32_t get32 (const unsigned char* p)
{
    return static_cast<std::uint32_t> (p[0]) | (static_cast<std::uint32_t> (p[1]) << 8) |
           (static_cast<std::uint32_t> (p[2]) << 16) | (static_cast<std::uint32_t> (p[3]) << 24);
}

std::uint16_t get16 (const unsigned char* p)
{
    return static_cast<std::uint16_t> (p[0] | (p[1] << 8));
}

void putId (unsigned char* p, const char* id)
{
    std::memcpy (p, id, 4);
}
} // namespace

bool MemorySink::writeAt (std::uint64_t offset, const void* data, std::size_t size)
{
    if (limit != 0 && offset + size > limit)
        return false;
    if (bytes.size() < offset + size)
        bytes.resize (static_cast<std::size_t> (offset + size), 0);
    if (size > 0)
        std::memcpy (bytes.data() + offset, data, size);
    return true;
}

std::int64_t WavFloatWriter::maxFrames (int channels) noexcept
{
    const std::uint64_t room = 0xffffffffull - kHeaderBytes - kInfoReserve;
    return static_cast<std::int64_t> (room /
                                      (static_cast<std::uint64_t> (std::max (1, channels)) * 4));
}

WavFloatWriter::WavFloatWriter (ByteSink& sink, int channels, double sampleRate)
    : sink_ (sink),
      channels_ (std::clamp (channels, 1, 2)),
      rate_ (static_cast<std::uint32_t> (std::llround (std::max (1.0, sampleRate))))
{
}

std::uint64_t WavFloatWriter::dataBytes() const noexcept
{
    return static_cast<std::uint64_t> (frames_) * static_cast<std::uint64_t> (channels_) * 4;
}

bool WavFloatWriter::begin()
{
    std::array<unsigned char, kHeaderBytes> h {};
    const std::uint32_t blockAlign = static_cast<std::uint32_t> (channels_) * 4;
    putId (&h[0], "RIFF");
    put32 (&h[4], static_cast<std::uint32_t> (kHeaderBytes - 8));
    putId (&h[8], "WAVE");
    putId (&h[12], "fmt ");
    put32 (&h[16], 18);
    put16 (&h[20], kIeeeFloat);
    put16 (&h[22], static_cast<std::uint32_t> (channels_));
    put32 (&h[24], rate_);
    put32 (&h[28], rate_ * blockAlign);
    put16 (&h[32], blockAlign);
    put16 (&h[34], 32);
    put16 (&h[36], 0); // cbSize
    putId (&h[38], "fact");
    put32 (&h[42], 4);
    put32 (&h[46], 0);
    putId (&h[50], "data");
    put32 (&h[54], 0);
    frames_ = 0;
    finished_ = false;
    return sink_.writeAt (0, h.data(), h.size());
}

bool WavFloatWriter::writeSilence (std::int64_t from, std::int64_t to)
{
    if (zeros_.empty())
        zeros_.assign (8192, 0.0f);
    const std::int64_t perWrite = static_cast<std::int64_t> (zeros_.size()) / channels_;
    for (std::int64_t f = from; f < to; f += perWrite)
    {
        const std::int64_t n = std::min (perWrite, to - f);
        const auto offset = kHeaderBytes + static_cast<std::uint64_t> (f) *
                                               static_cast<std::uint64_t> (channels_) * 4;
        if (!sink_.writeAt (offset, zeros_.data(),
                            static_cast<std::size_t> (n) * static_cast<std::size_t> (channels_) *
                                4))
            return false;
    }
    return true;
}

bool WavFloatWriter::write (std::int64_t position, const float* interleaved, int frames)
{
    if (finished_ || position < 0 || frames < 0 || position + frames > maxFrames (channels_))
        return false;
    if (position > frames_ && !writeSilence (frames_, position))
        return false;
    const auto offset = kHeaderBytes + static_cast<std::uint64_t> (position) *
                                           static_cast<std::uint64_t> (channels_) * 4;
    if (frames > 0 && !sink_.writeAt (offset, interleaved,
                                      static_cast<std::size_t> (frames) *
                                          static_cast<std::size_t> (channels_) * 4))
        return false;
    frames_ = std::max (frames_, position + frames);
    return true;
}

bool WavFloatWriter::extendTo (std::int64_t frames)
{
    if (finished_ || frames > maxFrames (channels_))
        return false;
    if (frames <= frames_)
        return true;
    if (!writeSilence (frames_, frames))
        return false;
    frames_ = frames;
    return true;
}

bool WavFloatWriter::updateHeader()
{
    const auto data = dataBytes();
    std::array<unsigned char, 4> v {};
    put32 (v.data(), static_cast<std::uint32_t> (kHeaderBytes - 8 + data));
    bool ok = sink_.writeAt (kRiffSizeAt, v.data(), 4);
    put32 (v.data(), static_cast<std::uint32_t> (frames_));
    ok = ok && sink_.writeAt (kFactFramesAt, v.data(), 4);
    put32 (v.data(), static_cast<std::uint32_t> (data));
    ok = ok && sink_.writeAt (kDataSizeAt, v.data(), 4);
    return ok && sink_.flush();
}

bool WavFloatWriter::finish (const std::vector<InfoField>& info)
{
    if (finished_)
        return false;

    // LIST/INFO: each entry is its id, its size, the text with a terminating zero, padded to an
    // even length. Texts are cut so the whole chunk fits the reserve.
    std::vector<unsigned char> list (12, 0);
    putId (&list[0], "LIST");
    putId (&list[8], "INFO");
    for (const auto& field : info)
    {
        const std::size_t room = kInfoReserve - list.size();
        if (room < 10)
            break;
        std::string text = field.text.substr (0, room - 10);
        const std::size_t size = text.size() + 1;
        const std::size_t start = list.size();
        list.resize (start + 8 + size + (size & 1), 0);
        std::memcpy (&list[start], field.id.data(), 4);
        put32 (&list[start + 4], static_cast<std::uint32_t> (size));
        std::memcpy (&list[start + 8], text.data(), text.size());
    }
    put32 (&list[4], static_cast<std::uint32_t> (list.size() - 8));

    // The samples are always a whole number of 4-byte words, so the data chunk needs no pad.
    const auto end = kHeaderBytes + dataBytes();
    if (!sink_.writeAt (end, list.data(), list.size()))
        return false;

    const auto riff = static_cast<std::uint32_t> (end + list.size() - 8);
    std::array<unsigned char, 4> v {};
    put32 (v.data(), riff);
    bool ok = sink_.writeAt (kRiffSizeAt, v.data(), 4);
    put32 (v.data(), static_cast<std::uint32_t> (frames_));
    ok = ok && sink_.writeAt (kFactFramesAt, v.data(), 4);
    put32 (v.data(), static_cast<std::uint32_t> (dataBytes()));
    ok = ok && sink_.writeAt (kDataSizeAt, v.data(), 4);
    finished_ = true;
    return ok && sink_.flush();
}

// -------------------------------------------------------------------------------------------------

std::string WavFloatFile::infoText (const char* id) const
{
    for (const auto& f : info)
        if (std::memcmp (f.id.data(), id, 4) == 0)
            return f.text;
    return {};
}

bool WavFloatFile::parse (const std::vector<unsigned char>& b, WavFloatFile& out)
{
    out = {};
    if (b.size() < 12 || std::memcmp (b.data(), "RIFF", 4) != 0 ||
        std::memcmp (b.data() + 8, "WAVE", 4) != 0)
        return false;
    out.riffSize = get32 (b.data() + 4);
    std::size_t pos = 12;
    bool haveFormat = false, haveData = false;
    while (pos + 8 <= b.size())
    {
        const unsigned char* id = b.data() + pos;
        const std::uint32_t size = get32 (b.data() + pos + 4);
        const std::size_t body = pos + 8;
        if (body + size > b.size())
            return false;
        if (std::memcmp (id, "fmt ", 4) == 0 && size >= 16)
        {
            out.formatTag = get16 (b.data() + body);
            out.channels = get16 (b.data() + body + 2);
            out.sampleRate = get32 (b.data() + body + 4);
            out.bitsPerSample = get16 (b.data() + body + 14);
            haveFormat = true;
        }
        else if (std::memcmp (id, "fact", 4) == 0 && size >= 4)
        {
            out.factFrames = get32 (b.data() + body);
        }
        else if (std::memcmp (id, "data", 4) == 0)
        {
            if (!haveFormat || out.formatTag != kIeeeFloat || out.bitsPerSample != 32 ||
                out.channels < 1)
                return false;
            const std::size_t frames = size / (static_cast<std::size_t> (out.channels) * 4);
            out.samples.assign (static_cast<std::size_t> (out.channels),
                                std::vector<float> (frames));
            for (std::size_t f = 0; f < frames; ++f)
                for (int c = 0; c < out.channels; ++c)
                    std::memcpy (&out.samples[static_cast<std::size_t> (c)][f],
                                 b.data() + body +
                                     (f * static_cast<std::size_t> (out.channels) +
                                      static_cast<std::size_t> (c)) *
                                         4,
                                 4);
            haveData = true;
        }
        else if (std::memcmp (id, "LIST", 4) == 0 && size >= 4 &&
                 std::memcmp (b.data() + body, "INFO", 4) == 0)
        {
            std::size_t p = body + 4;
            while (p + 8 <= body + size)
            {
                InfoField f;
                std::memcpy (f.id.data(), b.data() + p, 4);
                const std::uint32_t n = get32 (b.data() + p + 4);
                if (p + 8 + n > body + size)
                    return false;
                const unsigned char* text = b.data() + p + 8;
                const unsigned char* stop = std::find (text, text + n, '\0');
                f.text.assign (reinterpret_cast<const char*> (text),
                               static_cast<std::size_t> (stop - text));
                out.info.push_back (std::move (f));
                p += 8 + n + (n & 1);
            }
        }
        pos = body + size + (size & 1);
    }
    return haveFormat && haveData;
}

} // namespace sedef::record
