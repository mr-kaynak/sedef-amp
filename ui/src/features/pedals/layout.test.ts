// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { BOARD_HEIGHT, BOARD_WIDTH, gapAt, layoutBoard, type BoardLayout } from './layout';
import { LEGACY_BOARD, PEDALS } from './registry';
import { PEDAL_BOX, ROW_UNITS } from './sizes';

const EIGHT = ['gate', 'trench', 'conch', 'tide', 'pearl', 'kelp', 'gyre', 'ripple'];

/** Everything inside the canvas, nothing on top of anything else. */
function expectTidy(layout: BoardLayout) {
  const boxes = [layout.input, ...layout.pedals, layout.output];
  for (const b of boxes) {
    expect(b.x).toBeGreaterThanOrEqual(0);
    expect(b.x + b.w).toBeLessThanOrEqual(BOARD_WIDTH);
    expect(b.y).toBeGreaterThanOrEqual(0);
    expect(b.y + b.h).toBeLessThanOrEqual(layout.height);
  }
  boxes.forEach((a, i) => {
    for (const b of boxes.slice(i + 1)) {
      const apart = a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
      expect(apart).toBe(true);
    }
  });
}

/** A repeatable shuffle. */
function shuffled(ids: readonly string[], seed: number): string[] {
  const out = [...ids];
  let s = seed;
  for (let i = out.length - 1; i > 0; i -= 1) {
    s = (s * 16807) % 2147483647;
    const j = s % (i + 1);
    [out[i], out[j]] = [out[j]!, out[i]!];
  }
  return out;
}

describe('the board layout', () => {
  it('has a box for every pedal in the registry, none taller than a row', () => {
    expect(Object.keys(PEDAL_BOX).sort()).toEqual(PEDALS.map((p) => p.id).sort());
    for (const box of Object.values(PEDAL_BOX)) expect(box.h).toBeLessThanOrEqual(ROW_UNITS);
    // Pedals differ in size: at least a compact and a regular width.
    expect(new Set(Object.values(PEDAL_BOX).map((b) => b.w)).size).toBeGreaterThanOrEqual(2);
  });

  it('draws a short board large, centred in one row', () => {
    const layout = layoutBoard(['gate', 'pearl', 'ripple']);
    expect(layout.density).toBe('large');
    expect(layout.rows).toHaveLength(1);
    expect(layout.height).toBeLessThanOrEqual(BOARD_HEIGHT);
    expectTidy(layout);
    // As much room left of the input as right of the amp.
    expect(layout.input.x).toBeCloseTo(BOARD_WIDTH - (layout.output.x + layout.output.w), 0);
    // A pedal is 1.4 canvas pixels per unit there, standing on the row's bottom line.
    const pearl = layout.pedals[1]!;
    expect(pearl.w).toBeCloseTo(PEDAL_BOX.pearl.w * 1.4, 1);
    const bottoms = layout.pedals.map((p) => Math.round(p.y + p.h));
    expect(new Set(bottoms).size).toBe(1);
  });

  it('fits eight pedals in one small row without scrolling', () => {
    const layout = layoutBoard(EIGHT);
    expect(layout.density).toBe('small');
    expect(layout.rows).toHaveLength(1);
    expect(layout.pedals.map((p) => p.id)).toEqual(EIGHT);
    expect(layout.height).toBeLessThanOrEqual(BOARD_HEIGHT);
    expectTidy(layout);
  });

  it('fits every pedal there is in two rows without scrolling, in any order', () => {
    const all = [...LEGACY_BOARD];
    expect(all).toHaveLength(16);
    const orders = [all, [...all].reverse()];
    for (let i = 1; i < all.length; i += 1) orders.push([...all.slice(i), ...all.slice(0, i)]);
    // The widest pedals first and last are the hard cases for a greedy fill.
    const byWidth = [...all].sort((a, b) => PEDAL_BOX[b as 'gate'].w - PEDAL_BOX[a as 'gate'].w);
    orders.push(byWidth, [...byWidth].reverse());
    for (let seed = 1; seed <= 200; seed += 1) orders.push(shuffled(all, seed * 7919));
    for (const order of orders) {
      const layout = layoutBoard(order);
      expect(layout.rows.length).toBeLessThanOrEqual(2);
      expect(layout.height).toBeLessThanOrEqual(BOARD_HEIGHT);
      expect(layout.pedals.map((p) => p.id)).toEqual(order);
      expectTidy(layout);
    }
  });

  it('keeps the board small while the Add pedal shelf is open', () => {
    expect(layoutBoard(['gate', 'pearl'], false).density).toBe('small');
    expect(layoutBoard(['gate', 'pearl'], true).density).toBe('large');
  });

  it('runs the cable from the input jack to the amp, turning back between rows', () => {
    const one = layoutBoard(EIGHT);
    expect(one.cable.startsWith('M')).toBe(true);
    // One row: a straight run, an arrow in every gap (input to first, between, last to amp).
    expect(one.cable).not.toContain('Q');
    expect(one.arrows).toHaveLength(EIGHT.length + 1);
    expect(one.arrows.every((a) => a.heading === 'right' && a.y === one.rows[0]!.cableY)).toBe(
      true,
    );

    const two = layoutBoard(LEGACY_BOARD);
    expect(two.rows).toHaveLength(2);
    expect(two.cable).toContain('Q');
    expect(two.arrows.filter((a) => a.heading === 'left')).toHaveLength(1);
    expect(two.arrows.filter((a) => a.heading === 'down')).toHaveLength(2);
    // The amp follows the last pedal, on its row.
    const last = two.pedals[two.pedals.length - 1]!;
    expect(last.row).toBe(1);
    expect(two.output.x).toBeGreaterThan(last.x + last.w);
  });

  it('gives an empty board a cable straight from the jack to the amp', () => {
    const layout = layoutBoard([]);
    expect(layout.pedals).toEqual([]);
    expect(layout.output.x).toBeGreaterThan(layout.input.x + layout.input.w + 100);
    expect(layout.arrows).toHaveLength(1);
    expectTidy(layout);
  });

  it('finds the gap a pedal would go into', () => {
    const layout = layoutBoard(EIGHT);
    const [first, second] = layout.pedals;
    expect(gapAt(layout, 0)!.x).toBeCloseTo(first!.x - layout.gap / 2, 1);
    expect(gapAt(layout, 1)!.x).toBeCloseTo(second!.x - layout.gap / 2, 1);
    const last = layout.pedals[layout.pedals.length - 1]!;
    expect(gapAt(layout, EIGHT.length)!.x).toBeCloseTo(last.x + last.w + layout.gap / 2, 1);
    expect(gapAt(layout, 3)!.h).toBe(layout.rows[0]!.height);
    expect(gapAt(layoutBoard([]), 0)).toBeNull();
  });
});
