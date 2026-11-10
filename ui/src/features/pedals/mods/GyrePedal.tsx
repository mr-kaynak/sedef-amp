// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import { Pedal, PedalRow } from '../Pedal';
import { PedalKnob } from '../PedalKnob';
import { PedalSwitch } from '../PedalSwitch';
import { MODS_FINISH } from './finishes';
import { lampBrightness, triangle } from './lamps';
import { LfoLed } from './LfoLed';

/** Gyre: a four- or six-stage phaser. The lamp follows the sweep. */
export function GyrePedal() {
  const phase = useRef(0);
  return (
    <Pedal
      id="gyre"
      name="Gyre"
      kind="Phaser"
      enableParam="gyreEnabled"
      finish={MODS_FINISH.gyre}
      status={<PedalSwitch param="gyreStages" labels={['4-stage', '6-stage']} />}
      lamp="beside"
      led={
        <LfoLed
          enableParam="gyreEnabled"
          speedParam="gyreSpeed"
          brightness={(dt, speed) => {
            phase.current = (phase.current + speed * dt) % 1;
            return lampBrightness(triangle(phase.current));
          }}
        />
      }
    >
      <PedalRow>
        <PedalKnob param="gyreSpeed" label="Speed" />
        <PedalKnob param="gyreDepth" label="Depth" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="gyreFeedback" label="Feedback" />
      </PedalRow>
    </Pedal>
  );
}
