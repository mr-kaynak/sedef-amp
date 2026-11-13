// SPDX-License-Identifier: AGPL-3.0-or-later
// What the looper panel says and draws for a telemetry frame (docs/looper.md). Pure, so the
// wording and the ring are tested without a backend.
import type { LooperStateName, LooperTelemetry } from '../../bridge/protocol';

export const IDLE: LooperTelemetry = {
  state: 'unavailable',
  length: 0,
  position: 0,
  bars: 0,
  waitBars: 0,
  armedForPlay: false,
  synced: false,
  canUndo: false,
  canRedo: false,
  undoIsClear: false,
};

/** The colour family of a state: recording red, overdub amber, playing nacre. */
export type Tone = 'idle' | 'wait' | 'record' | 'play' | 'dub';

export function toneOf(state: LooperStateName): Tone {
  switch (state) {
    case 'recording':
    case 'closing':
      return 'record';
    case 'playing':
      return 'play';
    case 'overdubbing':
      return 'dub';
    case 'armed':
      return 'wait';
    default:
      return 'idle';
  }
}

export type MainIcon = 'record' | 'play' | 'overdub' | 'close';

/** What the main button does next, in the engine's own rules (docs/looper.md, section 4). */
export function mainAction(t: LooperTelemetry): {
  label: string;
  icon: MainIcon;
  disabled: boolean;
} {
  switch (t.state) {
    case 'unavailable': // the first press reserves the memory and records
    case 'empty':
      return { label: 'Record', icon: 'record', disabled: false };
    case 'armed':
      return {
        label: t.armedForPlay ? 'Cancel playing' : 'Cancel recording',
        icon: 'close',
        disabled: false,
      };
    case 'recording':
      return { label: 'Close the loop and play', icon: 'play', disabled: false };
    case 'closing':
      return { label: 'Closing at the bar line', icon: 'play', disabled: true };
    case 'playing':
      return { label: 'Overdub', icon: 'overdub', disabled: false };
    case 'overdubbing':
      return { label: 'Stop overdubbing and play', icon: 'play', disabled: false };
    case 'stopped':
      return { label: 'Play', icon: 'play', disabled: false };
  }
}

/** The state as a word or two, for the panel's heading and the header button's name. */
export function stateTitle(t: LooperTelemetry): string {
  switch (t.state) {
    case 'unavailable':
      return 'Getting ready';
    case 'empty':
      return 'Ready to record';
    case 'armed':
      if (t.armedForPlay) return 'Plays on the 1';
      return t.waitBars > 1 ? 'Count-in next bar' : 'Records on the 1';
    case 'recording':
      return 'Recording';
    case 'closing':
      return 'Closing at the bar line';
    case 'playing':
      return 'Playing';
    case 'overdubbing':
      return 'Overdubbing';
    case 'stopped':
      return 'Stopped';
  }
}

/** Seconds to a tenth, rounded down as a clock is: "8.2". */
function tenths(samples: number, rate: number): string {
  const s = rate > 0 ? samples / rate : 0;
  return (Math.floor(s * 10 + 1e-9) / 10).toFixed(1);
}

/** Seconds with one decimal: "8.2 s". */
export function seconds(samples: number, rate: number): string {
  return `${tenths(samples, rate)} s`;
}

function bars(n: number): string {
  return `${n} ${n === 1 ? 'bar' : 'bars'}`;
}

/** Where things are: "Bar 2 of 4 · 8.2 s", "Bar 3 · 5.4 s", "3.1 of 8.2 s". */
export function stateDetail(
  t: LooperTelemetry,
  rate: number,
  beat: { step: number; steps: number } | null,
): string {
  switch (t.state) {
    case 'unavailable':
      return 'Reserving memory for the loop';
    case 'empty':
      return '';
    case 'armed': {
      if (!beat || beat.step < 0) return t.waitBars > 1 ? 'A bar of clicks first' : '';
      const left = (t.waitBars - 1) * beat.steps + (beat.steps - beat.step);
      return left === 1 ? 'Starts on the next beat' : `Starts in ${left} beats`;
    }
    case 'recording':
    case 'closing':
      return t.bars > 0 || beat
        ? `Bar ${t.bars + 1} · ${seconds(t.length, rate)}`
        : seconds(t.length, rate);
    case 'playing':
    case 'overdubbing':
    case 'stopped': {
      const total = seconds(t.length, rate);
      if (t.state === 'stopped') return t.bars > 0 ? `${bars(t.bars)} · ${total}` : total;
      if (t.bars > 0) {
        const bar = Math.min(t.bars, Math.floor((t.position / Math.max(1, t.length)) * t.bars) + 1);
        return `Bar ${bar} of ${t.bars} · ${total}`;
      }
      return `${tenths(t.position, rate)} of ${total}`;
    }
  }
}

/** What to do next, in one line under the state. */
export function stateHint(t: LooperTelemetry, clickRunning: boolean): string {
  switch (t.state) {
    case 'unavailable':
      return '';
    case 'empty':
      return clickRunning
        ? 'The click is on: the loop snaps to whole bars.'
        : 'The loop is as long as you play. Start the click to snap to bars.';
    case 'armed':
      return 'Press again to cancel.';
    case 'recording':
      return clickRunning
        ? 'Press to close the loop at the nearest bar line.'
        : 'Press to close the loop and play it.';
    case 'closing':
      return 'It plays from the next 1.';
    case 'playing':
      return 'Press to add a part on top.';
    case 'overdubbing':
      return 'Press to stop adding and keep playing.';
    case 'stopped':
      return clickRunning ? 'Press to play from the next 1.' : 'Press to play from the start.';
  }
}

/** The ring around the main button, 0..1: the loop's position, or the current bar of a take. */
export function ringProgress(t: LooperTelemetry, barSamples: number | null): number {
  switch (t.state) {
    case 'playing':
    case 'overdubbing':
      return t.length > 0 ? t.position / t.length : 0;
    case 'recording':
    case 'closing':
      if (barSamples && barSamples > 0) return (t.length % barSamples) / barSamples;
      return 1;
    default:
      return 0;
  }
}

/** A polite announcement when the state changes, or null when there is nothing to say. */
export function announcement(prev: LooperTelemetry | null, t: LooperTelemetry): string | null {
  if (prev && prev.state === t.state) return null;
  switch (t.state) {
    case 'armed':
      return t.armedForPlay ? 'Plays on the next 1' : 'Waiting for the 1 to record';
    case 'recording':
      return 'Recording';
    case 'closing':
      return 'Closing at the bar line';
    case 'playing':
      if (prev?.state === 'overdubbing') return 'Playing';
      return t.bars > 0 ? `Playing, ${bars(t.bars)}` : 'Playing';
    case 'overdubbing':
      return 'Overdubbing';
    case 'stopped':
      return prev && prev.state !== 'unavailable' ? 'Stopped' : null;
    case 'empty':
      return prev && prev.state !== 'unavailable' ? 'Looper empty' : null;
    default:
      return null;
  }
}
