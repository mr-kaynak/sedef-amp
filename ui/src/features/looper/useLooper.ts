// SPDX-License-Identifier: AGPL-3.0-or-later
// The looper's state from telemetry, re-rendering only when what is shown changes.
import { useMemo } from 'react';
import type { LooperTelemetry } from '../../bridge/protocol';
import { telemetry, useLiveSelector } from '../../state/live';
import { IDLE, stateDetail } from './model';

/**
 * The looper's frame without its position, which moves at 30 Hz (the ring draws that itself). A
 * take's growing length is kept to tenths of a second, as the panel shows it.
 */
export function useLooper(rate: number): LooperTelemetry {
  const tenth = Math.max(1, Math.round(rate / 10));
  const key = useLiveSelector(telemetry, (e) => {
    const l = e?.looper ?? IDLE;
    const taking = l.state === 'recording' || l.state === 'closing';
    return [
      l.state,
      taking ? l.length - (l.length % tenth) : l.length,
      l.bars,
      l.waitBars,
      l.armedForPlay,
      l.synced,
      l.canUndo,
      l.canRedo,
      l.undoIsClear,
    ].join('|');
  });
  return useMemo(() => {
    const [state, length, bars, waitBars, armedForPlay, synced, canUndo, canRedo, undoIsClear] =
      key.split('|');
    return {
      state: (state ?? 'unavailable') as LooperTelemetry['state'],
      length: Number(length),
      position: 0,
      bars: Number(bars),
      waitBars: Number(waitBars),
      armedForPlay: armedForPlay === 'true',
      synced: synced === 'true',
      canUndo: canUndo === 'true',
      canRedo: canRedo === 'true',
      undoIsClear: undoIsClear === 'true',
    };
  }, [key]);
}

/**
 * The line under the state ("Bar 2 of 4 · 8.2 s"), which moves with the position: it re-renders
 * only when its text changes. `steps` is the click's steps per bar while it runs.
 */
export function useLooperDetail(rate: number, steps: number | null): string {
  return useLiveSelector(telemetry, (e) =>
    stateDetail(e?.looper ?? IDLE, rate, steps ? { step: e?.metronomeStep ?? -1, steps } : null),
  );
}
