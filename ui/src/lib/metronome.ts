// SPDX-License-Identifier: AGPL-3.0-or-later
// Metronome step model. Values match sedef::dsp::Metronome::StepState.

export const STEP_OFF = 0;
export const STEP_NORMAL = 1;
export const STEP_ACCENT = 2;
export const STEP_SOFT = 3;
export const MAX_STEPS = 13;

/** Steps per bar for each metronomeSignature choice (2/4 … 12/8). */
const STEPS_FOR_SIGNATURE = [2, 3, 4, 5, 6, 7, 9, 12];

export function stepsFor(signature: number): number {
  return STEPS_FOR_SIGNATURE[signature] ?? 4;
}

/** Eighth-note meters click on every eighth; quarter-note meters on every quarter. */
export function stepBeats(signature: number): number {
  return signature >= 4 ? 0.5 : 1;
}

export function defaultSteps(): number[] {
  return Array.from({ length: MAX_STEPS }, (_, i) => (i === 0 ? STEP_ACCENT : STEP_NORMAL));
}

/** 13 steps from saved state or the plug-in; anything that isn't a step 0-3 is the default's. */
export function sanitizeSteps(value: unknown): number[] {
  const fallback = defaultSteps();
  if (!Array.isArray(value)) return fallback;
  return fallback
    .map((d, i) => {
      const v: unknown = value[i];
      return typeof v === 'number' && v >= 0 && v <= 3 ? Math.round(v) : d;
    })
    .slice(0, MAX_STEPS);
}

/** Off -> Normal -> Accent -> Soft -> Off. */
export function cycleStep(state: number): number {
  switch (state) {
    case STEP_OFF:
      return STEP_NORMAL;
    case STEP_NORMAL:
      return STEP_ACCENT;
    case STEP_ACCENT:
      return STEP_SOFT;
    default:
      return STEP_OFF;
  }
}
