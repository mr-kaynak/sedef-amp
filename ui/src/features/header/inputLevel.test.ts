// SPDX-License-Identifier: AGPL-3.0-or-later
// The input level verdict (docs/ui/input-level.md): each state, the windows, the hold, the Input
// knob, the hysteresis and silence, from synthetic telemetry at 30 events per second.
import { afterEach, describe, expect, it } from 'vitest';
import { INPUT_LEVEL, InputLevelJudge, startInputLevel, useInputLevel } from './inputLevel';
import { setLocalValue } from '../../params/paramStore';
import { telemetry } from '../../state/live';
import type { TelemetryEvent } from '../../bridge/protocol';

const RATE = 30;
const FLOOR = -100;

/** Feeds `seconds` of events; `level(t)` gives the peak after Input at time t (or channels). */
function run(
  judge: InputLevelJudge,
  from: number,
  seconds: number,
  level: (t: number) => number | number[],
  gain = 0,
) {
  const n = Math.round(seconds * RATE);
  let state = judge.state;
  for (let i = 1; i <= n; i += 1) {
    const t = from + i / RATE;
    const l = level(t);
    state = judge.push(Array.isArray(l) ? l : [l], gain, t);
  }
  return { state, end: from + n / RATE };
}

/** A strum every `every` seconds peaking at `peak`, the noise floor between. */
const strums =
  (peak: number, every = 1.5, noise = -80) =>
  (t: number) =>
    t % every < 1 / RATE ? peak : noise;

