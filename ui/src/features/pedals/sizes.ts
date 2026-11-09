// SPDX-License-Identifier: AGPL-3.0-or-later
// How big each enclosure is, in board units (one canvas pixel at the smallest scale). The width
// follows what the pedal carries: one column of controls is compact, two are regular, and a pedal
// with a wide switch or four knobs and a switch is wide. The board's layout (layout.ts) and each
// panel read the same table.

export interface PedalBox {
  /** Width and height in board units. */
  w: number;
  h: number;
}

const compact = (h: number): PedalBox => ({ w: 92, h });
const regular = (h: number): PedalBox => ({ w: 120, h });
const wide = (h: number): PedalBox => ({ w: 148, h });

export const PEDAL_BOX = {
  gate: compact(236),
  breaker: compact(252),
  beacon: regular(244),
  ripple: regular(244),
  coral: regular(260),
  pearl: regular(264),
  sargasso: regular(264),
  jetty: regular(264),
  kelp: regular(272),
  gyre: regular(272),
  tide: regular(276),
  urchin: regular(280),
  conch: regular(280),
  anemone: wide(268),
  trench: wide(280),
  remora: wide(280),
} as const satisfies Record<string, PedalBox>;

/** The tallest enclosure: a row of the board is this high. */
export const ROW_UNITS = 280;

const FALLBACK: PedalBox = regular(ROW_UNITS);

export function pedalBox(id: string): PedalBox {
  return (PEDAL_BOX as Record<string, PedalBox>)[id] ?? FALLBACK;
}

/**
 * Canvas pixels per board unit. A board draws its pedals as large as fits one row, between
 * these; a board that needs more rows is at the smallest.
 */
export const MIN_SCALE = 1;
export const MAX_SCALE = 1.4;
