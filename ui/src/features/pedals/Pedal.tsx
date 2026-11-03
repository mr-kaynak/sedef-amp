// SPDX-License-Identifier: AGPL-3.0-or-later
import type { CSSProperties, ReactNode } from 'react';
import { Footswitch } from '../../controls/Footswitch';
import { Led } from '../../controls/Led';
import { useParamValue } from '../../params/useParam';
import styles from './Pedal.module.css';

export interface PedalFinish {
  /** Enclosure paint, top and bottom of the gradient. */
  paint: [string, string];
  legend: string;
  legendDim: string;
  /** Optional layer between the gloss and the paint (Pearl's nacre sheen). */
  sheen?: string;
}

export interface PedalProps {
  name: string;
  kind: string;
  enableParam: string;
  finish: PedalFinish;
  /** Knob area. */
  children: ReactNode;
  /** Optional status element between the name and the footswitch (meters, rate lamps). */
  status?: ReactNode;
  led?: ReactNode;
  /** A narrower enclosure for a pedal with one column of controls. */
  variant?: 'narrow';
}

/** A stompbox enclosure: knobs on top, the name, an LED, and the footswitch. */
export function Pedal({
  name,
  kind,
  enableParam,
  finish,
  children,
  status,
  led,
  variant,
}: PedalProps) {
  const on = useParamValue(enableParam) >= 0.5;
  const style = {
    '--paint-top': finish.paint[0],
    '--paint-bottom': finish.paint[1],
    '--legend': finish.legend,
    '--legend-dim': finish.legendDim,
    '--plate': finish.paint[0],
    ...(finish.sheen ? { '--sheen': finish.sheen } : {}),
  } as CSSProperties;

  return (
    <section
      className={[styles.pedal, variant && styles[variant]].filter(Boolean).join(' ')}
      style={style}
      aria-label={`${name} ${kind}`}
      data-on={on || undefined}
    >
      <span className={`${styles.screw} ${styles.tl}`} aria-hidden="true" />
      <span className={`${styles.screw} ${styles.tr}`} aria-hidden="true" />
      <span className={`${styles.screw} ${styles.bl}`} aria-hidden="true" />
      <span className={`${styles.screw} ${styles.br}`} aria-hidden="true" />
      <div className={styles.controls}>{children}</div>
      <div className={styles.identity}>
        <h2 className={styles.name}>{name}</h2>
        <p className={styles.kind}>{kind}</p>
      </div>
      <div className={styles.status}>{status}</div>
      <div className={styles.lamp}>{led ?? <Led on={on} size={10} />}</div>
      <Footswitch param={enableParam} label={`${name} on/off`} />
    </section>
  );
}
