// SPDX-License-Identifier: AGPL-3.0-or-later
// High-rate data (telemetry ~30 Hz, pitch ~60 Hz) bypasses React state entirely. Meters and
// needles draw from these channels in requestAnimationFrame through refs; text that changes
// rarely (a note name, a step index) subscribes through useLiveSelector and re-renders only
// when the selected value changes.
import { useEffect, useRef, useSyncExternalStore } from 'react';
import type { PitchEvent, TelemetryEvent } from '../bridge/protocol';

export class LiveChannel<T> {
  private value: T | null = null;
  private version = 0;
  private listeners = new Set<() => void>();

  publish(value: T): void {
    this.value = value;
    this.version += 1;
    this.listeners.forEach((fn) => fn());
  }

  latest(): T | null {
    return this.value;
  }

  getVersion(): number {
    return this.version;
  }

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  clear(): void {
    this.value = null;
    this.version += 1;
    this.listeners.forEach((fn) => fn());
  }
}

export const telemetry = new LiveChannel<TelemetryEvent>();
export const pitch = new LiveChannel<PitchEvent>();

const raf =
  typeof requestAnimationFrame === 'function'
    ? requestAnimationFrame
    : (fn: FrameRequestCallback) => setTimeout(() => fn(Date.now()), 16) as unknown as number;
const cancelRaf =
  typeof cancelAnimationFrame === 'function'
    ? cancelAnimationFrame
    : (id: number) => clearTimeout(id);

/**
 * Calls draw(latest, dtSeconds) once per animation frame while mounted. The callback reads the
 * latest value directly, so components using this never re-render for live data.
 */
export function useLiveFrame<T>(
  channel: LiveChannel<T>,
  draw: (value: T | null, dt: number) => void,
): void {
  const drawRef = useRef(draw);
  useEffect(() => {
    drawRef.current = draw;
  });
  useEffect(() => {
    let id = 0;
    let last = 0;
    const frame = (t: number) => {
      const dt = last ? Math.min(0.1, (t - last) / 1000) : 1 / 60;
      last = t;
      drawRef.current(channel.latest(), dt);
      id = raf(frame);
    };
    id = raf(frame);
    return () => cancelRaf(id);
  }, [channel]);
}

/**
 * Re-renders only when selector(latest) changes (compared with Object.is), so the selector must
 * return primitives or stable references.
 */
export function useLiveSelector<T, S>(
  channel: LiveChannel<T>,
  selector: (value: T | null) => S,
): S {
  return useSyncExternalStore(
    (fn) => channel.subscribe(fn),
    () => selector(channel.latest()),
    () => selector(null),
  );
}
