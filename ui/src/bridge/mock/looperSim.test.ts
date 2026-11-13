// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { LooperSim } from './looperSim';

const RATE = 48000;

function sim(values: Record<string, number> = {}) {
  const all: Record<string, number> = {
    metronomeEnabled: 0,
    metronomeSignature: 2, // 4/4
    tempo: 120, // a bar is 2 s: 96000 samples
    looperCountIn: 0,
    ...values,
  };
  const s = new LooperSim((id) => all[id] ?? 0);
  return { s, all };
}

describe('the mock looper', () => {
  it('is unavailable until opened', () => {
    const { s } = sim();
    expect(s.telemetry().state).toBe('unavailable');
    s.command('main');
    expect(s.telemetry().state).toBe('unavailable');
    expect(s.open()).toBe(true);
    expect(s.telemetry().state).toBe('empty');
  });

  it('records free, closes and plays, overdubs, undoes and clears with undo', () => {
    const { s } = sim();
    s.open();
    s.command('main');
    expect(s.telemetry().state).toBe('recording');
    s.step(1.5);
    expect(s.telemetry().length).toBe(1.5 * RATE);
    s.command('main');
    expect(s.telemetry()).toMatchObject({ state: 'playing', length: 1.5 * RATE, bars: 0 });
    s.step(1);
    expect(s.telemetry().position).toBe(RATE);
    s.command('main');
    expect(s.telemetry()).toMatchObject({ state: 'overdubbing', canUndo: true });
    s.command('main');
    s.command('undo');
    expect(s.telemetry()).toMatchObject({ state: 'playing', canUndo: false, canRedo: true });
    s.command('clear');
    expect(s.telemetry()).toMatchObject({ state: 'empty', canUndo: true, undoIsClear: true });
    s.command('undo');
    expect(s.telemetry()).toMatchObject({ state: 'stopped', length: 1.5 * RATE });
  });

  it('waits for the 1 with the click on, and closes on the nearest bar line', () => {
    const { s } = sim({ metronomeEnabled: 1 });
    s.open();
    s.step(0.5); // the click started at 0: downbeats at 0, 2, 4 s
    s.command('main');
    expect(s.telemetry()).toMatchObject({ state: 'armed', waitBars: 1 });
    s.step(1.6); // past the downbeat at 2 s
    expect(s.telemetry().state).toBe('recording');
    s.step(3.2); // 5.3 s: past half of the take's second bar
    s.command('main');
    expect(s.telemetry().state).toBe('closing');
    s.step(1.5); // the downbeat at 6 s closes it
    expect(s.telemetry()).toMatchObject({
      state: 'playing',
      bars: 2,
      synced: true,
      length: 4 * RATE,
    });
  });

  it('counts a bar in when asked', () => {
    const { s } = sim({ metronomeEnabled: 1, looperCountIn: 1 });
    s.open();
    s.step(0.5);
    s.command('main');
    expect(s.telemetry()).toMatchObject({ state: 'armed', waitBars: 2 });
    s.step(1.6);
    expect(s.telemetry()).toMatchObject({ state: 'armed', waitBars: 1 });
    s.step(2);
    expect(s.telemetry().state).toBe('recording');
  });

  it('opens in a demo state for screenshots', () => {
    const { s } = sim({ metronomeEnabled: 1 });
    s.demo('playing');
    expect(s.telemetry()).toMatchObject({ state: 'playing', bars: 4, synced: true, canUndo: true });
    s.demo('recording');
    expect(s.telemetry()).toMatchObject({ state: 'recording', bars: 2 });
  });
});
