// SPDX-License-Identifier: AGPL-3.0-or-later
// What is painted on each enclosure: one line drawing per pedal, after the sea thing it is named
// for, so a pedal can be told from its neighbours without reading it. Every figure is computed
// here from a few numbers (rings, rays, spirals, waves); nothing is traced or imported. The lines
// take the enclosure's legend colour.
import type { PedalBox } from './sizes';
import styles from './Pedal.module.css';

const n = (v: number) => Math.round(v * 10) / 10;
const TAU = Math.PI * 2;

/** A polyline through points given by t in 0..1. */
function curve(steps: number, at: (t: number) => [number, number]): string {
  let d = '';
  for (let i = 0; i <= steps; i += 1) {
    const [x, y] = at(i / steps);
    d += `${i === 0 ? 'M' : 'L'}${n(x)} ${n(y)}`;
  }
  return d;
}

const line = (x0: number, y0: number, x1: number, y1: number) =>
  `M${n(x0)} ${n(y0)}L${n(x1)} ${n(y1)}`;

const ring = (cx: number, cy: number, r: number) =>
  `M${n(cx - r)} ${n(cy)}a${n(r)} ${n(r)} 0 1 0 ${n(2 * r)} 0a${n(r)} ${n(r)} 0 1 0 ${n(-2 * r)} 0`;

/** A repeatable stand-in for chance, so a drawing is the same every time. */
function scatter(seed: number): () => number {
  let s = seed;
  return () => {
    s = (s * 16807) % 2147483647;
    return s / 2147483647;
  };
}

type Figure = (w: number, h: number) => { lines: string; dots?: string };

