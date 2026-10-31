// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import {
  adjustmentFor,
  formatCents,
  formatMm,
  IN_TUNE_CENTS,
  inchesToMm,
  octaveErrorCents,
  saddleOffsetMm,
  verdictFor,
} from './math';

describe('octaveErrorCents', () => {
  it('is zero for a perfect octave', () => {
    expect(octaveErrorCents(82.41, 164.82)).toBeCloseTo(0, 9);
  });

  it('measures sharp and flat deviations in cents', () => {
    expect(octaveErrorCents(100, 200 * Math.pow(2, 5 / 1200))).toBeCloseTo(5, 9);
    expect(octaveErrorCents(100, 200 * Math.pow(2, -3.2 / 1200))).toBeCloseTo(-3.2, 9);
  });
});

describe('saddleOffsetMm', () => {
  it('matches the worked example: +4 cents on a 648 mm scale is about +1.50 mm', () => {
    expect(saddleOffsetMm(648, 4)).toBeCloseTo(1.5, 2);
  });

  it('follows L * (2^(c/1200) - 1) and is negative for flat notes', () => {
    expect(saddleOffsetMm(864, 10)).toBeCloseTo(864 * (Math.pow(2, 10 / 1200) - 1), 12);
    expect(saddleOffsetMm(648, -4)).toBeCloseTo(-1.4955, 3);
    expect(saddleOffsetMm(648, 0)).toBe(0);
  });

  it('scales with the scale length', () => {
    expect(saddleOffsetMm(889, 3) / saddleOffsetMm(628, 3)).toBeCloseTo(889 / 628, 9);
  });
});

describe('verdict', () => {
  it('treats |c| <= 1.5 cents as in tune', () => {
    expect(IN_TUNE_CENTS).toBe(1.5);
    expect(verdictFor(1.5)).toBe('in-tune');
    expect(verdictFor(-1.5)).toBe('in-tune');
    expect(verdictFor(0)).toBe('in-tune');
  });

  it('sends sharp saddles back and flat saddles forward', () => {
    expect(verdictFor(1.51)).toBe('move-back');
    expect(verdictFor(-2)).toBe('move-forward');
    expect(adjustmentFor(648, 4)).toEqual({
      cents: 4,
      verdict: 'move-back',
      offsetMm: saddleOffsetMm(648, 4),
    });
  });
});

describe('formatting', () => {
  it('formats millimetres and cents with signs', () => {
    expect(formatMm(1.4991)).toBe('+1.50 mm');
    expect(formatMm(-0.42)).toBe('−0.42 mm');
    expect(formatMm(0.001)).toBe('0.00 mm');
    expect(formatCents(3.64)).toBe('+3.6¢');
    expect(formatCents(-0.04)).toBe('0.0¢');
  });

  it('converts inches', () => {
    expect(inchesToMm(25.5)).toBeCloseTo(647.7, 6);
  });
});
