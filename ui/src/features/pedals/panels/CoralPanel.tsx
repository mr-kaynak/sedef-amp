// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../../controls/Knob';
import { Pedal } from '../Pedal';
import { CORAL } from './finishes';
import styles from './Panels.module.css';

export function CoralPanel() {
  return (
    <Pedal name="Coral" kind="Distortion" enableParam="coralEnabled" finish={CORAL}>
      <Knob param="coralGain" label="Gain" />
      <Knob param="coralCut" label="Cut" />
      <Knob param="coralLevel" label="Level" className={styles.span2} />
    </Pedal>
  );
}
