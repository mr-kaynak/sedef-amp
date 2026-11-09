// SPDX-License-Identifier: AGPL-3.0-or-later
// The fuzzes and the booster: Remora, Sargasso and Breaker. Each panel is self-contained (an
// enclosure with its own parameters), so any page or board can host it.
import { Pedal, PedalRow, type PedalFinish } from './Pedal';
import { PedalKnob } from './PedalKnob';
import { PedalSwitch } from './PedalSwitch';

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
      id="remora"
      name="Remora"
      kind="Fuzz"
      enableParam="remoraEnabled"
      finish={FINISH.remora}
      status={<PedalSwitch param="remoraTransistor" labels={['Germanium', 'Silicon']} />}
      lamp="beside"
    >
      <PedalRow set="tight">
        <PedalKnob param="remoraFuzz" label="Fuzz" weight="big" />
        <PedalKnob param="remoraVolume" label="Volume" weight="big" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="remoraBias" label="Bias" />
        <PedalKnob param="remoraGuitarVolume" label="Guitar volume" />
      </PedalRow>
    </Pedal>
  );
}

/** Four-stage sustain fuzz. */
export function SargassoPedal() {
  return (
    <Pedal
      id="sargasso"
      name="Sargasso"
      kind="Sustain fuzz"
      enableParam="sargassoEnabled"
      finish={FINISH.sargasso}
      nameAt="top"
    >
      <PedalRow>
        <PedalKnob param="sargassoSustain" label="Sustain" />
        <PedalKnob param="sargassoTone" label="Tone" />
      </PedalRow>
      <PedalRow>
        <PedalKnob param="sargassoVolume" label="Volume" />
      </PedalRow>
    </Pedal>
  );
}

/** Germanium treble booster. */
export function BreakerPedal() {
  return (
    <Pedal
      id="breaker"
      name="Breaker"
      kind="Treble booster"
      enableParam="breakerEnabled"
      finish={FINISH.breaker}
      status={<PedalSwitch param="breakerRange" labels={['Treble', 'Full']} />}
    >
      <PedalRow>
        <PedalKnob param="breakerBoost" label="Boost" weight="hero" />
      </PedalRow>
    </Pedal>
  );
}
