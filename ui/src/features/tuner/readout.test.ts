// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { createRandom, gaussian } from '../../bridge/mock/random';
import { FADE_SECONDS, HELD_OPACITY, HOLD_SECONDS, NeedleSpring, needleOpacity } from './needle';
import {
  ENTER_SECONDS,
  LEAVE_SECONDS,
  TEXT_SECONDS,
  TunerReadout,
  type ReadoutInput,
  type TuneState,
} from './readout';

const HZ = 60;
const reading = (cents: number, extra: Partial<ReadoutInput> = {}): ReadoutInput => ({
  note: 40,
  cents,
  hz: 82.41 * Math.pow(2, cents / 1200),
  settled: true,
  held: false,
  ...extra,
});

/** Feeds `seconds` of readings at 60 Hz from `t0`; returns the states seen. */
function feed(
  r: TunerReadout,
  t0: number,
  seconds: number,
  at: (t: number) => ReadoutInput | null,
): TuneState[] {
  const states: TuneState[] = [];
  for (let i = 0; i < Math.round(seconds * HZ); i += 1) {
    const t = t0 + i / HZ;
    states.push(r.update(at(t), t).tune);
  }
  return states;
}

const changes = (xs: unknown[]) => xs.filter((x, i) => i > 0 && x !== xs[i - 1]).length;

describe('TunerReadout', () => {
  it('confirms in tune only after a settled reading has held in the zone', () => {
    const r = new TunerReadout(1);
    // The attack: unsettled and a few cents sharp, as a plucked string reads.
    const attack = feed(r, 0, 0.25, (t) => reading(4 * Math.exp(-t / 0.08), { settled: false }));
    expect(attack.every((s) => s === 'listening')).toBe(true);
    const states = feed(r, 0.25, 1, () => reading(0.3));
    const enteredAt = states.indexOf('in-tune') / HZ;
    expect(enteredAt).toBeGreaterThanOrEqual(ENTER_SECONDS - 1 / HZ);
    expect(enteredAt).toBeLessThan(ENTER_SECONDS + 2 / HZ);
  });

  it('does not flicker at the edge of the zone', () => {
    // A settled reading wandering around +1 cent, the edge of the tuner's zone, for ten seconds.
    const random = createRandom(7);
    const r = new TunerReadout(1);
    const states = feed(r, 0, 10, () => reading(1 + 0.15 * gaussian(random)));
    expect(changes(states)).toBeLessThanOrEqual(1);
  });

  it('leaves in tune promptly when the peg is turned away, and stays out', () => {
    const r = new TunerReadout(1);
    feed(r, 0, 1, () => reading(0));
    const states = feed(r, 1, 1, (t) => reading(Math.min(8, (t - 1) * 20)));
    const leftAt = states.findIndex((s) => s === 'sharp') / HZ;
    // Out of the zone and its margin at 65 ms, then LEAVE_SECONDS (sooner once far out).
    expect(leftAt).toBeGreaterThan(0.1);
    expect(leftAt).toBeLessThan(0.065 + LEAVE_SECONDS + 1 / HZ);
    expect(changes(states)).toBe(1);
  });

  it('never calls a string in the zone sharp because of its attack', () => {
    const r = new TunerReadout(1);
    // Seven cents sharp for 120 ms after the pick, then settling to pitch.
    const states = feed(r, 0, 1.2, (t) =>
      t < 0.12 ? reading(7, { settled: false }) : reading(0.2, { settled: t > 0.35 }),
    );
    expect(states).not.toContain('sharp');
    expect(states[states.length - 1]).toBe('in-tune');
  });

  it('calls a badly tuned string flat before it settles', () => {
    const r = new TunerReadout(1);
    const states = feed(r, 0, 0.4, () => reading(-15, { settled: false }));
    expect(states.indexOf('flat') / HZ).toBeLessThan(0.2);
  });

  it('keeps in tune and the tenths through a re-pluck of the same note', () => {
    const r = new TunerReadout(1);
    feed(r, 0, 1, () => reading(0.4));
    expect(r.current.centsText).toBe('+0.4¢');
    // The engine holds the needle still and unsettled while the new pluck's long window fills.
    const states = feed(r, 1, 0.3, () => reading(0.4, { settled: false }));
    expect(states.every((s) => s === 'in-tune')).toBe(true);
    expect(r.current.centsText).toBe('+0.4¢');
  });

  it('holds everything when the note ends', () => {
    const r = new TunerReadout(1);
    feed(r, 0, 1, () => reading(-6));
    const before = r.current;
    feed(r, 1, 2, () => reading(-6.3, { held: true }));
    expect(r.current).toMatchObject({
      note: before.note,
      tune: 'flat',
      held: true,
      centsText: before.centsText,
    });
  });

  it('goes idle when there is nothing to show', () => {
    const r = new TunerReadout(1);
    feed(r, 0, 1, () => reading(0));
    expect(r.update(null, 1).tune).toBe('idle');
    expect(r.current.centsText).toBe('—');
  });

  it('shows whole cents while the reading moves and tenths once it settles', () => {
    const r = new TunerReadout(1);
    feed(r, 0, 1, (t) => reading(-20 + 12 * t, { settled: false }));
    expect(r.current.centsText).toMatch(/^−\d+¢$/);
    feed(r, 1, 0.5, () => reading(-8.04));
    expect(r.current.centsText).toBe('−8.0¢');
  });

  it('changes the numbers at most every TEXT_SECONDS', () => {
    const random = createRandom(3);
    const r = new TunerReadout(1);
    const texts: string[] = [];
    for (let i = 0; i < 2 * HZ; i += 1) {
      const t = i / HZ;
      texts.push(r.update(reading(-10 + 6 * t + gaussian(random)), t).centsText);
    }
    const at = texts.flatMap((x, i) => (i > 0 && x !== texts[i - 1] ? [i / HZ] : []));
    for (let i = 1; i < at.length; i += 1)
      expect((at[i] ?? 0) - (at[i - 1] ?? 0)).toBeGreaterThanOrEqual(TEXT_SECONDS - 1e-9);
  });

  it('starts over on a new note', () => {
    const r = new TunerReadout(1);
    feed(r, 0, 1, () => reading(0));
    expect(r.update(reading(3, { note: 45, settled: false }), 1).tune).toBe('listening');
  });
});

