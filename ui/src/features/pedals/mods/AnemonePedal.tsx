// SPDX-License-Identifier: AGPL-3.0-or-later
import { Pedal, PedalRow } from '../Pedal';
import { PedalKnob } from '../PedalKnob';
import { PedalSwitch } from '../PedalSwitch';
import { MODS_FINISH } from './finishes';

/** Anemone: an envelope filter. The cutoff opens (or closes) as you pick. */
export function AnemonePedal() {
  return (
    <Pedal
      id="anemone"
      name="Anemone"
      kind="Envelope filter"
      enableParam="anemoneEnabled"
      finish={MODS_FINISH.anemone}
      status={
        <>
          <PedalSwitch param="anemoneMode" labels={['Low', 'Band', 'High']} />
          <PedalSwitch param="anemoneDirection" />
        </>
      }
    >
      <PedalRow set="arc">
        <PedalKnob param="anemoneSensitivity" label="Sensitivity" />
        <PedalKnob param="anemoneRange" label="Range" />
        <PedalKnob param="anemoneResonance" label="Resonance" />
      </PedalRow>
    </Pedal>
  );
}
