// SPDX-License-Identifier: AGPL-3.0-or-later
// The size a pedal is drawn at comes from whatever hosts it: the board picks by how many pedals
// it holds, the Add pedal shelf always draws small. A panel on its own is large.
import { createContext, useContext } from 'react';
import type { KnobSize } from '../../controls/Knob';
import type { PedalDensity } from './sizes';

export const PedalDensityContext = createContext<PedalDensity>('large');

export function usePedalDensity(): PedalDensity {
  return useContext(PedalDensityContext);
}

/** How much of the enclosure a knob takes: most are normal, a pedal's main control is bigger. */
export type KnobWeight = 'normal' | 'big' | 'hero';

export const KNOB_SIZE: Record<PedalDensity, Record<KnobWeight, KnobSize>> = {
  small: { normal: 'sm', big: 'md', hero: 'lg' },
  large: { normal: 'md', big: 'lg', hero: 'xl' },
};
