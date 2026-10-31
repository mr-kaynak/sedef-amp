// SPDX-License-Identifier: AGPL-3.0-or-later
import type { PitchEvent } from '../../bridge/protocol';
import { centsBetween, midiToHz } from '../../lib/music';
import { getValue } from '../../params/paramStore';

/** Chromatic: nearest note. Target: a fixed note (the open string being tuned). */
export type GaugeTarget = { kind: 'chromatic' } | { kind: 'target'; midi: number };

export function centsFor(e: PitchEvent | null, target: GaugeTarget): number | null {
  if (!e || !e.valid) return null;
  if (target.kind === 'chromatic') return e.cents;
  return centsBetween(e.frequencyHz, midiToHz(target.midi, getValue('tunerReference')));
}
