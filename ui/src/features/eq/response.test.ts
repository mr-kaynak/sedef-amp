// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import {
  EQ_BAND_HZ,
  EQ_SAMPLE_RATE,
  highpass,
  logFrequencies,
  magnitude,
  peaking,
  responseCurve,
  responseDb,
  type EqSettings,
} from './response';

const flat: EqSettings = { bandsDb: Array(9).fill(0), lowCutHz: 20, highCutHz: 20000 };
const withBand = (index: number, db: number): EqSettings => ({
  ...flat,
  bandsDb: flat.bandsDb.map((v, i) => (i === index ? db : v)),
});

describe('EQ response (mirrors GraphicEq)', () => {
  it('is flat with every band at 0 dB and both cuts off', () => {
    for (const hz of [20, 65, 1000, 16000, 20000]) expect(responseDb(flat, hz)).toBeCloseTo(0, 9);
  });

  it('reaches the band gain exactly at the band centre', () => {
    EQ_BAND_HZ.forEach((hz, i) => {
      expect(responseDb(withBand(i, 12), hz)).toBeCloseTo(12, 3);
      expect(responseDb(withBand(i, -12), hz)).toBeCloseTo(-12, 3);
    });
  });

  it('keeps a boost local: one octave away it has fallen well below half', () => {
    const s = withBand(4, 12); // 1 kHz
    const octaveUp = responseDb(s, 2000);
    expect(octaveUp).toBeGreaterThan(1);
    expect(octaveUp).toBeLessThan(6);
    expect(responseDb(s, 50)).toBeLessThan(0.1);
  });

  it('is symmetric for boost and cut (RBJ peaking)', () => {
    for (const hz of [700, 1000, 1400, 3000]) {
      expect(responseDb(withBand(4, 9), hz)).toBeCloseTo(-responseDb(withBand(4, -9), hz), 9);
    }
  });

  it('puts the Butterworth cuts at -3.01 dB on their corner', () => {
    expect(responseDb({ ...flat, lowCutHz: 100 }, 100)).toBeCloseTo(-3.0103, 3);
    expect(responseDb({ ...flat, highCutHz: 5000 }, 5000)).toBeCloseTo(-3.0103, 3);
    // 12 dB/oct: two octaves below the low cut is about -24 dB.
    expect(responseDb({ ...flat, lowCutHz: 400 }, 100)).toBeCloseTo(-24.1, 0);
  });

  it('bypasses the cuts at their extremes', () => {
    expect(responseDb({ ...flat, lowCutHz: 20 }, 20)).toBeCloseTo(0, 9);
    expect(responseDb({ ...flat, highCutHz: 20000 }, 20000)).toBeCloseTo(0, 9);
    expect(responseDb({ ...flat, lowCutHz: 21 }, 21)).toBeCloseTo(-3.0103, 3);
  });

  it('adds the sections in dB', () => {
    const s: EqSettings = { ...withBand(4, 6), lowCutHz: 100 };
    const expected =
      20 * Math.log10(magnitude(peaking(EQ_SAMPLE_RATE, 1000, 1.41, 6), 300, EQ_SAMPLE_RATE)) +
      20 * Math.log10(magnitude(highpass(EQ_SAMPLE_RATE, 100), 300, EQ_SAMPLE_RATE));
    expect(responseDb(s, 300)).toBeCloseTo(expected, 12);
  });

  it('computes curves that match point evaluation', () => {
    const s: EqSettings = {
      bandsDb: [3, -2, 0, 1, 4, -6, 2, 0, -1],
      lowCutHz: 80,
      highCutHz: 9000,
    };
    const freqs = logFrequencies(32);
    const curve = responseCurve(s, freqs);
    freqs.forEach((hz, i) => expect(curve[i]).toBeCloseTo(responseDb(s, hz), 12));
    expect(freqs[0]).toBeCloseTo(20);
    expect(freqs[31]).toBeCloseTo(20000);
  });
});
