// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../../controls/Knob';
import { Segmented } from '../../../controls/Segmented';
import { Pedal } from '../Pedal';
import { MODS_FINISH } from './finishes';
import styles from './ModsPedals.module.css';

/** Anemone: an envelope filter. The cutoff opens (or closes) as you pick. */
export function AnemonePedal() {
  return (
    <Pedal
      name="Anemone"
      kind="Envelope filter"
      enableParam="anemoneEnabled"
      finish={MODS_FINISH.anemone}
      status={
        <div className={styles.switches}>
          <Segmented
            param="anemoneMode"
            size="sm"
            labels={['Low', 'Band', 'High']}
            className={styles.opaque}
          />
          <Segmented param="anemoneDirection" size="sm" className={styles.opaque} />
        </div>
      }
    >
      <Knob param="anemoneSensitivity" label="Sensitivity" />
      <Knob param="anemoneRange" label="Range" />
      <Knob param="anemoneResonance" label="Resonance" className={styles.span2} />
    </Pedal>
  );
}
