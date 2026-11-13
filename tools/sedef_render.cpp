// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Offline renderer for presets: QA, loudness balancing and regression checks.
//
//   sedef-render --preset presets/factory/nacre.sedefpreset --out nacre.wav
//   sedef-render --in my-di.wav --set ampModel=2 --set abyssGain=0.8 --out lead.wav
//   sedef-render --check presets/factory/*.sedefpreset
//   sedef-render --check-boards presets/factory/*.sedefpreset
//   sedef-render --capture amp.nam --set captureCab=0 --out capture.wav
//   sedef-render --set pearlEnabled=1 --set coralEnabled=1 --board coral,pearl --out swapped.wav
//   sedef-render --board gate,pearl,ripple --switch 2.5=ripple,gate,pearl --out reorder.wav
//   sedef-render --preset presets/factory/nacre.sedefpreset --set oversampling=1 --out lin.wav
//   sedef-render --preset presets/factory/nacre.sedefpreset --di-gain -18 --out quiet.wav
//
// Without --in, a synthetic guitar DI (plucked-string chords and single notes) is used. The
// pedalboard is --board, else the preset's "board", else the board a preset saved before the
// board gets: the legacy board plus any other pedal the parameters switch on.

#include "WavIo.h"
#include "sedef/Engine.h"
#include "sedef/board/Board.h"
#include "sedef/board/BoardText.h"
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
using sedef::tools::readWav;
using sedef::tools::writeWav;

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

/// A preset's "board": pedal ids in signal order (docs/board.md §3.3). Nothing when the preset
/// has none.
std::optional<Board> readPresetBoard (const std::string& path)
{
    std::ifstream f (path);
    const std::string text ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    const auto key = text.find ("\"board\"");
    if (key == std::string::npos)
        return std::nullopt;
    const auto open = text.find ('[', key);
    const auto close = open == std::string::npos ? std::string::npos : text.find (']', open);
    if (close == std::string::npos)
        return std::nullopt;
    BoardReader reader;
    for (auto q1 = text.find ('"', open); q1 < close; q1 = text.find ('"', q1 + 1))
    {
        const auto q2 = text.find ('"', q1 + 1);
        reader.read (std::string_view (text).substr (q1 + 1, q2 - q1 - 1));
        q1 = q2;
    }
    return reader.board();
}

/// --board and --switch: "gate,kelp,kelp#2,amp,gyre" (docs/board.md §3.3).
Board boardFromList (const std::string& list)
{
    std::stringstream ss (list);
    for (std::string id; std::getline (ss, id, ',');)
        if (id != kAmpToken && !refOf (id))
            std::fprintf (stderr, "--board: unknown pedal '%s'\n", id.c_str());
    return boardFromText (list);
}

