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
