// SPDX-License-Identifier: AGPL-3.0-or-later
// Which parameters belong to a preset. Mirrors PresetManager::isGlobal in
// plugin/src/PresetManager.cpp: session settings are never stored in or loaded from presets, and
// never make a preset dirty.

export const GLOBAL_PARAM_IDS: ReadonlySet<string> = new Set([
  'inputGain',
  'outputGain',
  'transpose',
  'stereoMode',
  'tempo',
  'tunerReference',
  'tunerMute',
  'metronomeEnabled',
  'metronomeVolume',
  'metronomeSignature',
  'metronomeSound',
  'presetPrevious',
  'presetNext',
  'captureInterfaceLevel',
  'oversampling',
  // The scenes carry it (docs/live.md §6.6).
  'scene',
  'metronomeMode',
  'drumsGroove',
  'drumsIntensity',
  'drumsFills',
  'drumsLevel',
  'drumsTone',
]);

export function isGlobalParam(id: string): boolean {
  return GLOBAL_PARAM_IDS.has(id);
}

/** Preset id slug. Mirrors PresetManager::slugify: lowercase, keep [a-z0-9 -], trim, spaces to '-'. */
export function slugify(name: string): string {
  return name
    .toLowerCase()
    .replace(/[^a-z0-9 -]/g, '')
    .trim()
    .replace(/ /g, '-');
}

/** Two values are the same preset value within 1e-4 of the range (PresetManager sameValue). */
export function sameValue(a: number, b: number, min: number, max: number): boolean {
  return Math.abs(a - b) <= 1e-4 * (max - min);
}
