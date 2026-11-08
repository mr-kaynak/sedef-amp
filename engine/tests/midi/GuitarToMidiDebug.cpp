// SPDX-License-Identifier: AGPL-3.0-or-later
// Developer aid: prints one scenario's played and sent notes side by side. Hidden; pick the
// scenario with SEDEF_MIDI_DEBUG ("bass:<seed>:<bridge|neck|piezo>", "palm:<hz>:<perBeat>:<bpm>",
// "legato:<hz>:<cents>", "ringing:<hz>:<cents>", "held:<hz>:<vibrato>:<bend>").
#include "MidiTestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

using namespace sedef;
using namespace sedef::test::midi;

TEST_CASE ("Guitar-to-MIDI debug dump", "[.mididebug]")
{
    const char* spec = std::getenv ("SEDEF_MIDI_DEBUG");
    if (spec == nullptr)
        return;
    std::stringstream ss (spec);
    std::string kind, a, b, c;
    std::getline (ss, kind, ':');
    std::getline (ss, a, ':');
    std::getline (ss, b, ':');
    std::getline (ss, c, ':');
    const double fs = 48000.0;
    Scenario sc;
    if (kind == "bass")
        sc = bassLine (static_cast<std::uint32_t> (std::stoi (a)),
                       b == "piezo" ? Pickup::piezo : (b == "neck" ? Pickup::neck : Pickup::bridge),
                       120.0 + 4.0 * std::stoi (a), fs);
    else if (kind == "palm")
        sc = palmMutedRun (std::stod (a), std::stod (c), std::stoi (b), fs);
    else if (kind == "legato")
        sc = legato (std::stod (a), std::stod (b), 4, 0.15, fs);
    else if (kind == "ringing")
        sc = overRinging (std::stod (a), std::stod (b), fs);
    else if (kind == "one")
    {
        sc.fs = fs;
        sc.signal.assign (static_cast<std::size_t> (0.6 * fs), 0.0f);
        const int seed = std::stoi (b);
        auto p =
            pluckFor (c == "piezo" ? Pickup::piezo : Pickup::bridge, std::stod (a),
                      static_cast<float> (0.1 + 0.05 * seed), static_cast<std::uint32_t> (seed));
        test::addPluck (sc.signal, fs, static_cast<std::size_t> (0.1 * fs), p);
        test::addNoise (sc.signal, -80.0f, static_cast<std::uint32_t> (seed));
        sc.notes.push_back ({ pitchOf (std::stod (a)), 0.1, 0.6, true, p.peak });
    }
    else if (kind == "held")
        sc = held (std::stod (a), fs, std::stod (b), std::stod (c));
    midi::Settings s;
    s.octave = 0;
    const auto run = convert (sc.signal, fs, s);
    const auto st = score (run, sc, 0);
    std::printf ("played:\n");
    for (const auto& n : sc.notes)
        std::printf ("  %3d  %8.3f - %8.3f  %s  %.2f\n", n.pitch, n.start, n.end,
                     n.picked ? "pick  " : "legato", static_cast<double> (n.strength));
    std::printf ("sent:\n");
    for (const auto& n : run.notes)
        std::printf ("  %3d  %8.3f - %8.3f  vel %3d\n", n.pitch, n.on, n.off, n.velocity);
    if (const char* w = std::getenv ("SEDEF_MIDI_DEBUG_WINDOW"))
    {
        const double from = std::atof (w), to = std::atof (std::strchr (w, ':') + 1);
        analysis::MonoTracker m;
        m.prepare (fs);
        std::uint32_t picks = 0;
        for (std::size_t i = 0; i < sc.signal.size(); ++i)
        {
            m.push (sc.signal[i]);
            const double t = static_cast<double> (i) / fs;
            const bool pick = m.pickCount() != picks;
            picks = m.pickCount();
            if (t < from || t > to)
                continue;
            if (pick || m.event() != analysis::MonoTracker::Event::none || i % 48 == 0)
                std::printf (
                    "  t %.4f %s%s hz %7.2f (%5.2f) est %7.2f clar %.2f lvl %.3f\n", t,
                    pick ? "PICK " : "",
                    m.event() == analysis::MonoTracker::Event::noteOn
                        ? "ON "
                        : (m.event() == analysis::MonoTracker::Event::noteChange
                               ? "CHG "
                               : (m.event() == analysis::MonoTracker::Event::noteOff ? "OFF "
                                                                                     : "")),
                    static_cast<double> (m.hz()),
                    m.hz() > 0 ? 69 + 12 * std::log2 (m.hz() / 440.0) : 0.0,
                    static_cast<double> (m.estimateHz()), static_cast<double> (m.clarity()),
                    static_cast<double> (m.pickLevel()));
        }
    }
    std::printf ("correct %d/%d wrong %d missed %d extra %d lateOff %d earlyOff %d\n", st.correct,
                 st.expected, st.wrongPitch, st.missed, st.extra, st.lateOff, st.earlyOff);
}
