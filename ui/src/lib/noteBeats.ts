// SPDX-License-Identifier: AGPL-3.0-or-later
// Note-value labels ("1/8D", "1/4T") to quarter-note beats. Matches Drift::noteBeats in C++.

export function noteBeats(label: string): number {
  const m = /^1\/(\d+)([TD]?)$/.exec(label);
  if (!m) return 1;
  const base = 4 / Number(m[1]);
  return m[2] === 'T' ? (base * 2) / 3 : m[2] === 'D' ? base * 1.5 : base;
}

export function beatsToMs(beats: number, bpm: number): number {
  return (beats * 60000) / bpm;
}
