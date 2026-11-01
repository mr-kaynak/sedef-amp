// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { PitchEvent } from '../../bridge/protocol';
import { pitchEvent } from '../../app/demoHook';
import { centsBetween, midiToHz } from '../../lib/music';
import { resetStores, setupMock, type Harness } from '../../test/harness';
import { CAPTURE_FROM_SECONDS, CAPTURE_SECONDS } from './capture';
import { IntonationController, startIntonationSession, TUNE_TOLERANCE_CENTS } from './controller';
import { intonationActions, useIntonationStore } from './store';

/** The plugin editor sends pitch on its 60 Hz timer. */
const DT = 1 / 60;
const LOW_E = 40;
const OPEN_HZ = midiToHz(LOW_E, 440);
/** A capture of a pluck first seen at t = 0 is complete this long after it. */
const DONE = CAPTURE_FROM_SECONDS + CAPTURE_SECONDS;
const centsFromE2 = (hz: number) => centsBetween(hz, OPEN_HZ);
const state = () => useIntonationStore.getState();

describe('intonation measurement with the simulated guitar', () => {
  let h: Harness;
  let stop: (() => void) | undefined;
  const setup = async (options: { intonationErrors?: number[]; tuningOffsets?: number[] }) => {
    vi.useFakeTimers({ toFake: ['performance'] });
    h = await setupMock(options);
    intonationActions.start();
    stop = startIntonationSession();
  };
  afterEach(() => {
    stop?.();
    stop = undefined;
    h.dispose();
    vi.useRealTimers();
  });

  const run = (seconds: number, until?: () => boolean) => {
    for (let t = 0; t < seconds; t += DT) {
      vi.advanceTimersByTime(DT * 1000);
      h.mock.tickPitch(DT);
      if (until?.()) return;
    }
  };

  it('tunes the open string, captures the 12th fret and reports the error', async () => {
    await setup({ intonationErrors: [3.6, -2.4, 0.5] });
    expect(h.mock.isTunerActive()).toBe(true);

    run(10, () => state().step === 'fret');
    expect(state().step).toBe('fret');
    expect(state().openHz).toBeCloseTo(82.41, 0);

    run(10, () => state().step === 'result');
    expect(state().step).toBe('result');
    expect(state().results[0]?.cents).toBeCloseTo(3.6, 0);
    expect(state().results[0]?.stdCents).toBeLessThan(0.5);
  });

  it('moves on by itself from a string that is close but never still', async () => {
    // 3.5 cents sharp, wobbling by a cent with half a cent of jitter on every reading and sagging
    // flat as it rings: never within a cent of the note for a second.
    await setup({ tuningOffsets: [3.5] });
    let close = false;
    run(10, () => {
      close ||= state().pluck === 'measuring' && state().tuneClose;
      return state().step === 'fret';
    });
    expect(state().step).toBe('fret');
    expect(close).toBe(true);
    const cents = centsFromE2(state().openHz ?? 0);
    expect(cents).toBeGreaterThan(1.5);
    expect(cents).toBeLessThan(TUNE_TOLERANCE_CENTS);
  });

  it('offers the reading of a string 8 cents flat and measures against it when asked', async () => {
    await setup({ tuningOffsets: [-8], intonationErrors: [2] });
    run(10, () => state().tuneOffer !== null);
    expect(state().step).toBe('tune');
    expect(state().tuneOffer?.cents).toBeLessThan(-7.5);
    expect(state().tuneOffer?.cents).toBeGreaterThan(-10);
    // It stands while the pluck rings; the workbench does not move on by itself.
    run(0.5);
    expect(state().step).toBe('tune');
    const offer = state().tuneOffer;
    expect(offer).not.toBeNull();

    intonationActions.acceptTuning();
    expect(state().step).toBe('fret');
    expect(state().openHz).toBe(offer?.hz);
    run(10, () => state().step === 'result');
    // Measured against the string as it is: the error, not the error plus 8 cents.
    expect(state().results[0]?.cents).toBeCloseTo(2, 0);
  });

  it.each([
    [40, 12],
    [-40, -12],
  ])(
    'measures the octave of a string used %d cents off with a %d-cent saddle error',
    async (offset, error) => {
      // The octave lands more than half a semitone from the nominal 12th-fret note.
      await setup({ tuningOffsets: [offset], intonationErrors: [error] });
      run(10, () => state().tuneOffer !== null);
      expect(Math.abs(state().tuneOffer?.cents ?? 0)).toBeGreaterThan(38);
      intonationActions.acceptTuning();
      run(10, () => state().step === 'result');
      expect(state().step).toBe('result');
      expect(Math.abs((state().results[0]?.cents ?? 0) - error)).toBeLessThan(0.3);
    },
  );

  it('measures the octave against the open string as it was tuned', async () => {
    await setup({ tuningOffsets: [2.5], intonationErrors: [1.2] });
    run(20, () => state().step === 'result');
    const r = state().results[0];
    // The open string read sharp, as it was left, and the result is the error against it; against
    // the nominal E2 it would be about 3.7 cents.
    expect(centsFromE2(r?.openHz ?? 0)).toBeGreaterThan(1);
    expect(Math.abs((r?.cents ?? 0) - 1.2)).toBeLessThan(0.3);
  });

  it('never moves on from the wrong note', async () => {
    // Left 70 cents sharp: the nearest note is F2.
    await setup({ tuningOffsets: [70] });
    run(15);
    expect(state().step).toBe('tune');
    expect(state().tuneOffer).toBeNull();
    expect(state().pluck).not.toBe('measuring');
  });

  it('moves through every string to the summary', async () => {
    vi.useFakeTimers({ toFake: ['performance'] });
    h = await setupMock({ intonationErrors: [3.6, -2.4, 0.5] });
    intonationActions.chooseInstrument('custom');
    intonationActions.setCustomStringCount(4);
    intonationActions.start();
    stop = startIntonationSession();
    for (let i = 0; i < 4; i += 1) {
      run(20, () => state().step === 'result');
      intonationActions.next();
    }
    expect(state().phase).toBe('summary');
    expect(state().results.filter(Boolean)).toHaveLength(4);
    expect(state().results[1]?.cents).toBeCloseTo(-2.4, 0);
  });

  it('stops analysis when the session ends', async () => {
    await setup({});
    stop?.();
    stop = undefined;
    expect(h.mock.isTunerActive()).toBe(false);
  });
});

