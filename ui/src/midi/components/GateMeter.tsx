// SPDX-License-Identifier: AGPL-3.0-or-later
// The input's level on the same scale as the gate, with the gate on it. In Auto the gate follows
// the input's own noise floor and the line shows where it is; in Manual it is a handle: drag it
// just above the noise and under the softest note.
import { useRef } from 'react';
import type { CSSProperties } from 'react';
import { Segmented } from '../../controls/Segmented';
import { useValueControl } from '../../controls/useValueControl';
import { formatValue } from '../../params/format';
import { useParam } from '../../params/useParam';
import { useLiveFrame, useLiveSelector } from '../../state/live';
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
  const auto = Math.round(useParam('gateMode').value) === 0;
  // The gate in effect, whole dB: the automatic one moves with the noise floor.
  const autoDb = useLiveSelector(midiTelemetry, (t) => (t ? Math.round(t.gateDb) : null));
  const gateDb = auto ? (autoDb ?? FLOOR_DB) : gate.value;
  const span = gate.spec.max - gate.spec.min;
  const { active, handlers, wheelRef } = useValueControl(gate, {
    pixelsForRange: (height * span) / (CEIL_DB - FLOOR_DB),
    layoutPixels: true,
    fineFactor: 8,
    disabled: auto,
  });

  const fill = useRef<HTMLDivElement>(null);
  const level = useRef(FLOOR_DB);
  const open = useRef<HTMLDivElement>(null);
  useLiveFrame(midiTelemetry, (t, dt) => {
    const target = t?.levelDb ?? FLOOR_DB;
    level.current = Math.max(target, level.current - RELEASE_DB_PER_S * dt);
    if (fill.current) fill.current.style.transform = `scaleY(${meterPosition(level.current)})`;
    if (open.current) open.current.toggleAttribute('data-on', !!t && t.levelDb >= t.gateDb);
  });

  const style = {
    '--h': `${height}px`,
    '--gate': meterPosition(gateDb),
  } as CSSProperties;
  const shown = auto ? (autoDb === null ? 'Auto' : formatValue(gate.spec, autoDb)) : gate.display;

  return (
    <div className={styles.wrap} style={style}>
      <div className={styles.head}>
        <span className={styles.legend}>Input</span>
        <div
          ref={open}
          className={styles.open}
          title="Over the gate: a pluck or pick can start a note"
        >
          <span className={styles.dot} aria-hidden="true" />
          <span>Signal</span>
        </div>
      </div>
      <div
        ref={wheelRef}
        className={styles.track}
        role="slider"
        aria-orientation="vertical"
        tabIndex={auto ? -1 : 0}
        aria-label="Gate"
        aria-disabled={auto || undefined}
        aria-valuemin={gate.spec.min}
        aria-valuemax={gate.spec.max}
        aria-valuenow={Number(Math.min(gate.spec.max, Math.max(gate.spec.min, gateDb)).toFixed(1))}
        aria-valuetext={auto ? `Automatic, ${shown}` : gate.display}
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
        {shown}
      </div>
      <Segmented param="gateMode" size="sm" ariaLabel="Gate mode" />
    </div>
  );
}
