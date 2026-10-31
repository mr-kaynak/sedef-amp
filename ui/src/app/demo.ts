// SPDX-License-Identifier: AGPL-3.0-or-later
// URL flags for demos, screenshots and manual QA against the mock backend:
//   ?section=pedals|amp|eq|cab|fx   open a section
//   ?overlay=tuner|intonation        open an overlay on load
//   ?host=plugin                     behave like a plugin (no metronome) instead of standalone
//   ?errors=3.6,-2.4,1.1             12th-fret errors (cents) the simulated guitar produces
//   ?preset=night-swim               start from a factory preset (slug)
//   ?fresh                           ignore UI state saved by earlier browser sessions
import { isSection, type Overlay, type Section } from '../state/uiStore';

export interface DemoFlags {
  section: Section | null;
  overlay: Overlay;
  plugin: boolean;
  errors: number[] | undefined;
  preset: string | null;
  fresh: boolean;
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
    plugin: q.get('host') === 'plugin',
    errors: errors && errors.length ? errors : undefined,
    preset: q.get('preset'),
    fresh: q.has('fresh'),
  };
}
