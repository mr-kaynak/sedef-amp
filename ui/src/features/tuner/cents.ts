// SPDX-License-Identifier: AGPL-3.0-or-later
import type { PitchEvent } from '../../bridge/protocol';
import { centsBetween, midiToHz, noteOf } from '../../lib/music';
import { getValue } from '../../params/paramStore';

/** Chromatic: nearest note. Target: a fixed note (the open string being tuned). */
export type GaugeTarget = { kind: 'chromatic' } | { kind: 'target'; midi: number };

export function centsFor(e: PitchEvent | null, target: GaugeTarget): number | null {
  if (!e || !e.valid) return null;
  if (target.kind === 'chromatic') return e.cents;
  return centsBetween(e.frequencyHz, midiToHz(target.midi, getValue('tunerReference')));
}

/** Matches the workbench's tuning window: a precision tuner, not a forgiving one. */
export const IN_TUNE_CENTS = 1;

/** "C sharp 4, flat": a reading as a screen reader should say it. */
export function spokenReading(midi: number, cents: number): string {
  const { note, octave } = noteOf(midi);
  const name = note.endsWith('#') ? `${note.charAt(0)} sharp` : note;
  const state = Math.abs(cents) <= IN_TUNE_CENTS ? 'in tune' : cents > 0 ? 'sharp' : 'flat';
  return `${name} ${octave}, ${state}`;
}
