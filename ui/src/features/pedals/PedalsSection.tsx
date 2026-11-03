// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import { Knob } from '../../controls/Knob';
import { Led } from '../../controls/Led';
import { Segmented } from '../../controls/Segmented';
import { Toggle } from '../../controls/Toggle';
import { useParamValue } from '../../params/useParam';
import { telemetry, useLiveFrame, useLiveSelector } from '../../state/live';
import { KelpPedal } from './KelpPedal';
import { Pedal, type PedalFinish } from './Pedal';
import styles from './PedalsSection.module.css';
import { TrenchPedal } from './TrenchPedal';
import { UrchinPedal } from './UrchinPedal';

const FINISH: Record<'tide' | 'pearl' | 'coral' | 'ripple', PedalFinish> = {
  tide: { paint: ['#46677a', '#2c4757'], legend: '#eef0ea', legendDim: 'rgb(238 240 234 / 0.86)' },
  pearl: {
    paint: ['#efeadf', '#d8d1c2'],
    legend: '#13293a',
    legendDim: 'rgb(19 41 58 / 0.75)',
    // Pearl gets the only nacre-sheened enclosure: the pedal is named for it.
    sheen:
      'conic-gradient(from 210deg at 70% 30%, rgb(205 238 238 / 0.5), rgb(221 214 246 / 0.4), rgb(246 219 226 / 0.5), rgb(242 232 203 / 0.45), rgb(205 238 238 / 0.5))',
  },
  // Deep enough for 4.5:1 small text: a mid coral fails with light and dark legends alike.
  coral: { paint: ['#b04e56', '#8b363f'], legend: '#fbefe6', legendDim: 'rgb(251 239 230 / 0.95)' },
  ripple: {
    paint: ['#4a4a82', '#2f2f5c'],
    legend: '#ece9f8',
    legendDim: 'rgb(236 233 248 / 0.75)',
  },
};

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

/**
 * Ripple's rate lamp blinks at the modulation speed. The phase advances every frame by speed × dt,
 * so turning Speed changes the rate smoothly: retiming a running CSS animation re-maps its elapsed
 * time and makes the lamp jump. CSS applies the brightness only where motion is welcome.
 */
function RateLed() {
  const ref = useRef<HTMLSpanElement>(null);
  const phase = useRef(0);
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
      <Led on={on} size={10} />
    </span>
  );
}

function GateLamp() {
  const enabled = useParamValue('gateEnabled') >= 0.5;
  const open = useLiveSelector(telemetry, (t) => t?.gateOpen ?? true);
  return (
    <span className={styles.gateState}>
      <Led on={enabled && open} size={7} />
      {enabled ? (open ? 'Open' : 'Closed') : 'Bypassed'}
    </span>
  );
}

export function PedalsSection() {
  return (
    <div className={styles.section}>
      <div className={styles.rail}>
        <div className={styles.block}>
          <div className={styles.blockHead}>
            <h2 className={styles.blockTitle}>Gate</h2>
            <GateLamp />
          </div>
          <div className={styles.blockBody}>
            <Toggle param="gateEnabled" label="On" />
            <Knob param="gateThreshold" size="sm" label="Threshold" />
          </div>
        </div>
      </div>

      <div className={styles.board}>
        <TrenchPedal />
        <Pedal
          name="Tide"
          kind="Compressor"
          enableParam="tideEnabled"
          finish={FINISH.tide}
          status={<ReductionMeter />}
        >
          <Knob param="tideComp" label="Comp" />
          <Knob param="tideTone" label="Tone" />
          <Knob param="tideLevel" label="Level" />
          <Knob param="tideBlend" label="Blend" />
        </Pedal>
        <UrchinPedal />
        <Pedal name="Pearl" kind="Overdrive" enableParam="pearlEnabled" finish={FINISH.pearl}>
          <Knob param="pearlGain" label="Gain" />
          <Knob param="pearlTone" label="Tone" />
          <Knob param="pearlLevel" label="Level" />
          <Knob param="pearlMid" label="Mid boost" />
        </Pedal>
        <KelpPedal />
        <Pedal name="Coral" kind="Distortion" enableParam="coralEnabled" finish={FINISH.coral}>
          <Knob param="coralGain" label="Gain" />
          <Knob param="coralCut" label="Cut" />
          <Knob param="coralLevel" label="Level" className={styles.span2} />
        </Pedal>
        <Pedal
          name="Ripple"
          kind="Modulation"
          enableParam="rippleEnabled"
          finish={FINISH.ripple}
          led={<RateLed />}
        >
          <Segmented
            param="rippleMode"
            size="sm"
            className={styles.span2}
            labels={['Trem', 'Chorus', 'Vibe']}
          />
          <Knob param="rippleSpeed" label="Speed" />
          <Knob param="rippleDepth" label="Depth" />
        </Pedal>
      </div>
    </div>
  );
}
