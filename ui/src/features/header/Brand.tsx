// SPDX-License-Identifier: AGPL-3.0-or-later
import { INK, NACRE_STOPS, STAR_POINTS } from './starMark';
import styles from './Brand.module.css';

/**
 * The mark is an eight-pointed star of the kind set in mother-of-pearl inlay ("sedef kakma"),
 * drawn as the star polygon {8/3} and inlaid with nacre.
 */
export function NacreStar({ size = 30 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 40 40" aria-hidden="true">
      <defs>
        <linearGradient id="nacre-a" x1="0" y1="0" x2="1" y2="1">
          {NACRE_STOPS.map(([offset, color]) => (
            <stop key={offset} offset={offset} stopColor={color} />
          ))}
        </linearGradient>
        <radialGradient id="nacre-sheen" cx="0.34" cy="0.3" r="0.55">
          <stop offset="0" stopColor="#fff" stopOpacity="0.8" />
          <stop offset="1" stopColor="#fff" stopOpacity="0" />
        </radialGradient>
      </defs>
      <polygon points={STAR_POINTS} fill="url(#nacre-a)" />
      <polygon points={STAR_POINTS} fill="url(#nacre-sheen)" />
      <circle cx="20" cy="20" r="2.4" fill={INK} opacity="0.8" />
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
