// SPDX-License-Identifier: AGPL-3.0-or-later
// Transpose's added latency (docs/dsp/transpose.md, section 5). The engine's numbers:
// kLatencyDownMs and kLatencyUpMs in engine/src/dsp/PitchShifter.cpp.

export const TRANSPOSE_LATENCY_DOWN_MS = 3;
export const TRANSPOSE_LATENCY_UP_MS = 10;

/** The latency a transpose setting adds, in ms: none at 0, 3 ms down, 10 ms up. */
export function transposeLatencyMs(semitones: number): number {
  const st = Math.round(semitones);
  if (st === 0) return 0;
  return st < 0 ? TRANSPOSE_LATENCY_DOWN_MS : TRANSPOSE_LATENCY_UP_MS;
}

/** "2 semitones down", "an octave up", for screen readers and notes. */
export function describeTranspose(semitones: number): string {
  const st = Math.round(semitones);
  if (st === 0) return 'off';
  const n = Math.abs(st);
  const amount = n === 12 ? 'an octave' : n === 1 ? '1 semitone' : `${n} semitones`;
  return `${amount} ${st < 0 ? 'down' : 'up'}`;
}
