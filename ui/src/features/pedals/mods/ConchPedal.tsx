// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../../controls/Knob';
import { Segmented } from '../../../controls/Segmented';
import { Pedal } from '../Pedal';
import { MODS_FINISH } from './finishes';
import styles from './ModsPedals.module.css';

/** Conch: an inductor-style wah. Position is the rocker (heel to toe); Auto rocks it for you. */
export function ConchPedal() {
  return (
    <Pedal
      name="Conch"
      kind="Wah"
      enableParam="conchEnabled"
      finish={MODS_FINISH.conch}
      status={
        <div className={styles.switches}>
          <Segmented param="conchMode" size="sm" className={styles.opaque} />
        </div>
      }
    >
      <Knob param="conchPosition" label="Position" />
      <Knob param="conchResonance" label="Resonance" />
      <Knob param="conchSensitivity" label="Auto sensitivity" className={styles.span2} />
    </Pedal>
  );
}
