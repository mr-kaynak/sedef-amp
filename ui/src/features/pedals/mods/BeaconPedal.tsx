// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import { useParamValue } from '../../../params/useParam';
import { Pedal, PedalRow } from '../Pedal';
import { PedalKnob } from '../PedalKnob';
import { PedalSwitch } from '../PedalSwitch';
import { MODS_FINISH } from './finishes';
import { lampBrightness, VibeLamp } from './lamps';
import { LfoLed } from './LfoLed';

/**
 * Beacon: a photocell vibe. Its lamp shows the vibe's own lamp: a quick brightening and a slow
 * fade, not a steady blink.
 */
export function BeaconPedal() {
  const lamp = useRef(new VibeLamp());
  const depth = useParamValue('beaconDepth');
  return (
    <Pedal
      id="beacon"
      name="Beacon"
      kind="Photocell vibe"
      enableParam="beaconEnabled"
      finish={MODS_FINISH.beacon}
      status={<PedalSwitch param="beaconMode" />}
      led={
        <LfoLed
          enableParam="beaconEnabled"
          speedParam="beaconSpeed"
          brightness={(dt, speed) => lampBrightness(lamp.current.advance(dt, speed, depth))}
        />
      }
    >
      <PedalRow set="tight">
        <PedalKnob param="beaconSpeed" label="Speed" weight="big" />
        <PedalKnob param="beaconDepth" label="Depth" weight="big" />
      </PedalRow>
    </Pedal>
  );
}
