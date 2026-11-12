// SPDX-License-Identifier: AGPL-3.0-or-later
// The three amps share one layout and differ in finish and controls.

export type AmpControl =
  | { type: 'knob'; param: string; label: string }
  | { type: 'lever'; param: string; label: string }
  | { type: 'voice'; param: string; label: string };

export interface AmpFinish {
  plate: [string, string];
  legend: string;
  legendDim: string;
  grille: string;
  grilleThread: string;
  badge: string;
  piping: string;
}

export interface AmpDefinition {
  index: number;
  name: string;
  character: string;
  finish: AmpFinish;
  controls: AmpControl[];
}

const knob = (param: string, label: string): AmpControl => ({ type: 'knob', param, label });

export const AMPS: AmpDefinition[] = [
  {
    index: 0,
    name: 'Lagoon',
    character: 'Glassy clean with plenty of headroom',
    finish: {
      plate: ['#d6e5df', '#b7cdc6'],
      legend: '#12283a',
      legendDim: 'rgb(18 40 58 / 0.75)',
      grille: '#c5d3cb',
      grilleThread: 'rgb(36 72 84 / 0.22)',
      badge: '#12283a',
      piping: '#e9f1ed',
    },
    controls: [
      knob('lagoonVolume', 'Volume'),
      knob('lagoonBass', 'Bass'),
      knob('lagoonCut', 'Cut'),
      knob('lagoonMaster', 'Master'),
      knob('lagoonOutput', 'Output'),
    ],
  },
  {
    index: 1,
    name: 'Reef',
    character: 'Crunch in three voicings, open to thick',
    finish: {
      plate: ['#643539', '#47252a'],
      legend: '#f3e6d2',
      legendDim: 'rgb(243 230 210 / 0.75)',
      grille: '#2a1a1c',
      grilleThread: 'rgb(226 180 150 / 0.2)',
      badge: '#f3e6d2',
      piping: '#c99f86',
    },
    controls: [
      { type: 'voice', param: 'reefVoice', label: 'Voice' },
      knob('reefGain', 'Gain'),
      knob('reefBass', 'Bass'),
      knob('reefMid', 'Middle'),
      knob('reefTreble', 'Treble'),
      knob('reefPresence', 'Presence'),
      knob('reefMaster', 'Master'),
      knob('reefOutput', 'Output'),
    ],
  },
  {
    index: 2,
    name: 'Abyss',
    character: 'Tight high gain, from rhythm to lead',
    finish: {
      plate: ['#0d1923', '#060d13'],
      legend: '#e8e2d3',
      legendDim: 'rgb(232 226 211 / 0.75)',
      grille: '#05090d',
      grilleThread: 'rgb(205 238 238 / 0.09)',
      badge: 'nacre',
      piping: '#2b3f4d',
    },
    controls: [
      { type: 'lever', param: 'abyssBright', label: 'Bright' },
      knob('abyssGain', 'Gain'),
      knob('abyssBass', 'Bass'),
      knob('abyssMid', 'Middle'),
      knob('abyssTreble', 'Treble'),
      { type: 'lever', param: 'abyssShift', label: 'Shift' },
      knob('abyssPresence', 'Presence'),
      knob('abyssMaster', 'Master'),
      knob('abyssOutput', 'Output'),
    ],
  },
];
