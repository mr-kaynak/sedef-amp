// SPDX-License-Identifier: AGPL-3.0-or-later
import styles from './Brand.module.css';

/**
 * The mark is an eight-pointed star of the kind set in mother-of-pearl inlay ("sedef kakma"),
 * drawn as the star polygon {8/3} and inlaid with nacre.
 */
const STAR_POINTS =
  '20.00,1.50 23.83,10.75 33.08,6.92 29.25,16.17 38.50,20.00 29.25,23.83 33.08,33.08 23.83,29.25 20.00,38.50 16.17,29.25 6.92,33.08 10.75,23.83 1.50,20.00 10.75,16.17 6.92,6.92 16.17,10.75';

export function NacreStar({ size = 30 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 40 40" aria-hidden="true">
      <defs>
        <linearGradient id="nacre-a" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="#cdeeee" />
          <stop offset="0.35" stopColor="#ddd6f6" />
          <stop offset="0.65" stopColor="#f6dbe2" />
          <stop offset="1" stopColor="#f2e8cb" />
        </linearGradient>
        <radialGradient id="nacre-sheen" cx="0.34" cy="0.3" r="0.55">
          <stop offset="0" stopColor="#fff" stopOpacity="0.8" />
          <stop offset="1" stopColor="#fff" stopOpacity="0" />
        </radialGradient>
      </defs>
      <polygon points={STAR_POINTS} fill="url(#nacre-a)" />
      <polygon points={STAR_POINTS} fill="url(#nacre-sheen)" />
      <circle cx="20" cy="20" r="2.4" fill="#0b1c27" opacity="0.8" />
    </svg>
  );
}

export function Brand() {
  return (
    <div className={styles.brand}>
      <NacreStar />
      <div className={styles.words}>
        <span className={styles.name}>Sedef</span>
        <span className={styles.sub}>amp</span>
      </div>
    </div>
  );
}
