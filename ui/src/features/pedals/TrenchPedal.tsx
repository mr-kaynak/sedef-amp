// SPDX-License-Identifier: AGPL-3.0-or-later
import { useParamValue } from '../../params/useParam';
import { Pedal, PedalRow, type PedalFinish } from './Pedal';
import { PedalKnob } from './PedalKnob';
import { PedalSwitch } from './PedalSwitch';

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
      id="trench"
      name="Trench"
      kind="Octave down"
      enableParam="trenchEnabled"
      finish={FINISH}
      status={<PedalSwitch param="trenchMode" />}
      hint={HINTS[poly ? 1 : 0]}
      lamp="beside"
    >
      <PedalRow>
        <PedalKnob param="trenchSub1" label="Sub −1" />
        <PedalKnob param="trenchSub2" label="Sub −2" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="trenchDirect" label="Direct" />
        <PedalKnob param="trenchTone" label="Tone" />
      </PedalRow>
    </Pedal>
  );
}
