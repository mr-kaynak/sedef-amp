// SPDX-License-Identifier: AGPL-3.0-or-later
// The whole needle path on streams recorded from the real PitchTracker (fixtures/streams.json,
// docs/dsp/tuner.md "Display"): the engine's display fields at 60 Hz, the gauge's readout on each
// of them and the needle at 60 frames per second. These are the bars the display was designed to.
import { describe, expect, it } from 'vitest';
import { displayEvent, replay, type ReplayFrame } from '../../test/tunerReplay';
import { IN_TUNE_CENTS } from './cents';
import streams from './fixtures/streams.json';

interface Scenario {
  name: string;
  what: string;
  nominalMidi: number;
  sustainFrom: number;
  sustainTo: number;
  moveAt: number;
  moveEnd: number;
  rows: number[][];
}

const scenarios = streams.scenarios as Scenario[];

function play(name: string, fps = 60) {
  const s = scenarios.find((x) => x.name === name);
  if (!s) throw new Error(`no scenario ${name}`);
  const stream = s.rows.map(([t = 0, valid = 0, held = 0, settled = 0, note = 0, cents = 0]) => ({
    t,
    event: displayEvent(valid === 1, held === 1, settled === 1, note, cents),
  }));
  const truth = (t: number) => {
    // The recorded true pitch at the reading before t.
    let row = s.rows[0];
    for (const r of s.rows) {
      if ((r[0] ?? 0) > t) break;
      row = r;
    }
    return row?.[6] ?? 0;
  };
  const frames = replay(stream, { inTuneCents: IN_TUNE_CENTS, fps });
  // The needle relative to the note being tuned, as the true pitch is.
  const needle = (f: ReplayFrame) => f.needle + 100 * ((f.note ?? s.nominalMidi) - s.nominalMidi);
  const lastLive = Math.max(
    ...s.rows.filter((r) => r[1] === 1 && r[2] === 0).map((r) => r[0] ?? 0),
  );
  return { s, frames, truth, needle, lastLive };
}

/** RMS and peak of a series around its own centred moving average (fast movement only). */
function jitter(values: number[], half: number) {
  let sum = 0;
  let peak = 0;
  let n = 0;
  for (let i = half; i + half < values.length; i += 1) {
    let mean = 0;
    for (let k = -half; k <= half; k += 1) mean += values[i + k] ?? 0;
    mean /= 2 * half + 1;
    const d = (values[i] ?? 0) - mean;
    sum += d * d;
    peak = Math.max(peak, Math.abs(d));
    n += 1;
  }
  return { rms: Math.sqrt(sum / Math.max(1, n)), peak };
}

function flips(frames: ReplayFrame[], from = 0) {
  let notes = 0;
  let entries = 0;
  let exits = 0;
  let wrongSide = 0;
  let prev: ReplayFrame | null = null;
  for (const f of frames) {
    if (f.t < from) {
      prev = f;
      continue;
    }
    if (prev?.note != null && f.note != null && f.note !== prev.note) notes += 1;
    if (f.tune === 'in-tune' && prev?.tune !== 'in-tune') entries += 1;
    if (prev?.tune === 'in-tune' && (f.tune === 'sharp' || f.tune === 'flat')) exits += 1;
    if (f.tune === 'sharp' || f.tune === 'flat') wrongSide += 1;
    prev = f;
  }
  return { notes, entries, exits, wrongSide };
}

