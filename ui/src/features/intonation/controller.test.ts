// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { PitchEvent } from '../../bridge/protocol';
import { pitchEvent } from '../../app/demoHook';
import { midiToHz } from '../../lib/music';
import { resetStores, setupMock, type Harness } from '../../test/harness';
import {
  CAPTURE_SECONDS,
  IntonationController,
  startIntonationSession,
  TUNE_HOLD_SECONDS,
} from './controller';
import { intonationActions, useIntonationStore } from './store';

/** The plugin editor sends pitch on its 60 Hz timer. */
const DT = 1 / 60;
const LOW_E = 40;
const OPEN_HZ = midiToHz(LOW_E, 440);

describe('intonation measurement with the simulated guitar', () => {
  let h: Harness;
  let stop: () => void;
  beforeEach(async () => {
    vi.useFakeTimers({ toFake: ['performance'] });
    h = await setupMock({ intonationErrors: [3.6, -2.4, 0.5] });
  });
  afterEach(() => {
    stop?.();
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

  it('tunes the open string, captures the 12th fret and reports the error', () => {
    intonationActions.start();
    stop = startIntonationSession();
    expect(h.mock.isTunerActive()).toBe(true);

    run(10, () => useIntonationStore.getState().step === 'fret');
    const afterTune = useIntonationStore.getState();
    expect(afterTune.step).toBe('fret');
    expect(afterTune.openHz).toBeCloseTo(82.41, 0);

    run(10, () => useIntonationStore.getState().step === 'result');
    const s = useIntonationStore.getState();
    expect(s.step).toBe('result');
    expect(s.results[0]?.cents).toBeCloseTo(3.6, 0);
    expect(s.results[0]?.stdCents).toBeLessThan(0.5);
  });

  it('moves through every string to the summary', () => {
    intonationActions.chooseInstrument('custom');
    intonationActions.setCustomStringCount(4);
    intonationActions.start();
    stop = startIntonationSession();
    for (let i = 0; i < 4; i += 1) {
      run(20, () => useIntonationStore.getState().step === 'result');
      intonationActions.next();
    }
    const s = useIntonationStore.getState();
    expect(s.phase).toBe('summary');
    expect(s.results.filter(Boolean)).toHaveLength(4);
    expect(s.results[1]?.cents).toBeCloseTo(-2.4, 0);
  });

  it('stops analysis when the session ends', () => {
    intonationActions.start();
    stop = startIntonationSession();
    stop();
    expect(h.mock.isTunerActive()).toBe(false);
  });
});

describe('intonation timing against the tracker', () => {
  let c: IntonationController;
  beforeEach(() => {
    resetStores();
    c = new IntonationController();
    intonationActions.start();
  });
  afterEach(() => resetStores());

  /** A tracker reading of the 12th-fret note: stable, `cents` off a perfect octave. */
  const fretted = (cents: number, over: Partial<PitchEvent> = {}): PitchEvent => {
    const hz = 2 * OPEN_HZ * Math.pow(2, cents / 1200);
    return {
      ...pitchEvent(LOW_E + 12, 0, { stableCount: 400 }),
      frequencyHz: hz,
      stableHz: hz,
      ...over,
    };
  };
  /** Feeds events at 60 Hz for `seconds` from time `from`; returns the time after the last. */
  const feed = (from: number, seconds: number, event: (t: number) => PitchEvent) => {
    let t = from;
    for (; t < from + seconds - 1e-9; t += DT) c.handle(event(t), t);
    return t;
  };
  const step = () => useIntonationStore.getState().step;

  describe('fretted capture', () => {
    beforeEach(() => useIntonationStore.setState({ step: 'fret', openHz: OPEN_HZ }));

    it('waits for half a second of stable readings, however many detections they report', () => {
      // The engine's stableCount grows by 200 a second, so it is past any count from the start.
      const t = feed(0, CAPTURE_SECONDS - 2 * DT, () => fretted(3));
      expect(step()).toBe('fret');
      expect(useIntonationStore.getState().captureProgress).toBeGreaterThan(0.8);
      feed(t, 4 * DT, () => fretted(3));
      expect(step()).toBe('result');
      expect(useIntonationStore.getState().results[0]?.cents).toBeCloseTo(3, 6);
      // A reading that never moved is as steady as its detections.
      expect(useIntonationStore.getState().results[0]?.stdCents).toBeCloseTo(0.06, 6);
    });

    it('averages stableHz over the hold and reports how much it moved', () => {
      feed(0, 1, (t) => fretted(Math.round(t / DT) % 2 ? 2.8 : 3.2));
      const r = useIntonationStore.getState().results[0];
      expect(r?.cents).toBeCloseTo(3, 1);
      expect(r?.stdCents).toBeGreaterThan(0.15);
      expect(r?.stdCents).toBeLessThan(0.25);
    });

    it('starts again on a new pluck, a break in stability or a restarted segment', () => {
      let t = feed(0, 0.4, () => fretted(3));
      t = feed(t, 0.4, () => fretted(3, { onsetId: 2 }));
      expect(step()).toBe('fret');
      c.handle(fretted(3, { onsetId: 2, stable: false }), t);
      expect(useIntonationStore.getState().captureProgress).toBe(0);
      t = feed(t + DT, 0.4, () => fretted(3, { onsetId: 2 }));
      expect(step()).toBe('fret');
      t = feed(t, 0.4, () => fretted(3, { onsetId: 2, stableCount: 10 }));
      expect(step()).toBe('fret');
      feed(t, 0.2, () => fretted(3, { onsetId: 2, stableCount: 10 }));
      expect(step()).toBe('result');
    });

    it('ignores a stable reading of another note', () => {
      feed(0, 1, () => fretted(3, { stableHz: OPEN_HZ }));
      expect(step()).toBe('fret');
    });
  });

  describe('open-string tuning', () => {
    const open = (cents: number, over: Partial<PitchEvent> = {}): PitchEvent => ({
      ...pitchEvent(LOW_E, cents),
      ...over,
    });

    it('holds for a second of clock time, not a number of events', () => {
      // 20 events a second (a busy host): still a one-second hold.
      for (let i = 0; i < 20; i += 1) c.handle(open(0.3), i * 0.05);
      expect(step()).toBe('tune');
      c.handle(open(0.3), TUNE_HOLD_SECONDS);
      expect(step()).toBe('fret');
      expect(useIntonationStore.getState().openHz).toBeCloseTo(OPEN_HZ * Math.pow(2, 0.3 / 1200));
    });

    it('bridges short dropouts but not long ones or readings out of the window', () => {
      let t = feed(0, 0.5, () => open(0.2));
      t = feed(t, 0.2, () => open(0, { valid: false }));
      t = feed(t, 0.35, () => open(0.2));
      expect(step()).toBe('fret');

      intonationActions.remeasure();
      c.reset();
      t = feed(t, 0.5, () => open(0.2));
      t = feed(t, 0.4, () => open(0, { valid: false }));
      t = feed(t, 0.5, () => open(0.2));
      expect(step()).toBe('tune');
      t = feed(t, 0.1, () => open(1.5));
      feed(t, 0.9, () => open(0.2));
      expect(step()).toBe('tune');
    });
  });
});
