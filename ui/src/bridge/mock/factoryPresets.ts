// SPDX-License-Identifier: AGPL-3.0-or-later
// A handful of plausible presets for the browser mock. Values are overrides on top of the schema
// defaults, in real units.
import type { PresetInfo } from '../protocol';

export interface MockPreset {
  info: PresetInfo;
  values: Record<string, number>;
}

function factory(
  slug: string,
  name: string,
  folder: string,
  tags: string[],
  values: Record<string, number>,
): MockPreset {
  return {
    info: {
      id: `factory:${slug}`,
      name,
      category: 'Factory',
      folder,
      author: 'Sedef',
      tags,
      favorite: false,
    },
    values,
  };
}

export const FACTORY_PRESETS: MockPreset[] = [
  factory('glass-shore', 'Glass Shore', 'Clean', ['clean', 'chorus'], {
    ampModel: 0,
    lagoonVolume: 0.42,
    lagoonBass: 0.55,
    lagoonCut: 0.35,
    lagoonMaster: 0.6,
    rippleEnabled: 1,
    rippleMode: 1,
    rippleSpeed: 1.2,
    rippleDepth: 0.35,
    fathomEnabled: 1,
    fathomMode: 0,
    fathomMix: 0.22,
    fathomDecay: 2.2,
    eqLagoonBand2: -1.5,
    eqLagoonBand7: 2,
    eqLagoonBand8: 1.5,
  }),
  factory('night-swim', 'Night Swim', 'Clean', ['ambient', 'echo'], {
    ampModel: 0,
    lagoonVolume: 0.5,
    tideEnabled: 1,
    tideComp: 0.55,
    driftEnabled: 1,
    driftHeads: 2,
    driftSync: 1,
    driftNote: 12,
    driftFeedback: 0.48,
    driftMix: 0.32,
    fathomEnabled: 1,
    fathomMode: 1,
    fathomMix: 0.38,
    fathomDecay: 6.5,
    fathomPreDelay: 45,
    doublerEnabled: 1,
  }),
  factory('low-tide', 'Low Tide Crunch', 'Crunch', ['crunch', 'rhythm'], {
    ampModel: 1,
    reefVoice: 1,
    reefGain: 0.58,
    reefBass: 0.45,
    reefMid: 0.62,
    reefTreble: 0.55,
    reefPresence: 0.48,
    eqReefBand1: -2,
    eqReefBand5: 1.5,
    eqReefLowCut: 80,
    eqReefHighCut: 9000,
  }),
  factory('kelp-forest', 'Kelp Forest', 'Crunch', ['crunch', 'tremolo'], {
    ampModel: 1,
    reefVoice: 0,
    reefGain: 0.4,
    rippleEnabled: 1,
    rippleMode: 0,
    rippleSpeed: 4.6,
    rippleDepth: 0.55,
    fathomEnabled: 1,
    fathomMix: 0.18,
  }),
  factory('pressure-ridge', 'Pressure Ridge', 'Lead', ['high gain', 'rhythm'], {
    ampModel: 2,
    abyssGain: 0.62,
    abyssBass: 0.58,
    abyssMid: 0.4,
    abyssTreble: 0.56,
    abyssShift: 1,
    gateThreshold: -58,
    pearlEnabled: 1,
    pearlGain: 0.1,
    pearlLevel: 0.75,
    eqAbyssLowCut: 70,
    eqAbyssBand3: -2.5,
  }),
  factory('abyssal-lead', 'Abyssal Lead', 'Lead', ['lead', 'echo'], {
    ampModel: 2,
    abyssGain: 0.74,
    abyssMid: 0.64,
    abyssPresence: 0.58,
    coralEnabled: 1,
    coralGain: 0.3,
    coralCut: 0.4,
    driftEnabled: 1,
    driftHeads: 1,
    driftFeedback: 0.32,
    driftMix: 0.24,
    driftTapeMod: 0.4,
    fathomEnabled: 1,
    fathomMode: 1,
    fathomMix: 0.2,
    fathomDecay: 3.8,
    eqAbyssBand4: 2,
  }),
];

export const USER_PRESETS: MockPreset[] = [
  {
    info: {
      id: 'user:Live/Sunday set.json',
      name: 'Sunday set',
      category: 'User',
      folder: 'Live',
      author: '',
      tags: [],
      favorite: true,
    },
    values: { ampModel: 1, reefGain: 0.46, driftEnabled: 1, driftMix: 0.2, fathomEnabled: 1 },
  },
];
