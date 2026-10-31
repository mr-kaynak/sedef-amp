// SPDX-License-Identifier: AGPL-3.0-or-later
// Magnitude response of the graphic EQ, mirroring sedef::dsp::GraphicEq (engine/include/sedef/
// dsp/GraphicEq.h) and the RBJ designs in engine/src/Biquad.cpp:
//   - nine RBJ peaking bands at kBandHz with constant Q 1.41 (one octave)
//   - 2nd-order Butterworth high-pass (low cut) and low-pass (high cut), Q = 1/sqrt(2)
//   - the low cut is bypassed at <= 20 Hz, the high cut at >= 20 kHz
//   - evaluated at 48 kHz; frequencies are clamped to [1, 0.49 * fs] like clampFrequency()
// Keep this file and the C++ definitions in sync.

export const EQ_BAND_HZ = [65, 125, 250, 500, 1000, 2000, 4000, 8000, 16000] as const;
export const EQ_BAND_Q = 1.41;
export const EQ_CUT_Q = 0.7071067811865476;
export const EQ_SAMPLE_RATE = 48000;
export const LOW_CUT_OFF_HZ = 20;
export const HIGH_CUT_OFF_HZ = 20000;

export interface Biquad {
  b0: number;
  b1: number;
  b2: number;
  a1: number;
  a2: number;
}

export interface EqSettings {
  bandsDb: readonly number[];
  lowCutHz: number;
  highCutHz: number;
}

function clampFrequency(hz: number, fs: number): number {
  return Math.min(fs * 0.49, Math.max(1, hz));
}

function normalise(b0: number, b1: number, b2: number, a0: number, a1: number, a2: number): Biquad {
  return { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 };
}

function omega(fs: number, hz: number): { cw: number; sw: number } {
  const w = (2 * Math.PI * clampFrequency(hz, fs)) / fs;
  return { cw: Math.cos(w), sw: Math.sin(w) };
}

export function peaking(fs: number, hz: number, q: number, gainDb: number): Biquad {
  const { cw, sw } = omega(fs, hz);
  const a = Math.pow(10, gainDb / 40);
  const alpha = sw / (2 * q);
  return normalise(1 + alpha * a, -2 * cw, 1 - alpha * a, 1 + alpha / a, -2 * cw, 1 - alpha / a);
}

export function highpass(fs: number, hz: number, q = EQ_CUT_Q): Biquad {
  const { cw, sw } = omega(fs, hz);
  const alpha = sw / (2 * q);
  return normalise((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

export function lowpass(fs: number, hz: number, q = EQ_CUT_Q): Biquad {
  const { cw, sw } = omega(fs, hz);
  const alpha = sw / (2 * q);
  return normalise((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

/** |H(e^jw)| at hz. */
export function magnitude(c: Biquad, hz: number, fs: number): number {
  const w = (2 * Math.PI * hz) / fs;
  const cos1 = Math.cos(w);
  const sin1 = Math.sin(w);
  const cos2 = Math.cos(2 * w);
  const sin2 = Math.sin(2 * w);
  // z^-1 = cos w - j sin w
  const nr = c.b0 + c.b1 * cos1 + c.b2 * cos2;
  const ni = -(c.b1 * sin1 + c.b2 * sin2);
  const dr = 1 + c.a1 * cos1 + c.a2 * cos2;
  const di = -(c.a1 * sin1 + c.a2 * sin2);
  return Math.sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

/** The active filter sections for a setting (flat bands and bypassed cuts are omitted). */
export function eqSections(s: EqSettings, fs = EQ_SAMPLE_RATE): Biquad[] {
  const sections: Biquad[] = [];
  EQ_BAND_HZ.forEach((hz, i) => {
    const g = s.bandsDb[i] ?? 0;
    if (g !== 0) sections.push(peaking(fs, hz, EQ_BAND_Q, g));
  });
  if (s.lowCutHz > LOW_CUT_OFF_HZ) sections.push(highpass(fs, s.lowCutHz));
  if (s.highCutHz < HIGH_CUT_OFF_HZ) sections.push(lowpass(fs, s.highCutHz));
  return sections;
}

export function responseDb(s: EqSettings, hz: number, fs = EQ_SAMPLE_RATE): number {
  return eqSections(s, fs).reduce((db, c) => db + 20 * Math.log10(magnitude(c, hz, fs)), 0);
}

/** Response at many frequencies, designing the filters once. */
export function responseCurve(
  s: EqSettings,
  freqs: readonly number[],
  fs = EQ_SAMPLE_RATE,
): number[] {
  const sections = eqSections(s, fs);
  return freqs.map((hz) =>
    sections.reduce((db, c) => db + 20 * Math.log10(magnitude(c, hz, fs)), 0),
  );
}

/** Log-spaced frequencies for plotting. */
export function logFrequencies(count: number, lo = 20, hi = 20000): number[] {
  const ratio = Math.log(hi / lo);
  return Array.from({ length: count }, (_, i) => lo * Math.exp((ratio * i) / (count - 1)));
}
