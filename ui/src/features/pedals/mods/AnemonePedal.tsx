// SPDX-License-Identifier: AGPL-3.0-or-later
import { Pedal, PedalRow, PedalStack } from '../Pedal';
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
    >
      <PedalRow>
        <PedalKnob param="anemoneSensitivity" label="Sensitivity" />
        <PedalKnob param="anemoneResonance" label="Resonance" />
      </PedalRow>
      <PedalRow set="tight">
        <PedalKnob param="anemoneRange" label="Range" />
        <PedalStack>
          <PedalSwitch param="anemoneMode" labels={['Low', 'Band', 'High']} />
          <PedalSwitch param="anemoneDirection" />
        </PedalStack>
      </PedalRow>
    </Pedal>
  );
}
