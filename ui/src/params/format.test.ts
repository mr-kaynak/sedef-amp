// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { formatHz, formatMs, formatPan, formatValue, MINUS } from './format';
import { bundledSchema, indexSchema } from './schema';

const specs = indexSchema(bundledSchema);
const fmt = (id: string, v: number) => {
  const s = specs[id];
  if (!s) throw new Error(id);
  return formatValue(s, v);
};

describe('formatValue', () => {
  it('signs decibels with a typographic minus', () => {
    expect(fmt('inputGain', 3)).toBe('+3.0 dB');
    expect(fmt('inputGain', -12)).toBe(`${MINUS}12.0 dB`);
    expect(fmt('inputGain', 0)).toBe('0.0 dB');
    expect(fmt('inputGain', -0.04)).toBe('0.0 dB');
  });

  it('shows knobs on a 0-10 scale', () => {
    expect(fmt('pearlGain', 0.5)).toBe('5.0');
    expect(fmt('pearlGain', 1)).toBe('10.0');
  });

  it('formats frequencies in Hz and kHz', () => {
    expect(formatHz(65)).toBe('65 Hz');
    expect(formatHz(3.92)).toBe('3.92 Hz');
    expect(formatHz(1000)).toBe('1 kHz');
    expect(formatHz(1250)).toBe('1.25 kHz');
    expect(formatHz(16000)).toBe('16 kHz');
    expect(fmt('rippleSpeed', 3.92)).toBe('3.92 Hz');
  });

  it('shows the EQ cuts as Off at their extremes', () => {
    expect(fmt('eqReefLowCut', 20)).toBe('Off');
    expect(fmt('eqReefHighCut', 20000)).toBe('Off');
    expect(fmt('eqReefLowCut', 80)).toBe('80 Hz');
    expect(fmt('fathomLowCut', 20)).toBe('20 Hz');
  });

  it('formats times', () => {
    expect(formatMs(375)).toBe('375 ms');
    expect(formatMs(6)).toBe('6.0 ms');
    expect(formatMs(1250)).toBe('1.25 s');
    expect(fmt('fathomDecay', 2.5)).toBe('2.5 s');
    expect(fmt('fathomDecay', 12)).toBe('12 s');
  });

  it('formats semitones, pan, bpm, choices and switches', () => {
    expect(fmt('transpose', 3)).toBe('+3 st');
    expect(fmt('transpose', -12)).toBe(`${MINUS}12 st`);
    expect(fmt('transpose', 0)).toBe('0 st');
    expect(formatPan(0)).toBe('C');
    expect(fmt('micAPan', -40)).toBe('L40');
    expect(fmt('micAPan', 25)).toBe('R25');
    expect(fmt('tempo', 120)).toBe('120 bpm');
    expect(fmt('tempo', 97.5)).toBe('97.5 bpm');
    expect(fmt('ampModel', 2)).toBe('Abyss');
    expect(fmt('driftNote', 13)).toBe('1/4');
    expect(fmt('gateEnabled', 1)).toBe('On');
    expect(fmt('gateEnabled', 0)).toBe('Off');
  });
});
