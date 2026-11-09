// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import { Pedal, PedalRow } from '../Pedal';
import { PedalKnob } from '../PedalKnob';
import { MODS_FINISH } from './finishes';
import { lampBrightness, triangle } from './lamps';
import { LfoLed } from './LfoLed';

/** Jetty: a bucket-brigade-style flanger. The lamp follows the sweep. */
export function JettyPedal() {
  const phase = useRef(0);
  return (
    <Pedal
      id="jetty"
      name="Jetty"
      kind="Flanger"
      enableParam="jettyEnabled"
      finish={MODS_FINISH.jetty}
      nameAt="top"
      led={
        <LfoLed
          enableParam="jettyEnabled"
          speedParam="jettySpeed"
          brightness={(dt, speed) => {
            phase.current = (phase.current + speed * dt) % 1;
            return lampBrightness(triangle(phase.current));
          }}
        />
      }
    >
      <PedalRow>
        <PedalKnob param="jettySpeed" label="Speed" />
        <PedalKnob param="jettyDepth" label="Depth" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="jettyDelay" label="Delay" />
        <PedalKnob param="jettyFeedback" label="Feedback" />
      </PedalRow>
    </Pedal>
  );
}
