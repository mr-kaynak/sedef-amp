// SPDX-License-Identifier: AGPL-3.0-or-later
// Preset library state mirrored from the plugin. Every action returns the plugin's new
// PresetState, which replaces ours; the plugin also broadcasts sedef:presets for changes made
// elsewhere (host program changes, another editor window).
import { create } from 'zustand';
import type {
  CompareState,
  PresetDetails,
  PresetInfo,
  PresetState,
  SnapshotSlot,
} from '../bridge/protocol';
import { invoke } from './backendRef';

interface PresetStore {
  state: PresetState;
}

export const EMPTY_PRESET_STATE: PresetState = {
  items: [],
  currentId: null,
  currentName: 'Init',
  dirty: false,
  recent: [],
  compare: { active: 'A', other: null },
};

export const usePresetStore = create<PresetStore>()(() => ({ state: EMPTY_PRESET_STATE }));

const strings = (v: unknown): string[] =>
  Array.isArray(v) ? v.filter((x): x is string => typeof x === 'string') : [];

function normaliseInfo(p: PresetInfo): PresetInfo {
  return {
    ...p,
    tags: strings(p.tags),
    description: typeof p.description === 'string' ? p.description : '',
    amp: typeof p.amp === 'number' ? p.amp : 1,
    cab: typeof p.cab === 'number' ? p.cab : 1,
    blocks: strings(p.blocks) as PresetInfo['blocks'],
  };
}

function normaliseCompare(c: CompareState | undefined): CompareState {
  return {
    active: c?.active === 'B' ? 'B' : 'A',
    other: c?.other && typeof c.other.name === 'string' ? { ...c.other } : null,
  };
}

/** Fills what an older plugin may leave out, so a version mismatch degrades instead of crashing. */
export function normalisePresetState(state: PresetState): PresetState {
  return {
    ...state,
    items: (state.items ?? []).map(normaliseInfo),
    recent: strings(state.recent),
    compare: normaliseCompare(state.compare),
  };
}

/**
 * Takes a state from the plugin unless it is older than the one we have: an action's answer can
 * arrive after a broadcast the plugin sent later. A state without a revision always applies.
 */
export function applyPresetState(state: PresetState): void {
  const have = usePresetStore.getState().state.revision;
  if (typeof state.revision === 'number' && typeof have === 'number' && state.revision < have)
    return;
  usePresetStore.setState({ state: normalisePresetState(state) });
}

async function run(promise: Promise<PresetState | undefined>): Promise<void> {
  const next = await promise;
  if (next) applyPresetState(next);
}

export const presetActions = {
  /** Loads a preset; an audition (browser click) is not recorded in Recent. */
  load: (id: string, audition = false) => run(invoke('sedef_presets_load', id, audition)),
  markRecent: (id: string) => run(invoke('sedef_presets_markRecent', id)),
  step: (delta: number) => run(invoke('sedef_presets_step', delta)),
  save: (name: string, folder: string, details?: PresetDetails) =>
    run(
      details
        ? invoke('sedef_presets_save', name, folder, details)
        : invoke('sedef_presets_save', name, folder),
    ),
  remove: (id: string) => run(invoke('sedef_presets_delete', id)),
  undoDelete: () => run(invoke('sedef_presets_undoDelete')),
  rename: (id: string, name: string) => run(invoke('sedef_presets_rename', id, name)),
  setFavorite: (id: string, favorite: boolean) =>
    run(invoke('sedef_presets_setFavorite', id, favorite)),
  init: () => run(invoke('sedef_presets_init')),
  reveal: () => invoke('sedef_presets_revealFolder'),
  snapshot: (slot: SnapshotSlot) => run(invoke('sedef_presets_snapshot', slot)),
  restore: (slot: SnapshotSlot) => run(invoke('sedef_presets_restore', slot)),
  compare: (action: 'switch' | 'copy') => run(invoke('sedef_presets_compare', action)),
};

export function currentPreset(state: PresetState): PresetInfo | undefined {
  return state.items.find((p) => p.id === state.currentId);
}
