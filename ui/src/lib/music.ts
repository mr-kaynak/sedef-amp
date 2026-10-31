// SPDX-License-Identifier: AGPL-3.0-or-later
// Equal-tempered pitch helpers shared by the tuner, the intonation workbench and the mock.

export const NOTE_NAMES = [
  'C',
  'C#',
  'D',
  'D#',
  'E',
  'F',
  'F#',
  'G',
  'G#',
  'A',
  'A#',
  'B',
] as const;
export type NoteName = (typeof NOTE_NAMES)[number];

export function midiToHz(midi: number, a4Hz = 440): number {
  return a4Hz * Math.pow(2, (midi - 69) / 12);
}

/** Fractional MIDI note number for a frequency. */
export function hzToMidi(hz: number, a4Hz = 440): number {
  return 69 + 12 * Math.log2(hz / a4Hz);
}

export function centsBetween(hz: number, referenceHz: number): number {
  return 1200 * Math.log2(hz / referenceHz);
}

export function noteOf(midi: number): { note: NoteName; octave: number } {
  const m = Math.round(midi);
  return { note: NOTE_NAMES[((m % 12) + 12) % 12] as NoteName, octave: Math.floor(m / 12) - 1 };
}

export function midiLabel(midi: number): string {
  const { note, octave } = noteOf(midi);
  return `${note}${octave}`;
}

/** Parses "E2", "C#3", "Bb1" into a MIDI note number; null when malformed. */
export function parseNote(text: string): number | null {
  const m = /^([A-Ga-g])([#b]?)(-?\d)$/.exec(text.trim());
  if (!m) return null;
  const [, letter, accidental, octave] = m as unknown as [string, string, string, string];
  const base = NOTE_NAMES.indexOf(letter.toUpperCase() as NoteName);
  const shift = accidental === '#' ? 1 : accidental === 'b' ? -1 : 0;
  return (Number(octave) + 1) * 12 + base + shift;
}

/** Splits a note name into letter and accidental for typesetting ("C#" -> ["C", "♯"]). */
export function splitAccidental(note: string): [string, string] {
  return [note.charAt(0), note.length > 1 ? (note.charAt(1) === '#' ? '♯' : '♭') : ''];
}
