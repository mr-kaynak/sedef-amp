// SPDX-License-Identifier: AGPL-3.0-or-later
// The looper for the browser mock: the engine's states and its answer to every press
// (docs/looper.md, sections 4 and 5), in samples at the mock's 48 kHz, with the downbeats of the
// mock's click. No audio: what the panel shows is what the plug-in would report.
import type { LooperAction, LooperStateName, LooperTelemetry } from '../protocol';
import { stepBeats, stepsFor } from '../../lib/metronome';

const RATE = 48000;
const MIN_LOOP = Math.round(0.25 * RATE);
const MAX_LOOP = 120 * RATE;
const GRACE = Math.round(0.05 * RATE);

/** States a demo URL (?looper=) can open the looper in. */
export const LOOPER_DEMOS = [
  'empty',
  'armed',
  'recording',
  'closing',
  'playing',
  'overdubbing',
  'stopped',
] as const;
export type LooperDemo = (typeof LOOPER_DEMOS)[number];

export function isLooperDemo(value: unknown): value is LooperDemo {
  return typeof value === 'string' && (LOOPER_DEMOS as readonly string[]).includes(value);
}

/** What a footswitch asks of the looper (docs/live.md §4.5). */
export type LooperPress = 'main' | 'stop' | 'undo' | 'clear' | 'doubleTap';

export class LooperSim {
  private state: LooperStateName = 'unavailable';
  /** Overdub layers begun, and the state and count when the last footswitch main press acted. */
  private layers = 0;
  private tapState: LooperStateName = 'unavailable';
  private tapLayers = 0;
  private now = 0;
  private clickOn = false;
  private clickStart = 0;
  private lastDownbeat = -1;
  // The take
  private takeStart = 0;
  private takeSync = false;
  private barsSoFar = 0;
  private closeAt = -1;
  private stopAfterClose = false;
  // The loop
  private length = 0;
  private position = 0;
  private bars = 0;
  private synced = false;
  private waitBars = 0;
  private armedForPlay = false;
  private lastAction: 'none' | 'layer' | 'clear' = 'none';
  private undone = false;
  private cleared: { length: number; bars: number; synced: boolean } | null = null;

  constructor(private readonly value: (id: string) => number) {}

  open(): boolean {
    if (this.state === 'unavailable') this.state = 'empty';
    return true;
  }

  get ready(): boolean {
    return this.state !== 'unavailable';
  }

  private bar(): number {
    const signature = Math.round(this.value('metronomeSignature'));
    const step = (RATE * 60 * stepBeats(signature)) / Math.max(1, this.value('tempo'));
    return step * stepsFor(signature);
  }

  private nextDownbeat(after: number): number {
    const bar = this.bar();
    const k = Math.max(0, Math.ceil((after - this.clickStart) / bar));
    let d = Math.ceil(this.clickStart + k * bar);
    if (d <= after) d = Math.ceil(this.clickStart + (k + 1) * bar);
    return d;
  }

  /** Advances the simulated audio by dt seconds. */
  step(dt: number): void {
    if (this.state === 'unavailable') return;
    const on = this.value('metronomeEnabled') >= 0.5;
    if (on && !this.clickOn) {
      this.clickStart = this.now;
      this.lastDownbeat = this.now;
      this.onDownbeat(this.now);
    }
    this.clickOn = on;
    if (!on) {
      if (this.state === 'armed') this.cancelArm();
      else if (this.state === 'closing' && this.closeAt < 0) this.closeAt = this.now;
    }
    const end = this.now + Math.round(dt * RATE);
    // Every event moves time on or changes the state, so this ends.
    for (;;) {
      const events: number[] = [];
      if (on) events.push(this.nextDownbeat(this.now));
      if (this.state === 'closing' && this.closeAt >= 0)
        events.push(Math.max(this.now, this.closeAt));
      if (this.state === 'recording' || this.state === 'closing')
        events.push(this.takeStart + MAX_LOOP);
      const at = Math.min(...events.filter((e) => e < end));
      if (!Number.isFinite(at)) break;
      this.advance(at - this.now);
      if (this.state === 'closing' && this.closeAt >= 0 && at >= this.closeAt) {
        this.close(this.closeAt, this.closeAt - this.takeStart, 0, this.stopAfterClose);
      } else if (
        (this.state === 'recording' || this.state === 'closing') &&
        at >= this.takeStart + MAX_LOOP
      ) {
        this.close(at, MAX_LOOP, 0, this.stopAfterClose);
      } else {
        this.lastDownbeat = at;
        this.onDownbeat(at);
      }
    }
    this.advance(end - this.now);
  }

