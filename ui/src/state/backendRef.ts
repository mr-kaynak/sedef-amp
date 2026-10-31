// SPDX-License-Identifier: AGPL-3.0-or-later
// The active backend. Set once at startup (or per test); stores and hooks call through it.
import type { Backend, NativeArgs, NativeName, NativeResult } from '../bridge/backend';

let current: Backend | null = null;
const reported = new Set<string>();

export function setBackend(backend: Backend | null): void {
  current = backend;
}

export function getBackend(): Backend | null {
  return current;
}

/**
 * Calls a native function. Failures are reported once per function as a warning: the UI keeps
 * working optimistically and the next successful call or event resynchronises it.
 */
export function invoke<K extends NativeName>(
  name: K,
  ...args: NativeArgs<K>
): Promise<NativeResult<K> | undefined> {
  if (!current) return Promise.resolve(undefined);
  return current.call(name, ...args).catch((error: unknown) => {
    if (!reported.has(name)) {
      reported.add(name);
      console.warn(`Sedef bridge: ${name} failed`, error);
    }
    return undefined;
  });
}
