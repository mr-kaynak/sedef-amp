// SPDX-License-Identifier: AGPL-3.0-or-later
// The scale a pedal is drawn at (canvas pixels per board unit) comes from whatever hosts it: the
// board picks the largest that fits, the Add pedal shelf always draws at the smallest. A panel
// on its own is at the largest.
import { createContext, useContext } from 'react';
import { MAX_SCALE } from './sizes';

export const PedalScaleContext = createContext<number>(MAX_SCALE);

export function usePedalScale(): number {
  return useContext(PedalScaleContext);
}

/** How much of the enclosure a knob takes: most are normal, a pedal's main control is bigger. */
export type KnobWeight = 'normal' | 'big' | 'hero';

/** A knob's dial in board units. */
export const KNOB_UNITS: Record<KnobWeight, number> = { normal: 36, big: 50, hero: 66 };