  private advance(samples: number): void {
    if (samples <= 0) return;
    if ((this.state === 'playing' || this.state === 'overdubbing') && this.length > 0)
      this.position = (this.position + samples) % this.length;
    this.now += samples;
  }

  private onDownbeat(t: number): void {
    switch (this.state) {
      case 'armed':
        this.waitBars -= 1;
        if (this.waitBars <= 0) {
          if (this.armedForPlay) this.play(0);
          else this.startTake(t);
        }
        break;
      case 'recording':
        if (this.takeSync && t > this.takeStart) this.barsSoFar += 1;
        break;
      case 'closing':
        if (this.takeSync && this.closeAt < 0 && t > this.takeStart) {
          this.barsSoFar += 1;
          this.close(t, t - this.takeStart, 0, this.stopAfterClose);
        }
        break;
      default:
        break;
    }
  }

  command(action: LooperAction): void {
    const t = this.now;
    switch (action) {
      case 'main':
        switch (this.state) {
          case 'empty': {
            if (!this.clickOn) return this.startTake(t);
            const late = this.lastDownbeat >= 0 && t - this.lastDownbeat <= GRACE;
            const countIn = this.value('looperCountIn') >= 0.5;
            if (!countIn && late) return this.startTake(this.lastDownbeat);
            this.state = 'armed';
            this.armedForPlay = false;
            this.waitBars = (countIn ? 1 : 0) + (late ? 0 : 1);
            return;
          }
          case 'armed':
            return this.cancelArm();
          case 'recording':
            return this.requestClose(t, false);
          case 'playing':
            this.state = 'overdubbing';
            this.lastAction = 'layer';
            this.undone = false;
            this.layers += 1;
            return;
          case 'overdubbing':
            this.state = 'playing';
            return;
          case 'stopped':
            if (!this.clickOn) return this.play(0);
            if (this.lastDownbeat >= 0 && t - this.lastDownbeat <= GRACE)
              return this.play((t - this.lastDownbeat) % this.length);
            this.state = 'armed';
            this.armedForPlay = true;
            this.waitBars = 1;
            return;
          default:
            return;
        }
      case 'stop':
        switch (this.state) {
          case 'armed':
            return this.cancelArm();
          case 'recording':
            return this.requestClose(t, true);
          case 'closing':
            this.stopAfterClose = true;
            return;
          case 'playing':
          case 'overdubbing':
            this.state = 'stopped';
            this.position = 0;
            return;
          default:
            return;
        }
      case 'undo':
      case 'redo': {
        const undo = action === 'undo';
        if (!(undo ? this.canUndo() : this.canRedo())) return;
        if (this.lastAction === 'clear') {
          if (undo && this.cleared) {
            ({ length: this.length, bars: this.bars, synced: this.synced } = this.cleared);
            this.state = 'stopped';
            this.position = 0;
            this.undone = true;
          } else {
            this.clear();
          }
          return;
        }
        if (this.state === 'overdubbing') this.state = 'playing';
        this.undone = undo;
        return;
      }
      case 'clear':
        switch (this.state) {
          case 'armed':
            if (this.armedForPlay) return this.clear();
            this.state = 'empty';
            return;
          case 'recording':
          case 'closing':
            this.state = 'empty';
            this.length = 0;
            return;
          case 'playing':
          case 'overdubbing':
          case 'stopped':
            return this.clear();
          default:
            return;
        }
    }
  }

  /** A footswitch press, resolved as the engine resolves it (Engine::footswitchCommands). */
  footswitch(press: LooperPress): void {
    switch (press) {
      case 'main':
        this.tapState = this.state;
        this.tapLayers = this.layers;
        return this.command('main');
      case 'undo':
        // One switch for both: redo when the last layer (or clear) was just undone.
        return this.command(!this.canUndo() && this.canRedo() ? 'redo' : 'undo');
      case 'stop':
      case 'clear':
        return this.command(press);
      case 'doubleTap': {
        // Stop, and take back what the first press started.
        const now = this.state;
        if (
          this.tapState === 'empty' &&
          (now === 'armed' || now === 'recording' || now === 'closing')
        )
          return this.command('clear');
        this.command('stop');
        if (this.layers !== this.tapLayers) this.command('undo');
        return;
      }
    }
  }

