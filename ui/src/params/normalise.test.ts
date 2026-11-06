// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { bundledSchema, indexSchema } from './schema';
import { fromNormalised, isBipolar, snapValue, stepCount, toNormalised } from './normalise';

const specs = indexSchema(bundledSchema);
const spec = (id: string) => {
  const s = specs[id];
  if (!s) throw new Error(id);
  return s;
};

describe('schema', () => {
  it('bundles all 178 parameters', () => {
    expect(bundledSchema).toHaveLength(178);
  });
});

describe('normalisation (JUCE skew semantics)', () => {
  it('is linear when skew is 1', () => {
    expect(toNormalised(spec('inputGain'), 0)).toBeCloseTo(0.5);
    expect(toNormalised(spec('inputGain'), -24)).toBe(0);
    expect(toNormalised(spec('inputGain'), 24)).toBe(1);
  });

  it('applies norm = ((v - min) / (max - min)) ^ skew', () => {
    // fathomDecay: 1..60 s, skew 0.2
    const expected = Math.pow((2.5 - 1) / 59, 0.2);
    expect(toNormalised(spec('fathomDecay'), 2.5)).toBeCloseTo(expected, 10);
    expect(expected).toBeCloseTo(0.4797, 3);
  });

  it('round-trips through the inverse for skewed floats', () => {
    for (const id of ['driftTime', 'rippleSpeed', 'micALevel', 'eqReefHighCut', 'fathomPreDelay']) {
      const s = spec(id);
      for (const n of [0, 0.1, 0.33, 0.5, 0.9, 1]) {
        expect(toNormalised(s, fromNormalised(s, n))).toBeCloseTo(n, 9);
      }
    }
  });

  it('clamps out-of-range input', () => {
    expect(toNormalised(spec('driftTime'), 99999)).toBe(1);
    expect(fromNormalised(spec('driftTime'), -1)).toBe(16);
  });

  it('snaps stepped kinds to whole values', () => {
    expect(fromNormalised(spec('transpose'), 0.52)).toBe(0);
    expect(fromNormalised(spec('transpose'), 1)).toBe(12);
    expect(fromNormalised(spec('driftNote'), 0.5)).toBe(10);
    expect(snapValue(spec('gateEnabled'), 0.7)).toBe(1);
    expect(stepCount(spec('ampModel'))).toBe(2);
    expect(stepCount(spec('inputGain'))).toBe(0);
  });

  it('knows which ranges are bipolar', () => {
    expect(isBipolar(spec('eqReefBand3'))).toBe(true);
    expect(isBipolar(spec('pearlGain'))).toBe(false);
  });
});
