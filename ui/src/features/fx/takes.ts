// SPDX-License-Identifier: AGPL-3.0-or-later
// How far the Doubler's second take trails the first, mirroring sedef::dsp::Doubler: each note
// lands at spread * 2^(variation * z) ms, z roughly unit normal and clamped to +-2.5, within
// 2.5..40 ms. Keep the constants in sync with engine/src/dsp/Doubler.cpp.

const MIN_OFFSET_MS = 2.5;
const MAX_OFFSET_MS = 40;
const Z_LIMIT = 2.5;
/** 80 % of a normal distribution lies within +-1.2816. */
const Z_CENTRAL_80 = 1.2816;

export function offsetMs(spreadMs: number, variation: number, z: number): number {
  const clampedZ = Math.max(-Z_LIMIT, Math.min(Z_LIMIT, z));
  const offset = spreadMs * 2 ** (variation * clampedZ);
  return Math.max(MIN_OFFSET_MS, Math.min(MAX_OFFSET_MS, offset));
}

/** The range most notes (the central 80 %) of the second take land in, rounded to whole ms. */
export function typicalRangeMs(spreadMs: number, variation: number): [number, number] {
  return [
    Math.round(offsetMs(spreadMs, variation, -Z_CENTRAL_80)),
    Math.round(offsetMs(spreadMs, variation, Z_CENTRAL_80)),
  ];
}

/** "6 ms behind" for a fixed offset, "4–9 ms behind" when the notes vary. */
export function describeOffsets(spreadMs: number, variation: number): string {
  const [lo, hi] = typicalRangeMs(spreadMs, variation);
  return lo === hi ? `${lo} ms behind` : `${lo}–${hi} ms behind`;
}

/** Where the two takes sit, from the Width control (0..1). */
export function describeWidth(width: number): string {
  if (width <= 0.02) return 'Both takes in the centre';
  if (width >= 0.98) return 'Second take on the right';
  return 'Second take right of centre';
}

/** A fixed, made-up phrase for the sketch: z per note, partly carried from the note before. */
export const SKETCH_Z = [0.3, -0.5, 0.6, 1.1, 0.2, -1.2, -0.4, 0.9];
/** Per-note level of the sketch's second take, -1..1 of the variation's range. */
export const SKETCH_LEVEL = [0.2, -0.6, 0.5, -0.1, 0.8, -0.4, 0.1, -0.8];
