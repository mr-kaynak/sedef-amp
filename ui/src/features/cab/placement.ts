// SPDX-License-Identifier: AGPL-3.0-or-later
// Geometry of the speaker stage: a mic's radial position (0 dust cap, 1 cone edge) and azimuth
// (degrees clockwise from 12 o'clock as you face the cab) against points in SVG units.

export const SIZE = 400;
export const C = SIZE / 2;
export const CONE_R = 168;
const RAD = Math.PI / 180;

/** Marker centre for a radial position (0..1) and an azimuth (degrees clockwise from 12 o'clock). */
export function markerPoint(position: number, azimuthDeg: number): { x: number; y: number } {
  const a = azimuthDeg * RAD;
  return { x: C + position * CONE_R * Math.sin(a), y: C - position * CONE_R * Math.cos(a) };
}

/**
 * The placement under an SVG point: radial position, clamped to the cone, and azimuth. At the very
 * centre the azimuth is undefined, so `azimuthDeg` is kept.
 */
export function placementAt(
  x: number,
  y: number,
  azimuthDeg: number,
): { position: number; azimuth: number } {
  const dx = x - C;
  const dy = C - y;
  const r = Math.hypot(dx, dy);
  if (r < 1e-6) return { position: 0, azimuth: azimuthDeg };
  return { position: Math.min(1, r / CONE_R), azimuth: Math.atan2(dx, dy) / RAD };
}

/** "2 o'clock": the azimuth as an engineer would say it, to the nearest hour. */
export function clockLabel(azimuthDeg: number): string {
  const hour = Math.round((((azimuthDeg % 360) + 360) % 360) / 30) % 12;
  return `${hour === 0 ? 12 : hour} o'clock`;
}
