// SPDX-License-Identifier: AGPL-3.0-or-later
import type { CSSProperties } from 'react';
import { ChoiceLever } from '../../controls/ChoiceLever';
import { Knob } from '../../controls/Knob';
import { Toggle } from '../../controls/Toggle';
import type { AmpDefinition } from './amps';
import styles from './AmpHead.module.css';

/** Front of an amp head: grille with the badge on top, control plate below. */
export function AmpHead({ amp }: { amp: AmpDefinition }) {
  const f = amp.finish;
  const style = {
    '--plate-top': f.plate[0],
    '--plate-bottom': f.plate[1],
    '--plate': f.plate[0],
    '--legend': f.legend,
    '--legend-dim': f.legendDim,
    '--grille': f.grille,
    '--thread': f.grilleThread,
    '--piping': f.piping,
  } as CSSProperties;

  return (
    <section
      className={styles.head}
      style={style}
      aria-label={`${amp.name} amp controls`}
      data-amp={amp.name}
    >
      <div className={styles.grille}>
        <div
          className={styles.badge}
          data-nacre={f.badge === 'nacre' || undefined}
          style={f.badge === 'nacre' ? undefined : { color: f.badge }}
        >
          {amp.name}
        </div>
      </div>
      <div className={styles.plate}>
        <div className={styles.controls}>
          {amp.controls.map((c) => {
            switch (c.type) {
              case 'knob':
                return <Knob key={c.param} param={c.param} label={c.label} size="lg" />;
              case 'lever':
                return (
                  <div key={c.param} className={styles.switch}>
                    <Toggle param={c.param} label={c.label} variant="lever" />
                  </div>
                );
              case 'voice':
                return (
                  <div key={c.param} className={styles.switch}>
                    <ChoiceLever param={c.param} label={c.label} />
                  </div>
                );
            }
          })}
        </div>
        <div className={styles.pilot} aria-hidden="true">
          <span className={styles.jewel} />
          <span className={styles.power}>Power</span>
        </div>
      </div>
    </section>
  );
}
