// SPDX-License-Identifier: AGPL-3.0-or-later
// Where everything on the board sits. The canvas is always 1200 px wide (app/Stage scales it), so
// the layout is arithmetic over the boxes: no measuring. The chain runs from the Guitar jack
// through the pedals in front of the amp, the amp itself (a block in the chain), the pedals after
// it and on to the Out jack (docs/board.md §5). A board that fits one row is drawn as large as
// that row allows, up to 1.4 canvas pixels per unit, and centred; otherwise the pedals are at the
// smallest scale and fill rows evenly from the left, the signal running along each row and back
// to the start of the next. The pedals after the amp stand on a band, named at its start. The
// widths are chosen so that sixteen different pedals fit two rows in any order, on either side of
// the amp (a greedy fill wastes less than the widest pedal and a gap at the end of the first row:
// 2 x 1168 px hold 1948 px of pedals, the amp, two jacks, the band's name and 17 gaps with that
// to spare). A larger board takes more rows and scrolls.
import { AMP, baseId, withAmp } from './registry';
import { MAX_SCALE, MIN_SCALE, pedalBox } from './sizes';

export const BOARD_WIDTH = 1200;
/** Room for two small rows under the toolbar, in canvas pixels. */
export const BOARD_HEIGHT = 588;

const PAD_X = 16;
const PAD_Y = 6;
const ROW_GAP = 16;
/** The cable's corners. */
const BEND = 6;
/** How far the band behind the pedals after the amp reaches past them. */
const BAND_PAD = 6;
/** The band's name ("After the amp") stands upright at its start, above the cable: this wide. */
const BAND_LABEL = 14;

interface Metrics {
  /** Canvas pixels per board unit. */
  unit: number;
  /** Space between neighbours in a row. */
  gap: number;
  /** The jacks' (Guitar, Out) and the amp's boxes. */
  jack: { w: number; h: number };
  amp: { w: number; h: number };
}

/** The spacing, the jacks and the amp grow with the pedals. */
function metrics(scale: number): Metrics {
  const grown = scale - MIN_SCALE;
  return {
    unit: scale,
    gap: 7 + 10 * grown,
    jack: { w: 26 + 20 * grown, h: 56 + 20 * grown },
    amp: { w: 40 + 30 * grown, h: 136 + 60 * grown },
  };
}

/**
 * The largest the board is drawn while the Add pedal shelf takes the lower part of the page: 317 px
 * are left between the toolbar and the shelf (its modulation cards carry After the amp), and a
 * row of the tallest pedals at 1.08 is 302 px plus the padding.
 */
const SHELF_SCALE = 1.08;

export interface Rect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface PlacedPedal extends Rect {
  /** The pedal's token ("kelp", "kelp#2"), or "amp" for the amp. */
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
  /** Everything in the chain, in signal order: the pedals and the amp between them. */
  items: PlacedPedal[];
  /** The pedals alone. */
  pedals: PlacedPedal[];
  /** The amp's block. */
  amp: Rect;
  rows: BoardRow[];
  /** The Guitar jack and the Out jack. */
  input: Rect;
  output: Rect;
  /** Behind the pedals after the amp (and Out): one band per row they stand in. */
  afterAmp: Rect[];
  /** Where the band's name stands, at its start; null without pedals after the amp. */
  afterAmpLabel: Rect | null;
  /** The signal's path from the Guitar jack to Out, as SVG path data. */
  cable: string;
  arrows: Arrow[];
  height: number;
}

const round = (v: number) => Math.round(v * 10) / 10;

const RIGHT = BOARD_WIDTH - PAD_X;

function itemBox(id: string, m: Metrics): { w: number; h: number } {
  if (id === AMP) return m.amp;
  const box = pedalBox(baseId(id));
  return { w: box.w * m.unit, h: box.h * m.unit };
}

/** The index of the first pedal after the amp, which the band's name stands in front of; -1 if none. */
function firstAfterAmp(ids: readonly string[]): number {
  const at = ids.indexOf(AMP);
  return at >= 0 && at + 1 < ids.length ? at + 1 : -1;
}

/**
 * Items in rows, in order, each row filled up to `right`; null when they need more than
 * `maxRows`.
 */
