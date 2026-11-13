// SPDX-License-Identifier: AGPL-3.0-or-later
import { useId } from 'react';
import type { CSSProperties, ReactNode } from 'react';
import { Footswitch } from '../../controls/Footswitch';
import { Led } from '../../controls/Led';
import { useParamValue } from '../../params/useParam';
import { PedalArt } from './art';
import { useCopyParam, usePedalCopy } from './copy';
import { usePedalScale } from './density';
import { pedalBox } from './sizes';
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
  /** The pedal's id in the registry: its box (sizes.ts) and its painted figure (art.tsx). */
  id: string;
  name: string;
  kind: string;
  enableParam: string;
  finish: PedalFinish;
  /** The controls, as rows (PedalRow) from the top. */
  children: ReactNode;
  /** Optional status element under the name (meters, switches). */
  status?: ReactNode;
  /** Optional engraved instruction in the status area ("One note at a time"). */
  hint?: string;
  led?: ReactNode;
  /** Where the lamp sits: over the footswitch (default) or beside it, on a full enclosure. */
  lamp?: 'above' | 'beside';
}

/** One row of an enclosure's controls. `tight` keeps two close, standing on one line. */
export function PedalRow({ children, set }: { children: ReactNode; set?: 'tight' }) {
  return <div className={set ? `${styles.row} ${styles[set]}` : styles.row}>{children}</div>;
}

/** Switches stacked in a row, beside a knob. */
export function PedalStack({ children }: { children: ReactNode }) {
  return <div className={styles.stack}>{children}</div>;
}

/**
 * A stompbox enclosure: its painted figure, the controls, the name, an LED and the footswitch.
 * It draws itself at the scale of whatever hosts it (density.ts) and needs nothing else.
 */
export function Pedal({
  id,
  name,
  kind,
  enableParam,
  finish,
  children,
  status,
  hint,
  led,
  lamp = 'above',
}: PedalProps) {
  // A second copy reads its own switch, and carries its number in its name ("Kelp 2").
  const copyParam = useCopyParam();
  const copy = usePedalCopy();
  const shownName = copy && copy.copy > 1 ? `${name} ${copy.copy}` : name;
  const enable = copyParam(enableParam);
  const on = useParamValue(enable) >= 0.5;
  const scale = usePedalScale();
  // Type steps once, where the enclosure is clearly the larger kind.
  const density = scale >= 1.25 ? 'large' : 'small';
  const hintId = useId();
  const box = pedalBox(id);
  const style = {
    '--u': `${scale}px`,
    '--w': box.w,
    '--h': box.h,
    '--paint-top': finish.paint[0],
    '--paint-bottom': finish.paint[1],
    '--legend': finish.legend,
    '--legend-dim': finish.legendDim,
    '--plate': finish.paint[0],
    ...(finish.sheen ? { '--sheen': finish.sheen } : {}),
  } as CSSProperties;

  return (
    <section
      className={styles.pedal}
      style={style}
      aria-label={`${shownName} ${kind}`}
      data-on={on || undefined}
      data-density={density}
      data-lamp={lamp}
    >
      <PedalArt id={id} box={box} />
      <span className={`${styles.screw} ${styles.tl}`} aria-hidden="true" />
      <span className={`${styles.screw} ${styles.tr}`} aria-hidden="true" />
      <span className={`${styles.screw} ${styles.bl}`} aria-hidden="true" />
      <span className={`${styles.screw} ${styles.br}`} aria-hidden="true" />
      <div className={styles.controls}>{children}</div>
      <div className={styles.identity}>
        <h2 className={styles.name}>{shownName}</h2>
        <p className={styles.kind}>{kind}</p>
      </div>
      {(status || hint) && (
        <div className={styles.status}>
          {status}
          {hint && (
            <p id={hintId} className={styles.hint}>
              {hint}
            </p>
          )}
        </div>
      )}
      <div className={styles.foot}>
        <div className={styles.lamp}>
          {led ?? <Led on={on} size={density === 'large' ? 10 : 8} />}
        </div>
        <Footswitch
          param={enable}
          label={`${shownName} on/off`}
          describedBy={hint ? hintId : undefined}
        />
      </div>
    </section>
  );
}
