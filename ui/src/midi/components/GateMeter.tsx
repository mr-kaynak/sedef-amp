// SPDX-License-Identifier: AGPL-3.0-or-later
// The input's level on the same scale as the gate, with the gate as a handle on it: drag the
// handle just above the noise and under the softest pick.
import { useRef } from 'react';
import type { CSSProperties } from 'react';
import { useValueControl } from '../../controls/useValueControl';
import { useParam } from '../../params/useParam';
import { useLiveFrame } from '../../state/live';
import { midiTelemetry } from '../store';
import styles from './GateMeter.module.css';

const FLOOR_DB = -90;
const CEIL_DB = 0;
const RELEASE_DB_PER_S = 30;

function meterPosition(db: number): number {
  return Math.min(1, Math.max(0, (db - FLOOR_DB) / (CEIL_DB - FLOOR_DB)));
}

export function GateMeter({ height = 280 }: { height?: number }) {
  const gate = useParam('gate');
  const span = gate.spec.max - gate.spec.min;
  const { active, handlers, wheelRef } = useValueControl(gate, {
    pixelsForRange: (height * span) / (CEIL_DB - FLOOR_DB),
    layoutPixels: true,
    fineFactor: 8,
  });

  const fill = useRef<HTMLDivElement>(null);
  const level = useRef(FLOOR_DB);
  const open = useRef<HTMLDivElement>(null);
  useLiveFrame(midiTelemetry, (t, dt) => {
    const target = t?.levelDb ?? FLOOR_DB;
    level.current = Math.max(target, level.current - RELEASE_DB_PER_S * dt);
    if (fill.current) fill.current.style.transform = `scaleY(${meterPosition(level.current)})`;
    if (open.current) open.current.toggleAttribute('data-on', !!t && t.levelDb >= gate.value);
  });

  const style = {
    '--h': `${height}px`,
    '--gate': meterPosition(gate.value),
  } as CSSProperties;

  return (
    <div className={styles.wrap} style={style}>
      <div className={styles.head}>
        <span className={styles.legend}>Input</span>
        <div ref={open} className={styles.open} title="Over the gate: a pick can start a note">
          <span className={styles.dot} aria-hidden="true" />
          <span>Signal</span>
        </div>
      </div>
      <div
        ref={wheelRef}
        className={styles.track}
        role="slider"
        aria-orientation="vertical"
        tabIndex={0}
        aria-label="Gate"
        aria-valuemin={gate.spec.min}
        aria-valuemax={gate.spec.max}
        aria-valuenow={Number(gate.value.toFixed(1))}
        aria-valuetext={gate.display}
        data-active={active || undefined}
        {...handlers}
      >
        <div className={styles.groove} aria-hidden="true">
          <div ref={fill} className={styles.fill} />
          <div className={styles.below} />
        </div>
        <div className={styles.ticks} aria-hidden="true">
          {[0, -20, -40, -60, -80].map((db) => (
            <span key={db} style={{ bottom: `${meterPosition(db) * 100}%` }}>
              {db === 0 ? '0' : `−${-db}`}
            </span>
          ))}
        </div>
        <div className={styles.handle} aria-hidden="true">
          <span className={styles.handleLabel}>Gate</span>
        </div>
      </div>
      <div className={styles.value} data-active={active || undefined}>
        {gate.display}
      </div>
    </div>
  );
}
