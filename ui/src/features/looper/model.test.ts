// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import type { LooperTelemetry } from '../../bridge/protocol';
import {
  announcement,
  IDLE,
  mainAction,
  ringProgress,
  seconds,
  stateDetail,
  stateHint,
  stateTitle,
  toneOf,
} from './model';

const at = (patch: Partial<LooperTelemetry>): LooperTelemetry => ({ ...IDLE, ...patch });
const RATE = 48000;

describe('the looper panel model', () => {
  it('names what the main button does next, as the engine decides it', () => {
    // Not yet reserved: the first press reserves the memory and records.
    expect(mainAction(at({ state: 'unavailable' }))).toMatchObject({
      label: 'Record',
      disabled: false,
    });
    expect(mainAction(at({ state: 'empty' }))).toMatchObject({ label: 'Record', icon: 'record' });
    expect(mainAction(at({ state: 'armed' }))).toMatchObject({
      label: 'Cancel recording',
      icon: 'close',
    });
    expect(mainAction(at({ state: 'armed', armedForPlay: true })).label).toBe('Cancel playing');
    expect(mainAction(at({ state: 'recording' })).label).toBe('Close the loop and play');
    expect(mainAction(at({ state: 'closing' }))).toMatchObject({ disabled: true });
    expect(mainAction(at({ state: 'playing' }))).toMatchObject({
      label: 'Overdub',
      icon: 'overdub',
    });
    expect(mainAction(at({ state: 'overdubbing' })).label).toBe('Stop overdubbing and play');
    expect(mainAction(at({ state: 'stopped' }))).toMatchObject({ label: 'Play', icon: 'play' });
  });

  it('colours recording red, overdub amber, playing nacre', () => {
    expect(toneOf('recording')).toBe('record');
    expect(toneOf('closing')).toBe('record');
    expect(toneOf('overdubbing')).toBe('dub');
    expect(toneOf('playing')).toBe('play');
    expect(toneOf('armed')).toBe('wait');
    expect(toneOf('stopped')).toBe('idle');
  });

  it('names the drums, not the click, when the metronome plays drums', () => {
    const empty = at({ state: 'empty' });
    expect(stateHint(empty, true)).toBe('The click is on: the loop snaps to whole bars.');
    expect(stateHint(empty, true, true)).toBe('The drums are on: the loop snaps to whole bars.');
    expect(stateHint(empty, false, true)).toBe(
      'The loop is as long as you play. Start the drums to snap to bars.',
    );
  });

  it('says what waits: the count-in bar, then the beats to the 1', () => {
    expect(stateTitle(at({ state: 'armed', waitBars: 2 }))).toBe('Count-in next bar');
    expect(stateTitle(at({ state: 'armed', waitBars: 1 }))).toBe('Records on the 1');
    expect(stateTitle(at({ state: 'armed', waitBars: 1, armedForPlay: true }))).toBe(
      'Plays on the 1',
    );
    // 4/4, on beat 2 of the last bar: three beats to go.
    expect(stateDetail(at({ state: 'armed', waitBars: 1 }), RATE, { step: 1, steps: 4 })).toBe(
      'Starts in 3 beats',
    );
    expect(stateDetail(at({ state: 'armed', waitBars: 1 }), RATE, { step: 3, steps: 4 })).toBe(
      'Starts on the next beat',
    );
    // With the count-in bar still ahead, a whole bar more.
    expect(stateDetail(at({ state: 'armed', waitBars: 2 }), RATE, { step: 1, steps: 4 })).toBe(
      'Starts in 7 beats',
    );
  });

  it('gives a take in bars and seconds, a free one in seconds', () => {
    const take = at({ state: 'recording', length: 2.55 * RATE, bars: 1 });
    expect(stateDetail(take, RATE, { step: 2, steps: 4 })).toBe('Bar 2 · 2.5 s');
    expect(stateDetail({ ...take, bars: 0 }, RATE, null)).toBe('2.5 s');
  });

  it('places a playing loop in its bars, or in seconds when free', () => {
    const loop = at({ state: 'playing', length: 8 * RATE, position: 2.5 * RATE, bars: 4 });
    expect(stateDetail(loop, RATE, null)).toBe('Bar 2 of 4 · 8.0 s');
    expect(stateDetail({ ...loop, bars: 0 }, RATE, null)).toBe('2.5 of 8.0 s');
    expect(stateDetail({ ...loop, state: 'stopped', position: 0 }, RATE, null)).toBe(
      '4 bars · 8.0 s',
    );
    expect(seconds(0, RATE)).toBe('0.0 s');
  });

  it('fills the ring with the loop, or with the current bar of a take', () => {
    expect(ringProgress(at({ state: 'playing', length: 1000, position: 250 }), null)).toBe(0.25);
    expect(ringProgress(at({ state: 'recording', length: 1500 }), 1000)).toBe(0.5);
    expect(ringProgress(at({ state: 'recording', length: 1500 }), null)).toBe(1);
    expect(ringProgress(at({ state: 'stopped', length: 1000 }), null)).toBe(0);
  });

  it('announces each new state once, in words', () => {
    const empty = at({ state: 'empty' });
    expect(announcement(null, empty)).toBeNull();
    expect(announcement(empty, at({ state: 'recording' }))).toBe('Recording');
    expect(announcement(at({ state: 'recording' }), at({ state: 'recording' }))).toBeNull();
    expect(announcement(at({ state: 'recording' }), at({ state: 'playing', bars: 4 }))).toBe(
      'Playing, 4 bars',
    );
    expect(announcement(at({ state: 'overdubbing' }), at({ state: 'playing', bars: 4 }))).toBe(
      'Playing',
    );
    expect(announcement(empty, at({ state: 'armed' }))).toBe('Waiting for the 1 to record');
  });
});