describe('InputLevelJudge', () => {
  it('waits until something is played', () => {
    const j = new InputLevelJudge();
    expect(j.state).toBe('waiting');
    expect(run(j, 0, 2, () => -80).state).toBe('waiting');
    expect(j.loudestDb(0)).toBeNull();
  });

  it('calls strums peaking in the zone good, and under it quiet', () => {
    expect(run(new InputLevelJudge(), 0, 4, strums(-6)).state).toBe('good');
    expect(run(new InputLevelJudge(), 0, 4, strums(-11.9)).state).toBe('good');
    expect(run(new InputLevelJudge(), 0, 4, strums(-12.1)).state).toBe('quiet');
    // The musician's own interface: peaks at −30 dBFS.
    expect(run(new InputLevelJudge(), 0, 4, strums(-30)).state).toBe('quiet');
  });

  it('calls 0 dBFS after Input hot, and the interface at full scale clipping', () => {
    expect(run(new InputLevelJudge(), 0, 2, strums(-0.1), 2).state).toBe('good'); // raw −2.1
    expect(run(new InputLevelJudge(), 0, 2, strums(1), 2).state).toBe('hot'); // raw −1
    expect(run(new InputLevelJudge(), 0, 2, strums(0)).state).toBe('clip'); // raw 0
    expect(run(new InputLevelJudge(), 0, 2, strums(-0.4)).state).toBe('clip');
    // Turning Input down does not hide a clipping interface.
    expect(run(new InputLevelJudge(), 0, 2, strums(-12), -12).state).toBe('clip');
  });

  it('judges by the louder of two channels', () => {
    const j = new InputLevelJudge();
    expect(run(j, 0, 2, (t) => (t % 1.5 < 1 / RATE ? [-40, -5] : [-80, -80])).state).toBe('good');
  });

  it('remembers the loudest playing for 8 s, so a soft passage stays good', () => {
    const j = new InputLevelJudge();
    let r = run(j, 0, 1, strums(-5, 1)); // one hard strum at t = 1
    expect(r.state).toBe('good');
    r = run(j, r.end, 6.9, strums(-25, 0.5)); // soft playing until t = 7.9
    expect(r.state).toBe('good');
    r = run(j, r.end, 0.2, strums(-25, 0.1)); // the hard strum leaves the window at t = 9
    expect(r.state).toBe('good');
    r = run(j, r.end, 1, strums(-25, 0.1));
    expect(r.state).toBe('quiet');
  });

  it('clears hot after 3 s of quieter playing', () => {
    const j = new InputLevelJudge();
    let r = run(j, 0, 1, strums(2, 1), 4); // raw −2, after Input +2
    expect(r.state).toBe('hot');
    r = run(j, r.end, 1.9, strums(-6, 0.5), 4);
    expect(r.state).toBe('hot');
    r = run(j, r.end, 1.2, strums(-6, 0.5), 4);
    expect(r.state).toBe('good');
  });

  it('holds the verdict when the playing stops, and moves it with the Input knob', () => {
    const j = new InputLevelJudge();
    let r = run(j, 0, 2, strums(-15));
    expect(r.state).toBe('quiet');
    r = run(j, r.end, 20, () => -80); // nothing played for 20 s
    expect(r.state).toBe('quiet');
    expect(j.loudestDb(0)).toBeCloseTo(-15);
    // Input up by 6 dB, between strums: the held peak is now −9 dBFS.
    expect(j.judge(6)).toBe('good');
    expect(j.loudestDb(6)).toBeCloseTo(-9);
    // The next events carry that Input, and the peaks they bring are measured after it.
    r = run(j, r.end, 1, () => -74, 6);
    expect(r.state).toBe('good');
    expect(j.judge(18)).toBe('hot');
  });

  it('keeps 1 dB of hysteresis at the bottom of the zone', () => {
    const j = new InputLevelJudge();
    expect(run(j, 0, 2, strums(-13)).state).toBe('quiet');
    // −11.5: over −12 but under −11, so a quiet verdict stays quiet …
    expect(j.judge(1.5)).toBe('quiet');
    // … until the level reaches −11.
    expect(j.judge(2)).toBe('good');
    // From good, −11.5 stays good.
    expect(j.judge(1.5)).toBe('good');
    expect(j.judge(0.9)).toBe('quiet');
  });

  it('says no signal after 3 s of digital silence, and starts over when sound returns', () => {
    const j = new InputLevelJudge();
    let r = run(j, 0, 2, strums(-6));
    expect(r.state).toBe('good');
    r = run(j, r.end, INPUT_LEVEL.silenceS - 0.1, () => FLOOR);
    expect(r.state).toBe('good');
    r = run(j, r.end, 0.2, () => FLOOR);
    expect(r.state).toBe('none');
    expect(j.loudestDb(0)).toBeNull();
    // A knob turn during silence changes nothing.
    expect(j.judge(12)).toBe('none');
    // Noise comes back: not judged yet.
    r = run(j, r.end, 1, () => -85);
    expect(r.state).toBe('waiting');
    r = run(j, r.end, 2, strums(-30));
    expect(r.state).toBe('quiet');
  });

  it('treats a pause in the noise as no silence', () => {
    const j = new InputLevelJudge();
    // Floor events broken by one noise event every 2 s never add up to 3 s.
    const r = run(j, 0, 10, (t) => (t % 2 < 1 / RATE ? -90 : FLOOR));
    expect(r.state).toBe('waiting');
  });
});

describe('startInputLevel', () => {
  const event = (inputDb: number[]): TelemetryEvent => ({
    inputDb,
    outputDb: [-90],
    inputClip: false,
    outputClip: false,
    gateOpen: true,
    gateReductionDb: 0,
    tideReductionDb: 0,
    metronomeStep: -1,
    driftTimeMs: 500,
    cpu: 0.05,
  });
  let stop: (() => void) | null = null;
  afterEach(() => {
    stop?.();
    stop = null;
    setLocalValue('inputGain', 0);
  });

  it('publishes the verdict and the loudest peak from telemetry and the Input knob', () => {
    let now = 0;
    stop = startInputLevel(() => now);
    expect(useInputLevel.getState()).toEqual({ state: 'waiting', loudestDb: null });
    now = 0.1;
    telemetry.publish(event([-20.4]));
    expect(useInputLevel.getState()).toEqual({ state: 'quiet', loudestDb: -20 });
    setLocalValue('inputGain', 12);
    expect(useInputLevel.getState()).toEqual({ state: 'good', loudestDb: -8 });
  });
});
