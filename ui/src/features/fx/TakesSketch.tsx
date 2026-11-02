// SPDX-License-Identifier: AGPL-3.0-or-later
import { useParamValue } from '../../params/useParam';
import { describeOffsets, describeWidth, offsetMs, SKETCH_LEVEL, SKETCH_Z } from './takes';
import styles from './TakesSketch.module.css';

const W = 300;
const H = 46;
const LANE = 14;
const NOTE_W = 30;
const STEP = 42; // px between notes; 1 px per ms of offset
const LANES = [6, 26] as const;

/** One note as an envelope: a sharp attack at x that decays to the right. */
function Note({
  className,
  x,
  base,
  height,
}: {
  className?: string;
  x: number;
  base: number;
  height: number;
}) {
  const top = base - height;
  const envelope = `M${x} ${base} L${x} ${top} Q${x + NOTE_W * 0.3} ${base - height * 0.35} ${x + NOTE_W} ${base - 1}`;
  return (
    <g className={className}>
      <path className={styles.body} d={`${envelope} L${x + NOTE_W} ${base} Z`} />
      <path className={styles.line} d={envelope} />
    </g>
  );
}

/**
 * Sketch of two takes of one phrase: the part as played, and the double trailing it by a
 * different amount on every note. Time is squeezed between notes but true to scale in the
 * offsets (1 px per ms), so Spread moves the lower lane and Variation scatters it.
 */
export function TakesSketch() {
  const spread = useParamValue('doublerSpread');
  const variation = useParamValue('doublerVariation');
  const width = useParamValue('doublerWidth');
  const mix = useParamValue('doublerMix');
  const summary = describeOffsets(spread, variation);

  return (
    <div className={styles.wrap}>
      <div className={styles.sketch}>
        <div className={styles.lanes} aria-hidden="true">
          <span>Played</span>
          <span>Double</span>
        </div>
        <svg
          viewBox={`0 0 ${W} ${H}`}
          preserveAspectRatio="xMinYMid slice"
          className={styles.svg}
          role="img"
          aria-label={`Second take, ${summary} the first`}
        >
          {SKETCH_Z.map((z, i) => {
            const x = 6 + i * STEP;
            const shift = offsetMs(spread, variation, z);
            const level = 1 + 0.18 * variation * (SKETCH_LEVEL[i] ?? 0);
            return (
              <g key={i}>
                <Note className={styles.played} x={x} base={LANES[0] + LANE} height={LANE} />
                <g style={{ opacity: 0.35 + 0.65 * mix }}>
                  <Note
                    className={styles.double}
                    x={x + shift}
                    base={LANES[1] + LANE}
                    height={LANE * level}
                  />
                </g>
              </g>
            );
          })}
        </svg>
      </div>
      <div className={styles.legend}>
        <span>{describeWidth(width)}</span>
        <span>{summary}</span>
      </div>
    </div>
  );
}
