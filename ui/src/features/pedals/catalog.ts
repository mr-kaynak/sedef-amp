// SPDX-License-Identifier: AGPL-3.0-or-later
// The UI half of the pedal registry: each pedal's panel and what the Add pedal picker says about
// it. The engine half (id, name, category, default position, switch) is
// engine/include/sedef/board/PedalList.inc, read here through ./registry. Adding a pedal:
// docs/architecture.md, "Adding a pedal".
import type { ComponentType } from 'react';
import { CoralPanel } from './panels/CoralPanel';
import { GatePanel } from './panels/GatePanel';
import { PearlPanel } from './panels/PearlPanel';
import { RipplePanel } from './panels/RipplePanel';
import { TidePanel } from './panels/TidePanel';

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
};
