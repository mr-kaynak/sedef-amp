// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import { Led } from '../../../controls/Led';
import { useParamValue } from '../../../params/useParam';
import { telemetry, useLiveFrame } from '../../../state/live';
import { usePedalScale } from '../density';
import { Pedal, PedalRow } from '../Pedal';
import { PedalKnob } from '../PedalKnob';
import { PedalSwitch } from '../PedalSwitch';
import { RIPPLE } from './finishes';
import styles from './Panels.module.css';

/**
 * Ripple's rate lamp blinks at the modulation speed. The phase advances every frame by speed × dt,
 * so turning Speed changes the rate smoothly: retiming a running CSS animation re-maps its elapsed
 * time and makes the lamp jump. CSS applies the brightness only where motion is welcome.
 */
function RateLed() {
  const ref = useRef<HTMLSpanElement>(null);
  const phase = useRef(0);
  const large = usePedalScale() >= 1.25;
  const on = useParamValue('rippleEnabled') >= 0.5;
  const speed = useParamValue('rippleSpeed');
  useLiveFrame(telemetry, (_t, dt) => {
    const el = ref.current;
    if (!el || !on) return;
    phase.current = (phase.current + speed * dt) % 1;
    // Full brightness at the start of each cycle, 0.35 halfway, as the lamp's old keyframes.
    const brightness = 0.675 + 0.325 * Math.cos(2 * Math.PI * phase.current);
    el.style.setProperty('--blink', brightness.toFixed(3));
  });
  return (
    <span ref={ref} className={styles.rate} data-on={on || undefined}>
      <Led on={on} size={large ? 10 : 8} />
    </span>
  );
}

export function RipplePanel() {
  return (
    <Pedal
      id="ripple"
      name="Ripple"
      kind="Modulation"
      enableParam="rippleEnabled"
      finish={RIPPLE}
      status={<PedalSwitch param="rippleMode" labels={['Trem', 'Chorus', 'Vibe']} />}
      led={<RateLed />}
    >
      <PedalRow set="tight">
        <PedalKnob param="rippleSpeed" label="Speed" weight="big" />
        <PedalKnob param="rippleDepth" label="Depth" />
      </PedalRow>
    </Pedal>
  );
}
