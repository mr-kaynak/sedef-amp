// SPDX-License-Identifier: AGPL-3.0-or-later
// URL flags for demos, screenshots and manual QA against the mock backend:
//   ?section=pedals|amp|eq|cab|fx   open a section
//   ?overlay=tuner|intonation        open an overlay on load
//   ?host=plugin|standalone          present as a plugin (no metronome) or the standalone app
//                                    instead of the browser preview
//   ?errors=3.6,-2.4,1.1             12th-fret errors (cents) the simulated guitar produces
//   ?preset=soft-focus               start from a factory preset (slug, default nacre)
//   ?set=pearlEnabled:1,pearlGain:0.4  parameter values applied as part of that preset
//   ?freeze                          no simulation: static meters, pitch only from the demo hook
//   ?demo                            expose window.__sedefDemo for scripted frames (captures)
//   ?grain=0                         hide the lacquer grain (smaller screenshots)
//   ?fresh                           ignore UI state saved by earlier browser sessions
import { isSection, type Overlay, type Section } from '../state/uiStore';

export interface DemoFlags {
  section: Section | null;
  overlay: Overlay;
  host: 'browser' | 'plugin' | 'standalone';
  errors: number[] | undefined;
  preset: string | null;
  overrides: Record<string, number>;
  freeze: boolean;
  demo: boolean;
  grain: boolean;
  fresh: boolean;
}

function parseOverrides(text: string | null): Record<string, number> {
  const out: Record<string, number> = {};
  for (const pair of (text ?? '').split(',')) {
    const [id, value] = pair.split(':');
    const n = Number(value);
    if (id && value !== undefined && Number.isFinite(n)) out[id.trim()] = n;
  }
  return out;
}

export function readDemoFlags(
  search: string = typeof window !== 'undefined' ? window.location.search : '',
): DemoFlags {
  const q = new URLSearchParams(search);
  const section = q.get('section');
  const overlay = q.get('overlay');
  const errors = q
    .get('errors')
    ?.split(',')
    .map(Number)
    .filter((n) => Number.isFinite(n));
  return {
    section: isSection(section) ? section : null,
    overlay: overlay === 'tuner' || overlay === 'intonation' ? overlay : null,
    host:
      q.get('host') === 'plugin'
        ? 'plugin'
        : q.get('host') === 'standalone'
          ? 'standalone'
          : 'browser',
    errors: errors && errors.length ? errors : undefined,
    preset: q.get('preset'),
    overrides: parseOverrides(q.get('set')),
    freeze: q.has('freeze'),
    demo: q.has('demo'),
    grain: q.get('grain') !== '0',
    fresh: q.has('fresh'),
  };
}