  private canUndo(): boolean {
    if (this.undone) return false;
    if (this.lastAction === 'clear') return this.state === 'empty';
    return (
      this.lastAction === 'layer' &&
      (this.state === 'playing' || this.state === 'overdubbing' || this.state === 'stopped')
    );
  }

  private canRedo(): boolean {
    if (!this.undone) return false;
    if (this.lastAction === 'clear') return this.state === 'stopped' || this.state === 'playing';
    return (
      this.lastAction === 'layer' &&
      (this.state === 'playing' || this.state === 'overdubbing' || this.state === 'stopped')
    );
  }

  private cancelArm(): void {
    this.state = this.armedForPlay ? 'stopped' : 'empty';
    this.waitBars = 0;
  }

  private startTake(start: number): void {
    this.state = 'recording';
    this.takeStart = start;
    this.takeSync = this.clickOn;
    this.barsSoFar = 0;
    this.closeAt = -1;
    this.stopAfterClose = false;
    this.length = 0;
    this.position = 0;
    this.lastAction = 'none';
    this.undone = false;
    this.cleared = null;
  }

  private requestClose(t: number, thenStop: boolean): void {
    if (this.takeSync && this.clickOn) {
      const a = this.lastDownbeat;
      if (this.barsSoFar >= 1 && a > this.takeStart && t - a < this.bar() / 2) {
        this.close(a, a - this.takeStart, t - a, thenStop);
        return;
      }
      this.state = 'closing';
      this.closeAt = -1;
      this.stopAfterClose = thenStop;
      return;
    }
    if (t - this.takeStart >= MIN_LOOP) {
      this.close(t, t - this.takeStart, 0, thenStop);
      return;
    }
    this.state = 'closing';
    this.closeAt = this.takeStart + MIN_LOOP;
    this.stopAfterClose = thenStop;
  }

  private close(at: number, length: number, from: number, thenStop: boolean): void {
    this.length = Math.max(1, Math.round(length));
    this.synced = this.takeSync && this.clickOn && at === this.lastDownbeat;
    this.bars = this.synced ? this.barsSoFar : 0;
    this.closeAt = -1;
    if (thenStop) {
      this.state = 'stopped';
      this.position = 0;
    } else {
      this.play(from);
    }
  }

  private play(from: number): void {
    this.state = 'playing';
    this.position = this.length > 0 ? from % this.length : 0;
  }

  private clear(): void {
    this.cleared = { length: this.length, bars: this.bars, synced: this.synced };
    this.length = 0;
    this.position = 0;
    this.bars = 0;
    this.synced = false;
    this.lastAction = 'clear';
    this.undone = false;
    this.state = 'empty';
  }

  telemetry(): LooperTelemetry {
    const taking = this.state === 'recording' || this.state === 'closing';
    return {
      state: this.state,
      length: taking ? Math.max(0, this.now - this.takeStart) : this.length,
      position: this.position,
      bars: taking ? (this.takeSync ? this.barsSoFar : 0) : this.synced ? this.bars : 0,
      waitBars: this.state === 'armed' ? this.waitBars : 0,
      armedForPlay: this.state === 'armed' && this.armedForPlay,
      synced: this.length > 0 && this.synced && !taking,
      canUndo: this.canUndo(),
      canRedo: this.canRedo(),
      undoIsClear: this.lastAction === 'clear',
    };
  }

  /** Demos and screenshots: the looper in a given state at once, a four-bar loop when it has one. */
  demo(state: LooperDemo): void {
    this.open();
    this.clickOn = this.value('metronomeEnabled') >= 0.5;
    const bar = Math.round(this.bar());
    const withLoop = () => {
      this.length = 4 * bar;
      this.bars = this.clickOn ? 4 : 0;
      this.synced = this.clickOn;
      this.lastAction = 'layer';
      this.undone = false;
    };
    switch (state) {
      case 'empty':
        this.state = 'empty';
        return;
      case 'armed':
        this.state = 'armed';
        this.armedForPlay = false;
        this.waitBars = 1;
        return;
      case 'recording':
      case 'closing':
        this.state = state;
        this.takeSync = this.clickOn;
        this.takeStart = this.now - Math.round(2.6 * bar);
        this.barsSoFar = this.clickOn ? 2 : 0;
        this.closeAt = -1;
        return;
      case 'playing':
      case 'overdubbing':
        withLoop();
        this.state = state;
        this.position = Math.round(1.4 * bar);
        return;
      case 'stopped':
        withLoop();
        this.state = 'stopped';
        this.position = 0;
        return;
    }
  }
}
