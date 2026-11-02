// SPDX-License-Identifier: AGPL-3.0-or-later
import { noteOf } from '../../lib/music';

/** Chromatic: nearest note. Target: a fixed note (the open string being tuned). */
export type GaugeTarget = { kind: 'chromatic' } | { kind: 'target'; midi: number };

/**
 * The tuner's in-tune zone: a precision tuner, not a forgiving one. The intonation workbench
 * passes its own, wider tolerance for the open string (TunerGauge's inTuneCents).
 */
export const IN_TUNE_CENTS = 1;

function spokenNote(midi: number): string {
  const { note, octave } = noteOf(midi);
  const name = note.endsWith('#') ? `${note.charAt(0)} sharp` : note;
  return `${name} ${octave}`;
}

/** A state the gauge has settled on, as a screen reader should say it: "E 2, flat, tune up". */
export function spokenState(midi: number, tune: 'in-tune' | 'sharp' | 'flat'): string {
  const what =
    tune === 'in-tune' ? 'in tune' : tune === 'sharp' ? 'sharp, tune down' : 'flat, tune up';
  return `${spokenNote(midi)}, ${what}`;
}
