// SPDX-License-Identifier: AGPL-3.0-or-later
// Stereo/mono peak meter drawn from telemetry in requestAnimationFrame; it never re-renders.
// The input meter (`zones`) has its own colours and a mark under the bars where the presets
// want the level (docs/ui/input-level.md); the output meter warns in amber near 0 dBFS.
import { useRef } from 'react';
import type { CSSProperties } from 'react';
import { telemetry, useLiveFrame } from '../../state/live';
import { INPUT_LEVEL } from './inputLevel';
import { meterPosition } from './meterScale';
import styles from './LevelMeter.module.css';

const RELEASE_DB_PER_S = 26;
const PEAK_HOLD_S = 1.2;
const PEAK_FALL_DB_PER_S = 14;

const fraction = (db: number) => meterPosition(db).toFixed(4);

/** Where the input meter's colours change, as fractions of the bars: the zone and its middle. */
const ZONE_STYLE = {
  '--zone-lo': fraction(INPUT_LEVEL.quietDb),
  '--zone-mid': fraction(-3),
  '--zone-hi': fraction(INPUT_LEVEL.hotDb),
} as CSSProperties;

export function LevelMeter({
  which,
  label,
  zones = false,
  size = 'sm',
  withClip = true,
}: {
  which: 'input' | 'output';
  label: string;
  /** The input meter's colours and its target zone. */
  zones?: boolean;
  /** `lg`: the Input level panel's meter. */
  size?: 'sm' | 'lg';
  /** The clip lamp (one per meter on screen, so the panel's meter leaves it to the header). */
  withClip?: boolean;
}) {
  const els = useRef<{ covers: (HTMLDivElement | null)[]; peaks: (HTMLDivElement | null)[] }>({
    covers: [],
    peaks: [],
  });
  const clipRef = useRef<HTMLButtonElement>(null);
  const state = useRef({ level: [-90, -90], peak: [-90, -90], age: [0, 0], clip: false });
  const clipName = `${label} clip light`;
  const clippedName = `${label} clipped. Click to reset`;

  useLiveFrame(telemetry, (t, dt) => {
    const s = state.current;
    const dbs = t ? (which === 'input' ? t.inputDb : t.outputDb) : [];
    for (let ch = 0; ch < 2; ch += 1) {
      const target = dbs[ch] ?? dbs[0] ?? -90;
      s.level[ch] = Math.max(target, (s.level[ch] ?? -90) - RELEASE_DB_PER_S * dt);
      if (target >= (s.peak[ch] ?? -90)) {
        s.peak[ch] = target;
        s.age[ch] = 0;
      } else {
        s.age[ch] = (s.age[ch] ?? 0) + dt;
        if ((s.age[ch] ?? 0) > PEAK_HOLD_S)
          s.peak[ch] = (s.peak[ch] ?? -90) - PEAK_FALL_DB_PER_S * dt;
      }
      const cover = els.current.covers[ch];
      if (cover) cover.style.transform = `scaleX(${1 - meterPosition(s.level[ch] ?? -90)})`;
      const peak = els.current.peaks[ch];
      if (peak) {
        const p = meterPosition(s.peak[ch] ?? -90);
        peak.style.transform = `translateX(${p * 100}%)`;
        peak.style.opacity = p > 0.02 ? '1' : '0';
      }
    }
    const clipped = t ? (which === 'input' ? t.inputClip : t.outputClip) : false;
    if (clipped && !s.clip) {
      s.clip = true;
      const lamp = clipRef.current;
      lamp?.setAttribute('data-on', '');
      lamp?.setAttribute('aria-label', clippedName);
      lamp?.setAttribute('title', clippedName);
    }
  });

  const resetClip = () => {
    state.current.clip = false;
    const lamp = clipRef.current;
    lamp?.removeAttribute('data-on');
    lamp?.setAttribute('aria-label', clipName);
    lamp?.setAttribute('title', clipName);
  };

  return (
    <div
      className={styles.meter}
      data-size={size}
      data-zones={zones || undefined}
      data-clip={withClip || undefined}
      style={zones ? ZONE_STYLE : undefined}
      role="img"
      aria-label={`${label} level`}
    >
      {[0, 1].map((ch) => (
        <div key={ch} className={styles.bar}>
          <div
            className={styles.cover}
            ref={(el) => {
              els.current.covers[ch] = el;
            }}
          />
          <div className={styles.peakTrack}>
            <div
              className={styles.peak}
              ref={(el) => {
                els.current.peaks[ch] = el;
              }}
            />
          </div>
        </div>
      ))}
      {zones && <div className={styles.zone} aria-hidden="true" />}
      {withClip && (
        <button
          ref={clipRef}
          type="button"
          className={styles.clip}
          onClick={resetClip}
          aria-label={clipName}
          title={clipName}
        />
      )}
    </div>
  );
}
