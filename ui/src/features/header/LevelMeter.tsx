// SPDX-License-Identifier: AGPL-3.0-or-later
// Stereo/mono peak meter drawn from telemetry in requestAnimationFrame; it never re-renders.
import { useRef } from 'react';
import { telemetry, useLiveFrame } from '../../state/live';
import styles from './LevelMeter.module.css';

const FLOOR_DB = -60;
const CEIL_DB = 3;
const RELEASE_DB_PER_S = 26;
const PEAK_HOLD_S = 1.2;
const PEAK_FALL_DB_PER_S = 14;

function meterPosition(db: number): number {
  return Math.min(1, Math.max(0, (db - FLOOR_DB) / (CEIL_DB - FLOOR_DB)));
}

export function LevelMeter({ which, label }: { which: 'input' | 'output'; label: string }) {
  const els = useRef<{ covers: (HTMLDivElement | null)[]; peaks: (HTMLDivElement | null)[] }>({
    covers: [],
    peaks: [],
  });
  const clipRef = useRef<HTMLButtonElement>(null);
  const state = useRef({ level: [-90, -90], peak: [-90, -90], age: [0, 0], clip: false });

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
      clipRef.current?.setAttribute('data-on', '');
    }
  });

  const resetClip = () => {
    state.current.clip = false;
    clipRef.current?.removeAttribute('data-on');
  };

  return (
    <div className={styles.meter} role="img" aria-label={`${label} level`}>
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
      <button
        ref={clipRef}
        type="button"
        className={styles.clip}
        onClick={resetClip}
        aria-label={`${label} clip indicator, click to reset`}
        title="Clip. Click to reset."
      />
    </div>
  );
}
