// SPDX-License-Identifier: AGPL-3.0-or-later
// Display formatting per unit. Typographic minus (U+2212) keeps signed columns aligned with
// tabular numerals.
import type { ParamSpec } from '../bridge/protocol';

export const MINUS = '−';

export function signed(value: number, digits: number): string {
  const rounded = Number(value.toFixed(digits));
  if (rounded === 0) return (0).toFixed(digits);
  const body = Math.abs(rounded).toFixed(digits);
  return rounded > 0 ? `+${body}` : `${MINUS}${body}`;
}

/** Fixed digits without trailing zeros: trim(2.50, 2) -> "2.5", trim(16.0, 1) -> "16". */
function trim(n: number, digits: number): string {
  return String(Number(n.toFixed(digits)));
}

export function formatHz(hz: number): string {
  if (hz >= 1000) return `${trim(hz / 1000, hz >= 10000 ? 1 : 2)} kHz`;
  if (hz < 10) return `${hz.toFixed(2)} Hz`;
  if (hz < 100) return `${trim(hz, 1)} Hz`;
  return `${Math.round(hz)} Hz`;
}

export function formatMs(ms: number): string {
  if (ms >= 1000) return `${trim(ms / 1000, 2)} s`;
  if (ms < 10) return `${ms.toFixed(1)} ms`;
  return `${Math.round(ms)} ms`;
}

export function formatSeconds(s: number): string {
  return s >= 10 ? `${s.toFixed(0)} s` : `${s.toFixed(1)} s`;
}

export function formatPan(percent: number): string {
  const p = Math.round(percent);
  if (p === 0) return 'C';
  return p < 0 ? `L${-p}` : `R${p}`;
}

/** Parameters whose extreme setting switches the stage off; show "Off" rather than a number. */
function offAtExtreme(spec: ParamSpec, value: number): boolean {
  if (/^eq\w+LowCut$/.test(spec.id)) return value <= 20.0001;
  if (/^eq\w+HighCut$/.test(spec.id)) return value >= 19999.99;
  return false;
}

export function formatValue(spec: ParamSpec, value: number): string {
  if (spec.kind === 'bool') return value >= 0.5 ? 'On' : 'Off';
  if (spec.kind === 'choice') return spec.choices[Math.round(value)] ?? String(Math.round(value));
  if (offAtExtreme(spec, value)) return 'Off';

  switch (spec.unit) {
    case 'dB':
      return `${signed(value, 1)} dB`;
    case 'dBu':
      return `${signed(value, 1)} dBu`;
    case 'Hz':
      return formatHz(value);
    case 'ms':
      return formatMs(value);
    case 's':
      return formatSeconds(value);
    case 'st':
      // A sign only where the range has both (a transpose, not a bend range).
      return spec.min < 0 ? `${signed(Math.round(value), 0)} st` : `${Math.round(value)} st`;
    case '%':
      if (/Pan$/.test(spec.id)) return formatPan(value);
      // Bipolar percentages (Jetty's feedback) carry their sign, with the typographic minus.
      return spec.min < 0 ? `${signed(Math.round(value), 0)}%` : `${Math.round(value)}%`;
    case 'bpm':
      return `${Math.abs(value - Math.round(value)) < 0.05 ? Math.round(value) : value.toFixed(1)} bpm`;
    case 'knob':
      return (value * 10).toFixed(1);
    case 'deg':
      return `${signed(Math.round(value), 0).replace('+', '')}°`;
    default:
      return spec.kind === 'int' ? String(Math.round(value)) : value.toFixed(2);
  }
}
