// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { describeOffsets, describeWidth, offsetMs, typicalRangeMs } from './takes';

describe('Doubler take offsets', () => {
  it('follows the engine: spread * 2^(variation * z), clamped to 2.5..40 ms', () => {
    expect(offsetMs(6, 0.5, 0)).toBe(6);
    expect(offsetMs(6, 0.5, 2)).toBeCloseTo(12);
    expect(offsetMs(6, 1, -1)).toBeCloseTo(3);
    expect(offsetMs(3, 1, -2.5)).toBe(2.5);
    expect(offsetMs(20, 1, 2.5)).toBe(40);
    expect(offsetMs(10, 1, 9)).toBe(offsetMs(10, 1, 2.5)); // z is clamped
  });

  it('summarises where most notes land', () => {
    expect(typicalRangeMs(6, 0.5)).toEqual([4, 9]);
    expect(describeOffsets(6, 0.5)).toBe('4–9 ms behind');
    expect(describeOffsets(6, 0)).toBe('6 ms behind');
    expect(describeOffsets(20, 1)).toBe('8–40 ms behind');
  });

  it('names where the takes sit', () => {
    expect(describeWidth(1)).toBe('Second take on the right');
    expect(describeWidth(0.5)).toBe('Second take right of centre');
    expect(describeWidth(0)).toBe('Both takes in the centre');
  });
});
