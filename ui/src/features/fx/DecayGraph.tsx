// SPDX-License-Identifier: AGPL-3.0-or-later
import { useParamValue } from '../../params/useParam';
import { formatMs, formatSeconds } from '../../params/format';
import styles from './DecayGraph.module.css';

const W = 270;
const H = 116;

/** Sketch of the reverb envelope: the pre-delay gap, then a 60 dB decay over the decay time. */
export function DecayGraph() {
  const decay = useParamValue('fathomDecay');
  const pre = useParamValue('fathomPreDelay') / 1000;
  const hall = Math.round(useParamValue('fathomMode')) === 1;
  // A log time axis keeps 1 s and 60 s tails both readable.
  const span = Math.log10(1 + (pre + decay) * 4);
  const x = (t: number) => 6 + (Math.log10(1 + t * 4) / Math.max(span, 0.001)) * (W - 12);
  const points: string[] = [];
  const steps = 60;
  for (let i = 0; i <= steps; i += 1) {
    const t = pre + (decay * i) / steps;
    const db = (-60 * (t - pre)) / decay;
    const y = 8 + (-db / 60) * (H - 16);
    points.push(`${x(t).toFixed(1)} ${y.toFixed(1)}`);
  }
  const d = `M6 ${H - 8} L${x(pre).toFixed(1)} ${H - 8} L${x(pre).toFixed(1)} 8 L${points.join(' L')}`;
  return (
    <div className={styles.wrap}>
      <svg
        viewBox={`0 0 ${W} ${H}`}
        width={W}
        height={H}
        className={styles.svg}
        role="img"
        aria-label="Reverb envelope"
      >
        <path className={styles.fill} d={`${d} L${W - 6} ${H - 8} Z`} />
        <path className={styles.line} d={d} />
      </svg>
      <div className={styles.legend}>
        <span>{hall ? 'Hall' : 'Plate'}</span>
        <span>
          {formatMs(pre * 1000)} gap, {formatSeconds(decay)} tail
        </span>
      </div>
    </div>
  );
}