describe('Needle', () => {
  it('comes to rest within 1 cent of a 20-cent jump in 80 ms, and never overshoots', () => {
    const spring = new NeedleSpring();
    let within = -1;
    let peak = 0;
    for (let i = 1; i <= 60; i += 1) {
      const x = spring.step(20, 1 / 60);
      peak = Math.max(peak, x);
      if (within < 0 && Math.abs(x - 20) <= 1) within = i / 60;
    }
    expect(within).toBeLessThanOrEqual(0.084);
    expect(peak).toBeLessThanOrEqual(20);
  });

  it('is stable at any frame time and lands where a ramp stops', () => {
    for (const dt of [1 / 240, 1 / 60, 0.1]) {
      const spring = new NeedleSpring();
      let peak = 0;
      for (let t = 0; t < 2; t += dt) peak = Math.max(peak, spring.step(Math.min(10, t * 30), dt));
      expect(peak).toBeLessThanOrEqual(10 + 1e-9);
      expect(spring.position).toBeCloseTo(10, 3);
    }
  });

  it('holds a reading at full strength, then dims it', () => {
    expect(needleOpacity(true, null)).toBe(1);
    expect(needleOpacity(true, HOLD_SECONDS)).toBe(1);
    expect(needleOpacity(true, HOLD_SECONDS + FADE_SECONDS / 2)).toBeLessThan(1);
    expect(needleOpacity(true, HOLD_SECONDS + FADE_SECONDS)).toBeCloseTo(HELD_OPACITY);
  });
});
