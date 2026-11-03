// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../controls/Knob';
import styles from './KelpPedal.module.css';
import { Pedal, type PedalFinish } from './Pedal';

// Kelp's olive brown-green (4.8:1 for the dim legend at the lighter top).
const FINISH: PedalFinish = {
  paint: ['#56653a', '#3a4626'],
  legend: '#f1f0df',
  legendDim: 'rgb(241 240 223 / 0.9)',
};

/** Kelp, the mid-hump overdrive: a warm drive, or with Drive down and Level up, a tight boost. */
export function KelpPedal() {
  return (
    <Pedal name="Kelp" kind="Mid overdrive" enableParam="kelpEnabled" finish={FINISH}>
      <Knob param="kelpDrive" label="Drive" />
      <Knob param="kelpTone" label="Tone" />
      <Knob param="kelpLevel" label="Level" className={styles.level} />
    </Pedal>
  );
}
