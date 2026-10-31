// SPDX-License-Identifier: AGPL-3.0-or-later
import { useParamValue } from '../../params/useParam';
import styles from './TapeHeads.module.css';

/** The tape path past the record head and up to four playback heads; active heads glow. */
export function TapeHeads() {
  const heads = Math.round(useParamValue('driftHeads')) + 1;
  const on = useParamValue('driftEnabled') >= 0.5;
  return (
    <svg className={styles.tape} viewBox="0 0 250 64" width={250} height={64} aria-hidden="true">
      <circle className={styles.reel} cx={22} cy={30} r={19} />
      <circle className={styles.hub} cx={22} cy={30} r={5} />
      <circle className={styles.reel} cx={228} cy={30} r={19} />
      <circle className={styles.hub} cx={228} cy={30} r={5} />
      <path className={styles.path} d="M22 49 L228 49" />
      <rect className={styles.rec} x={50} y={38} width={16} height={11} rx={2} />
      {[0, 1, 2, 3].map((i) => (
        <rect
          key={i}
          className={styles.head}
          data-active={(i < heads && on) || undefined}
          data-selected={i < heads || undefined}
          x={92 + i * 30}
          y={38}
          width={16}
          height={11}
          rx={2}
        />
      ))}
      <text className={styles.caption} x={58} y={62}>
        rec
      </text>
      <text className={styles.caption} x={150} y={62}>
        playback
      </text>
    </svg>
  );
}
