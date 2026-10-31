// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ReactNode } from 'react';
import styles from './ProgressRing.module.css';

export function ProgressRing({
  value,
  size = 168,
  label,
  children,
}: {
  value: number;
  size?: number;
  label: string;
  children?: ReactNode;
}) {
  const r = size / 2 - 8;
  const c = 2 * Math.PI * r;
  return (
    <div
      className={styles.ring}
      style={{ width: size, height: size }}
      role="progressbar"
      aria-label={label}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(value * 100)}
    >
      <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} aria-hidden="true">
        <defs>
          <linearGradient id="ring-nacre" x1="0" y1="0" x2="1" y2="1">
            <stop offset="0" stopColor="#cdeeee" />
            <stop offset="0.4" stopColor="#ddd6f6" />
            <stop offset="0.7" stopColor="#f6dbe2" />
            <stop offset="1" stopColor="#f2e8cb" />
          </linearGradient>
        </defs>
        <circle className={styles.track} cx={size / 2} cy={size / 2} r={r} />
        <circle
          className={styles.value}
          cx={size / 2}
          cy={size / 2}
          r={r}
          stroke="url(#ring-nacre)"
          strokeDasharray={`${(c * Math.min(1, Math.max(0, value))).toFixed(1)} ${c.toFixed(1)}`}
          transform={`rotate(-90 ${size / 2} ${size / 2})`}
        />
      </svg>
      <div className={styles.center}>{children}</div>
    </div>
  );
}
