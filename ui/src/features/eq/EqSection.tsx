// SPDX-License-Identifier: AGPL-3.0-or-later
import { useMemo } from 'react';
import { Knob } from '../../controls/Knob';
import { Toggle } from '../../controls/Toggle';
import { VSlider } from '../../controls/VSlider';
import buttons from '../../controls/Button.module.css';
import { setParam, useParamValue } from '../../params/useParam';
import { beginGesture, endGesture } from '../../params/sender';
import { useShallow } from 'zustand/react/shallow';
import { useParamStore } from '../../params/paramStore';
import { AMP_NAMES } from '../../params/schema';
import { NACRE_STOPS } from '../../styles/nacre';
import { EQ_BAND_HZ, logFrequencies, responseCurve, type EqSettings } from './response';
import styles from './EqSection.module.css';

const GRAPH_W = 1152;
const GRAPH_H = 250;
const DB_RANGE = 15;
const F_LO = 20;
const F_HI = 20000;
const FREQS = logFrequencies(280, F_LO, F_HI);
const GRID_HZ = [50, 100, 200, 500, 1000, 2000, 5000, 10000];
const BAND_LABELS = ['65', '125', '250', '500', '1k', '2k', '4k', '8k', '16k'];

const xForHz = (hz: number) => (Math.log(hz / F_LO) / Math.log(F_HI / F_LO)) * GRAPH_W;
const yForDb = (db: number) => GRAPH_H / 2 - (db / DB_RANGE) * (GRAPH_H / 2 - 8);

function bankIds(prefix: string): string[] {
  return [
    ...EQ_BAND_HZ.map((_, i) => `${prefix}Band${i + 1}`),
    `${prefix}LowCut`,
    `${prefix}HighCut`,
    `${prefix}Enabled`,
  ];
}

/** Subscribes to this bank's twelve values only. */
function useBank(prefix: string): { settings: EqSettings; enabled: boolean } {
  const ids = useMemo(() => bankIds(prefix), [prefix]);
  const values = useParamStore(useShallow((s) => ids.map((id) => s.values[id] ?? 0)));
  return useMemo(
    () => ({
      settings: {
        bandsDb: values.slice(0, 9),
        lowCutHz: values[9] ?? 20,
        highCutHz: values[10] ?? 20000,
      },
      enabled: (values[11] ?? 1) >= 0.5,
    }),
    [values],
  );
}

function ResponseGraph({ settings, enabled }: { settings: EqSettings; enabled: boolean }) {
  const path = useMemo(() => {
    const db = responseCurve(settings, FREQS);
    return FREQS.map((hz, i) => {
      const y = yForDb(Math.max(-DB_RANGE - 4, Math.min(DB_RANGE + 4, db[i] ?? 0)));
      return `${i ? 'L' : 'M'}${xForHz(hz).toFixed(1)} ${y.toFixed(1)}`;
    }).join(' ');
  }, [settings]);
  const zero = yForDb(0);

  return (
    <svg
      className={styles.graph}
      viewBox={`0 0 ${GRAPH_W} ${GRAPH_H}`}
      width={GRAPH_W}
      height={GRAPH_H}
      role="img"
      aria-label="EQ frequency response"
      data-enabled={enabled || undefined}
    >
      <defs>
        <linearGradient id="eq-stroke" x1="0" x2="1" y1="0" y2="0">
          {NACRE_STOPS.map(([offset, color]) => (
            <stop key={offset} offset={offset} style={{ stopColor: color }} />
          ))}
        </linearGradient>
        <linearGradient id="eq-fill" x1="0" x2="0" y1="0" y2="1">
          <stop offset="0" style={{ stopColor: 'var(--nacre-aqua)' }} stopOpacity="0.12" />
          <stop offset="0.5" style={{ stopColor: 'var(--nacre-aqua)' }} stopOpacity="0.01" />
          <stop offset="1" style={{ stopColor: 'var(--nacre-aqua)' }} stopOpacity="0.07" />
        </linearGradient>
      </defs>
      <g className={styles.grid}>
        {GRID_HZ.map((hz) => (
          <line key={hz} x1={xForHz(hz)} x2={xForHz(hz)} y1={0} y2={GRAPH_H} />
        ))}
        {[-12, -6, 6, 12].map((db) => (
          <line key={db} x1={0} x2={GRAPH_W} y1={yForDb(db)} y2={yForDb(db)} />
        ))}
        <line className={styles.zero} x1={0} x2={GRAPH_W} y1={zero} y2={zero} />
      </g>
      <g className={styles.axis}>
        {GRID_HZ.map((hz) => (
          <text key={hz} x={xForHz(hz) + 4} y={GRAPH_H - 6}>
            {hz >= 1000 ? `${hz / 1000}k` : hz}
          </text>
        ))}
        {[-12, -6, 6, 12].map((db) => (
          <text key={db} x={6} y={yForDb(db) - 4}>
            {db > 0 ? `+${db}` : `−${-db}`}
          </text>
        ))}
      </g>
      <path
        className={styles.area}
        d={`${path} L${GRAPH_W} ${zero} L0 ${zero} Z`}
        fill="url(#eq-fill)"
      />
      <path className={styles.curve} d={path} stroke="url(#eq-stroke)" />
      {EQ_BAND_HZ.map((hz, i) => (
        <circle
          key={hz}
          className={styles.node}
          cx={xForHz(hz)}
          cy={yForDb(settings.bandsDb[i] ?? 0)}
          r={3.5}
        />
      ))}
    </svg>
  );
}

function flatten(prefix: string) {
  EQ_BAND_HZ.forEach((_, i) => {
    const id = `${prefix}Band${i + 1}`;
    beginGesture(id);
    setParam(id, 0);
    endGesture(id);
  });
}

export function EqSection() {
  const amp = AMP_NAMES[Math.round(useParamValue('ampModel'))] ?? 'Reef';
  const prefix = `eq${amp}`;
  const { settings, enabled } = useBank(prefix);

  return (
    <div className={styles.section}>
      <div className={styles.top}>
        <div className={styles.title}>
          <h2>{amp} EQ</h2>
          <p>Each amp keeps its own EQ. This one follows the amp you play.</p>
        </div>
        <Toggle param={`${prefix}Enabled`} label="EQ on" />
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.small}`}
          onClick={() => flatten(prefix)}
        >
          Flatten bands
        </button>
        <div className={styles.cuts}>
          <Knob param={`${prefix}LowCut`} label="Low cut" size="sm" />
          <Knob param={`${prefix}HighCut`} label="High cut" size="sm" />
        </div>
      </div>
      <div className={styles.panel}>
        <ResponseGraph settings={settings} enabled={enabled} />
        <div className={styles.sliders}>
          {EQ_BAND_HZ.map((hz, i) => (
            <div key={hz} className={styles.band} style={{ left: xForHz(hz) }}>
              <VSlider param={`${prefix}Band${i + 1}`} label={BAND_LABELS[i]} height={196} />
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}
