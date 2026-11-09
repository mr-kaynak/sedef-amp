// SPDX-License-Identifier: AGPL-3.0-or-later
import { Pedal, PedalRow } from '../Pedal';
import { PedalKnob } from '../PedalKnob';
import { PEARL } from './finishes';

export function PearlPanel() {
  return (
    <Pedal id="pearl" name="Pearl" kind="Overdrive" enableParam="pearlEnabled" finish={PEARL}>
      <PedalRow>
        <PedalKnob param="pearlGain" label="Gain" />
        <PedalKnob param="pearlTone" label="Tone" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="pearlLevel" label="Level" />
        <PedalKnob param="pearlMid" label="Mid boost" />
      </PedalRow>
    </Pedal>
  );
}
