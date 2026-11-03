// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Tuner display scenarios: what a player does while tuning, as synthetic signals, and the
// PitchTracker readings the editor would send for them (one per 60 Hz timer tick). Shared by the
// display tests, the [.report] that prints the before/after table in docs/dsp/tuner.md and the
// recorder of the UI's test streams (ui/src/features/tuner/fixtures).

#include "PitchTestSignals.h"
#include "sedef/analysis/PitchTracker.h"

#include <cmath>
#include <string>
#include <vector>

namespace sedef::test
{

constexpr double kTunerFs = 48000.0;
constexpr double kTunerPoll = 1.0 / 60.0; ///< the editor's timer

/// One scenario. The last pluck is the note being tuned; times are seconds after its pick.
struct TunerScenario
{
    std::string name;
    std::string what;
    double seconds = 3.2; ///< from the start of the signal
    double pickAt = 0.2;  ///< of the note being tuned
    Pluck note;
    bool ringing = false;     ///< an earlier pluck (another string, or this one before a retune)
    double previousAt = -1.0; ///< its pick, relative to the tuned note's
    Pluck previous;
    float noiseDb = -75.0f;
    float humDb = -200.0f;
    double mainsHz = 60.0;
    double nominalHz = 0.0;   ///< the equal-tempered note being tuned (A4 = 440)
    double sustainFrom = 0.6; ///< window for the steadiness metrics
    double sustainTo = 2.4;   ///<
    double moveAt = -1.0;     ///< start of a step or peg turn
    double moveEnd = -1.0;    ///< end of the move (== moveAt for a step)
    double endsAt = 1.0e9;    ///< the note is dying from about here (for the hold metric)

    std::vector<float> render (double fs) const
    {
        std::vector<float> sig (static_cast<size_t> (seconds * fs), 0.0f);
        if (ringing)
            addPluck (sig, fs, static_cast<size_t> ((pickAt + previousAt) * fs), previous);
        addPluck (sig, fs, static_cast<size_t> (pickAt * fs), note);
        addNoise (sig, noiseDb, note.seed + 11u);
        if (humDb > -150.0f)
            addHum (sig, fs, mainsHz, humDb);
        return sig;
    }

