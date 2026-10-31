// SPDX-License-Identifier: AGPL-3.0-or-later
import type { Backend, NativeArgs, NativeName, NativeResult } from './backend';
import { PROTOCOL_VERSION, type EventName, type Events, type HelloResponse } from './protocol';

type NativeFn = (...args: unknown[]) => Promise<unknown>;
type JuceModule = typeof import('@juce-framework/webview');

/** True when the page runs inside JUCE's WebBrowserComponent (the global is injected first). */
export function isInsideJuce(): boolean {
  return typeof window !== 'undefined' && typeof window.__JUCE__?.backend !== 'undefined';
}

/**
 * Backend over JUCE's native-function and event bridge. The @juce-framework/webview module is
 * imported lazily: at import time it installs a placeholder window.__JUCE__ (and warns) when the
 * global is missing, which must never happen in a plain browser.
 */
export class JuceBackend implements Backend {
  readonly kind = 'juce' as const;
  private fns = new Map<string, NativeFn>();

  private constructor(private readonly juce: JuceModule) {}

  static async create(): Promise<JuceBackend> {
    const juce = await import('@juce-framework/webview');
    return new JuceBackend(juce);
  }

  private fn(name: string): NativeFn {
    let f = this.fns.get(name);
    if (!f) {
      f = this.juce.getNativeFunction(name);
      this.fns.set(name, f);
    }
    return f;
  }

  hello(): Promise<HelloResponse> {
    return this.call('sedef_hello', PROTOCOL_VERSION);
  }

  call<K extends NativeName>(name: K, ...args: NativeArgs<K>): Promise<NativeResult<K>> {
    return this.fn(name)(...args) as Promise<NativeResult<K>>;
  }

  on<E extends EventName>(event: E, fn: (payload: Events[E]) => void): () => void {
    const handle = window.__JUCE__.backend.addEventListener(event, (payload) =>
      fn(payload as Events[E]),
    );
    return () => window.__JUCE__.backend.removeEventListener(handle);
  }
}
