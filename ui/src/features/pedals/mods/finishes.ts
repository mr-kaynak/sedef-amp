// SPDX-License-Identifier: AGPL-3.0-or-later
import type { PedalFinish } from '../Pedal';

// Enclosure paints. Every legend and its dim tone reach 4.5:1 on both ends of the gradient, and
// each paint stays clear of the other pedals' (navy Trench, plum Urchin, sea-green Sargasso,
// bronze Remora), so no two enclosures on the board look alike.
export const MODS_FINISH: Record<'conch' | 'anemone' | 'gyre' | 'jetty' | 'beacon', PedalFinish> = {
  // The pink-apricot inside of a conch shell.
  conch: {
    paint: ['#f2cdb4', '#dfa98a'],
    legend: '#3a2117',
    legendDim: 'rgb(58 33 23 / 0.85)',
  },
  // A yellow sea anemone, open in the current.
  anemone: {
    paint: ['#dcb04c', '#c39434'],
    legend: '#2b1d05',
    legendDim: 'rgb(43 29 5 / 0.9)',
  },
  // Lavender, for the slow turning of an ocean gyre.
  gyre: { paint: ['#c3b3de', '#a794ca'], legend: '#24183a', legendDim: 'rgb(36 24 58 / 0.9)' },
  // Driftwood, silvered by the sea.
  jetty: {
    paint: ['#c2bdb2', '#a39e94'],
    legend: '#1f1d19',
    legendDim: 'rgb(31 29 25 / 0.9)',
  },
  // A lighthouse's black iron with a lamp-gold legend.
  beacon: {
    paint: ['#3a3d42', '#232528'],
    legend: '#f4e6bf',
    legendDim: 'rgb(244 230 191 / 0.8)',
  },
};
