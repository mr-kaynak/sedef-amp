// SPDX-License-Identifier: AGPL-3.0-or-later
// Geometry for rotary scales: 0 degrees points up, angles grow clockwise, the sweep is 270°.

export const SWEEP = 270;
export const START_ANGLE = -SWEEP / 2;

export function angleFor(normalised: number): number {
  return START_ANGLE + SWEEP * normalised;
}

export function polar(cx: number, cy: number, r: number, angleDeg: number): [number, number] {
  const a = ((angleDeg - 90) * Math.PI) / 180;
  return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
}

export function arcPath(cx: number, cy: number, r: number, from: number, to: number): string {
  if (Math.abs(to - from) < 0.01) return '';
  const [a0, a1] = from < to ? [from, to] : [to, from];
  const [x0, y0] = polar(cx, cy, r, a0);
  const [x1, y1] = polar(cx, cy, r, a1);
  const large = a1 - a0 > 180 ? 1 : 0;
  return `M ${x0.toFixed(2)} ${y0.toFixed(2)} A ${r} ${r} 0 ${large} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}
