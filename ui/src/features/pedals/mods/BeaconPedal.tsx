// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import { Knob } from '../../../controls/Knob';
import { Segmented } from '../../../controls/Segmented';
import { useParamValue } from '../../../params/useParam';
import { Pedal } from '../Pedal';
import { MODS_FINISH } from './finishes';
import { lampBrightness, VibeLamp } from './lamps';
import { LfoLed } from './LfoLed';
import styles from './ModsPedals.module.css';

/**
 * Beacon: a photocell vibe. Its lamp shows the vibe's own lamp: a quick brightening and a slow
 * fade, not a steady blink.
 */
export function BeaconPedal() {
  const lamp = useRef(new VibeLamp());
  const depth = useParamValue('beaconDepth');
  return (
    <Pedal
      name="Beacon"
      kind="Photocell vibe"
      enableParam="beaconEnabled"
      finish={MODS_FINISH.beacon}
      led={
        <LfoLed
          enableParam="beaconEnabled"
          speedParam="beaconSpeed"
          brightness={(dt, speed) => lampBrightness(lamp.current.advance(dt, speed, depth))}
        />
      }
    >
      <Segmented param="beaconMode" size="sm" className={styles.span2} />
      <Knob param="beaconSpeed" label="Speed" />
      <Knob param="beaconDepth" label="Depth" />
    </Pedal>
  );
}
