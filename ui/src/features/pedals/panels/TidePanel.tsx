// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import { useParamValue } from '../../../params/useParam';
import { telemetry, useLiveFrame } from '../../../state/live';
import { Pedal, PedalRow } from '../Pedal';
import { PedalKnob } from '../PedalKnob';
import { TIDE } from './finishes';
import styles from './Panels.module.css';

/** Gain-reduction ladder for the compressor, drawn from telemetry without re-rendering. */
function ReductionMeter() {
  const ref = useRef<HTMLDivElement>(null);
  const on = useParamValue('tideEnabled') >= 0.5;
  useLiveFrame(telemetry, (t) => {
    const el = ref.current;
    if (!el) return;
    const gr = on && t ? Math.min(12, Math.max(0, -t.tideReductionDb)) : 0;
    el.style.setProperty('--gr', String(gr / 12));
  });
  return (
    <div className={styles.gr} aria-hidden="true" title="Gain reduction">
      <div ref={ref} className={styles.grFill} />
    </div>
  );
}

export function TidePanel() {
  return (
    <Pedal
      id="tide"
      name="Tide"
      kind="Compressor"
      enableParam="tideEnabled"
      finish={TIDE}
      status={<ReductionMeter />}
    >
      <PedalRow>
        <PedalKnob param="tideComp" label="Comp" />
        <PedalKnob param="tideTone" label="Tone" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="tideLevel" label="Level" />
        <PedalKnob param="tideBlend" label="Blend" />
      </PedalRow>
    </Pedal>
  );
}
