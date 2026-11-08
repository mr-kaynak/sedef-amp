// SPDX-License-Identifier: AGPL-3.0-or-later
// Enclosure paints. Each legend colour keeps 4.5:1 small text on its paint.
import type { PedalFinish } from '../Pedal';

export const GATE: PedalFinish = {
  paint: ['#2a3d47', '#1b2a33'],
  legend: '#ece7da',
  legendDim: 'rgb(236 231 218 / 0.8)',
};

export const TIDE: PedalFinish = {
  paint: ['#46677a', '#2c4757'],
  legend: '#eef0ea',
  legendDim: 'rgb(238 240 234 / 0.86)',
};

export const PEARL: PedalFinish = {
  paint: ['#efeadf', '#d8d1c2'],
  legend: '#13293a',
  legendDim: 'rgb(19 41 58 / 0.75)',
  // Pearl gets the only nacre-sheened enclosure: the pedal is named for it.
  sheen:
    'conic-gradient(from 210deg at 70% 30%, rgb(205 238 238 / 0.5), rgb(221 214 246 / 0.4), rgb(246 219 226 / 0.5), rgb(242 232 203 / 0.45), rgb(205 238 238 / 0.5))',
};

// Deep enough for 4.5:1 small text: a mid coral fails with light and dark legends alike.
export const CORAL: PedalFinish = {
  paint: ['#b04e56', '#8b363f'],
  legend: '#fbefe6',
  legendDim: 'rgb(251 239 230 / 0.95)',
};

export const RIPPLE: PedalFinish = {
  paint: ['#4a4a82', '#2f2f5c'],
  legend: '#ece9f8',
  legendDim: 'rgb(236 233 248 / 0.75)',
};
