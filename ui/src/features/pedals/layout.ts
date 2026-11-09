// SPDX-License-Identifier: AGPL-3.0-or-later
// Where everything on the board sits. The canvas is always 1200 px wide (app/Stage scales it), so
// the layout is arithmetic over the pedals' boxes: no measuring. A board that fits one row is
// drawn as large as that row allows, up to 1.4 canvas pixels per unit, and centred; otherwise the
// pedals are at the smallest scale and fill rows evenly from the left, the signal running along
// each row and back to the start of the next. The widths are chosen so that every pedal there is
// fits two rows, in any order.
import { MAX_SCALE, MIN_SCALE, pedalBox } from './sizes';

export const BOARD_WIDTH = 1200;
/** Room for two small rows under the toolbar, in canvas pixels. */
export const BOARD_HEIGHT = 588;

const PAD_X = 16;
const PAD_Y = 6;
const ROW_GAP = 16;
/** The cable's corners. */
const BEND = 6;

interface Metrics {
  /** Canvas pixels per board unit. */
  unit: number;
  /** Space between neighbours in a row. */
  gap: number;
  /** The input jack's and the amp's boxes. */
  input: { w: number; h: number };
  output: { w: number; h: number };
}

/** The spacing and the two ends of the chain grow with the pedals. */
function metrics(scale: number): Metrics {
  const grown = scale - MIN_SCALE;
  return {
    unit: scale,
    gap: 8 + 10 * grown,
    input: { w: 34 + 25 * grown, h: 56 + 20 * grown },
    output: { w: 48 + 30 * grown, h: 136 + 60 * grown },
  };
}

/** The largest the board is drawn while the Add pedal shelf takes the lower part of the page. */
const SHELF_SCALE = 1.2;

export interface Rect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface PlacedPedal extends Rect {
  id: string;
  row: number;
}

export interface BoardRow {
  top: number;
  height: number;
  /** The height the signal runs at: the jacks on the pedals' sides. */
  cableY: number;
}

export interface Arrow {
  x: number;
  y: number;
  /** Which way the signal goes there. */
  heading: 'right' | 'left' | 'down';
}

export interface BoardLayout {
  /** Canvas pixels per board unit: how large the pedals are drawn. */
  scale: number;
  gap: number;
  pedals: PlacedPedal[];
  rows: BoardRow[];
  input: Rect;
  output: Rect;
  /** The signal's path from the input to the amp, as SVG path data. */
  cable: string;
  arrows: Arrow[];
  height: number;
}

const round = (v: number) => Math.round(v * 10) / 10;

const RIGHT = BOARD_WIDTH - PAD_X;

/**
 * Pedals in rows, in order, each row filled up to `right`; null when they need more than
 * `maxRows`.
 */
function flow(
  ids: readonly string[],
  scale: number,
  maxRows: number,
  right = RIGHT,
): string[][] | null {
  const m = metrics(scale);
  const width = (id: string) => pedalBox(id).w * m.unit;
  const rows: string[][] = [[]];
  let x = PAD_X + m.input.w + m.gap;
  for (const id of ids) {
    const row = rows[rows.length - 1]!;
    if (row.length > 0 && x + width(id) > right) {
      rows.push([id]);
      x = PAD_X + width(id) + m.gap;
    } else {
      row.push(id);
      x += width(id) + m.gap;
    }
  }
  // The amp follows the last pedal. Without room for it, the last pedal goes down a row with it.
  if (x + m.output.w > right) {
    const row = rows[rows.length - 1]!;
    const last = row.length > 1 ? row.pop() : undefined;
    rows.push(last ? [last] : []);
  }
  return rows.length <= maxRows ? rows : null;
}

/**
 * The same number of rows, evened out: the narrowest fill line that still takes no more rows,
 * so a ninth pedal does not sit alone under a row of eight.
 */
function balanced(ids: readonly string[], scale: number): string[][] {
  const full = flow(ids, scale, Infinity) ?? [[...ids]];
  if (full.length < 2) return full;
  let best = full;
  let low = PAD_X;
  let high = RIGHT;
  while (high - low > 1) {
    const middle = (low + high) / 2;
    const rows = flow(ids, scale, full.length, middle);
    if (rows) {
      best = rows;
      high = middle;
    } else low = middle;
  }
  return best;
}

/** A path through the points with rounded corners (the points turn at right angles). */
function wire(points: [number, number][]): string {
  let d = '';
  points.forEach(([x, y], i) => {
    const prev = points[i - 1];
    const next = points[i + 1];
    if (!prev) d += `M${round(x)} ${round(y)}`;
    else if (!next) d += `L${round(x)} ${round(y)}`;
    else {
      const towards = (from: [number, number]) => {
        const length = Math.hypot(from[0] - x, from[1] - y) || 1;
        const reach = Math.min(BEND, length / 2);
        return [x + ((from[0] - x) / length) * reach, y + ((from[1] - y) / length) * reach];
      };
      const a = towards(prev);
      const b = towards(next);
      d += `L${round(a[0]!)} ${round(a[1]!)}Q${round(x)} ${round(y)} ${round(b[0]!)} ${round(b[1]!)}`;
    }
  });
  return d;
}

