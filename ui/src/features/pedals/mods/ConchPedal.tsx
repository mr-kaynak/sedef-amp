// SPDX-License-Identifier: AGPL-3.0-or-later
import { Pedal, PedalRow } from '../Pedal';
import { PedalKnob } from '../PedalKnob';
import { PedalSwitch } from '../PedalSwitch';
import { MODS_FINISH } from './finishes';

/** Conch: an inductor-style wah. Position is the rocker (heel to toe); Auto rocks it for you. */
export function ConchPedal() {
  return (
    <Pedal
      id="conch"
      name="Conch"
      kind="Wah"
      enableParam="conchEnabled"
      finish={MODS_FINISH.conch}
      status={<PedalSwitch param="conchMode" />}
      lamp="beside"
    >
      <PedalRow set="tight">
        <PedalKnob param="conchPosition" label="Position" weight="big" />
        <PedalKnob param="conchResonance" label="Resonance" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="conchSensitivity" label="Auto sensitivity" />
      </PedalRow>
    </Pedal>
  );
}