Board legacyBoard (const ParamSnapshot& snap)
{
    return Board::legacyFor ([&snap] (ParamId id) { return snap.flag (id); });
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

/// A board the render switches to at a given sample (--switch), as the plug-in would mid-play.
struct BoardSwitchAt
{
    std::size_t sample = 0;
    Board board;
};

RenderResult render (const std::vector<float>& input, double fs, const ParamSnapshot& params,
                     const Board& board, int block = 256, const dsp::CaptureFile* capture = nullptr,
                     std::vector<BoardSwitchAt> switches = {})
{
    Engine engine;
    engine.setBoard (board);
    // The oversampling is a session setting the engine takes at prepare (--set oversampling=1
    // renders in linear phase).
    engine.setOversampling (params.choice (ParamId::oversampling) == 1
                                ? OversamplingMode::linearPhase
                                : OversamplingMode::lowLatency);
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
    std::sort (switches.begin(), switches.end(),
               [] (const auto& a, const auto& b) { return a.sample < b.sample; });
    auto pending = switches.begin();
    for (std::size_t s = 0, n = 0; s < input.size(); s += n)
    {
        while (pending != switches.end() && pending->sample <= s)
            engine.setBoard ((pending++)->board);
        // A block ends where the next switch is due, so it lands on its sample.
        auto end = std::min (s + static_cast<std::size_t> (block), input.size());
        if (pending != switches.end())
            end = std::min (end, pending->sample);
        n = end - s;
        const float* in[1] = { input.data() + s };
        float* out[2] = { r.left.data() + s, r.right.data() + s };
        engine.process (in, out, static_cast<int> (n), params, transport);
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

// The quiet-guitar part of --check: the DI's first chords peaking at -30 dBFS, then fading out
// at 12 dB a second, a note left to ring.
constexpr float kQuietPeakDb = -30.0f;
constexpr double kQuietPlaySeconds = 2.0;
constexpr double kQuietFadeSeconds = 3.5;
constexpr float kQuietFadeDb = 42.0f;
constexpr float kMinGateHoldDb = 24.0f;

std::vector<float> decayingDi (double fs)
{
    auto x = syntheticDi (fs, kQuietPlaySeconds + kQuietFadeSeconds);
    float peak = 0.0f;
    for (float v : x)
        peak = std::max (peak, std::abs (v));
    const float g = dbToGain (kQuietPeakDb) / std::max (peak, 1.0e-9f);
    const auto fadeFrom = static_cast<std::size_t> (kQuietPlaySeconds * fs);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double into = i > fadeFrom ? static_cast<double> (i - fadeFrom) / fs : 0.0;
        x[i] *= g * dbToGain (static_cast<float> (-kQuietFadeDb * into / kQuietFadeSeconds));
    }
    return x;
}

/// How far under its peak the DI has fallen (dB, in 50 ms windows) when the gate first takes
/// more than 6 dB off the render; kQuietFadeDb when it never does.
float gateHoldsDb (const std::vector<float>& di, double fs, const RenderResult& gated,
                   const RenderResult& open)
{
    const auto window = static_cast<std::size_t> (fs * 0.05);
    const auto energy = [] (const std::vector<float>& x, std::size_t from, std::size_t n) {
        double acc = 1.0e-20;
        for (std::size_t i = from; i < from + n; ++i)
            acc += static_cast<double> (x[i]) * x[i];
        return acc;
    };
    for (auto at = static_cast<std::size_t> (kQuietPlaySeconds * fs); at + window <= di.size();
         at += window)
    {
        const double g = energy (gated.left, at, window) + energy (gated.right, at, window);
        const double o = energy (open.left, at, window) + energy (open.right, at, window);
        if (g >= o * 0.25)
            continue;
        float peak = 0.0f;
        for (std::size_t i = at; i < at + window; ++i)
            peak = std::max (peak, std::abs (di[i]));
        return kQuietPeakDb - gainToDb (peak);
    }
    return kQuietFadeDb;
}

int usage()
{
    std::fputs (
        "usage: sedef-render [--preset FILE] [--in DI.wav] [--out OUT.wav] [--seconds S]\n"
        "                    [--di-gain DB] [--rate HZ] [--capture FILE.nam] [--set id=value]...\n"
        "                    [--board id,id,...] [--switch SECONDS=id,id,...]...\n"
        "       sedef-render --check PRESET...\n"
        "       sedef-render --check-boards PRESET...\n",
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
        // Its gate must also suit a quiet guitar (an interface at minimum gain): on a DI that
        // peaks at -30 dBFS and then dies away, the gate has to stay open until the guitar is at
        // least 24 dB under that peak.
        const auto di = syntheticDi (rate, seconds);
        const auto quietDi = decayingDi (rate);
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
            const auto board = readPresetBoard (args[i]).value_or (legacyBoard (snap));
            const auto r = render (di, rate, snap, board);
            const auto s = measure (r.left, r.right, static_cast<std::size_t> (rate * 0.5));
            auto open = snap;
            open[ParamId::gateThreshold] = spec (ParamId::gateThreshold).min; // the gate off
            const float held = gateHoldsDb (quietDi, rate, render (quietDi, rate, snap, board),
                                            render (quietDi, rate, open, board));
            const bool ok = s.finite && s.peakDb < -1.0f && s.rmsDb > -24.0f && s.rmsDb < -18.0f &&
                            held >= kMinGateHoldDb;
            std::printf ("%s  %-48s rms %6.1f dBFS  peak %6.1f dBFS  gate holds a quiet note "
                         "%s%.0f dB down\n",
                         ok ? "ok  " : "FAIL", args[i].c_str(), static_cast<double> (s.rmsDb),
                         static_cast<double> (s.peakDb), held >= kQuietFadeDb ? "over " : "",
                         static_cast<double> (std::min (held, kQuietFadeDb)));
            failures += ok ? 0 : 1;
        }
        return failures == 0 ? 0 : 1;
    }

    if (args[0] == "--check-boards")
    {
        // A factory preset's board holds exactly the pedals it switches on, and plays exactly
        // what the fixed chain played (the legacy board), in either oversampling mode.
        const auto di = syntheticDi (rate, 3.0);
        int failures = 0;
        for (std::size_t i = 1; i < args.size(); ++i)
        {
            auto snap = ParamSnapshot::defaults();
            const auto values = readPresetParameters (args[i]);
            const auto board = readPresetBoard (args[i]);
            std::string why;
            if (!values || !applyParameters (snap, *values, args[i]))
                why = "unreadable";
            else if (!board)
                why = "no board";
            else
                for (std::size_t k = 0; k < kNumSlots && why.empty(); ++k)
                    if (snap.flag (enableOf (slotRef (k))) != board->contains (slotRef (k)))
                        why = tokenOf (slotRef (k)) + (board->contains (slotRef (k))
                                                           ? " is off but on the board"
                                                           : " is on but off the board");
            for (const float mode : { 0.0f, 1.0f })
            {
                if (!why.empty())
                    break;
                snap[ParamId::oversampling] = mode;
                const auto a = render (di, rate, snap, *board);
                const auto b = render (di, rate, snap, legacyBoard (snap));
                if (a.left != b.left || a.right != b.right || a.latency != b.latency)
                    why = mode > 0.5f ? "differs from the legacy board (linear phase)"
                                      : "differs from the legacy board (low latency)";
            }
            std::printf ("%s  %-48s %s\n", why.empty() ? "ok  " : "FAIL", args[i].c_str(),
                         why.c_str());
            failures += why.empty() ? 0 : 1;
        }
        return failures == 0 ? 0 : 1;
    }

    std::string presetPath, inPath, outPath, capturePath;
    float diGainDb = 0.0f; // --di-gain: a quieter or hotter guitar, whatever the DI is
    std::optional<std::string> boardList;
    std::vector<std::pair<double, std::string>> switchLists;
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
        else if (args[i] == "--di-gain")
            diGainDb = std::stof (next());
        else if (args[i] == "--capture")
            capturePath = next();
        else if (args[i] == "--board")
            boardList = next();
        else if (args[i] == "--switch")
        {
            const auto at = next();
            const auto eq = at.find ('=');
            if (eq == std::string::npos)
                return usage();
            switchLists.emplace_back (std::stod (at.substr (0, eq)), at.substr (eq + 1));
        }
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
    float diPeak = 0.0f;
    for (auto& v : input)
    {
        v *= dbToGain (diGainDb);
        diPeak = std::max (diPeak, std::abs (v));
    }

    std::optional<Board> board;
    if (!presetPath.empty())
        board = readPresetBoard (presetPath);
    if (boardList)
        board = boardFromList (*boardList);
    std::vector<BoardSwitchAt> switches;
    for (const auto& [at, list] : switchLists)
        switches.push_back (
            { static_cast<std::size_t> (std::max (0.0, at) * rate), boardFromList (list) });
    const auto r = render (input, rate, snap, board.value_or (legacyBoard (snap)), 256,
                           capture.get(), std::move (switches));
    const auto s = measure (r.left, r.right, 0);
    std::printf ("{\"rmsDb\": %.2f, \"peakDb\": %.2f, \"finite\": %s, \"latency\": %d, "
                 "\"diPeakDb\": %.2f}\n",
                 static_cast<double> (s.rmsDb), static_cast<double> (s.peakDb),
                 s.finite ? "true" : "false", r.latency, static_cast<double> (gainToDb (diPeak)));
    if (!outPath.empty())
        writeWav (outPath, r.left, r.right, rate);
    return s.finite ? 0 : 1;
}