function place(rowsOfIds: string[][], scale: number): BoardLayout {
  const m = metrics(scale);
  const rowHeight = (ids: string[]) =>
    ids.length === 0
      ? m.output.h
      : Math.max(m.output.h, ...ids.map((id) => pedalBox(id).h * m.unit));
  const single = rowsOfIds.length === 1;
  const chain = (ids: string[], first: boolean, last: boolean) =>
    ids.reduce((sum, id) => sum + pedalBox(id).w * m.unit, 0) +
    Math.max(0, ids.length - 1) * m.gap +
    (first ? m.input.w + m.gap : 0) +
    (last ? m.output.w + (ids.length > 0 || first ? m.gap : 0) : 0);

  const pedals: PlacedPedal[] = [];
  const rows: BoardRow[] = [];
  const arrows: Arrow[] = [];
  const path: [number, number][] = [];
  let input: Rect = { x: 0, y: 0, w: 0, h: 0 };
  let output: Rect = { x: 0, y: 0, w: 0, h: 0 };
  let top = PAD_Y;
  /** Where each row's last pedal ends: the cable turns down just past it. */
  const ends: number[] = [];

  rowsOfIds.forEach((ids, r) => {
    const first = r === 0;
    const last = r === rowsOfIds.length - 1;
    const height = rowHeight(ids);
    // The jacks sit a third of the way up a full-height enclosure.
    const cableY = top + height - Math.min(height / 2, 96 * m.unit);
    rows.push({ top, height, cableY });
    // One row is centred, with an empty board's cable a comfortable length; more fill from the left.
    const slack = ids.length === 0 && first ? 220 : 0;
    let x = single ? (BOARD_WIDTH - chain(ids, true, true) - slack) / 2 : PAD_X;

    if (first) {
      input = { x, y: cableY - m.input.h / 2, w: m.input.w, h: m.input.h };
      path.push([x + m.input.w - 6, cableY]);
      x += m.input.w + m.gap;
    } else {
      // Down from the row above: out past its right end, back along the gap, in from the left.
      const above = rows[r - 1]!;
      const between = above.top + above.height + ROW_GAP / 2;
      const outRight = Math.min(BOARD_WIDTH - PAD_X / 2, ends[r - 1]! + PAD_X / 2);
      const inLeft = PAD_X / 2;
      path.push([outRight, above.cableY], [outRight, between], [inLeft, between], [inLeft, cableY]);
      arrows.push({ x: (outRight + inLeft) / 2, y: between, heading: 'left' });
      arrows.push({ x: outRight, y: (above.cableY + between) / 2, heading: 'down' });
      arrows.push({ x: inLeft, y: (between + cableY) / 2, heading: 'down' });
    }

    ids.forEach((id, i) => {
      const box = pedalBox(id);
      const w = box.w * m.unit;
      const h = box.h * m.unit;
      if (i > 0 || first) arrows.push({ x: x - m.gap / 2, y: cableY, heading: 'right' });
      pedals.push({
        id,
        row: r,
        x: round(x),
        y: round(top + height - h),
        w: round(w),
        h: round(h),
      });
      x += w + m.gap;
    });
    ends.push(x - m.gap);

    if (last) {
      x += slack;
      if (ids.length > 0) arrows.push({ x: x - m.gap / 2, y: cableY, heading: 'right' });
      else if (slack) arrows.push({ x: x - m.gap - slack / 2, y: cableY, heading: 'right' });
      output = {
        x: round(x),
        y: round(cableY - m.output.h / 2),
        w: round(m.output.w),
        h: round(m.output.h),
      };
      path.push([x + 4, cableY]);
    }
    top += height + ROW_GAP;
  });

  return {
    scale,
    gap: m.gap,
    pedals,
    rows,
    input: { x: round(input.x), y: round(input.y), w: round(input.w), h: round(input.h) },
    output,
    cable: wire(path),
    arrows: arrows.map((a) => ({ ...a, x: round(a.x), y: round(a.y) })),
    height: round(top - ROW_GAP + PAD_Y),
  };
}

/** How wide one row of these pedals is at a scale, ends and padding included. */
function rowWidth(ids: readonly string[], scale: number): number {
  const m = metrics(scale);
  const pedals = ids.reduce((sum, id) => sum + pedalBox(id).w * m.unit, 0);
  return 2 * PAD_X + m.input.w + m.output.w + (ids.length + 1) * m.gap + pedals;
}

/**
 * Lays the board out: in one row as large as fits, or at the smallest scale in as many rows as
 * it takes. `roomy` is false while the Add pedal shelf takes the lower part of the page: the
 * board is then drawn no larger than fits above it, so all of it stays in view.
 */
export function layoutBoard(ids: readonly string[], roomy = true): BoardLayout {
  const most = roomy ? MAX_SCALE : SHELF_SCALE;
  if (rowWidth(ids, MIN_SCALE) <= BOARD_WIDTH) {
    // The row's width is linear in the scale: the largest that fits, to a hundredth.
    const perScale = rowWidth(ids, MIN_SCALE + 1) - rowWidth(ids, MIN_SCALE);
    const fits = MIN_SCALE + (BOARD_WIDTH - rowWidth(ids, MIN_SCALE)) / perScale;
    return place([[...ids]], Math.min(most, Math.floor(fits * 100) / 100));
  }
  return place(balanced(ids, MIN_SCALE), MIN_SCALE);
}

/**
 * The gap a pedal at `index` would go into, as a bar down the row: before the pedal that is at
 * `index` now, or after the last one.
 */
export function gapAt(layout: BoardLayout, index: number): Rect | null {
  const at = layout.pedals[index];
  const before = layout.pedals[index - 1];
  const beside = at ?? before;
  if (!beside) return null;
  const row = layout.rows[beside.row]!;
  const x = at ? at.x - layout.gap / 2 : before!.x + before!.w + layout.gap / 2;
  return { x: round(x), y: row.top, w: 0, h: row.height };
}
