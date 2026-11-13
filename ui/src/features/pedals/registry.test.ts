// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { PEDAL_CATALOG } from './catalog';
import {
  addAfterAmp,
  addBlocked,
  addPedal,
  AMP,
  canPlace,
  CATEGORIES,
  copyParam,
  COPIES,
  enableOf,
  LATENCY_SLOTS,
  LEGACY_BOARD,
  legacyBoardFor,
  movePedal,
  nextCopy,
  parseToken,
  pedalName,
  PEDALS,
  positionText,
  removePedal,
  sanitiseBoard,
  sides,
  switchedOnBoardFor,
  withAmp,
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
      'anemone',
      'conch',
      'remora',
      'tide',
      'urchin',
      'sargasso',
      'pearl',
      'kelp',
      'coral',
      'beacon',
      'gyre',
      'jetty',
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
    // Unknown, or both copies there already: no change.
    expect(addPedal(['gate'], 'nothing')).toEqual(['gate']);
    expect(addPedal(['gate', 'gate#2'], 'gate')).toEqual(['gate', 'gate#2']);
  });

  it('adds a second copy right after the first, on its side', () => {
    expect(addPedal(['gate', 'kelp', 'ripple'], 'kelp')).toEqual([
      'gate',
      'kelp',
      'kelp#2',
      'ripple',
    ]);
    expect(addPedal(['gate', AMP, 'gyre'], 'gyre')).toEqual(['gate', AMP, 'gyre', 'gyre#2']);
    // The second copy alone (the first was removed): the first goes back after it.
    expect(addPedal(['gate', 'kelp#2'], 'kelp')).toEqual(['gate', 'kelp#2', 'kelp']);
    expect(nextCopy(['gate'], 'gate')).toBe('gate#2');
    expect(nextCopy(['gate', 'gate#2'], 'gate')).toBeNull();
  });

  it('names copies and their parameters as the host does', () => {
    expect(COPIES).toBe(2);
    expect(parseToken('kelp#2')).toEqual({ id: 'kelp', copy: 1 });
    expect(parseToken('kelp')).toEqual({ id: 'kelp', copy: 0 });
    for (const bad of ['kelp#3', 'kelp#', '#2', 'amp', 'nope#2', 2])
      expect(parseToken(bad)).toBeNull();
    expect(pedalName('kelp#2')).toBe('Kelp 2');
    expect(pedalName('kelp')).toBe('Kelp');
    expect(enableOf('kelp#2')).toBe('kelp2Enabled');
    expect(copyParam('kelpDrive', 'kelp', 2)).toBe('kelp2Drive');
    expect(copyParam('kelpDrive', 'kelp', 1)).toBe('kelpDrive');
    for (const p of PEDALS) expect(p.enable2).toBe(copyParam(p.enable, p.id, 2));
  });

  it('lets only modulation stand after the amp', () => {
    const afterAmp = PEDALS.filter((p) => p.afterAmp).map((p) => p.id);
    expect(afterAmp).toEqual(PEDALS.filter((p) => p.category === 'Modulation').map((p) => p.id));
    expect(addAfterAmp(['gate'], 'gyre')).toEqual(['gate', AMP, 'gyre']);
    expect(addAfterAmp(['gate', AMP, 'gyre'], 'ripple')).toEqual(['gate', AMP, 'gyre', 'ripple']);
    expect(addAfterAmp(['gate'], 'kelp')).toEqual(['gate']);
    expect(canPlace(['gate'], 'kelp', 'post')).toBe(false);
    // "amp" is spelt only while a pedal stands after it.
    expect(removePedal(['gate', AMP, 'gyre'], 'gyre')).toEqual(['gate']);
    expect(withAmp(['gate'])).toEqual(['gate', AMP]);
    expect(sides(['gate', AMP, 'gyre'])).toEqual({ pre: ['gate'], post: ['gyre'] });
  });

  it('holds seven drives and fuzzes at most', () => {
    expect(LATENCY_SLOTS).toBe(7);
    const drives = PEDALS.filter((p) => p.latency).map((p) => p.id);
    expect(drives).toHaveLength(LATENCY_SLOTS);
    expect(addBlocked(drives, 'kelp')).toBe('drives');
    expect(addPedal(drives, 'kelp')).toEqual(drives);
    expect(addBlocked(drives, 'gate')).toBeNull();
    expect(addBlocked(['kelp', 'kelp#2'], 'kelp')).toBe('both');
    // The largest board: every pedal, a second copy of each without latency.
    let board: string[] = [];
    for (const p of PEDALS) board = addPedal(addPedal(board, p.id), p.id);
    expect(board).toHaveLength(2 * PEDALS.length - LATENCY_SLOTS);
  });

  it('moves a pedal across the amp, and stops one that cannot go there in front of it', () => {
    const board = ['gate', 'kelp', 'gyre', AMP, 'ripple'];
    // Indices are in the board as drawn without the pedal: gate kelp amp ripple for gyre.
    expect(movePedal(board, 'gyre', 3)).toEqual(['gate', 'kelp', AMP, 'gyre', 'ripple']);
    expect(movePedal(board, 'ripple', 2)).toEqual(['gate', 'kelp', 'ripple', 'gyre']);
    expect(movePedal(board, 'kelp', 4)).toEqual(['gate', 'gyre', 'kelp', AMP, 'ripple']);
    expect(positionText(['gate', AMP, 'gyre', 'ripple'], 'ripple')).toBe(
      '2 of 2 after the amp, after Gyre',
    );
    expect(positionText(['gate', 'kelp'], 'gate')).toBe('1 of 2 in front of the amp, first');
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
    // Second copies, the amp, and what a side can't take (sedef::BoardReader).
    expect(sanitiseBoard(['kelp', 'kelp#2', 'kelp#3', AMP, 'gyre', 'kelp', AMP, 'tide'])).toEqual([
      'kelp',
      'kelp#2',
      AMP,
      'gyre',
    ]);
    expect(sanitiseBoard(['gate', AMP])).toEqual(['gate']);
    const drives = PEDALS.filter((p) => p.latency).map((p) => p.id);
    expect(sanitiseBoard([...drives, 'kelp#2'])).toEqual(drives);
  });

  it('gives a preset without a board the legacy board plus the other pedals it switches on', () => {
    expect(legacyBoardFor(() => false)).toEqual(LEGACY_BOARD);
    expect(legacyBoardFor(() => true)).toEqual(PEDALS.map((p) => p.id).sort(byRank));
  });

  it('gives Init and the factory presets only the pedals they switch on, in default order', () => {
    expect(switchedOnBoardFor(() => false)).toEqual([]);
    expect(switchedOnBoardFor((enable) => enable === 'gateEnabled')).toEqual(['gate']);
    const on = new Set(['rippleEnabled', 'kelpEnabled', 'gateEnabled', 'trenchEnabled']);
    expect(switchedOnBoardFor((enable) => on.has(enable))).toEqual([
      'gate',
      'trench',
      'kelp',
      'ripple',
    ]);
    const firsts = new Set(PEDALS.map((p) => p.enable));
    expect(switchedOnBoardFor((enable) => firsts.has(enable))).toEqual(LEGACY_BOARD);
    // A second copy that is on follows its first, as far as the drives allow.
    const all = switchedOnBoardFor(() => true);
    expect(all).toHaveLength(2 * PEDALS.length - LATENCY_SLOTS);
    expect(all.slice(0, 3)).toEqual(['gate', 'gate#2', 'trench']);
    expect(switchedOnBoardFor((enable) => enable === 'kelp2Enabled')).toEqual(['kelp#2']);
  });
});

function byRank(a: string, b: string): number {
  const rank = (id: string) => PEDALS.find((p) => p.id === id)?.rank ?? 0;
  return rank(a) - rank(b);
}
