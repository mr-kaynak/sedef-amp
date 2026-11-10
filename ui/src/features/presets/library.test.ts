// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import type { PresetInfo } from '../../bridge/protocol';
import {
  allCategories,
  allTags,
  browse,
  chainText,
  DEFAULT_FILTER,
  groupByCategory,
  libraryCounts,
  matchesQuery,
  tagFacets,
  type BrowseFilter,
} from './library';

const preset = (over: Partial<PresetInfo>): PresetInfo => ({
  id: 'x',
  name: 'X',
  category: 'Factory',
  folder: '',
  author: 'Sedef',
  tags: [],
  favorite: false,
  description: '',
  amp: 1,
  cab: 1,
  blocks: ['gate'],
  ...over,
});

const items = [
  preset({
    id: 'a',
    name: 'Glass Shore',
    folder: 'Clean',
    tags: ['chorus', 'plate'],
    amp: 0,
    cab: 0,
    blocks: ['gate', 'tide', 'ripple', 'fathom'],
    description: 'Shimmering arpeggios',
  }),
  preset({ id: 'b', name: 'Abyssal Lead', folder: 'Lead', favorite: true, amp: 2, cab: 2 }),
  preset({ id: 'c', name: 'Night Swim', folder: 'Clean', tags: ['plate'], amp: 0, cab: 0 }),
  preset({ id: 'd', name: 'Sunday set', category: 'User', folder: 'Live', favorite: true }),
  preset({ id: 'e', name: 'Loose', category: 'User', tags: ['chorus'], author: 'Ada' }),
  preset({ id: 'f', name: 'night drive', category: 'User', folder: 'Clean' }),
];
const recent = ['e', 'b'];
const names = (list: PresetInfo[]) => list.map((p) => p.name);
const view = (over: Partial<BrowseFilter>) =>
  names(browse(items, { ...DEFAULT_FILTER, ...over }, recent));

describe('preset library', () => {
  it('counts each library and keeps Recent to presets that exist', () => {
    expect(libraryCounts(items, [...recent, 'gone'])).toEqual({
      all: 6,
      favorites: 2,
      recent: 2,
      user: 3,
    });
  });

  it('filters by library and search', () => {
    expect(view({ library: 'favorites' })).toEqual(['Abyssal Lead', 'Sunday set']);
    expect(view({ library: 'user' })).toEqual(['night drive', 'Sunday set', 'Loose']);
    expect(view({ library: 'user', query: 'clean' })).toEqual(['night drive']);
    expect(view({ query: 'plate' })).toEqual(['Glass Shore', 'Night Swim']);
    // Recent is newest first.
    expect(view({ library: 'recent' })).toEqual(['Loose', 'Abyssal Lead']);
  });

  it('lists in library order: clean to ambient, own folders, then none; factory first', () => {
    expect(view({})).toEqual([
      'Glass Shore',
      'Night Swim',
      'night drive',
      'Abyssal Lead',
      'Sunday set',
      'Loose',
    ]);
    const ordered = browse(
      [
        preset({ name: 'Pad', folder: 'Ambient' }),
        preset({ name: 'Riff', folder: 'High Gain' }),
        preset({ name: 'Mine', category: 'User', folder: 'Ballads' }),
        preset({ name: 'Solo', folder: 'Lead' }),
        preset({ name: 'Dirt', folder: 'Crunch' }),
        preset({ name: 'Bell', folder: 'Clean' }),
      ],
      DEFAULT_FILTER,
      [],
    );
    expect(names(ordered)).toEqual(['Bell', 'Dirt', 'Riff', 'Solo', 'Pad', 'Mine']);
  });

  it('groups what is shown by category, in order', () => {
    const groups = groupByCategory(browse(items, DEFAULT_FILTER, recent));
    expect(groups.map((g) => [g.name, names(g.items)])).toEqual([
      ['Clean', ['Glass Shore', 'Night Swim', 'night drive']],
      ['Lead', ['Abyssal Lead']],
      ['Live', ['Sunday set']],
      ['', ['Loose']],
    ]);
  });

  it('searches names, categories, tags, notes, authors, amps and blocks, all words', () => {
    const find = (q: string) => items.filter((p) => matchesQuery(p, q)).map((p) => p.id);
    expect(find('chorus')).toEqual(['a', 'e']);
    expect(find('lagoon')).toEqual(['a', 'c']);
    expect(find('ripple arpeggios')).toEqual(['a']);
    expect(find('ada')).toEqual(['e']);
    expect(find('LIVE')).toEqual(['d']);
    expect(find('clean night')).toEqual(['c', 'f']);
    expect(find('nothing')).toEqual([]);
    expect(find('  ')).toHaveLength(items.length);
  });

  it('offers the categories and tags in use to the save form', () => {
    expect(tagFacets(items)).toEqual([
      { name: 'chorus', count: 2 },
      { name: 'plate', count: 2 },
    ]);
    expect(allCategories(items)).toEqual(['Clean', 'Lead', 'Live']);
    expect(allTags(items)).toEqual(['chorus', 'plate']);
  });

  it('summarises the chain in signal order without the gate', () => {
    expect(chainText(items[0]!)).toBe('Tide → Ripple → Lagoon 1x12 → Fathom');
    expect(
      chainText(preset({ amp: 1, cab: 1, blocks: ['gate', 'pearl', 'coral', 'eq', 'drift'] })),
    ).toBe('Pearl → Coral → Reef 2x12 → EQ → Drift');
    expect(chainText(preset({ amp: 2, cab: 0, blocks: [] }))).toBe('Abyss 1x12');
    expect(
      chainText(
        preset({
          amp: 2,
          cab: 2,
          blocks: [
            'gate',
            'trench',
            'tide',
            'urchin',
            'pearl',
            'kelp',
            'coral',
            'ripple',
            'fathom',
          ],
        }),
      ),
    ).toBe('Trench → Tide → Urchin → Pearl → Kelp → Coral → Ripple → Abyss 4x12 → Fathom');
    // The drives lane's fuzzes and booster are pedals in front of the amp too.
    expect(
      chainText(
        preset({
          amp: 0,
          cab: 0,
          blocks: ['gate', 'remora', 'sargasso', 'ripple', 'breaker', 'drift'],
        }),
      ),
    ).toBe('Remora → Sargasso → Ripple → Breaker → Lagoon 1x12 → Drift');
  });
});