describe('Tuner needle on recorded tracker streams', () => {
  it('is rock-steady on a sustained note, even with hum 28 dB under it', () => {
    for (const name of ['e2-hum', 'b3-edge']) {
      const { s, frames, needle, truth } = play(name);
      const sustain = frames.filter((f) => f.t >= s.sustainFrom && f.t <= s.sustainTo);
      const j = jitter(sustain.map(needle), 9);
      const err = Math.sqrt(
        sustain.reduce((a, f) => a + (needle(f) - truth(f.t)) ** 2, 0) / sustain.length,
      );
      expect(j.rms, name).toBeLessThan(0.1);
      expect(j.peak, name).toBeLessThan(0.3);
      expect(err, name).toBeLessThan(0.3);
    }
  });

  it('is never busier than a string that wobbles by a cent', () => {
    const { s, frames, needle, truth } = play('g3-real');
    const sustain = frames.filter((f) => f.t >= s.sustainFrom && f.t <= s.sustainTo);
    const shown = jitter(sustain.map(needle), 9);
    const real = jitter(
      sustain.map((f) => truth(f.t)),
      9,
    );
    expect(shown.rms).toBeLessThan(real.rms);
  });

  it('follows a 20-cent step within 1 cent in under 250 ms, without overshoot', () => {
    const { s, frames, needle, truth } = play('g3-step');
    const after = frames.filter((f) => f.t >= s.moveEnd && f.t <= s.sustainTo);
    const final = truth(s.moveEnd + 0.5);
    const late = after.filter((f) => Math.abs(needle(f) - truth(f.t)) > 1);
    const settledAt = (late[late.length - 1]?.t ?? s.moveEnd) - s.moveEnd;
    expect(settledAt).toBeLessThan(0.25);
    expect(Math.max(...after.map((f) => needle(f) - final))).toBeLessThan(0.2);
  });

  it('keeps up with a turning peg and comes to rest on the pitch', () => {
    const { s, frames, needle, truth } = play('a2-peg');
    const turning = frames.filter((f) => f.t >= s.moveAt && f.t <= s.moveEnd);
    const lag = Math.max(...turning.map((f) => Math.abs(needle(f) - truth(f.t))));
    expect(lag).toBeLessThan(3); // the 0.2.0 needle: 3.1 cents behind
    const after = frames.filter((f) => f.t >= s.moveEnd && f.t <= s.sustainTo);
    const late = after.filter((f) => Math.abs(needle(f) - truth(f.t)) > 1);
    expect((late[late.length - 1]?.t ?? s.moveEnd) - s.moveEnd).toBeLessThan(0.25);
    expect(Math.max(...after.map((f) => needle(f) - truth(s.moveEnd + 0.5)))).toBeLessThan(0.2);
  });

  it('never flickers between notes or in and out of tune', () => {
    const inTune = ['e2-hum', 'b3-edge', 'a2-peg', 'g3-step', 'e4-repluck'];
    for (const name of inTune) {
      const { frames } = play(name);
      const f = flips(frames, name === 'e4-repluck' ? 0.2 : 0);
      expect(f.notes, name).toBe(0);
      expect(f.entries, name).toBe(1);
      expect(f.exits, name).toBe(0);
    }
    // In tune from the pick: no sharp or flat on the way (the attack reads sharp).
    for (const name of ['e2-hum', 'b3-edge']) expect(flips(play(name).frames).wrongSide).toBe(0);
    // Out of tune: never shown in tune, not even for a moment during the attack.
    for (const name of ['d3-dies', 'a2-after-e2']) {
      const f = flips(play(name).frames, 0.01);
      expect(f.entries, name).toBe(0);
      expect(f.notes, name).toBe(name === 'a2-after-e2' ? 1 : 0); // E2 to A2, once
    }
    // A string sagging flat as it decays leaves the zone once, for good.
    const real = flips(play('g3-real').frames);
    expect(real.entries).toBe(1);
    expect(real.exits).toBeLessThanOrEqual(1);
  });

  it('holds the last reading when the note dies', () => {
    const { frames, needle, lastLive } = play('d3-dies');
    const tail = frames.filter((f) => f.t >= lastLive && f.t <= lastLive + 1);
    const at = tail[0] ? needle(tail[0]) : 0;
    expect(Math.max(...tail.map((f) => Math.abs(needle(f) - at)))).toBeLessThan(0.1);
    expect(tail.every((f) => f.note === 50)).toBe(true);
    expect(tail[tail.length - 1]?.held).toBe(true);
  });

  it('changes the numbers rarely while the note holds', () => {
    const { s, frames } = play('e2-hum');
    const sustain = frames.filter((f) => f.t >= s.sustainFrom && f.t <= s.sustainTo);
    let changes = 0;
    for (let i = 1; i < sustain.length; i += 1)
      if (sustain[i]?.centsText !== sustain[i - 1]?.centsText) changes += 1;
    expect(changes).toBeLessThanOrEqual(2);
    expect(sustain[sustain.length - 1]?.centsText).toMatch(/^[+−]?0\.\d¢$/);
  });

  it('behaves the same at 120 frames per second', () => {
    const { s, frames, needle } = play('e2-hum', 120);
    const sustain = frames.filter((f) => f.t >= s.sustainFrom && f.t <= s.sustainTo);
    expect(jitter(sustain.map(needle), 18).rms).toBeLessThan(0.1);
  });
});