describe('intonation capture against the tracker', () => {
  let c: IntonationController;
  beforeEach(() => {
    resetStores();
    c = new IntonationController();
    intonationActions.start();
  });
  afterEach(() => resetStores());

  /** A tracker reading of `midi` played `cents` off, stable, from pluck 1. */
  const reading = (midi: number, cents: number, over: Partial<PitchEvent> = {}): PitchEvent => ({
    ...pitchEvent(midi, cents, { stableCount: 400 }),
    ...over,
  });
  /** A reading of the 12th-fret note, `cents` off a perfect octave of the open string. */
  const fretted = (cents: number, over: Partial<PitchEvent> = {}) =>
    reading(LOW_E + 12, cents, over);
  const open = (cents: number, over: Partial<PitchEvent> = {}) => reading(LOW_E, cents, over);
  /** Feeds events at 60 Hz for `seconds` from time `from`; returns the time after the last. */
  const feed = (from: number, seconds: number, event: (t: number) => PitchEvent) => {
    let t = from;
    for (; t < from + seconds - 1e-9; t += DT) c.handle(event(t), t);
    return t;
  };

  describe('fretted note', () => {
    beforeEach(() => useIntonationStore.setState({ step: 'fret', openHz: OPEN_HZ }));

    it('averages one second of readings from 0.6 s after the pluck', () => {
      let t = feed(0, CAPTURE_FROM_SECONDS - DT, () => fretted(3));
      expect(state().pluck).toBe('listening');
      t = feed(t, CAPTURE_SECONDS, () => fretted(3));
      expect(state().step).toBe('fret');
      expect(state().pluck).toBe('measuring');
      expect(state().captureProgress).toBeGreaterThan(0.9);
      feed(t, 4 * DT, () => fretted(3));
      expect(state().step).toBe('result');
      expect(state().results[0]?.cents).toBeCloseTo(3, 6);
      // A reading that never moved is as steady as its detections.
      expect(state().results[0]?.stdCents).toBeCloseTo(0.06, 6);
    });

    it('reports how much the reading moved', () => {
      feed(0, 2, (t) => fretted(Math.round(t / DT) % 2 ? 2.8 : 3.2));
      const r = state().results[0];
      expect(r?.cents).toBeCloseTo(3, 1);
      expect(r?.stdCents).toBeGreaterThan(0.15);
      expect(r?.stdCents).toBeLessThan(0.25);
    });

    it('carries on when the tracker briefly stops calling a wobbling note stable', () => {
      // A cent of wobble lifts the spread over the tracker's half cent now and then.
      const wobbly = (t: number) =>
        t > 0.9 && t < 1.2 ? fretted(3.4, { stable: false, stableStdCents: 0.7 }) : fretted(3);
      const t = feed(0, DONE - 0.1, wobbly);
      expect(state().step).toBe('fret');
      feed(t, 0.15, () => fretted(3));
      expect(state().step).toBe('result');
      expect(state().results[0]?.cents).toBeCloseTo(3.1, 1);
    });

    it('needs the tracker to call the note stable at least once', () => {
      // Two strings ringing together never reach the tracker's clarity for `stable`.
      const t = feed(0, 3, () => fretted(3, { stable: false, stableStdCents: 0.3 }));
      expect(state().step).toBe('fret');
      feed(t, DT, () => fretted(3));
      expect(state().step).toBe('result');
    });

    it('gives up on a pluck whose reading stops being steady, and measures the next', () => {
      let t = feed(0, 1, () => fretted(3));
      t = feed(t, 0.1, () => fretted(3, { stable: false, stableStdCents: 1.6 }));
      expect(state().pluck).toBe('missed');
      expect(state().captureProgress).toBe(0);
      t = feed(t, 2, () => fretted(3));
      expect(state().step).toBe('fret');
      t = feed(t, DONE - 2 * DT, () => fretted(2, { onsetId: 2 }));
      expect(state().step).toBe('fret');
      feed(t, 6 * DT, () => fretted(2, { onsetId: 2 }));
      expect(state().step).toBe('result');
      expect(state().results[0]?.cents).toBeCloseTo(2, 6);
    });

    it('gives up on a pluck that moves several cents, as a turned peg does', () => {
      feed(0, 3, (t) => fretted(t < 0.7 ? 3 : 3 + 8 * (t - 0.7)));
      expect(state().step).toBe('fret');
      expect(state().pluck).toBe('missed');
    });

    it('starts again on a new pluck or a restarted segment', () => {
      let t = feed(0, 1.2, () => fretted(3));
      t = feed(t, 1.2, () => fretted(3, { onsetId: 2 }));
      expect(state().step).toBe('fret');
      // The tracker restarted its segment (a legato change): stableCount falls back.
      t = feed(t, 1.2, () => fretted(3, { onsetId: 2, stableCount: 10 }));
      expect(state().step).toBe('fret');
      feed(t, 0.5, () => fretted(3, { onsetId: 2, stableCount: 10 }));
      expect(state().step).toBe('result');
    });

    it('bridges short dropouts but not long ones', () => {
      let t = feed(0, 1, () => fretted(3));
      t = feed(t, 0.2, () => fretted(0, { valid: false }));
      feed(t, 0.5, () => fretted(3));
      expect(state().step).toBe('result');

      useIntonationStore.setState({ step: 'fret', openHz: OPEN_HZ });
      c.reset();
      t = feed(0, 1, () => fretted(3, { onsetId: 2 }));
      t = feed(t, 0.4, () => fretted(0, { onsetId: 2, valid: false }));
      feed(t, 1, () => fretted(3, { onsetId: 2 }));
      expect(state().step).toBe('fret');
      expect(state().pluck).toBe('missed');
    });

    it('ignores a steady reading of another note', () => {
      feed(0, 3, () => fretted(3, { stableHz: OPEN_HZ }));
      expect(state().step).toBe('fret');
      // The 7th fret, a fifth above the open string.
      feed(0, 3, () => reading(LOW_E + 7, 0, { onsetId: 2 }));
      expect(state().step).toBe('fret');
      expect(state().pluck).toBe('missed');
    });

    it('judges the octave against the open string as measured, not the note grid', () => {
      // Open string used 42 cents sharp, saddle 12 cents sharp: 54 cents over E3, nearer F3.
      const openHz = OPEN_HZ * Math.pow(2, 42 / 1200);
      useIntonationStore.setState({ openHz });
      feed(0, 2, () => fretted(54));
      expect(state().step).toBe('result');
      expect(state().results[0]?.cents).toBeCloseTo(12, 6);
      expect(state().results[0]?.openHz).toBe(openHz);
    });

    it('times captures with the clock, not the number of events', () => {
      // 20 events a second (a busy host): still one second from 0.6 s.
      for (let i = 0; i < 31; i += 1) c.handle(fretted(3), i * 0.05);
      expect(state().step).toBe('fret');
      c.handle(fretted(3), DONE + 0.01);
      expect(state().step).toBe('result');
    });
  });

  describe('open string', () => {
    it('moves on within the tolerance, measured like the fretted note', () => {
      // The short-window reading swings by ±2 cents; the settled reading sits 3 cents sharp.
      const settledHz = OPEN_HZ * Math.pow(2, 3 / 1200);
      const swinging = (t: number) => open(3 + 2 * Math.sin(t * 40), { stableHz: settledHz });
      const t = feed(0, 1, swinging);
      expect(state().step).toBe('tune');
      expect(state().pluck).toBe('measuring');
      expect(state().tuneClose).toBe(true);
      feed(t, 0.7, swinging);
      expect(state().step).toBe('fret');
      expect(state().openHz).toBeCloseTo(settledHz, 9);
    });

    it('offers a steady reading further off, and uses it when asked', () => {
      let t = feed(0, 1, () => open(-8));
      expect(state().pluck).toBe('measuring');
      expect(state().tuneClose).toBe(false);
      t = feed(t, 0.7, () => open(-8));
      expect(state().step).toBe('tune');
      const offer = state().tuneOffer;
      expect(offer?.cents).toBeCloseTo(-8, 6);
      // It stands while the pluck rings.
      feed(t, 1, () => open(-8));
      expect(state().tuneOffer).toBe(offer);

      intonationActions.acceptTuning();
      expect(state().step).toBe('fret');
      expect(state().openHz).toBe(offer?.hz);
    });

    it('withdraws the offer when the string is plucked again', () => {
      let t = feed(0, DONE + 0.1, () => open(-8));
      expect(state().tuneOffer).not.toBeNull();
      t = feed(t, 0.5, () => open(-3, { onsetId: 2 }));
      expect(state().tuneOffer).toBeNull();
      feed(t, 1.2, () => open(-3, { onsetId: 2 }));
      expect(state().step).toBe('fret');
    });

    it('offers anything on the right note, up to half a semitone off', () => {
      feed(0, DONE + 0.1, () => open(-45));
      expect(state().tuneOffer?.cents).toBeCloseTo(-45, 6);
    });

    it('never moves on from the wrong note', () => {
      feed(0, 5, () => open(70));
      expect(state().step).toBe('tune');
      expect(state().tuneOffer).toBeNull();
      expect(state().pluck).toBe('missed');
    });
  });
});
