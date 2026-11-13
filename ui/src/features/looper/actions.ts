// SPDX-License-Identifier: AGPL-3.0-or-later
// Presses go to the engine, which decides what they mean from its own state (docs/looper.md).
import type { LooperAction } from '../../bridge/protocol';
import { invoke } from '../../state/backendRef';
import { telemetry } from '../../state/live';
import { dismissToast, showToast } from '../../state/toastStore';

let clearToast: number | null = null;

/** Reserves the looper's memory (once per session) so the first press needs no wait. */
export function openLooper(): void {
  void invoke('sedef_looper_open');
}

export function looperPress(action: LooperAction): void {
  if (action === 'clear') return clearLoop();
  void invoke('sedef_looper_command', action);
}

/** Clear with a way back: "Loop cleared · Undo", while the engine still holds the loop. */
function clearLoop(): void {
  const state = telemetry.latest()?.looper?.state;
  const hadLoop = state === 'playing' || state === 'overdubbing' || state === 'stopped';
  void invoke('sedef_looper_command', 'clear');
  if (!hadLoop && !(state === 'armed' && telemetry.latest()?.looper?.armedForPlay)) return;
  clearToast = showToast('Loop cleared', {
    label: 'Undo',
    run: () => void invoke('sedef_looper_command', 'undo'),
  });
}

/** A new take ends what Clear's Undo could bring back: its notice goes. */
export function dropClearNotice(): void {
  if (clearToast !== null) dismissToast(clearToast);
  clearToast = null;
}
