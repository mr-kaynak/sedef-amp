// SPDX-License-Identifier: AGPL-3.0-or-later
// Intonation math. The 12th fret halves the string, so the fretted note should sound exactly one
// octave above the open string. Any deviation means the saddle sits in the wrong place:
//
//   octave error   c  = 1200 · log2(f12 / (2 · f0))            cents
//   saddle offset  Δs = L · (2^(c/1200) − 1)                   same unit as L
//
// Positive c (sharp) -> positive Δs -> move the saddle back, away from the neck (lengthens the
// string). Negative c (flat) -> move it forward, toward the neck. The estimate is first order:
// string stiffness and fretting pressure also shift pitch, so it is a starting point to
// re-measure from, not a final answer (see docs/architecture.md §8).

/** Readings within this many cents of a perfect octave count as in tune. */
export const IN_TUNE_CENTS = 1.5;

export type Verdict = 'in-tune' | 'move-back' | 'move-forward';

export function octaveErrorCents(openHz: number, frettedHz: number): number {
  return 1200 * Math.log2(frettedHz / (2 * openHz));
}

export function saddleOffsetMm(scaleLengthMm: number, cents: number): number {
  return scaleLengthMm * (Math.pow(2, cents / 1200) - 1);
}

export function verdictFor(cents: number): Verdict {
  if (Math.abs(cents) <= IN_TUNE_CENTS) return 'in-tune';
  return cents > 0 ? 'move-back' : 'move-forward';
}

export const VERDICT_TEXT: Record<Verdict, string> = {
  'in-tune': 'In tune, leave the saddle where it is',
  'move-back': 'Move the saddle back, away from the neck',
  'move-forward': 'Move the saddle forward, toward the neck',
};

export interface Adjustment {
  cents: number;
  verdict: Verdict;
  /** Signed: positive = back (away from the neck). */
  offsetMm: number;
}

export function adjustmentFor(scaleLengthMm: number, cents: number): Adjustment {
  return { cents, verdict: verdictFor(cents), offsetMm: saddleOffsetMm(scaleLengthMm, cents) };
}

export function inchesToMm(inches: number): number {
  return inches * 25.4;
}

/** "+1.50 mm" style, with a typographic minus. */
export function formatMm(mm: number): string {
  const v = Number(mm.toFixed(2));
  if (v === 0) return '0.00 mm';
  return `${v > 0 ? '+' : '−'}${Math.abs(v).toFixed(2)} mm`;
}

export function formatCents(cents: number, digits = 1): string {
  const v = Number(cents.toFixed(digits));
  if (v === 0) return `${(0).toFixed(digits)}¢`;
  return `${v > 0 ? '+' : '−'}${Math.abs(v).toFixed(digits)}¢`;
}
