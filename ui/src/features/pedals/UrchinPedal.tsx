// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../controls/Knob';
import { Pedal, type PedalFinish } from './Pedal';

// A sea urchin's plum.
const FINISH: PedalFinish = {
  paint: ['#6e3460', '#4b2041'],
  legend: '#f8e8f1',
  legendDim: 'rgb(248 232 241 / 0.88)',
};

/** Urchin, the octave-up fuzz: the octave speaks on single notes high on the neck. */
export function UrchinPedal() {
  return (
    <Pedal
      name="Urchin"
      kind="Octave-up fuzz"
      enableParam="urchinEnabled"
      finish={FINISH}
      hint="Neck pickup, above the 12th fret"
    >
      <Knob param="urchinFuzz" label="Fuzz" />
      <Knob param="urchinOctave" label="Octave" />
      <Knob param="urchinTone" label="Tone" />
      <Knob param="urchinLevel" label="Level" />
    </Pedal>
  );
}
