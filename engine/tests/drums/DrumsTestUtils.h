// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TestUtils.h"
#include "sedef/dsp/Metronome.h"
#include "sedef/dsp/drums/DrumKit.h"
#include "sedef/dsp/drums/Grooves.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sedef::test::drums
{

using sedef::dsp::Metronome;
using sedef::dsp::drums::DrumKit;
using sedef::dsp::drums::Instrument;

struct Fired
{
    long long sample = 0;
    Instrument instrument = Instrument::kick;
    int velocity = 0;

    bool operator== (const Fired&) const = default;
};

struct Render
{
    std::vector<float> left, right;
    std::vector<Fired> events;
};

struct GrooveSetup
{
    double fs = 48000.0;
    float bpm = 120.0f;
    int groove = 0;
    int intensity = 3;
    int fillEvery = 4;
    float levelDb = 0.0f;
    float tone = 0.5f;
    int signature = -1; ///< -1: the groove's own
};

inline Metronome::Settings settingsFor (const GrooveSetup& g)
{
    Metronome::Settings s;
    s.enabled = true;
    s.mode = 1;
    s.bpm = g.bpm;
    const auto& groove = sedef::dsp::drums::grooves()[static_cast<size_t> (g.groove)];
    s.signature = g.signature >= 0 ? g.signature : groove.signature;
    s.drums.groove = g.groove;
    s.drums.intensity = g.intensity;
    s.drums.fillEvery = g.fillEvery;
    s.drumsLevelDb = g.levelDb;
    s.drumsTone = g.tone;
    return s;
}

/// Renders `frames` samples of a groove from a fresh metronome, in blocks given by `blockOf`
/// (called with the block's start; returns its length).
template <typename BlockFn>
Render renderGroove (const GrooveSetup& g, int frames, BlockFn&& blockOf, int maxBlock = 4096)
{
    Metronome m;
    m.prepare ({ g.fs, maxBlock, 2 });
    m.setSettings (settingsFor (g));
    Buffer b (2, frames);
    Render r;
    for (int start = 0; start < frames;)
    {
        const int len = std::min ({ frames - start, std::max (1, blockOf (start)), maxBlock });
        m.setSettings (settingsFor (g));
        m.process (b.view().subView (start, len));
        const Metronome::FiredEvent* fired = nullptr;
        const int n = m.firedEvents (&fired);
        for (int i = 0; i < n; ++i)
            r.events.push_back (
                { start + fired[i].offset, fired[i].instrument, fired[i].velocity });
        start += len;
    }
    r.left = b[0];
    r.right = b[1];
    return r;
}

inline Render renderGroove (const GrooveSetup& g, int frames, int block = 64)
{
    return renderGroove (g, frames, [block] (int) { return block; });
}

/// Samples per metronome step for a signature at a tempo (Metronome.cpp's samplesPerStep).
inline double stepSamples (double fs, double bpm, int signature)
{
    return fs * (signature >= 4 ? 30.0 : 60.0) / bpm;
}

inline int barFrames (double fs, double bpm, int signature, int bars)
{
    return static_cast<int> (
        std::ceil (stepSamples (fs, bpm, signature) * Metronome::stepsFor (signature) * bars));
}

/// FNV-1a over the bits of both channels.
inline std::uint64_t hashOf (const Render& r)
{
    std::uint64_t h = 1469598103934665603ull;
    for (const auto* ch : { &r.left, &r.right })
        for (float s : *ch)
        {
            std::uint32_t bits = 0;
            std::memcpy (&bits, &s, sizeof bits);
            for (int k = 0; k < 4; ++k)
            {
                h ^= (bits >> (8 * k)) & 0xFFu;
                h *= 1099511628211ull;
            }
        }
    return h;
}

/// One stroke on a fresh kit, rendered for `frames` samples (Q23 converted to float).
inline Render strikeOnce (Instrument instrument, int velocity, double fs, int frames)
{
    DrumKit kit;
    kit.prepare (fs);
    kit.strike (instrument, velocity);
    std::vector<std::int32_t> l (static_cast<size_t> (frames)), rr (static_cast<size_t> (frames));
    kit.render (l.data(), rr.data(), frames);
    Render r;
    r.left.resize (l.size());
    r.right.resize (rr.size());
    for (size_t i = 0; i < l.size(); ++i)
    {
        r.left[i] = static_cast<float> (l[i]) / 8388608.0f;
        r.right[i] = static_cast<float> (rr[i]) / 8388608.0f;
    }
    return r;
}

} // namespace sedef::test::drums
