// SPDX-License-Identifier: AGPL-3.0-or-later
// The UI half of the pedal registry: each pedal's panel and what the Add pedal picker says about
// it. The engine half (id, name, category, default position, switch) is
// engine/include/sedef/board/PedalList.inc, read here through ./registry. Adding a pedal:
// docs/architecture.md, "Adding a pedal".
import type { ComponentType } from 'react';
import { BreakerPedal, RemoraPedal, SargassoPedal } from './DrivePedals';
import { KelpPedal } from './KelpPedal';
import { AnemonePedal, BeaconPedal, ConchPedal, GyrePedal, JettyPedal } from './mods';
import { CoralPanel } from './panels/CoralPanel';
import { GatePanel } from './panels/GatePanel';
import { PearlPanel } from './panels/PearlPanel';
import { RipplePanel } from './panels/RipplePanel';
import { TidePanel } from './panels/TidePanel';
import { TrenchPedal } from './TrenchPedal';
import { UrchinPedal } from './UrchinPedal';

export interface PedalEntry {
  /** The enclosure: self-contained, it renders on its own in any container. */
  Panel: ComponentType;
  /** What kind of pedal it is, as its enclosure says ("Overdrive"). */
  kind: string;
  /** One line for the picker: what it does to the sound. */
  description: string;
}

export const PEDAL_CATALOG: Record<string, PedalEntry> = {
  gate: {
    Panel: GatePanel,
    kind: 'Noise gate',
    description: 'Silences hiss between notes; it listens to your guitar.',
  },
  tide: {
    Panel: TidePanel,
    kind: 'Compressor',
    description: 'Evens out your picking and adds sustain, with a dry blend.',
  },
  pearl: {
    Panel: PearlPanel,
    kind: 'Overdrive',
    description: 'Warm, mid-focused push that stays touch-sensitive.',
  },
  coral: {
    Panel: CoralPanel,
    kind: 'Distortion',
    description: 'Hard clipping with a cut control for darker tones.',
  },
  ripple: {
    Panel: RipplePanel,
    kind: 'Modulation',
    description: 'Tremolo, chorus or vibrato.',
  },
  trench: {
    Panel: TrenchPedal,
    kind: 'Octave down',
    description: 'One and two octaves below single notes, like a baritone.',
  },
  urchin: {
    Panel: UrchinPedal,
    kind: 'Octave-up fuzz',
    description: 'Fuzz with a ringing octave above, clearest high on the neck.',
  },
  kelp: {
    Panel: KelpPedal,
    kind: 'Mid overdrive',
    description: 'Mid-hump drive, or a tight boost into a high-gain amp.',
  },
  remora: {
    Panel: RemoraPedal,
    kind: 'Fuzz',
    description: 'Two-transistor fuzz that cleans up as you roll your volume back.',
  },
  sargasso: {
    Panel: SargassoPedal,
    kind: 'Sustain fuzz',
    description: 'Thick fuzz that holds notes, with a scooped-mid tone control.',
  },
  breaker: {
    Panel: BreakerPedal,
    kind: 'Treble booster',
    description: 'Pushes the upper mids into the amp; Full widens the boost.',
  },
  anemone: {
    Panel: AnemonePedal,
    kind: 'Envelope filter',
    description: 'Auto-wah: the filter follows how hard you pick, up or down.',
  },
  conch: {
    Panel: ConchPedal,
    kind: 'Wah',
    description: 'Vocal wah sweep from heel to toe, or Auto, rocked by your picking.',
  },
  beacon: {
    Panel: BeaconPedal,
    kind: 'Photocell vibe',
    description: 'Lamp-driven throb, as chorus or vibrato.',
  },
  gyre: {
    Panel: GyrePedal,
    kind: 'Phaser',
    description: 'Sweeping notches; 6 stages cut a third notch, and feedback sharpens them.',
  },
  jetty: {
    Panel: JettyPedal,
    kind: 'Flanger',
    description: 'Jet-plane comb sweep, with feedback for metallic ringing.',
  },
};
