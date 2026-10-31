// SPDX-License-Identifier: AGPL-3.0-or-later
import styles from './Led.module.css';

export function Led({
  on,
  tone = 'nacre',
  size = 8,
  className,
}: {
  on: boolean;
  tone?: 'nacre' | 'coral' | 'amber';
  size?: number;
  className?: string;
}) {
  return (
    <span
      className={[styles.led, styles[tone], className].filter(Boolean).join(' ')}
      data-on={on || undefined}
      style={{ width: size, height: size }}
      aria-hidden="true"
    />
  );
}
