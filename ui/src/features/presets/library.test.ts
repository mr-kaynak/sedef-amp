// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import type { PresetInfo } from '../../bridge/protocol';
import {
  allCategories,
  allTags,
  browse,
  categoryFacets,
  chainGroups,
  chainText,
  DEFAULT_FILTER,
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
      factory: 3,
      user: 3,
    });
  });

  it('filters by library, category and every selected tag', () => {
    expect(view({ library: 'favorites' })).toEqual(['Abyssal Lead', 'Sunday set']);
    expect(view({ library: 'user', category: 'Clean' })).toEqual(['night drive']);
    expect(view({ category: 'Clean' })).toEqual(['Glass Shore', 'night drive', 'Night Swim']);
    expect(view({ tags: ['plate'] })).toEqual(['Glass Shore', 'Night Swim']);
    expect(view({ tags: ['plate', 'chorus'] })).toEqual(['Glass Shore']);
    expect(view({ library: 'recent', sort: 'recent' })).toEqual(['Loose', 'Abyssal Lead']);
  });

  it('sorts by name, by recency and by category', () => {
    expect(view({})).toEqual([
      'Abyssal Lead',
      'Glass Shore',
      'Loose',
      'night drive',
      'Night Swim',
      'Sunday set',
    ]);
    expect(view({ sort: 'recent' }).slice(0, 2)).toEqual(['Loose', 'Abyssal Lead']);
    // Folders alphabetically, presets without one last.
    expect(view({ sort: 'category' })).toEqual([
      'Glass Shore',
      'night drive',
      'Night Swim',
      'Abyssal Lead',
      'Sunday set',
      'Loose',
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

  it('offers the categories and tags of what is in view', () => {
    expect(categoryFacets(items, 'all', recent)).toEqual([
      { name: 'Clean', count: 3 },
      { name: 'Lead', count: 1 },
      { name: 'Live', count: 1 },
    ]);
    expect(categoryFacets(items, 'factory', recent).map((c) => c.name)).toEqual(['Clean', 'Lead']);
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
  });

  it('groups the chain for the detail pane', () => {
    expect(
      chainGroups(preset({ amp: 1, cab: 2, blocks: ['gate', 'pearl', 'eq', 'drift'] })),
    ).toEqual({
      pedals: ['Pearl'],
      amp: 'Reef',
      cab: '4x12 Closed Back',
      fx: ['EQ', 'Drift'],
    });
  });
});
