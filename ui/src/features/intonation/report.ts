// SPDX-License-Identifier: AGPL-3.0-or-later
import { midiLabel } from '../../lib/music';
import {
  CUSTOM_INSTRUMENT_ID,
  INSTRUMENTS,
  SCALE_LENGTHS,
  stringNumber,
  tuningLabel,
} from './instruments';
import { adjustmentFor, formatCents, formatMm, IN_TUNE_CENTS, VERDICT_TEXT } from './math';
import { scaleLengthMm, stringNotes, type IntonationProfile, type StringResult } from './store';

export function instrumentName(p: IntonationProfile): string {
  return p.instrumentId === CUSTOM_INSTRUMENT_ID
    ? 'Custom tuning'
    : (INSTRUMENTS.find((i) => i.id === p.instrumentId)?.name ?? 'Instrument');
}

export function scaleLabel(p: IntonationProfile): string {
  const mm = scaleLengthMm(p);
  const preset = SCALE_LENGTHS.find((l) => l.id === p.scaleId);
  return preset ? `${preset.label} (${mm} mm)` : `${mm} mm`;
}

/** Plain-text report for pasting into notes or a message to a luthier. */
export function buildReport(
  p: IntonationProfile,
  results: readonly (StringResult | null)[],
): string {
  const notes = stringNotes(p);
  const L = scaleLengthMm(p);
  const lines = [
    'Sedef Amp intonation report',
    `${instrumentName(p)}, ${tuningLabel(notes)}, scale ${scaleLabel(p)}`,
    '',
  ];
  notes.forEach((midi, i) => {
    const r = results[i];
    const head = `String ${stringNumber(i, notes.length)}  ${midiLabel(midi).padEnd(4)}`;
    if (!r) {
      lines.push(`${head}  not measured`);
      return;
    }
    const a = adjustmentFor(L, r.cents);
    const text = VERDICT_TEXT[a.verdict];
    const mm =
      a.verdict === 'in-tune' ? '' : `  about ${formatMm(Math.abs(a.offsetMm)).replace('+', '')}`;
    lines.push(`${head}  ${formatCents(r.cents).padStart(7)}  ${text}${mm}`);
  });
  lines.push(
    '',
    `In tune means within ±${IN_TUNE_CENTS} cents of a perfect octave at the 12th fret.`,
  );
  return lines.join('\n');
}

export async function copyText(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    // Fall through: some WebViews deny the async clipboard outside secure contexts.
  }
  const area = document.createElement('textarea');
  area.value = text;
  area.setAttribute('readonly', '');
  area.style.position = 'fixed';
  area.style.opacity = '0';
  document.body.appendChild(area);
  area.select();
  let ok: boolean;
  try {
    ok = document.execCommand('copy');
  } catch {
    ok = false;
  }
  area.remove();
  return ok;
}
