// SPDX-License-Identifier: AGPL-3.0-or-later
import type { PedalFinish } from '../Pedal';

// Enclosure paints. Every legend and its dim tone reach 4.5:1 on both ends of the gradient.
export const MODS_FINISH: Record<'conch' | 'anemone' | 'gyre' | 'jetty' | 'beacon', PedalFinish> = {
  // The pink-apricot inside of a conch shell.
  conch: {
    paint: ['#f2cdb4', '#dfa98a'],
    legend: '#3a2117',
    legendDim: 'rgb(58 33 23 / 0.85)',
  },
  // A sea anemone's deep green.
  anemone: {
    paint: ['#2a6259', '#1c4640'],
    legend: '#eaf5f0',
    legendDim: 'rgb(234 245 240 / 0.9)',
  },
  // Plum, for the slow turning of an ocean gyre.
  gyre: { paint: ['#6e3d62', '#4d2a45'], legend: '#f6e9f1', legendDim: 'rgb(246 233 241 / 0.8)' },
  // Weathered timber.
  jetty: {
    paint: ['#6c5847', '#4f4134'],
    legend: '#f3ece2',
    legendDim: 'rgb(243 236 226 / 0.9)',
  },
  // Night sea with a lamp-gold legend.
  beacon: {
    paint: ['#2b3d58', '#1b293d'],
    legend: '#f4e6bf',
    legendDim: 'rgb(244 230 191 / 0.8)',
  },
};
