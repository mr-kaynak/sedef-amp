// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { BOARD_HEIGHT, BOARD_WIDTH, gapAt, layoutBoard, type BoardLayout } from './layout';
import { AMP, LEGACY_BOARD, PEDALS, switchedOnBoardFor } from './registry';
import { PEDAL_BOX, ROW_UNITS } from './sizes';

const EIGHT = ['gate', 'trench', 'conch', 'tide', 'pearl', 'kelp', 'gyre', 'ripple'];

/** Everything inside the canvas, nothing on top of anything else. */
function expectTidy(layout: BoardLayout) {
  const boxes = [
    layout.input,
    ...layout.items,
    layout.output,
    ...(layout.afterAmpLabel ? [layout.afterAmpLabel] : []),
  ];
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
    expect(layout.scale).toBe(1.4);
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

  it('fits eight pedals in one row without scrolling, as large as the row allows', () => {
    const layout = layoutBoard(EIGHT);
    expect(layout.scale).toBeGreaterThanOrEqual(1);
    expect(layout.scale).toBeLessThan(1.4);
    expect(layout.rows).toHaveLength(1);
    // Nothing to spare: a hundredth larger would not fit.
    expect(layout.input.x).toBeLessThan(16 + 12);
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
    // The same orders with their modulation after the amp, and with only the last pedal there.
    const modulation = new Set(PEDALS.filter((p) => p.afterAmp).map((p) => p.id));
    const boards = orders.flatMap((order) => {
      const front = order.filter((id) => !modulation.has(id));
      const back = order.filter((id) => modulation.has(id));
      return [order, [...front, AMP, ...back], [...front, ...back.slice(0, -1), AMP, back.at(-1)!]];
    });
    for (const board of boards) {
      const layout = layoutBoard(board);
      expect(layout.rows.length).toBeLessThanOrEqual(2);
      expect(layout.height).toBeLessThanOrEqual(BOARD_HEIGHT);
      expect(layout.pedals.map((p) => p.id)).toEqual(board.filter((id) => id !== AMP));
      expectTidy(layout);
    }
  });

  it('fits the largest board (25 pedals) in four rows at most, which scroll', () => {
    const largest = switchedOnBoardFor(() => true);
    expect(largest).toHaveLength(25);
    const modulation = new Set(PEDALS.filter((p) => p.afterAmp).map((p) => p.id));
    const isMod = (t: string) => modulation.has(t.split('#')[0]!);
    const rows = new Set<number>();
    for (let seed = 1; seed <= 200; seed += 1) {
      const order = shuffled(largest, seed * 7919);
      for (const board of [
        order,
        [...order.filter((t) => !isMod(t)), AMP, ...order.filter(isMod)],
      ]) {
        const layout = layoutBoard(board);
        rows.add(layout.rows.length);
        expect(layout.scale).toBe(1);
        expect(layout.pedals.map((p) => p.id)).toEqual(board.filter((t) => t !== AMP));
        expectTidy(layout);
      }
    }
    // Three rows in most orders, four where wide pedals end the rows early.
    expect([...rows].sort()).toEqual([3, 4]);
    expect(layoutBoard(largest).height).toBeGreaterThan(BOARD_HEIGHT);
  });

  it('puts the amp in the chain, and a named band behind the pedals after it', () => {
    const layout = layoutBoard(['gate', 'kelp', AMP, 'gyre', 'ripple']);
    expect(layout.items.map((p) => p.id)).toEqual(['gate', 'kelp', AMP, 'gyre', 'ripple']);
    expect(layout.pedals.map((p) => p.id)).toEqual(['gate', 'kelp', 'gyre', 'ripple']);
    expectTidy(layout);
    const [kelp, gyre, ripple] = [layout.pedals[1]!, layout.pedals[2]!, layout.pedals[3]!];
    // The amp sits on the cable between the two sides.
    const row = layout.rows[0]!;
    expect(layout.amp.y + layout.amp.h / 2).toBeCloseTo(row.cableY, 0);
    expect(layout.amp.x).toBeGreaterThan(kelp.x + kelp.w);
    expect(gyre.x).toBeGreaterThan(layout.amp.x + layout.amp.w);
    // One band, from past the amp to past Out, holding the pedals after the amp and not the others.
    expect(layout.afterAmp).toHaveLength(1);
    const band = layout.afterAmp[0]!;
    expect(band.x).toBeGreaterThan(layout.amp.x + layout.amp.w);
    expect(band.x).toBeLessThan(gyre.x);
    expect(band.x + band.w).toBeGreaterThan(layout.output.x + layout.output.w);
    expect(band.y).toBeLessThan(Math.min(gyre.y, ripple.y));
    // Its name stands in front of Gyre, above the cable.
    const label = layout.afterAmpLabel!;
    expect(label.x).toBeGreaterThanOrEqual(band.x);
    expect(label.x + label.w).toBeLessThanOrEqual(gyre.x);
    expect(label.y + label.h).toBeLessThan(row.cableY);
    // Nothing after the amp: no band, and the amp at the end as before.
    const front = layoutBoard(['gate', 'kelp']);
    expect(front.afterAmp).toEqual([]);
    expect(front.afterAmpLabel).toBeNull();
    expect(front.items.map((p) => p.id)).toEqual(['gate', 'kelp', AMP]);
  });

  it('carries the band into every row the pedals after the amp stand in', () => {
    const modulation = PEDALS.filter((p) => p.afterAmp).map((p) => p.id);
    const front = LEGACY_BOARD.filter((id) => !modulation.includes(id));
    const layout = layoutBoard([...front.slice(0, 6), AMP, ...modulation]);
    expect(layout.rows).toHaveLength(2);
    expectTidy(layout);
    for (const p of layout.pedals) {
      const inBand = layout.afterAmp.some(
        (b) => p.x >= b.x && p.x + p.w <= b.x + b.w && p.y >= b.y && p.y + p.h <= b.y + b.h,
      );
      expect(inBand, p.id).toBe(modulation.includes(p.id));
    }
  });

  it('grows a board of six or seven pedals to the width there is', () => {
    const six = layoutBoard(['trench', 'tide', 'pearl', 'kelp', 'coral', 'ripple']);
    const seven = layoutBoard(['trench', 'tide', 'pearl', 'kelp', 'coral', 'ripple', 'breaker']);
    expect(six.rows).toHaveLength(1);
    expect(seven.rows).toHaveLength(1);
    expect(six.scale).toBeGreaterThan(seven.scale);
    expect(seven.scale).toBeGreaterThan(1);
    expect(six.scale).toBeLessThan(1.4);
    expectTidy(six);
    expectTidy(seven);
    // A board that needs two rows is at the smallest scale.
    expect(layoutBoard(LEGACY_BOARD).scale).toBe(1);
  });

  it('draws the board no larger than fits above the Add pedal shelf while it is open', () => {
    expect(layoutBoard(['gate', 'pearl'], false).scale).toBe(1.08);
    expect(layoutBoard(['gate', 'pearl'], true).scale).toBe(1.4);
    const open = layoutBoard(['gate', 'trench', 'pearl'], false);
    // 317 px are left between the toolbar and the shelf.
    expect(open.height).toBeLessThanOrEqual(317);
  });

  it('runs the cable from the input jack to the amp, turning back between rows', () => {
    const one = layoutBoard(EIGHT);
    expect(one.cable.startsWith('M')).toBe(true);
    // One row: a straight run, an arrow in every gap (input to first, between, last to amp, amp
    // to Out).
    expect(one.cable).not.toContain('Q');
    expect(one.arrows).toHaveLength(EIGHT.length + 2);
    for (const arrow of one.arrows) {
      expect(arrow.heading).toBe('right');
      expect(arrow.y).toBeCloseTo(one.rows[0]!.cableY, 0);
    }

    const two = layoutBoard(LEGACY_BOARD);
    expect(two.rows).toHaveLength(2);
    expect(two.cable).toContain('Q');
    expect(two.arrows.filter((a) => a.heading === 'left')).toHaveLength(1);
    expect(two.arrows.filter((a) => a.heading === 'down')).toHaveLength(2);
    // The amp follows the last pedal, on its row, and Out follows the amp.
    const last = two.pedals[two.pedals.length - 1]!;
    expect(last.row).toBe(1);
    expect(two.amp.x).toBeGreaterThan(last.x + last.w);
    expect(two.output.x).toBeGreaterThan(two.amp.x + two.amp.w);
  });

  it('gives an empty board a cable from the jack through the amp to Out', () => {
    const layout = layoutBoard([]);
    expect(layout.pedals).toEqual([]);
    expect(layout.items.map((p) => p.id)).toEqual([AMP]);
    expect(layout.amp.x).toBeGreaterThan(layout.input.x + layout.input.w);
    expect(layout.output.x).toBeGreaterThan(layout.amp.x + layout.amp.w);
    expect(layout.arrows).toHaveLength(2);
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
    // On an empty board, in front of the amp.
    const empty = layoutBoard([]);
    expect(gapAt(empty, 0)!.x).toBeCloseTo(empty.amp.x - empty.gap / 2, 1);
  });
});
