// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import type { PresetInfo } from '../bridge/protocol';
import { getValue } from '../params/paramStore';
import { flush, setupMock, type Harness } from '../test/harness';
import { groupPresets, presetActions, usePresetStore } from './presetStore';

const preset = (over: Partial<PresetInfo>): PresetInfo => ({
  id: 'x',
  name: 'X',
  category: 'Factory',
  folder: '',
  author: '',
  tags: [],
  favorite: false,
  ...over,
});

describe('groupPresets', () => {
  const items = [
    preset({ id: 'a', name: 'Glass Shore', folder: 'Clean', tags: ['chorus'] }),
    preset({ id: 'b', name: 'Abyssal Lead', folder: 'Lead', favorite: true }),
    preset({ id: 'c', name: 'Night Swim', folder: 'Clean' }),
    preset({ id: 'd', name: 'Sunday set', category: 'User', folder: 'Live', favorite: true }),
    preset({ id: 'e', name: 'Loose', category: 'User' }),
  ];

  it('groups by folder, root first, names sorted', () => {
    const groups = groupPresets(items, { query: '', category: 'Factory' });
    expect(groups.map((g) => g.folder)).toEqual(['Clean', 'Lead']);
    expect(groups[0]?.items.map((p) => p.name)).toEqual(['Glass Shore', 'Night Swim']);
    expect(groupPresets(items, { query: '', category: 'User' }).map((g) => g.folder)).toEqual([
      '',
      'Live',
    ]);
  });

  it('searches names, folders and tags', () => {
    expect(groupPresets(items, { query: 'chorus', category: 'Factory' })[0]?.items[0]?.id).toBe(
      'a',
    );
    expect(groupPresets(items, { query: 'lead', category: 'Factory' })[0]?.items[0]?.id).toBe('b');
    expect(groupPresets(items, { query: 'nothing', category: 'Factory' })).toEqual([]);
  });

  it('collects favourites across categories', () => {
    const ids = groupPresets(items, { query: '', category: 'Favorites' }).flatMap((g) =>
      g.items.map((p) => p.id),
    );
    expect(ids.sort()).toEqual(['b', 'd']);
  });
});

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
    await presetActions.save('Rehearsal', 'Band');
    const s = usePresetStore.getState().state;
    expect(s.currentName).toBe('Rehearsal');
    expect(s.items.some((p) => p.category === 'User' && p.folder === 'Band')).toBe(true);
  });
});
