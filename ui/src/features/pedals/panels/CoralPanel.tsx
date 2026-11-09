// SPDX-License-Identifier: AGPL-3.0-or-later
import { Pedal, PedalRow } from '../Pedal';
import { PedalKnob } from '../PedalKnob';
import { CORAL } from './finishes';

export function CoralPanel() {
  return (
    <Pedal id="coral" name="Coral" kind="Distortion" enableParam="coralEnabled" finish={CORAL}>
      <PedalRow>
        <PedalKnob param="coralGain" label="Gain" />
        <PedalKnob param="coralCut" label="Cut" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="coralLevel" label="Level" />
      </PedalRow>
    </Pedal>
  );
}
