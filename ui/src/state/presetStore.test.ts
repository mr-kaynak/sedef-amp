// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import type { PresetState } from '../bridge/protocol';
import { getValue } from '../params/paramStore';
import { flush, setupMock, type Harness } from '../test/harness';
import {
  applyPresetState,
  EMPTY_PRESET_STATE,
  normalisePresetState,
  presetActions,
  usePresetStore,
} from './presetStore';

describe('preset actions against the mock', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock();
  });
  afterEach(() => h.dispose());

  it('loads a preset and applies its values', async () => {
    await presetActions.load('factory:tidewater');
    await flush();
    expect(usePresetStore.getState().state.currentName).toBe('Tidewater');
    expect(getValue('ampModel')).toBe(0);
    expect(getValue('rippleEnabled')).toBe(1);
    expect(getValue('rippleSpeed')).toBe(4.6);
  });

  it('saves the current sound as a user preset', async () => {
    await presetActions.save('Rehearsal', 'Band', { tags: ['loud'], description: 'Room' });
    const s = usePresetStore.getState().state;
    expect(s.currentName).toBe('Rehearsal');
    expect(s.items.find((p) => p.id === s.currentId)).toMatchObject({
      category: 'User',
      folder: 'Band',
      tags: ['loud'],
      description: 'Room',
    });
  });
});

describe('normalisePresetState', () => {
  it('fills what an older plugin leaves out', () => {
    const old = {
      items: [
        {
          id: 'factory:x',
          name: 'X',
          category: 'Factory',
          folder: '',
          author: '',
          tags: ['a', 3],
          favorite: false,
        },
      ],
      currentId: null,
      currentName: 'Init',
      dirty: false,
    } as unknown as PresetState;
    const s = normalisePresetState(old);
    expect(s.recent).toEqual([]);
    expect(s.compare).toEqual({ active: 'A', other: null });
    expect(s.items[0]).toMatchObject({ tags: ['a'], description: '', blocks: [], amp: 1 });
  });
});

describe('applyPresetState', () => {
  afterEach(() => usePresetStore.setState({ state: EMPTY_PRESET_STATE }));
  const at = (revision: number | undefined, currentName: string): PresetState => ({
    ...EMPTY_PRESET_STATE,
    currentName,
    ...(revision === undefined ? {} : { revision }),
  });
  const name = () => usePresetStore.getState().state.currentName;

  it('drops a state older than the one it has, and takes one without a revision', () => {
    applyPresetState(at(5, 'Riptide'));
    applyPresetState(at(4, 'Nacre')); // an answer overtaken by a later broadcast
    expect(name()).toBe('Riptide');
    applyPresetState(at(5, 'Riptide again'));
    expect(name()).toBe('Riptide again');
    applyPresetState(at(undefined, 'Old plugin'));
    expect(name()).toBe('Old plugin');
    applyPresetState(at(1, 'Reconnected'));
    expect(name()).toBe('Reconnected');
  });

  it('ends on the plugin state when answers and broadcasts arrive out of order', async () => {
    const h = await setupMock({ jitter: { maxMs: 12, seed: 7 } });
    try {
      const ids = ['factory:tidewater', 'factory:riptide', 'factory:brine', 'factory:mariana'];
      // Everything in flight at once: ten answers and broadcasts racing each other.
      await Promise.all([
        ...ids.map((id) => presetActions.load(id, true)),
        presetActions.setFavorite('factory:brine', true),
      ]);
      await new Promise((resolve) => setTimeout(resolve, 40));
      const s = usePresetStore.getState().state;
      const truth = await h.mock.call('sedef_presets_list');
      expect(s.currentId).toBe('factory:mariana');
      expect(s.items.find((p) => p.id === 'factory:brine')?.favorite).toBe(true);
      expect(s.revision).toBeLessThan(truth.revision ?? 0);
      expect({ ...s, revision: 0 }).toEqual({ ...normalisePresetState(truth), revision: 0 });
    } finally {
      h.dispose();
    }
  });
});
