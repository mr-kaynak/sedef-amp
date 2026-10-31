// SPDX-License-Identifier: AGPL-3.0-or-later
// Sends parameter changes to the host. Values set within one task are coalesced (a fast drag
// sends one value per pointer event batch, not per event), and gesture begin/end always flush
// pending values first so the host records the full automation gesture.
//
// Gestures are reference-counted per parameter: a wheel burst, a drag and a key press on the same
// control may overlap, and the host sees one begin and one end around all of them.
import type { ParamValues } from '../bridge/protocol';
import { invoke } from '../state/backendRef';

const ECHO_GUARD_MS = 250;

const pending = new Map<string, number>();
/** Calls the host has not completed yet, per id. */
const inFlight = new Map<string, number>();
const gestures = new Map<string, number>();
const lastLocalChange = new Map<string, number>();
/** What this UI last sent per parameter (cleared by a reset, where the host picks the value). */
const lastSent = new Map<string, number>();
/** Host values that arrived while the user owned the parameter, newest per id. */
const deferred = new Map<string, number>();
let deferredTimer: ReturnType<typeof setTimeout> | null = null;
let applyReleased: ((values: ParamValues) => void) | null = null;
let flushQueued = false;

const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

function track(id: string, call: () => Promise<unknown>): void {
  inFlight.set(id, (inFlight.get(id) ?? 0) + 1);
  void call().finally(() => {
    const n = (inFlight.get(id) ?? 1) - 1;
    if (n > 0) inFlight.set(id, n);
    else inFlight.delete(id);
  });
}

function flush(): void {
  flushQueued = false;
  for (const [id, value] of pending) track(id, () => invoke('sedef_setParam', id, value));
  pending.clear();
}

export function sendValue(id: string, value: number): void {
  pending.set(id, value);
  lastSent.set(id, value);
  lastLocalChange.set(id, now());
  // The host applies this after anything it sent before, so a deferred host value is stale.
  deferred.delete(id);
  if (!flushQueued) {
    flushQueued = true;
    queueMicrotask(flush);
  }
}

export function beginGesture(id: string): void {
  const depth = gestures.get(id) ?? 0;
  gestures.set(id, depth + 1);
  if (depth > 0) return;
  flush();
  void invoke('sedef_beginGesture', id);
}

export function endGesture(id: string): void {
  const depth = gestures.get(id) ?? 0;
  if (depth === 0) return;
  if (depth > 1) {
    gestures.set(id, depth - 1);
    return;
  }
  flush();
  gestures.delete(id);
  lastLocalChange.set(id, now());
  void invoke('sedef_endGesture', id);
}

export function resetParam(id: string): void {
  pending.delete(id);
  lastSent.delete(id);
  deferred.delete(id);
  lastLocalChange.set(id, now());
  track(id, () => invoke('sedef_resetParam', id));
}

/** True while the user owns a parameter: mid-gesture or within the echo guard after a change. */
export function isLocallyOwned(id: string): boolean {
  if (gestures.has(id)) return true;
  const t = lastLocalChange.get(id);
  return t !== undefined && now() - t < ECHO_GUARD_MS;
}

/**
 * Keeps a host value that arrived while the user owned the parameter. The plugin sends each
 * change once, so dropping it would leave the control stale (a preset loaded while a knob was
 * still under the wheel). It goes to the release handler when ownership ends, unless the user
 * changed the parameter again in the meantime or it only repeats what the UI sent.
 *
 * Events and call completions arrive in the order the plugin produced them, so a value that
 * arrives while one of our own calls for that id is still unanswered (or not yet sent) was
 * produced before the host applied it, and is superseded by it.
 */
export function deferHostValue(id: string, value: number): void {
  if (pending.has(id) || inFlight.has(id)) return;
  deferred.set(id, value);
  scheduleRelease();
}

/** Registers where released host values go (the parameter store). */
export function onHostValuesReleased(fn: (values: ParamValues) => void): void {
  applyReleased = fn;
}

function scheduleRelease(): void {
  if (deferredTimer !== null || deferred.size === 0) return;
  // Mid-gesture ids are polled at the guard interval; the rest are due when their guard ends.
  let wait = ECHO_GUARD_MS;
  for (const id of deferred.keys()) {
    const t = lastLocalChange.get(id);
    if (!gestures.has(id)) wait = Math.min(wait, t === undefined ? 0 : ECHO_GUARD_MS - (now() - t));
  }
  deferredTimer = setTimeout(releaseDeferred, Math.max(0, wait) + 1);
}

function releaseDeferred(): void {
  deferredTimer = null;
  const values: ParamValues = {};
  let any = false;
  for (const [id, value] of deferred) {
    if (isLocallyOwned(id)) continue;
    deferred.delete(id);
    if (value === lastSent.get(id)) continue;
    values[id] = value;
    any = true;
  }
  if (any) applyReleased?.(values);
  scheduleRelease();
}

/** Test helper: forget all gesture and echo bookkeeping. */
export function resetSenderState(): void {
  pending.clear();
  inFlight.clear();
  gestures.clear();
  lastLocalChange.clear();
  lastSent.clear();
  deferred.clear();
  if (deferredTimer !== null) clearTimeout(deferredTimer);
  deferredTimer = null;
  flushQueued = false;
}
