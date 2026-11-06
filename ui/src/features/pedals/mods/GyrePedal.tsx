// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import { Knob } from '../../../controls/Knob';
import { Segmented } from '../../../controls/Segmented';
import { Pedal } from '../Pedal';
import { MODS_FINISH } from './finishes';
import { lampBrightness, triangle } from './lamps';
import { LfoLed } from './LfoLed';
import styles from './ModsPedals.module.css';

/** Gyre: a four- or six-stage phaser. The lamp follows the sweep. */
export function GyrePedal() {
  const phase = useRef(0);
  return (
    <Pedal
      name="Gyre"
      kind="Phaser"
      enableParam="gyreEnabled"
      finish={MODS_FINISH.gyre}
      status={
        <div className={styles.switches}>
          <Segmented
            param="gyreStages"
            size="sm"
            labels={['4-stage', '6-stage']}
            className={styles.opaque}
          />
        </div>
      }
      led={
        <LfoLed
          enableParam="gyreEnabled"
          speedParam="gyreSpeed"
          brightness={(dt, speed) => {
            phase.current = (phase.current + speed * dt) % 1;
            return lampBrightness(triangle(phase.current));
          }}
        />
      }
    >
      <Knob param="gyreSpeed" label="Speed" />
      <Knob param="gyreDepth" label="Depth" />
      <Knob param="gyreFeedback" label="Feedback" className={styles.span2} />
    </Pedal>
  );
}
