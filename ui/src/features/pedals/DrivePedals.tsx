// SPDX-License-Identifier: AGPL-3.0-or-later
// The fuzzes and the booster: Remora, Sargasso and Breaker. Each panel is self-contained (a fixed-size
// enclosure with its own parameters), so any page or board can host it.
import { Knob } from '../../controls/Knob';
import { Segmented } from '../../controls/Segmented';
import { Pedal, type PedalFinish } from './Pedal';
import styles from './DrivePedals.module.css';

// Contrast of every legend against both ends of its paint is at least 4.5:1 (dim legends
// included), so the small knob labels and values read on each enclosure.
const FINISH: Record<'remora' | 'sargasso' | 'breaker', PedalFinish> = {
  // Burnished bronze, for the germanium years.
  remora: {
    paint: ['#7a5a34', '#4f381f'],
    legend: '#f6ecda',
    legendDim: 'rgb(246 236 218 / 0.95)',
  },
  // Deep sea green: the weed-covered sea that ocean currents ring.
  sargasso: {
    paint: ['#2a6359', '#1a423a'],
    legend: '#e8f3ee',
    legendDim: 'rgb(232 243 238 / 0.9)',
  },
  // Sea foam off a breaking wave.
  breaker: { paint: ['#d7ece6', '#acd0c7'], legend: '#0f2c35', legendDim: 'rgb(15 44 53 / 0.8)' },
};

/** Two-transistor feedback fuzz. Guitar volume stands in for the knob on the guitar. */
export function RemoraPedal() {
  return (
    <Pedal
      name="Remora"
      kind="Fuzz"
      enableParam="remoraEnabled"
      finish={FINISH.remora}
      status={
        <Segmented
          param="remoraTransistor"
          size="sm"
          className={styles.well}
          labels={['Germanium', 'Silicon']}
        />
      }
    >
      <Knob param="remoraFuzz" label="Fuzz" />
      <Knob param="remoraVolume" label="Volume" />
      <Knob param="remoraBias" label="Bias" />
      <Knob param="remoraGuitarVolume" label="Guitar volume" />
    </Pedal>
  );
}

/** Four-stage sustain fuzz. */
export function SargassoPedal() {
  return (
    <Pedal
      name="Sargasso"
      kind="Sustain fuzz"
      enableParam="sargassoEnabled"
      finish={FINISH.sargasso}
    >
      <Knob param="sargassoSustain" label="Sustain" />
      <Knob param="sargassoTone" label="Tone" />
      <Knob param="sargassoVolume" label="Volume" className={styles.span2} />
    </Pedal>
  );
}

/** Germanium treble booster. */
export function BreakerPedal() {
  return (
    <Pedal
      name="Breaker"
      kind="Treble booster"
      enableParam="breakerEnabled"
      finish={FINISH.breaker}
      status={
        <Segmented
          param="breakerRange"
          size="sm"
          className={styles.well}
          labels={['Treble', 'Full']}
        />
      }
    >
      <Knob param="breakerBoost" label="Boost" size="lg" className={styles.span2} />
    </Pedal>
  );
}
