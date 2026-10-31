// SPDX-License-Identifier: AGPL-3.0-or-later
// Mirrors JUCE NormalisableRange with a skew factor:
//   normalised = ((value - min) / (max - min)) ^ skew
//   value      = min + (max - min) * normalised ^ (1 / skew)
import type { ParamSpec } from '../bridge/protocol';

const clamp01 = (x: number) => (x < 0 ? 0 : x > 1 ? 1 : x);

export function isStepped(spec: ParamSpec): boolean {
  return spec.kind !== 'float';
}

/** Clamps to the range and snaps stepped kinds (int, bool, choice) to whole numbers. */
export function snapValue(spec: ParamSpec, value: number): number {
  const v = Math.min(spec.max, Math.max(spec.min, value));
  return isStepped(spec) ? Math.round(v) : v;
}

export function toNormalised(spec: ParamSpec, value: number): number {
  const span = spec.max - spec.min;
  if (span <= 0) return 0;
  const proportion = clamp01((value - spec.min) / span);
  return spec.skew === 1 ? proportion : Math.pow(proportion, spec.skew);
}

export function fromNormalised(spec: ParamSpec, normalised: number): number {
  const n = clamp01(normalised);
  const proportion = spec.skew === 1 ? n : Math.pow(n, 1 / spec.skew);
  return snapValue(spec, spec.min + (spec.max - spec.min) * proportion);
}

/** Number of discrete positions for stepped kinds; 0 for continuous floats. */
export function stepCount(spec: ParamSpec): number {
  return isStepped(spec) ? Math.round(spec.max - spec.min) : 0;
}

/** True when the range straddles zero, so arcs and sliders grow from the middle. */
export function isBipolar(spec: ParamSpec): boolean {
  return spec.min < 0 && spec.max > 0;
}
