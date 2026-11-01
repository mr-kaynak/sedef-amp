// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import type { PresetState } from '../bridge/protocol';
import { getValue } from '../params/paramStore';
import { flush, setupMock, type Harness } from '../test/harness';
import { normalisePresetState, presetActions, usePresetStore } from './presetStore';

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
