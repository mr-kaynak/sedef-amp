// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Offline renderer for presets: QA, loudness balancing and regression checks.
//
//   sedef-render --preset presets/factory/nacre.sedefpreset --out nacre.wav
//   sedef-render --in my-di.wav --set ampModel=2 --set abyssGain=0.8 --out lead.wav
//   sedef-render --check presets/factory/*.sedefpreset
//   sedef-render --capture amp.nam --set captureCab=0 --out capture.wav
//
// Without --in, a synthetic guitar DI (plucked-string chords and single notes) is used.

#include "sedef/Engine.h"
#include "sedef/core/Math.h"
#include "sedef/core/Random.h"
#include "sedef/dsp/capture/CaptureFile.h"
#include "sedef/dsp/capture/CaptureStage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace sedef;

namespace
{

// ---------------------------------------------------------------------------------------------
// Preset files: we only need the flat "parameters" object of numbers.

std::optional<std::map<std::string, float>> readPresetParameters (const std::string& path)
{
    std::ifstream f (path);
    if (!f)
        return std::nullopt;
    const std::string text ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    const auto start = text.find ("\"parameters\"");
    if (start == std::string::npos)
        return std::nullopt;
    const auto open = text.find ('{', start);
    const auto close = open == std::string::npos ? std::string::npos : text.find ('}', open);
    if (close == std::string::npos)
        return std::nullopt;

    // "parameters" is a flat object of "id": number pairs; anything else is a malformed preset.
    std::map<std::string, float> values;
    std::size_t pos = open + 1;
    while (pos < close)
    {
        const auto q1 = text.find ('"', pos);
        if (q1 == std::string::npos || q1 > close)
            break;
        const auto q2 = text.find ('"', q1 + 1);
        const auto colon = q2 == std::string::npos ? std::string::npos : text.find (':', q2);
        if (colon == std::string::npos || colon > close)
            return std::nullopt;
        const char* numberStart = text.c_str() + colon + 1;
        char* end = nullptr;
        const float v = std::strtof (numberStart, &end);
        if (end == numberStart)
            return std::nullopt;
        values[text.substr (q1 + 1, q2 - q1 - 1)] = v;
        pos = static_cast<std::size_t> (end - text.c_str());
    }
    return values;
}

// ---------------------------------------------------------------------------------------------
// WAV I/O (PCM 16/24/32 and IEEE float 32; input folded to mono).

struct Audio
{
    double sampleRate = 48000.0;
    std::vector<float> mono;
};

std::uint32_t readU32 (const unsigned char* p)
{
    return static_cast<std::uint32_t> (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

std::uint16_t readU16 (const unsigned char* p)
{
    return static_cast<std::uint16_t> (p[0] | (p[1] << 8));
}

std::optional<Audio> readWav (const std::string& path)
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

void writeWav (const std::string& path, const std::vector<float>& left,
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

// ---------------------------------------------------------------------------------------------
// Synthetic DI: Karplus-Strong strings, strummed chords then single notes, ~-18 dBFS RMS.

std::vector<float> syntheticDi (double fs, double seconds)
{
    std::vector<float> out (static_cast<std::size_t> (fs * seconds), 0.0f);
    Random rng (1234);

    const auto pluck = [&] (double hz, double start, double amp) {
        const int period = std::max (2, static_cast<int> (std::lround (fs / hz)));
        std::vector<float> ring (static_cast<std::size_t> (period));
        for (auto& s : ring)
            s = rng.bipolar() * static_cast<float> (amp);
        auto i0 = static_cast<std::size_t> (start * fs);
        float prev = 0.0f;
        for (std::size_t i = i0, k = 0; i < out.size() && k < static_cast<std::size_t> (fs * 2.5);
             ++i, ++k)
        {
            auto& cur = ring[k % ring.size()];
            const float next = 0.996f * 0.5f * (cur + prev);
            prev = cur;
            out[i] += cur;
            cur = next;
        }
    };

    const double chords[][6] = { { 82.41, 123.47, 164.81, 207.65, 246.94, 329.63 }, // E
                                 { 110.0, 164.81, 220.0, 277.18, 329.63, 0.0 },     // A
                                 { 146.83, 220.0, 293.66, 369.99, 0.0, 0.0 },       // D
                                 { 98.0, 123.47, 146.83, 196.0, 246.94, 392.0 } };  // G
    double t = 0.05;
    for (const auto& chord : chords)
    {
        for (int s = 0; s < 6; ++s)
            if (chord[s] > 0.0)
                pluck (chord[s], t + 0.012 * s, 0.22);
        t += 1.0;
    }
    const double melody[] = { 329.63, 392.0, 440.0, 493.88, 587.33, 659.26, 587.33, 493.88 };
    for (double hz : melody)
    {
        pluck (hz, t, 0.45);
        t += 0.25;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------

struct Stats
{
    float peakDb = -120.0f;
    float rmsDb = -120.0f;
    bool finite = true;
};

Stats measure (const std::vector<float>& l, const std::vector<float>& r, std::size_t skip)
{
    Stats s;
    double acc = 0.0;
    float peak = 0.0f;
    std::size_t n = 0;
    for (std::size_t i = skip; i < l.size(); ++i)
    {
        for (float v : { l[i], r[i] })
        {
            s.finite = s.finite && std::isfinite (v);
            peak = std::max (peak, std::abs (v));
            acc += static_cast<double> (v) * v;
            ++n;
        }
    }
    s.peakDb = gainToDb (peak);
    s.rmsDb = gainToDb (
        static_cast<float> (std::sqrt (acc / static_cast<double> (std::max<std::size_t> (n, 1)))));
    return s;
}

struct RenderResult
{
    std::vector<float> left, right;
    int latency = 0;
};

RenderResult render (const std::vector<float>& input, double fs, const ParamSnapshot& params,
                     int block = 256, const dsp::CaptureFile* capture = nullptr)
{
    Engine engine;
    engine.prepare (fs, block, 1, 2);
    if (capture != nullptr)
        for (int c = 0; c < engine.numCabs(); ++c)
        {
            std::string error;
            auto model = capture->instantiate (engine.capture (c).modelSpec(), &error);
            if (model == nullptr)
                std::fprintf (stderr, "capture: %s\n", error.c_str());
            engine.capture (c).setModel (std::move (model));
        }
    RenderResult r;
    r.left.assign (input.size(), 0.0f);
    r.right.assign (input.size(), 0.0f);
    r.latency = engine.latencySamples();
    Transport transport;
    for (std::size_t s = 0; s < input.size(); s += static_cast<std::size_t> (block))
    {
        const int n = static_cast<int> (
            std::min<std::size_t> (static_cast<std::size_t> (block), input.size() - s));
        const float* in[1] = { input.data() + s };
        float* out[2] = { r.left.data() + s, r.right.data() + s };
        engine.process (in, out, n, params, transport);
    }
    return r;
}

bool applyParameters (ParamSnapshot& snap, const std::map<std::string, float>& values,
                      const std::string& source)
{
    bool ok = true;
    for (const auto& [key, value] : values)
    {
        if (const auto id = findParam (key))
            snap[*id] = spec (*id).sanitise (value);
        else
        {
            std::fprintf (stderr, "%s: unknown parameter '%s'\n", source.c_str(), key.c_str());
            ok = false;
        }
    }
    return ok;
}

int usage()
{
    std::fputs ("usage: sedef-render [--preset FILE] [--in DI.wav] [--out OUT.wav] [--seconds S]\n"
                "                    [--rate HZ] [--capture FILE.nam] [--set id=value]...\n"
                "       sedef-render --check PRESET...\n",
                stderr);
    return 2;
}

} // namespace

int main (int argc, char** argv)
{
    std::vector<std::string> args (argv + 1, argv + argc);
    if (args.empty())
        return usage();

    double rate = 48000.0;
    double seconds = 7.0;

    if (args[0] == "--check")
    {
        // Every factory preset must render finite, with headroom, and inside a narrow level window.
        // Factory presets are balanced to -16 LUFS with this DI (see presets/README.md), which
        // lands between -23 and -19 dBFS RMS depending on how bright and compressed the sound is.
        const auto di = syntheticDi (rate, seconds);
        int failures = 0;
        for (std::size_t i = 1; i < args.size(); ++i)
        {
            auto snap = ParamSnapshot::defaults();
            const auto values = readPresetParameters (args[i]);
            if (!values || !applyParameters (snap, *values, args[i]))
            {
                std::printf ("FAIL  %-48s unreadable\n", args[i].c_str());
                ++failures;
                continue;
            }
            const auto r = render (di, rate, snap);
            const auto s = measure (r.left, r.right, static_cast<std::size_t> (rate * 0.5));
            const bool ok = s.finite && s.peakDb < -1.0f && s.rmsDb > -24.0f && s.rmsDb < -18.0f;
            std::printf ("%s  %-48s rms %6.1f dBFS  peak %6.1f dBFS\n", ok ? "ok  " : "FAIL",
                         args[i].c_str(), static_cast<double> (s.rmsDb),
                         static_cast<double> (s.peakDb));
            failures += ok ? 0 : 1;
        }
        return failures == 0 ? 0 : 1;
    }

    std::string presetPath, inPath, outPath, capturePath;
    std::map<std::string, float> overrides;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const auto next = [&] {
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        if (args[i] == "--preset")
            presetPath = next();
        else if (args[i] == "--in")
            inPath = next();
        else if (args[i] == "--out")
            outPath = next();
        else if (args[i] == "--seconds")
            seconds = std::stod (next());
        else if (args[i] == "--rate")
            rate = std::stod (next());
        else if (args[i] == "--capture")
            capturePath = next();
        else if (args[i] == "--set")
        {
            const auto kv = next();
            const auto eq = kv.find ('=');
            if (eq == std::string::npos)
                return usage();
            overrides[kv.substr (0, eq)] = std::stof (kv.substr (eq + 1));
        }
        else
            return usage();
    }

    auto snap = ParamSnapshot::defaults();
    if (!presetPath.empty())
    {
        const auto values = readPresetParameters (presetPath);
        if (!values)
        {
            std::fprintf (stderr, "cannot read preset %s\n", presetPath.c_str());
            return 1;
        }
        applyParameters (snap, *values, presetPath);
    }
    // A capture plays in place of the amp unless --set ampSource=0 says otherwise.
    std::shared_ptr<const dsp::CaptureFile> capture;
    if (!capturePath.empty())
    {
        std::ifstream file (capturePath, std::ios::binary);
        const std::string text ((std::istreambuf_iterator<char> (file)),
                                std::istreambuf_iterator<char>());
        auto parsed = dsp::CaptureFile::parse (text);
        if (parsed.file == nullptr)
        {
            std::fprintf (stderr, "%s: %s\n", capturePath.c_str(), parsed.error.c_str());
            return 1;
        }
        capture = parsed.file;
        const auto& info = capture->info();
        std::fprintf (
            stderr,
            "capture: %s, %ld weights, %.0f Hz, measured %.1f dBFS, normalised by %+.1f dB\n",
            info.architecture.c_str(), info.numWeights, info.sampleRate,
            static_cast<double> (capture->measuredDbfs()),
            static_cast<double> (capture->normalisationDb()));
        snap[ParamId::ampSource] = 1.0f;
    }
    applyParameters (snap, overrides, "--set");

    std::vector<float> input;
    if (!inPath.empty())
    {
        const auto wav = readWav (inPath);
        if (!wav)
        {
            std::fprintf (stderr, "cannot read %s (PCM/float WAV only)\n", inPath.c_str());
            return 1;
        }
        rate = wav->sampleRate;
        input = wav->mono;
    }
    else
    {
        input = syntheticDi (rate, seconds);
    }

    const auto r = render (input, rate, snap, 256, capture.get());
    const auto s = measure (r.left, r.right, 0);
    std::printf ("{\"rmsDb\": %.2f, \"peakDb\": %.2f, \"finite\": %s, \"latency\": %d}\n",
                 static_cast<double> (s.rmsDb), static_cast<double> (s.peakDb),
                 s.finite ? "true" : "false", r.latency);
    if (!outPath.empty())
        writeWav (outPath, r.left, r.right, rate);
    return s.finite ? 0 : 1;
}
