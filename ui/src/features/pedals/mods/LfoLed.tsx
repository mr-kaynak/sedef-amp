// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import { Led } from '../../../controls/Led';
import { useParamValue } from '../../../params/useParam';
import { telemetry, useLiveFrame } from '../../../state/live';
import { useCopyParam } from '../copy';
import { usePedalScale } from '../density';
import styles from './ModsPedals.module.css';

export interface LfoLedProps {
  enableParam: string;
  speedParam: string;
  /** Called every frame while the pedal is on: advance by dt seconds, return brightness 0..1. */
  brightness: (dt: number, speedHz: number) => number;
}

/**
 * A pedal lamp that follows the pedal's own sweep. Like Ripple's rate lamp, the brightness is
 * written every frame from the Speed knob (so turning it never makes the lamp jump), and CSS
 * applies it only where motion is welcome: with reduced motion the lamp is simply lit.
 */
export function LfoLed({ enableParam, speedParam, brightness }: LfoLedProps) {
  const ref = useRef<HTMLSpanElement>(null);
  const large = usePedalScale() >= 1.25;
  const copyParam = useCopyParam();
  const on = useParamValue(copyParam(enableParam)) >= 0.5;
  const speed = useParamValue(copyParam(speedParam));
  useLiveFrame(telemetry, (_t, dt) => {
    const el = ref.current;
    if (!el || !on) return;
    el.style.setProperty('--sweep', brightness(dt, speed).toFixed(3));
  });
  return (
    <span ref={ref} className={styles.sweepLed} data-on={on || undefined}>
      <Led on={on} size={large ? 10 : 8} />
    </span>
  );
}
