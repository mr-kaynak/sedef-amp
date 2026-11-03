// SPDX-License-Identifier: AGPL-3.0-or-later
// Nacre stops for SVG gradients drawn inside the editor (the EQ curve, the capture ring), taken
// from the colour tokens so they follow tokens.css. The offsets are the logo's
// (features/header/starMark.ts), which keeps literal colours because its SVG is also exported.

/** (offset, colour) pairs for `<stop offset style={{ stopColor }} />`. */
export const NACRE_STOPS: readonly (readonly [string, string])[] = [
  ['0', 'var(--nacre-aqua)'],
  ['0.35', 'var(--nacre-lilac)'],
  ['0.65', 'var(--nacre-rose)'],
  ['1', 'var(--nacre-gold)'],
];
