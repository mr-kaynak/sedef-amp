// SPDX-License-Identifier: AGPL-3.0-or-later
import { Pedal, PedalRow, type PedalFinish } from './Pedal';
import { PedalKnob } from './PedalKnob';

// Kelp's olive brown-green (4.8:1 for the dim legend at the lighter top).
const FINISH: PedalFinish = {
  paint: ['#56653a', '#3a4626'],
  legend: '#f1f0df',
  legendDim: 'rgb(241 240 223 / 0.9)',
};

/** Kelp, the mid-hump overdrive: a warm drive, or with Drive down and Level up, a tight boost. */
export function KelpPedal() {
  return (
    <Pedal id="kelp" name="Kelp" kind="Mid overdrive" enableParam="kelpEnabled" finish={FINISH}>
      <PedalRow>
        <PedalKnob param="kelpDrive" label="Drive" weight="big" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="kelpTone" label="Tone" />
        <PedalKnob param="kelpLevel" label="Level" />
      </PedalRow>
    </Pedal>
  );
}
