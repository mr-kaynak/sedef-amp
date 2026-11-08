// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../../controls/Knob';
import { Pedal } from '../Pedal';
import { PEARL } from './finishes';

export function PearlPanel() {
  return (
    <Pedal name="Pearl" kind="Overdrive" enableParam="pearlEnabled" finish={PEARL}>
      <Knob param="pearlGain" label="Gain" />
      <Knob param="pearlTone" label="Tone" />
      <Knob param="pearlLevel" label="Level" />
      <Knob param="pearlMid" label="Mid boost" />
    </Pedal>
  );
}