function flow(
  ids: readonly string[],
  scale: number,
  maxRows: number,
  right = RIGHT,
): string[][] | null {
  const m = metrics(scale);
  const named = firstAfterAmp(ids);
  const width = (id: string) => itemBox(id, m).w + (id === ids[named] ? BAND_LABEL : 0);
  const rows: string[][] = [[]];
  let x = PAD_X + m.jack.w + m.gap;
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
  // Out follows the last item. Without room for it, the last item goes down a row with it.
  if (x + m.jack.w > right) {
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

const roundRect = (r: Rect): Rect => ({
  x: round(r.x),
  y: round(r.y),
  w: round(r.w),
  h: round(r.h),
});

function place(rowsOfIds: string[][], scale: number): BoardLayout {
  const m = metrics(scale);
  const rowHeight = (ids: string[]) =>
    Math.max(m.amp.h, ...ids.map((id) => (id === AMP ? 0 : itemBox(id, m).h)));
  const single = rowsOfIds.length === 1;
  const flat = rowsOfIds.flat();
  const named = firstAfterAmp(flat);
  const chain = (ids: string[]) =>
    ids.reduce((sum, id) => sum + itemBox(id, m).w, 0) +
    (ids.length + 1) * m.gap +
    2 * m.jack.w +
    (named >= 0 ? BAND_LABEL : 0);

  const items: PlacedPedal[] = [];
  const rows: BoardRow[] = [];
  const arrows: Arrow[] = [];
  const path: [number, number][] = [];
  const afterAmp: Rect[] = [];
  let afterAmpLabel: Rect | null = null;
  let input: Rect = { x: 0, y: 0, w: 0, h: 0 };
  let output: Rect = { x: 0, y: 0, w: 0, h: 0 };
  let amp: Rect = { x: 0, y: 0, w: 0, h: 0 };
  let top = PAD_Y;
  let pastAmp = false;
  /** The item's index in the whole chain. */
  let k = 0;
  /** Where each row's last item ends: the cable turns down just past it. */
  const ends: number[] = [];

  rowsOfIds.forEach((ids, r) => {
    const first = r === 0;
    const last = r === rowsOfIds.length - 1;
    const height = rowHeight(ids);
    // The jacks sit a third of the way up a full-height enclosure.
    const cableY = top + height - Math.min(height / 2, 96 * m.unit);
    rows.push({ top, height, cableY });
    // One row is centred; more fill from the left.
    let x = single ? (BOARD_WIDTH - chain(ids)) / 2 : PAD_X;
    /** Where the band after the amp starts in this row, once past it, and whether it holds a pedal. */
    let bandFrom = pastAmp ? PAD_X / 2 : null;
    let bandHolds = false;

    if (first) {
      input = { x, y: cableY - m.jack.h / 2, w: m.jack.w, h: m.jack.h };
      path.push([x + m.jack.w - 6, cableY]);
      x += m.jack.w + m.gap;
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
      const box = itemBox(id, m);
      // The band's name stands upright in front of the first pedal after the amp, above the cable.
      if (k === named) {
        afterAmpLabel = { x, y: top, w: BAND_LABEL, h: cableY - top - 8 };
        x += BAND_LABEL;
      }
      k += 1;
      if (i > 0 || first) arrows.push({ x: x - m.gap / 2, y: cableY, heading: 'right' });
      // Pedals stand on the row's bottom line; the amp sits on the cable.
      const y = id === AMP ? cableY - box.h / 2 : top + height - box.h;
      const placed = { id, row: r, x: round(x), y: round(y), w: round(box.w), h: round(box.h) };
      items.push(placed);
      if (id === AMP) {
        amp = { x: placed.x, y: placed.y, w: placed.w, h: placed.h };
        pastAmp = true;
        bandFrom = x + box.w + m.gap / 2;
      } else if (pastAmp) bandHolds = true;
      x += box.w + m.gap;
    });
    ends.push(x - m.gap);

    if (last) {
      arrows.push({ x: x - m.gap / 2, y: cableY, heading: 'right' });
      output = { x, y: cableY - m.jack.h / 2, w: m.jack.w, h: m.jack.h };
      path.push([x + 6, cableY]);
      x += m.jack.w + m.gap;
    }
    if (bandFrom !== null && bandHolds) {
      const to = Math.min(BOARD_WIDTH - PAD_X / 2, x - m.gap + BAND_PAD);
      afterAmp.push({ x: bandFrom, y: top - BAND_PAD / 2, w: to - bandFrom, h: height + BAND_PAD });
    }
    top += height + ROW_GAP;
  });

  return {
    scale,
    gap: m.gap,
    items,
    pedals: items.filter((p) => p.id !== AMP),
    amp,
    rows,
    input: roundRect(input),
    output: roundRect(output),
    afterAmp: afterAmp.map(roundRect),
    afterAmpLabel: afterAmpLabel && roundRect(afterAmpLabel),
    cable: wire(path),
    arrows: arrows.map((a) => ({ ...a, x: round(a.x), y: round(a.y) })),
    height: round(top - ROW_GAP + PAD_Y),
  };
}

/** How wide one row of these items is at a scale, ends and padding included. */
function rowWidth(ids: readonly string[], scale: number): number {
  const m = metrics(scale);
  const widths = ids.reduce((sum, id) => sum + itemBox(id, m).w, 0);
  const label = firstAfterAmp(ids) >= 0 ? BAND_LABEL : 0;
  return 2 * PAD_X + 2 * m.jack.w + (ids.length + 1) * m.gap + widths + label;
}

/**
 * Lays the board out: in one row as large as fits, or at the smallest scale in as many rows as
 * it takes. `board` is the board's tokens; the amp goes where "amp" is, or after the last pedal.
 * `roomy` is false while the Add pedal shelf takes the lower part of the page: the board is then
 * drawn no larger than fits above it, so all of it stays in view.
 */
export function layoutBoard(board: readonly string[], roomy = true): BoardLayout {
  const ids = withAmp(board);
  const most = roomy ? MAX_SCALE : SHELF_SCALE;
  if (rowWidth(ids, MIN_SCALE) <= BOARD_WIDTH) {
    // The row's width is linear in the scale: the largest that fits, to a hundredth.
    const perScale = rowWidth(ids, MIN_SCALE + 1) - rowWidth(ids, MIN_SCALE);
    const fits = MIN_SCALE + (BOARD_WIDTH - rowWidth(ids, MIN_SCALE)) / perScale;
    return place([ids], Math.min(most, Math.floor(fits * 100) / 100));
  }
  return place(balanced(ids, MIN_SCALE), MIN_SCALE);
}

/**
 * The gap an item at `index` in `layout.items` would go into, as a bar down the row: before the
 * item that is at `index` now, or after the last one.
 */
export function gapAt(layout: BoardLayout, index: number): Rect | null {
  const at = layout.items[index];
  const before = layout.items[index - 1];
  const beside = at ?? before;
  if (!beside) return null;
  const row = layout.rows[beside.row]!;
  const x = at ? at.x - layout.gap / 2 : before!.x + before!.w + layout.gap / 2;
  return { x: round(x), y: row.top, w: 0, h: row.height };
}
