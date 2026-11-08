// SPDX-License-Identifier: AGPL-3.0-or-later
// WAV I/O for the developer tools: PCM 16/24/32 and IEEE float 32 in (folded to mono), stereo
// float out.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace sedef::tools
{

struct Audio
{
    double sampleRate = 48000.0;
    std::vector<float> mono;
};

inline std::uint32_t readU32 (const unsigned char* p)
{
    return static_cast<std::uint32_t> (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

inline std::uint16_t readU16 (const unsigned char* p)
{
    return static_cast<std::uint16_t> (p[0] | (p[1] << 8));
}

inline std::optional<Audio> readWav (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    if (!f)
        return std::nullopt;
    const std::vector<unsigned char> d ((std::istreambuf_iterator<char> (f)),
                                        std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::memcmp (d.data(), "RIFF", 4) != 0 ||
        std::memcmp (d.data() + 8, "WAVE", 4) != 0)
        return std::nullopt;

    int format = 0, channels = 0, bits = 0;
    Audio a;
    std::size_t pos = 12;
    while (pos + 8 <= d.size())
    {
        const auto size = readU32 (d.data() + pos + 4);
        const unsigned char* body = d.data() + pos + 8;
        if (std::memcmp (d.data() + pos, "fmt ", 4) == 0)
        {
            format = readU16 (body);
            channels = readU16 (body + 2);
            a.sampleRate = readU32 (body + 4);
            bits = readU16 (body + 14);
            if (format == 0xFFFE && size >= 26)
                format = readU16 (body + 24);
        }
        else if (std::memcmp (d.data() + pos, "data", 4) == 0 && channels > 0)
        {
            const int bytes = bits / 8;
            const std::size_t frames = std::min<std::size_t> (size, d.size() - (pos + 8)) /
                                       static_cast<std::size_t> (bytes * channels);
            a.mono.assign (frames, 0.0f);
            for (std::size_t i = 0; i < frames; ++i)
            {
                float acc = 0.0f;
                for (int c = 0; c < channels; ++c)
                {
                    const unsigned char* s = body + (i * static_cast<std::size_t> (channels) +
                                                     static_cast<std::size_t> (c)) *
                                                        static_cast<std::size_t> (bytes);
                    float v = 0.0f;
                    if (format == 3 && bits == 32)
                        std::memcpy (&v, s, 4);
                    else if (bits == 16)
                        v = static_cast<float> (static_cast<std::int16_t> (readU16 (s))) / 32768.0f;
                    else if (bits == 24)
                        v = static_cast<float> (
                                static_cast<std::int32_t> (readU32 (s - 1) & 0xFFFFFF00u)) /
                            2147483648.0f;
                    else if (bits == 32)
                        v = static_cast<float> (static_cast<std::int32_t> (readU32 (s))) /
                            2147483648.0f;
                    acc += v;
                }
                a.mono[i] = acc / static_cast<float> (channels);
            }
            return a;
        }
        pos += 8 + size + (size & 1);
    }
    return std::nullopt;
}

inline void writeWav (const std::string& path, const std::vector<float>& left,
                      const std::vector<float>& right, double rate)
{
    std::ofstream f (path, std::ios::binary);
    const auto u32 = [&f] (std::uint32_t v) {
        f.write (reinterpret_cast<const char*> (&v), 4);
    };
    const auto u16 = [&f] (std::uint16_t v) {
        f.write (reinterpret_cast<const char*> (&v), 2);
    };
    const auto frames = static_cast<std::uint32_t> (left.size());
    f.write ("RIFF", 4);
    u32 (36 + frames * 8);
    f.write ("WAVEfmt ", 8);
    u32 (16);
    u16 (3); // IEEE float
    u16 (2);
    u32 (static_cast<std::uint32_t> (rate));
    u32 (static_cast<std::uint32_t> (rate) * 8);
    u16 (8);
    u16 (32);
    f.write ("data", 4);
    u32 (frames * 8);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        f.write (reinterpret_cast<const char*> (&left[i]), 4);
        f.write (reinterpret_cast<const char*> (&right[i]), 4);
    }
}

} // namespace sedef::tools
