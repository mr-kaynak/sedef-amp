// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../controls/Knob';
import { Pedal, type PedalFinish } from './Pedal';

// The deepest water: navy, lifted enough to stand off the page's lacquer.
const FINISH: PedalFinish = {
  paint: ['#2a3a5e', '#18233d'],
  legend: '#e6ebf6',
  legendDim: 'rgb(230 235 246 / 0.82)',
};

/** Trench, the analog-style octave divider: one note at a time, one and two octaves down. */
export function TrenchPedal() {
  return (
    <Pedal
      name="Trench"
      kind="Octave down"
      enableParam="trenchEnabled"
      finish={FINISH}
      hint="One note at a time"
    >
      <Knob param="trenchSub1" label="Sub −1" />
      <Knob param="trenchSub2" label="Sub −2" />
      <Knob param="trenchDirect" label="Direct" />
      <Knob param="trenchTone" label="Tone" />
    </Pedal>
  );
}
