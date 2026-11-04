// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { PEDAL_CATALOG } from './catalog';
import {
  addPedal,
  CATEGORIES,
  LEGACY_BOARD,
  legacyBoardFor,
  movePedal,
  PEDALS,
  removePedal,
  sanitiseBoard,
} from './registry';

describe('pedal registry', () => {
  it('declares each pedal once, with a category, and the 0.3.0 order as the legacy board', () => {
    const ids = PEDALS.map((p) => p.id);
    expect(new Set(ids).size).toBe(ids.length);
    for (const p of PEDALS) expect(CATEGORIES).toContain(p.category);
    // The fixed chain the board replaced; 0.3.0's five pedals keep their order in it.
    expect(LEGACY_BOARD).toEqual([
      'gate',
      'trench',
      'remora',
      'tide',
      'urchin',
      'sargasso',
      'pearl',
      'kelp',
      'coral',
      'ripple',
      'breaker',
    ]);
    // Default positions agree with it.
    expect([...PEDALS].sort((a, b) => a.rank - b.rank).map((p) => p.id)).toEqual(LEGACY_BOARD);
  });

  it('has a panel and a one-line description for every pedal the engine declares', () => {
    for (const p of PEDALS) {
      const entry = PEDAL_CATALOG[p.id];
      expect(entry, p.id).toBeDefined();
      expect(entry?.description).toMatch(/^[A-Z].*\.$/);
      expect(entry?.description.length).toBeLessThan(80);
    }
    expect(Object.keys(PEDAL_CATALOG).sort()).toEqual(PEDALS.map((p) => p.id).sort());
  });

  it('adds a pedal at its default position, after the last pedal at or below its rank', () => {
    expect(addPedal(['gate', 'ripple'], 'pearl')).toEqual(['gate', 'pearl', 'ripple']);
    // A board in a custom order: after the last lower-ranked pedal, wherever it sits.
    expect(addPedal(['ripple', 'gate'], 'pearl')).toEqual(['ripple', 'gate', 'pearl']);
    expect(addPedal([], 'coral')).toEqual(['coral']);
    expect(addPedal(['tide', 'pearl'], 'gate')).toEqual(['gate', 'tide', 'pearl']);
    // Already there, or unknown: no change.
    expect(addPedal(['gate'], 'gate')).toEqual(['gate']);
    expect(addPedal(['gate'], 'nothing')).toEqual(['gate']);
  });

  it('moves a pedal to an index on the board without it, clamped', () => {
    const board = ['gate', 'tide', 'pearl', 'coral', 'ripple'];
    expect(movePedal(board, 'ripple', 2)).toEqual(['gate', 'tide', 'ripple', 'pearl', 'coral']);
    expect(movePedal(board, 'gate', 4)).toEqual(['tide', 'pearl', 'coral', 'ripple', 'gate']);
    expect(movePedal(board, 'gate', 99)).toEqual(['tide', 'pearl', 'coral', 'ripple', 'gate']);
    expect(movePedal(board, 'ripple', -3)).toEqual(['ripple', 'gate', 'tide', 'pearl', 'coral']);
    expect(movePedal(board, 'nothing', 0)).toEqual(board);
    expect(removePedal(board, 'pearl')).toEqual(['gate', 'tide', 'coral', 'ripple']);
  });

  it('keeps known ids once each, like the plugin', () => {
    expect(sanitiseBoard(['ripple', 'nope', 'gate', 'ripple', 3])).toEqual(['ripple', 'gate']);
    expect(sanitiseBoard('gate')).toEqual(LEGACY_BOARD);
    expect(sanitiseBoard([])).toEqual([]);
  });

  it('gives a preset without a board the legacy board plus the other pedals it switches on', () => {
    expect(legacyBoardFor(() => false)).toEqual(LEGACY_BOARD);
    expect(legacyBoardFor(() => true)).toEqual(PEDALS.map((p) => p.id).sort(byRank));
  });
});

function byRank(a: string, b: string): number {
  const rank = (id: string) => PEDALS.find((p) => p.id === id)?.rank ?? 0;
  return rank(a) - rank(b);
}