const FIGURES: Record<string, Figure> = {
  // A portcullis: bars that come down and shut.
  gate: (w, h) => {
    let d = '';
    const bars = 6;
    for (let i = 0; i < bars; i += 1) {
      const x = 13 + (i * (w - 26)) / (bars - 1);
      d += line(x, 0, x, h * 0.52) + `l-3 -6m3 6l3 -6`;
    }
    return { lines: d + line(6, h * 0.07, w - 6, h * 0.07) + line(6, h * 0.34, w - 6, h * 0.34) };
  },
  // Swell: each line down rides a little higher.
  tide: (w, h) => {
    let d = '';
    for (let k = 0; k < 7; k += 1) {
      const y = h * (0.1 + 0.105 * k);
      d += curve(40, (t) => [t * w, y + (2 + k * 0.9) * Math.sin(t * TAU * 1.5 + k * 0.7)]);
    }
    return { lines: d };
  },
  // A scallop: growth rings and ribs fanning from the hinge.
  pearl: (w, h) => {
    const cx = w / 2;
    const cy = h * 0.98;
    let d = '';
    for (let r = 34; r < h * 0.95; r += 22)
      d += curve(36, (t) => {
        const a = Math.PI * (1.1 + 0.8 * t);
        return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
      });
    for (let i = 0; i <= 10; i += 1) {
      const a = Math.PI * (1.1 + 0.08 * i);
      d += line(
        cx + 34 * Math.cos(a),
        cy + 34 * Math.sin(a),
        cx + h * Math.cos(a),
        cy + h * Math.sin(a),
      );
    }
    return { lines: d };
  },
  // A branching coral, forking as it climbs.
  coral: (w, h) => {
    let d = '';
    const grow = (x: number, y: number, angle: number, length: number, depth: number) => {
      const x1 = x + length * Math.cos(angle);
      const y1 = y + length * Math.sin(angle);
      d += line(x, y, x1, y1);
      if (depth === 0) return;
      grow(x1, y1, angle - 0.42 - depth * 0.03, length * 0.74, depth - 1);
      grow(x1, y1, angle + 0.36 + depth * 0.02, length * 0.7, depth - 1);
    };
    grow(w * 0.5, h * 0.9, -Math.PI / 2, h * 0.21, 5);
    return { lines: d };
  },
  // Rings spreading from where the stone went in.
  ripple: (w, h) => {
    let d = '';
    for (let r = 9; r < h * 0.8; r += 13) d += ring(w * 0.7, h * 0.3, r);
    return { lines: d };
  },
  // Canyon walls closing in as they go down.
  trench: (w, h) => {
    let d = '';
    for (let k = 0; k < 8; k += 1) {
      const inset = k * 9;
      const depth = h * 0.92 - k * 26;
      d += `M${n(inset)} 0L${n(w / 2 - 5)} ${n(depth)}h10L${n(w - inset)} 0`;
    }
    return { lines: d };
  },
  // Spines, long and short, around the shell.
  urchin: (w, h) => {
    const cx = w / 2;
    const cy = h * 0.46;
    let d = ring(cx, cy, 15);
    for (let i = 0; i < 40; i += 1) {
      const a = (i / 40) * TAU;
      const reach = i % 2 === 0 ? 68 : 46;
      d += line(
        cx + 19 * Math.cos(a),
        cy + 19 * Math.sin(a),
        cx + reach * Math.cos(a),
        cy + reach * Math.sin(a),
      );
    }
    return { lines: d };
  },
  // Fronds swaying up from the bottom, with their blades.
  kelp: (w, h) => {
    let d = '';
    [0.2, 0.42, 0.63, 0.84].forEach((fx, k) => {
      const top = h * (0.08 + 0.09 * ((k * 3) % 4));
      const sway = (t: number) => fx * w + 7 * Math.sin(t * TAU * 1.6 + k * 1.3);
      d += curve(40, (t) => [sway(t), h - t * (h - top)]);
      for (let b = 1; b < 9; b += 1) {
        const t = b / 9;
        const y = h - t * (h - top);
        const side = b % 2 === 0 ? 1 : -1;
        d += `M${n(sway(t))} ${n(y)}q${n(side * 9)} ${n(-3)} ${n(side * 11)} ${n(-12)}`;
      }
    });
    return { lines: d };
  },
  // The slatted disc a remora holds on with.
  remora: (w, h) => {
    const cx = w / 2;
    const cy = h * 0.42;
    const rx = w * 0.36;
    const ry = h * 0.3;
    let d = curve(48, (t) => [cx + rx * Math.cos(t * TAU), cy + ry * Math.sin(t * TAU)]);
    d += line(cx, cy - ry, cx, cy + ry);
    for (let k = -6; k <= 6; k += 1) {
      const y = cy + (k / 7) * ry;
      const half = rx * Math.sqrt(1 - (k / 7) ** 2);
      d += `M${n(cx - half)} ${n(y)}Q${n(cx)} ${n(y + 7)} ${n(cx + half)} ${n(y)}`;
    }
    return { lines: d };
  },
  // Drifting weed: strands and the floats that hold them up.
  sargasso: (w, h) => {
    const rand = scatter(41);
    let d = '';
    let dots = '';
    for (let k = 0; k < 6; k += 1) {
      const y = h * (0.1 + 0.14 * k);
      const phase = rand() * TAU;
      const wave = (t: number) => y + 6 * Math.sin(t * TAU * 1.2 + phase) + 3 * Math.sin(t * 17);
      d += curve(40, (t) => [t * w, wave(t)]);
      for (let f = 0; f < 4; f += 1) {
        const t = (f + rand()) / 4;
        dots += ring(t * w, wave(t) - 4, 2.6);
      }
    }
    return { lines: d, dots };
  },
  // A wave curling over before it breaks.
  breaker: (w, h) => {
    const cx = w * 0.56;
    const cy = h * 0.5;
    let d = curve(60, (t) => {
      const a = Math.PI * 0.95 - t * TAU * 1.35;
      const r = 40 - 30 * t;
      return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
    });
    d += `M0 ${n(cy + 46)}Q${n(w * 0.1)} ${n(cy + 44)} ${n(cx + 40 * Math.cos(Math.PI * 0.95))} ${n(cy + 40 * Math.sin(Math.PI * 0.95))}`;
    for (let k = 1; k < 5; k += 1)
      d += curve(24, (t) => [t * w, cy + 46 + k * 15 + 2.5 * Math.sin(t * TAU * 2 + k)]);
    let dots = '';
    const rand = scatter(7);
    for (let i = 0; i < 9; i += 1)
      dots += ring(w * (0.1 + 0.8 * rand()), cy - 46 - 30 * rand(), 1.6);
    return { lines: d, dots };
  },
  // Tentacles opening from the foot.
  anemone: (w, h) => {
    const cx = w / 2;
    const cy = h * 0.96;
    let d = '';
    let dots = '';
    for (let i = 0; i < 13; i += 1) {
      const a = -Math.PI / 2 + (i - 6) * 0.17;
      const reach = h * (0.86 - 0.012 * (i - 6) ** 2);
      const tip: [number, number] = [cx + reach * Math.cos(a), cy + reach * Math.sin(a)];
      const bend = (i % 2 === 0 ? 1 : -1) * 12;
      d += `M${n(cx + (i - 6) * 3)} ${n(cy)}Q${n((cx + tip[0]) / 2 + bend)} ${n((cy + tip[1]) / 2)} ${n(tip[0])} ${n(tip[1])}`;
      dots += ring(tip[0], tip[1], 2.4);
    }
    return { lines: d, dots };
  },
  // The shell's spiral, opening as it turns.
  conch: (w, h) => {
    const cx = w * 0.5;
    const cy = h * 0.44;
    const turns = 3.4;
    let d = curve(120, (t) => {
      const a = t * TAU * turns;
      const r = 2.5 * Math.exp(0.165 * a);
      return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
    });
    for (let i = 0; i < 9; i += 1) {
      const a = TAU * (turns - 1) + (i / 9) * TAU;
      const r0 = 2.5 * Math.exp(0.165 * (a - TAU));
      const r1 = 2.5 * Math.exp(0.165 * a);
      d += line(
        cx + r0 * Math.cos(a),
        cy + r0 * Math.sin(a),
        cx + r1 * Math.cos(a),
        cy + r1 * Math.sin(a),
      );
    }
    return { lines: d };
  },
  // A lamp at the top and its beams.
  beacon: (w, h) => {
    const cx = w / 2;
    const cy = h * 0.06;
    let d = ring(cx, cy, 5);
    for (let i = 0; i < 9; i += 1) {
      const a = Math.PI / 2 + (i - 4) * 0.2;
      d += line(
        cx + 11 * Math.cos(a),
        cy + 11 * Math.sin(a),
        cx + h * Math.cos(a),
        cy + h * Math.sin(a),
      );
    }
    for (const r of [h * 0.3, h * 0.55, h * 0.8])
      d += curve(24, (t) => {
        const a = Math.PI / 2 + (t - 0.5) * 1.7;
        return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
      });
    return { lines: d };
  },
  // Three arms of one slow turning.
  gyre: (w, h) => {
    const cx = w / 2;
    const cy = h * 0.44;
    let d = '';
    for (let arm = 0; arm < 3; arm += 1)
      d += curve(70, (t) => {
        const a = t * TAU * 1.25 + (arm * TAU) / 3;
        const r = 5 + 78 * t;
        return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
      });
    return { lines: d };
  },
  // Deck boards between two rows of piles.
  jetty: (w, h) => {
    let d = '';
    const left = w * 0.14;
    const right = w * 0.86;
    let row = 0;
    for (let y = h * 0.06; y < h * 0.98; y += 15) {
      d += line(left, y, right, y);
      const joint = left + (right - left) * (row % 2 === 0 ? 0.36 : 0.68);
      d += line(joint, y, joint, y + 15);
      row += 1;
    }
    d += line(left, 0, left, h) + line(right, 0, right, h);
    d += line(left - 5, 0, left - 5, h) + line(right + 5, 0, right + 5, h);
    return { lines: d };
  },
};

/** Each figure is worked out once: a pedal's box never changes. */
const DRAWN = new Map<string, ReturnType<Figure>>();

function draw(id: string, box: PedalBox): ReturnType<Figure> | undefined {
  const figure = FIGURES[id];
  if (!figure) return undefined;
  let drawn = DRAWN.get(id);
  if (!drawn) {
    drawn = figure(box.w, box.h);
    DRAWN.set(id, drawn);
  }
  return drawn;
}

/** The enclosure's painted figure, behind its controls. Decoration only. */
export function PedalArt({ id, box }: { id: string; box: PedalBox }) {
  const drawn = draw(id, box);
  if (!drawn) return null;
  const { lines, dots } = drawn;
  return (
    <svg
      className={styles.art}
      viewBox={`0 0 ${box.w} ${box.h}`}
      preserveAspectRatio="none"
      aria-hidden="true"
      focusable="false"
    >
      <path d={lines} />
      {dots && <path className={styles.artDots} d={dots} />}
    </svg>
  );
}
