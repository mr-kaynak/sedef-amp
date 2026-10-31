// SPDX-License-Identifier: AGPL-3.0-or-later
// Sends parameter changes to the host. Values set within one task are coalesced (a fast drag
// sends one value per pointer event batch, not per event), and gesture begin/end always flush
// pending values first so the host records the full automation gesture.
import { invoke } from '../state/backendRef';

const ECHO_GUARD_MS = 250;

const pending = new Map<string, number>();
const gestures = new Set<string>();
const lastLocalChange = new Map<string, number>();
let flushQueued = false;

const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

function flush(): void {
  flushQueued = false;
  for (const [id, value] of pending) void invoke('sedef_setParam', id, value);
  pending.clear();
}

export function sendValue(id: string, value: number): void {
  pending.set(id, value);
  lastLocalChange.set(id, now());
  if (!flushQueued) {
    flushQueued = true;
    queueMicrotask(flush);
  }
}

export function beginGesture(id: string): void {
  if (gestures.has(id)) return;
  flush();
  gestures.add(id);
  void invoke('sedef_beginGesture', id);
}

export function endGesture(id: string): void {
  if (!gestures.has(id)) return;
  flush();
  gestures.delete(id);
  lastLocalChange.set(id, now());
  void invoke('sedef_endGesture', id);
}

export function resetParam(id: string): void {
  pending.delete(id);
  lastLocalChange.set(id, now());
  void invoke('sedef_resetParam', id);
}

/** True while the user owns a parameter: mid-gesture or within the echo guard after a change. */
export function isLocallyOwned(id: string): boolean {
  if (gestures.has(id)) return true;
  const t = lastLocalChange.get(id);
  return t !== undefined && now() - t < ECHO_GUARD_MS;
}

/** Test helper: forget all gesture and echo bookkeeping. */
export function resetSenderState(): void {
  pending.clear();
  gestures.clear();
  lastLocalChange.clear();
  flushQueued = false;
}
