// SPDX-License-Identifier: AGPL-3.0-or-later
// The Sedef MIDI parameter schema: its own product, its own list (ADR-0004 applies to it as to
// Sedef Amp's). Ids are a public contract from the first release: append only, never rename,
// reorder or change a range or a choice count.
//
// SEDEF_MIDI_PARAM(id, name, kind, min, max, default, unit, choices, automatable)

SEDEF_MIDI_PARAM(octave, "Octave", Choice, 0.0f, 3.0f, 1.0f, "", "-2|-1|0|+1", true)
SEDEF_MIDI_PARAM(mode, "Mode", Choice, 0.0f, 1.0f, 0.0f, "", "Notes|Notes + Bend", true)
SEDEF_MIDI_PARAM(bendRange, "Bend Range", Int, 1.0f, 24.0f, 2.0f, "st", "", false)
SEDEF_MIDI_PARAM(timing, "Timing", Choice, 0.0f, 1.0f, 0.0f, "", "Fastest|Steady", false)
SEDEF_MIDI_PARAM(channel, "MIDI Channel", Int, 1.0f, 16.0f, 1.0f, "", "", false)
SEDEF_MIDI_PARAM(gate, "Gate", Float, -80.0f, -20.0f, -54.0f, "dB", "", true)
SEDEF_MIDI_PARAM(dynamics, "Velocity Sensitivity", Float, 0.0f, 100.0f, 75.0f, "%", "", true)
SEDEF_MIDI_PARAM(curve, "Velocity Curve", Choice, 0.0f, 2.0f, 1.0f, "", "Soft|Linear|Hard", false)
SEDEF_MIDI_PARAM(audio, "Audio", Choice, 0.0f, 1.0f, 0.0f, "", "Through|Mute", false)
