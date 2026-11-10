// SPDX-License-Identifier: AGPL-3.0-or-later
import { Pedal, PedalRow, type PedalFinish } from './Pedal';
import { PedalKnob } from './PedalKnob';

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
      id="urchin"
      name="Urchin"
      kind="Octave-up fuzz"
      enableParam="urchinEnabled"
      finish={FINISH}
      hint="Neck pickup, above the 12th fret"
      lamp="beside"
    >
      <PedalRow>
        <PedalKnob param="urchinFuzz" label="Fuzz" />
        <PedalKnob param="urchinOctave" label="Octave" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="urchinTone" label="Tone" />
        <PedalKnob param="urchinLevel" label="Level" />
      </PedalRow>
    </Pedal>
  );
}
