// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../controls/Knob';
import { Segmented } from '../../controls/Segmented';
import { useParamValue } from '../../params/useParam';
import { Pedal, type PedalFinish } from './Pedal';
import styles from './TrenchPedal.module.css';

// The deepest water: navy, lifted enough to stand off the page's lacquer.
const FINISH: PedalFinish = {
  paint: ['#2a3a5e', '#18233d'],
  legend: '#e6ebf6',
  legendDim: 'rgb(230 235 246 / 0.82)',
};

// What each mode can play, engraved under the switch and read with the footswitch.
const HINTS = ['One note at a time', 'Chords and single notes'] as const;

/**
 * Trench, the octave divider: one and two octaves down. Mono is the analog-style divider for
 * single notes; Poly divides every string of a chord.
 */
export function TrenchPedal() {
  const poly = Math.round(useParamValue('trenchMode')) === 1;
  return (
    <Pedal
      name="Trench"
      kind="Octave down"
      enableParam="trenchEnabled"
      finish={FINISH}
      status={<Segmented param="trenchMode" size="sm" className={styles.well} />}
      hint={HINTS[poly ? 1 : 0]}
    >
      <Knob param="trenchSub1" label="Sub −1" />
      <Knob param="trenchSub2" label="Sub −2" />
      <Knob param="trenchDirect" label="Direct" />
      <Knob param="trenchTone" label="Tone" />
    </Pedal>
  );
}
