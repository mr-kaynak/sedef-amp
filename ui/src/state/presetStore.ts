// SPDX-License-Identifier: AGPL-3.0-or-later
// Preset library state mirrored from the plugin. Every action returns the plugin's new
// PresetState, which replaces ours; the plugin also broadcasts sedef:presets for changes made
// elsewhere (host program changes, another editor window).
import { create } from 'zustand';
import type { PresetInfo, PresetState } from '../bridge/protocol';
import { invoke } from './backendRef';

interface PresetStore {
  state: PresetState;
}

export const usePresetStore = create<PresetStore>()(() => ({
  state: { items: [], currentId: null, currentName: 'Init', dirty: false },
}));

export function applyPresetState(state: PresetState): void {
  usePresetStore.setState({ state });
}

async function run(promise: Promise<PresetState | undefined>): Promise<void> {
  const next = await promise;
  if (next) applyPresetState(next);
}

export const presetActions = {
  load: (id: string) => run(invoke('sedef_presets_load', id)),
  step: (delta: number) => run(invoke('sedef_presets_step', delta)),
  save: (name: string, folder: string) => run(invoke('sedef_presets_save', name, folder)),
  remove: (id: string) => run(invoke('sedef_presets_delete', id)),
  rename: (id: string, name: string) => run(invoke('sedef_presets_rename', id, name)),
  setFavorite: (id: string, favorite: boolean) =>
    run(invoke('sedef_presets_setFavorite', id, favorite)),
  init: () => run(invoke('sedef_presets_init')),
  reveal: () => invoke('sedef_presets_revealFolder'),
};

export function currentPreset(state: PresetState): PresetInfo | undefined {
  return state.items.find((p) => p.id === state.currentId);
}

export interface PresetFilter {
  query: string;
  category: 'Factory' | 'User' | 'Favorites';
}

/** Filters and groups presets by folder for the browser list. */
export function groupPresets(
  items: readonly PresetInfo[],
  filter: PresetFilter,
): { folder: string; items: PresetInfo[] }[] {
  const q = filter.query.trim().toLowerCase();
  const matches = items.filter((p) => {
    if (filter.category === 'Favorites' ? !p.favorite : p.category !== filter.category)
      return false;
    if (!q) return true;
    return (
      p.name.toLowerCase().includes(q) ||
      p.folder.toLowerCase().includes(q) ||
      p.tags.some((t) => t.toLowerCase().includes(q))
    );
  });
  const groups = new Map<string, PresetInfo[]>();
  for (const p of matches) {
    const key = p.folder;
    groups.set(key, [...(groups.get(key) ?? []), p]);
  }
  return [...groups.entries()]
    .sort(([a], [b]) => (a === '' ? -1 : b === '' ? 1 : a.localeCompare(b)))
    .map(([folder, list]) => ({
      folder,
      items: list.sort((a, b) => a.name.localeCompare(b.name)),
    }));
}
