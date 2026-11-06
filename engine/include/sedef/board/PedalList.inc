// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pedal registry: the only place a pedal is declared. The engine's pedal rack, the board,
// the plug-in and the UI's generated table (ui/src/generated/pedals.json, from sedef-params
// --pedals) all come from this list. Adding a pedal: docs/architecture.md, "Adding a pedal".
//
// SEDEF_PEDAL(id, name, category, rank, enable, Module, settings)
//   id        stable id, saved in sessions and presets. Never rename one.
//   name      display name
//   category  Dynamics | Drive | Fuzz | Filter | Modulation | Pitch
//   rank      default position: Add pedal puts a pedal after the last one on the board with a
//             rank at or below its own. Leave gaps so later pedals fit in between.
//   enable    the pedal's on/off parameter in ParameterList.inc
//   Module    its engine module (dsp/), shaped like Pearl: prepare, reset, setSettings, process,
//             latencySamples. A process (io, sidechain) overload gets the raw instrument input
//             as a key, aligned to the pedal's place on the board.
//   settings  an expression building Module::Settings from the snapshot `p` (P is ParamId),
//             in parentheses
//
// The order of this list is not saved anywhere: append new pedals at the end.

SEDEF_PEDAL(gate, "Gate", Dynamics, 10, gateEnabled, dsp::NoiseGate,
            (dsp::NoiseGate::Settings { p[P::gateThreshold] }))
SEDEF_PEDAL(tide, "Tide", Dynamics, 30, tideEnabled, dsp::Tide,
            (dsp::Tide::Settings { p[P::tideComp], p[P::tideTone], p[P::tideLevel],
                                   p[P::tideBlend] }))
SEDEF_PEDAL(pearl, "Pearl", Drive, 50, pearlEnabled, dsp::Pearl,
            (dsp::Pearl::Settings { p[P::pearlGain], p[P::pearlTone], p[P::pearlLevel],
                                    p[P::pearlMid] }))
SEDEF_PEDAL(coral, "Coral", Drive, 60, coralEnabled, dsp::Coral,
            (dsp::Coral::Settings { p[P::coralGain], p[P::coralCut], p[P::coralLevel] }))
SEDEF_PEDAL(ripple, "Ripple", Modulation, 80, rippleEnabled, dsp::Ripple,
            (dsp::Ripple::Settings { p.choice (P::rippleMode), p[P::rippleSpeed],
                                     p[P::rippleDepth] }))
// The pedals lane: an octave divider, an octave-up fuzz and a mid-hump overdrive.
SEDEF_PEDAL(trench, "Trench", Pitch, 15, trenchEnabled, dsp::Trench,
            (dsp::Trench::Settings { p[P::trenchDirect], p[P::trenchSub1], p[P::trenchSub2],
                                     p[P::trenchTone] }))
SEDEF_PEDAL(urchin, "Urchin", Fuzz, 40, urchinEnabled, dsp::Urchin,
            (dsp::Urchin::Settings { p[P::urchinFuzz], p[P::urchinOctave], p[P::urchinTone],
                                     p[P::urchinLevel] }))
SEDEF_PEDAL(kelp, "Kelp", Drive, 55, kelpEnabled, dsp::Kelp,
            (dsp::Kelp::Settings { p[P::kelpDrive], p[P::kelpTone], p[P::kelpLevel] }))
// The drives lane: a two-transistor fuzz, a sustain fuzz and a treble booster.
SEDEF_PEDAL(remora, "Remora", Fuzz, 20, remoraEnabled, dsp::Remora,
            (dsp::Remora::Settings { p[P::remoraFuzz], p[P::remoraVolume], p[P::remoraBias],
                                     p[P::remoraGuitarVolume], p.choice (P::remoraTransistor) }))
SEDEF_PEDAL(sargasso, "Sargasso", Fuzz, 45, sargassoEnabled, dsp::Sargasso,
            (dsp::Sargasso::Settings { p[P::sargassoSustain], p[P::sargassoTone],
                                       p[P::sargassoVolume] }))
SEDEF_PEDAL(breaker, "Breaker", Drive, 95, breakerEnabled, dsp::Breaker,
            (dsp::Breaker::Settings { p[P::breakerBoost], p.choice (P::breakerRange) }))
// The mods lane: a wah, an envelope filter, a vibe, a phaser and a flanger. The two filters go
// after the octave divider and in front of the fuzzes and drives (a fuzz would flatten the
// picking the envelope filter reads, and wah into fuzz is the classic order); the three
// modulations after the drives, before Ripple (docs/dsp/conch.md et al.).
SEDEF_PEDAL(anemone, "Anemone", Filter, 17, anemoneEnabled, dsp::Anemone,
            (dsp::Anemone::Settings { p[P::anemoneSensitivity], p[P::anemoneRange],
                                      p[P::anemoneResonance], p.choice (P::anemoneMode),
                                      p.choice (P::anemoneDirection) }))
SEDEF_PEDAL(conch, "Conch", Filter, 18, conchEnabled, dsp::Conch,
            (dsp::Conch::Settings { p.choice (P::conchMode), p[P::conchPosition],
                                    p[P::conchResonance], p[P::conchSensitivity] }))
SEDEF_PEDAL(beacon, "Beacon", Modulation, 65, beaconEnabled, dsp::Beacon,
            (dsp::Beacon::Settings { p[P::beaconSpeed], p[P::beaconDepth],
                                     p.choice (P::beaconMode) }))
SEDEF_PEDAL(gyre, "Gyre", Modulation, 70, gyreEnabled, dsp::Gyre,
            (dsp::Gyre::Settings { p[P::gyreSpeed], p[P::gyreDepth], p[P::gyreFeedback],
                                   p.choice (P::gyreStages) }))
SEDEF_PEDAL(jetty, "Jetty", Modulation, 75, jettyEnabled, dsp::Jetty,
            (dsp::Jetty::Settings { p[P::jettySpeed], p[P::jettyDepth], p[P::jettyDelay],
                                    p[P::jettyFeedback] / 100.0f }))