    /// True pitch of the tuned note, cents from the nominal note, t seconds after its pick.
    double trueCents (double t) const
    {
        return centsError (note.f1, nominalHz) + note.centsAt (std::max (0.0, t));
    }
};

inline double etHz (int midi)
{
    return 440.0 * std::exp2 ((midi - 69) / 12.0);
}

inline Pluck guitarPluck (int midi, double detuneCents, std::uint32_t seed)
{
    Pluck p;
    p.f1 = etHz (midi) * std::exp2 (detuneCents / 1200.0);
    p.inharmonicity = midi >= 64 ? 1.5e-4 : 1.0e-4;
    p.glideCents = 9.0;
    p.decaySeconds = midi > 60 ? 1.6 : 2.5;
    p.seed = seed;
    return p;
}

/// The scenarios of docs/dsp/tuner.md, "Display".
inline std::vector<TunerScenario> tunerScenarios()
{
    std::vector<TunerScenario> out;
    const auto add = [&] (const char* name, const char* what, int midi, double detune,
                          std::uint32_t seed) -> TunerScenario& {
        TunerScenario s;
        s.name = name;
        s.what = what;
        s.note = guitarPluck (midi, detune, seed);
        s.nominalHz = etHz (midi);
        out.push_back (s);
        return out.back();
    };

    add ("e2-clean", "E2 in tune, clean", 40, 0.0, 1);

    auto& hum = add ("e2-hum", "E2 in tune, 60 Hz hum 28 dB under the note", 40, 0.0, 2);
    hum.humDb = -45.0f;

    auto& hum50 = add ("a2-hum", "A2 2 cents flat, 50 Hz hum at -45 dBFS", 45, -2.0, 3);
    hum50.humDb = -45.0f;
    hum50.mainsHz = 50.0;

    // As in the intonation tests: wobble, a slow flat sag, noise and a little hum.
    auto& real = add ("g3-real", "G3 in tune, +-1 cent wobble, -1.5 c/s sag, hum", 55, 0.0, 4);
    real.note.wobble = Wobble (1.0, 4u, 1.5);
    real.note.driftCentsPerSecond = -1.5;
    real.noiseDb = -70.0f;
    real.humDb = -62.0f;

    auto& edge = add ("b3-edge", "B3 0.6 cent sharp, +-0.3 cent wobble", 59, 0.6, 5);
    edge.note.wobble = Wobble (0.3, 5u, 1.5);

    auto& prev =
        add ("a2-after-e2", "A2 3 cents sharp while the E2 plucked 1 s earlier rings", 45, 3.0, 6);
    prev.ringing = true;
    prev.previous = guitarPluck (40, 0.0, 7);
    prev.pickAt = 1.2;
    prev.seconds = 4.2;
    prev.sustainFrom = 0.9; // the long window needs ~0.7 s to outweigh the pulled reading

    auto& vib = add ("g3-vibrato", "G3, +-12 cent vibrato at 5.5 Hz from 1 s", 55, 0.0, 8);
    vib.note.vibratoAt = 1.0;
    vib.note.vibratoCents = 12.0;
    vib.sustainFrom = 1.2;

    auto& turn = add ("a2-peg", "A2 20 cents flat, peg turned up to pitch over 1 s", 45, -20.0, 9);
    turn.note.turnAt = 0.8;
    turn.note.turnSeconds = 1.0;
    turn.note.turnCents = 20.0;
    turn.note.decaySeconds = 3.0;
    turn.seconds = 3.6;
    turn.moveAt = 0.8;
    turn.moveEnd = 1.8;
    turn.sustainFrom = 2.1;
    turn.sustainTo = 3.3;

    auto& step = add ("g3-step", "G3 20 cents flat, steps to pitch at 1 s", 55, -20.0, 10);
    step.note.stepAt = 1.0;
    step.note.stepCents = 20.0;
    step.moveAt = step.moveEnd = 1.0;
    step.sustainFrom = 1.4;

    auto& dies = add ("d3-dies", "D3 7 cents flat, dies away", 50, -7.0, 11);
    dies.note.decaySeconds = 0.45;
    dies.sustainTo = 1.2;
    dies.endsAt = 1.0;

    auto& weak = add ("e2-bridge", "E2 in tune, fundamental 20 dB down", 40, 0.0, 12);
    weak.note.fundamentalGain = 0.1;

    auto& bass = add ("e1-bass", "Bass E1 4 cents sharp", 28, 4.0, 13);
    bass.note.inharmonicity = 1.0e-4;
    bass.sustainFrom = 0.9;

    // A hammer-on: the pitch steps two semitones without a new pick.
    auto& legato = add ("g3-hammer", "G3, hammer-on to A3 at 1 s", 55, 0.0, 14);
    legato.note.stepAt = 1.0;
    legato.note.stepCents = 200.0;
    legato.sustainFrom = 1.4;

    // Muted, retuned in silence and picked again: the same note, 6 cents higher.
    auto& again =
        add ("e4-repluck", "E4 6 cents flat, muted, retuned and picked again", 64, 0.0, 15);
    again.ringing = true;
    again.previous = guitarPluck (64, -6.0, 16);
    again.previous.decaySeconds = 0.3;
    again.previousAt = -1.2;
    again.pickAt = 1.4;
    again.seconds = 3.4;
    return out;
}

/// What the editor sends: the tracker fed in 128-sample blocks, read every 60 Hz tick.
struct TunerPoll
{
    double t; ///< seconds after the pick of the tuned note
    analysis::PitchTracker::Reading r;
};

inline std::vector<TunerPoll> pollTracker (const TunerScenario& s, double fs = kTunerFs)
{
    const auto sig = s.render (fs);
    analysis::PitchTracker tracker;
    tracker.prepare (fs);
    std::vector<TunerPoll> out;
    size_t pos = 0;
    const double length = static_cast<double> (sig.size()) / fs;
    for (double t = kTunerPoll; t < length; t += kTunerPoll)
    {
        const auto end = std::min (sig.size(), static_cast<size_t> (t * fs));
        while (pos < end)
        {
            const int n = static_cast<int> (std::min<size_t> (128, end - pos));
            tracker.push (sig.data() + pos, n);
            pos += static_cast<size_t> (n);
        }
        out.push_back ({ t - s.pickAt, tracker.latest() });
    }
    return out;
}

/// Like pollTracker, but the host hands over whole blocks of `block` samples and each tick sees
/// only the blocks that have ended by then: with 4096 samples at 44.1 kHz the reading the editor
/// gets is up to 93 ms old.
inline std::vector<TunerPoll> pollHostBlocks (const TunerScenario& s, double fs, int block)
{
    const auto sig = s.render (fs);
    analysis::PitchTracker tracker;
    tracker.prepare (fs);
    std::vector<TunerPoll> out;
    const auto n = static_cast<size_t> (block);
    size_t pos = 0;
    const double length = static_cast<double> (sig.size()) / fs;
    for (double t = kTunerPoll; t < length; t += kTunerPoll)
    {
        const auto end = std::min (sig.size(), static_cast<size_t> (t * fs));
        for (; pos + n <= end; pos += n)
            tracker.push (sig.data() + pos, block);
        out.push_back ({ t - s.pickAt, tracker.latest() });
    }
    return out;
}

} // namespace sedef::test
