// SPDX-License-Identifier: AGPL-3.0-or-later
// Note names as players read them: MIDI 40 is E2, the low string of a guitar in standard tuning.
const NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'] as const;

export function noteName(note: number): { name: string; octave: number } {
  return { name: NAMES[((note % 12) + 12) % 12] ?? 'C', octave: Math.floor(note / 12) - 1 };
}

/** Spoken form for screen readers: "F sharp 2" reads better than "F♯2". */
export function spokenNote(note: number): string {
  const { name, octave } = noteName(note);
  return `${name.replace('♯', ' sharp')} ${octave}`;
}
