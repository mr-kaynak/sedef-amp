// SPDX-License-Identifier: AGPL-3.0-or-later
// The UI talks to the plugin only through this interface. JuceBackend implements it over the
// WebView bridge; MockBackend implements it in memory so the UI runs in a plain browser and in
// tests with the real parameter schema.
import type { EventName, Events, HelloResponse, NativeFunctions } from './protocol';

export type NativeName = keyof NativeFunctions;
export type NativeArgs<K extends NativeName> = Parameters<NativeFunctions[K]>;
export type NativeResult<K extends NativeName> = ReturnType<NativeFunctions[K]>;

/** What the simulated player should be doing. Only the mock backend uses this. */
export type SimulatedIntent =
  | { kind: 'idle' }
  | { kind: 'tune'; midi: number }
  | { kind: 'fret'; midi: number; stringIndex: number };

/**
 * Optional development hook: lets UI flows (the intonation workbench) tell the mock which note a
 * real player would be playing, so the flow is demoable without a guitar. Absent on JuceBackend.
 */
export interface PitchSimulator {
  setIntent(intent: SimulatedIntent): void;
}

export interface Backend {
  readonly kind: 'juce' | 'mock';
  hello(): Promise<HelloResponse>;
  call<K extends NativeName>(name: K, ...args: NativeArgs<K>): Promise<NativeResult<K>>;
  /** Subscribes to a bridge event; returns an unsubscribe function. */
  on<E extends EventName>(event: E, fn: (payload: Events[E]) => void): () => void;
  readonly simulator?: PitchSimulator;
}
