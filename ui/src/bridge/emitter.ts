// SPDX-License-Identifier: AGPL-3.0-or-later
import type { EventName, Events } from './protocol';

type Listener<E extends EventName> = (payload: Events[E]) => void;

/** Minimal typed event emitter used by the mock backend. */
export class Emitter {
  private listeners = new Map<EventName, Set<Listener<EventName>>>();

  on<E extends EventName>(event: E, fn: Listener<E>): () => void {
    let set = this.listeners.get(event);
    if (!set) {
      set = new Set();
      this.listeners.set(event, set);
    }
    set.add(fn as Listener<EventName>);
    return () => set.delete(fn as Listener<EventName>);
  }

  emit<E extends EventName>(event: E, payload: Events[E]): void {
    this.listeners.get(event)?.forEach((fn) => fn(payload));
  }

  listenerCount(event: EventName): number {
    return this.listeners.get(event)?.size ?? 0;
  }
}
