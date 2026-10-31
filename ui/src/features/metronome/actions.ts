// SPDX-License-Identifier: AGPL-3.0-or-later
import { setLocalValue } from '../../params/paramStore';
import { invoke } from '../../state/backendRef';
import { uiActions } from '../../state/uiStore';

export function setMetronomePattern(steps: number[]): void {
  uiActions.setMetronomeSteps(steps);
  void invoke('sedef_metronome_setPattern', steps);
}

/**
 * Registers a tap; the plugin averages recent taps, sets its tempo parameter and returns the new
 * value. Only the local copy is updated here: sending it back would be a redundant round trip.
 */
export async function tapTempo(): Promise<void> {
  const bpm = await invoke('sedef_metronome_tap');
  if (typeof bpm === 'number' && Number.isFinite(bpm)) setLocalValue('tempo', bpm);
}
