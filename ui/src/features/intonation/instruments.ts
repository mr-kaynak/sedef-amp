// SPDX-License-Identifier: AGPL-3.0-or-later
import { midiLabel } from '../../lib/music';

export interface InstrumentPreset {
  id: string;
  name: string;
  /** MIDI notes of the open strings, lowest (thickest) string first. */
  notes: number[];
  defaultScaleId: string;
}

export const INSTRUMENTS: InstrumentPreset[] = [
  {
    id: 'gtr6',
    name: '6-string, standard',
    notes: [40, 45, 50, 55, 59, 64],
    defaultScaleId: '25.5',
  },
  {
    id: 'gtr6-dropd',
    name: '6-string, drop D',
    notes: [38, 45, 50, 55, 59, 64],
    defaultScaleId: '25.5',
  },
  {
    id: 'gtr7',
    name: '7-string, standard',
    notes: [35, 40, 45, 50, 55, 59, 64],
    defaultScaleId: '25.5',
  },
  { id: 'bass4', name: '4-string bass', notes: [28, 33, 38, 43], defaultScaleId: '34' },
  { id: 'bass5', name: '5-string bass', notes: [23, 28, 33, 38, 43], defaultScaleId: '34' },
];

export const CUSTOM_INSTRUMENT_ID = 'custom';

export interface ScaleLength {
  id: string;
  label: string;
  mm: number;
}

export const SCALE_LENGTHS: ScaleLength[] = [
  { id: '24.75', label: '24.75 in', mm: 628 },
  { id: '25', label: '25 in', mm: 635 },
  { id: '25.5', label: '25.5 in', mm: 648 },
  { id: '27', label: '27 in', mm: 686 },
  { id: '34', label: '34 in', mm: 864 },
  { id: '35', label: '35 in', mm: 889 },
];

export const CUSTOM_SCALE_ID = 'custom';
export const MIN_CUSTOM_SCALE_MM = 400;
export const MAX_CUSTOM_SCALE_MM = 1000;

/** Guitar strings are numbered from the thinnest (1) to the thickest. */
export function stringNumber(index: number, count: number): number {
  return count - index;
}

export function tuningLabel(notes: readonly number[]): string {
  return notes.map((n) => midiLabel(n).replace(/-?\d+$/, '')).join(' ');
}

/** Selectable notes for custom tunings: B0 (lowest 5-string bass) to E5. */
export const CUSTOM_NOTE_RANGE = { min: 23, max: 76 };
