// SPDX-License-Identifier: AGPL-3.0-or-later
// Geometry of the Sedef mark: the star polygon {8/3} on a 40-unit grid, the eight-pointed star
// set in mother-of-pearl inlay. Plain TypeScript with no imports so scripts/brand-assets.mjs can
// load it directly and the exported SVG stays identical to the one in the header.

const OUTER = 18.5;
// Inner vertex radius of a {n/m} star polygon: R * cos(pi m / n) / cos(pi (m - 1) / n).
const INNER = (OUTER * Math.cos((3 * Math.PI) / 8)) / Math.cos(Math.PI / 4);

export const STAR_POINTS: string = Array.from({ length: 16 }, (_, i) => {
  const a = (Math.PI * i) / 8 - Math.PI / 2;
  const r = i % 2 === 0 ? OUTER : INNER;
  return `${(20 + r * Math.cos(a)).toFixed(2)},${(20 + r * Math.sin(a)).toFixed(2)}`;
}).join(' ');

/** Nacre gradient stops (offset, colour) along the diagonal. */
export const NACRE_STOPS: readonly (readonly [string, string])[] = [
  ['0', '#cdeeee'],
  ['0.35', '#ddd6f6'],
  ['0.65', '#f6dbe2'],
  ['1', '#f2e8cb'],
];

export const INK = '#0b1c27';
