// SPDX-License-Identifier: AGPL-3.0-or-later
// Top view of the bridge: strings run left (neck) to right (tail), one saddle per string.
// Offsets are drawn exaggerated so a 0.3 mm move is visible; the scale bar keeps it honest.
import { formatMm } from './math';
import styles from './SaddleDiagram.module.css';

const W = 470;
const PX_PER_MM = 30;
const MAX_PX = 104;
const SADDLE_X = 236;
const SADDLE_W = 18;
const SADDLE_H = 12;

export interface SaddleDiagramProps {
  /** Signed offsets per string in mm (positive = away from the neck), null if not measured. */
  offsets: (number | null)[];
  labels: string[];
  highlight: number | null;
}

const clampPx = (mm: number) => Math.max(-MAX_PX, Math.min(MAX_PX, mm * PX_PER_MM));

export function SaddleDiagram({ offsets, labels, highlight }: SaddleDiagramProps) {
  const rows = offsets.length;
  const pitch = Math.min(40, 280 / Math.max(rows, 1));
  const top = 56;
  const bottom = top + pitch * (rows - 1);
  const H = bottom + 56;
  const focus = highlight !== null ? (offsets[highlight] ?? null) : null;
  const truncated = focus !== null && Math.abs(focus) * PX_PER_MM > MAX_PX;

  return (
    <svg
      className={styles.diagram}
      viewBox={`0 0 ${W} ${H}`}
      width={W}
      height={H}
      role="img"
      aria-label="Saddle positions, seen from above"
    >
      <defs>
        <marker
          id="saddle-arrow"
          viewBox="0 0 10 10"
          refX="9"
          refY="5"
          markerWidth="6"
          markerHeight="6"
          orient="auto"
        >
          <path d="M0 0 L10 5 L0 10 z" className={styles.arrowHead} />
        </marker>
      </defs>
      <text className={styles.dir} x={0} y={14}>
        ← Neck
      </text>
      <text className={styles.dir} x={W} y={14} textAnchor="end">
        Tail →
      </text>
      <rect
        className={styles.plate}
        x={SADDLE_X - 120}
        y={top - pitch / 2 - 4}
        width={240}
        height={pitch * rows + 8}
        rx={6}
      />
      {offsets.map((mm, i) => {
        const y = top + pitch * i;
        const isFocus = i === highlight;
        const dx = mm === null ? 0 : clampPx(mm);
        const showMove = isFocus && mm !== null && Math.abs(dx) >= 4;
        return (
          <g
            key={i}
            className={styles.row}
            data-focus={isFocus || undefined}
            data-measured={mm !== null || undefined}
          >
            <line
              className={styles.string}
              x1={0}
              x2={W}
              y1={y}
              y2={y}
              strokeWidth={0.8 + (rows - i) * 0.3}
            />
            <text className={styles.label} x={0} y={y - 6}>
              {labels[i]}
            </text>
            <rect
              className={styles.saddle}
              x={SADDLE_X - SADDLE_W / 2}
              y={y - SADDLE_H / 2}
              width={SADDLE_W}
              height={SADDLE_H}
              rx={3}
            />
            {showMove && mm !== null && (
              <>
                <rect
                  className={styles.ghost}
                  x={SADDLE_X - SADDLE_W / 2 + dx}
                  y={y - SADDLE_H / 2}
                  width={SADDLE_W}
                  height={SADDLE_H}
                  rx={3}
                />
                <line
                  className={styles.arrow}
                  x1={SADDLE_X}
                  x2={SADDLE_X + dx}
                  y1={y - SADDLE_H / 2 - 7}
                  y2={y - SADDLE_H / 2 - 7}
                  markerEnd="url(#saddle-arrow)"
                />
                <text
                  className={styles.amount}
                  x={SADDLE_X + dx / 2}
                  y={y - SADDLE_H / 2 - 13}
                  textAnchor="middle"
                >
                  {formatMm(Math.abs(mm)).replace('+', '')}
                  {truncated ? ' (not to scale)' : ''}
                </text>
              </>
            )}
            {mm !== null && !isFocus && Math.abs(dx) >= 1 && (
              <circle className={styles.pip} cx={SADDLE_X + dx} cy={y} r={2.5} />
            )}
          </g>
        );
      })}
      <g className={styles.scale} transform={`translate(${W - PX_PER_MM}, ${H - 14})`}>
        <line x1={0} x2={PX_PER_MM} y1={0} y2={0} />
        <text x={-6} y={4} textAnchor="end">
          1 mm
        </text>
      </g>
    </svg>
  );
}
