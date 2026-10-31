// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { setupMock, type Harness } from '../../test/harness';
import { startIntonationSession } from './controller';
import { intonationActions, useIntonationStore } from './store';

describe('intonation measurement with the simulated guitar', () => {
  let h: Harness;
  let stop: () => void;
  beforeEach(async () => {
    h = await setupMock({ intonationErrors: [3.6, -2.4, 0.5] });
  });
  afterEach(() => {
    stop?.();
    h.dispose();
  });

  const run = (seconds: number, until?: () => boolean) => {
    for (let t = 0; t < seconds; t += 0.02) {
      h.mock.tickPitch(0.02);
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
    expect(s.results[0]?.stdCents).toBeLessThan(0.6);
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
