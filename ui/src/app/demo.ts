// SPDX-License-Identifier: AGPL-3.0-or-later
// URL flags for demos, screenshots and manual QA against the mock backend:
//   ?section=pedals|amp|eq|cab|fx   open a section
//   ?overlay=tuner|intonation        open an overlay on load
//   ?host=plugin|standalone          present as a plugin (no metronome) or the standalone app
//                                    instead of the browser preview
//   ?errors=3.6,-2.4,1.1             12th-fret errors (cents) the simulated guitar produces
//   ?preset=soft-focus               start from a factory preset (slug, default nacre)
//   ?set=pearlEnabled:1,pearlGain:0.4  parameter values applied as part of that preset
//   ?board=gate,pearl,ripple         the pedalboard, as part of that preset
//   ?freeze                          no simulation: static meters, pitch only from the demo hook
//   ?demo                            expose window.__sedefDemo for scripted frames (captures)
//   ?grain=0                         hide the lacquer grain (smaller screenshots)
//   ?fresh                           ignore UI state saved by earlier browser sessions
//   ?midi=demo                       a few MIDI bindings, picked scene values and made scenes
//   ?update=available                what the update checker finds: current|available|skipped|
//                                    error|noinstaller|mismatch|unsupported (docs/updates.md)
//   ?input=silent|quiet|clipping     what the simulated interface delivers (docs/ui/input-level.md)
//   ?recorder=recording              the recorder or re-amp in a state: ready|recording|saved|
//                                    dropouts|error|reamp|converting|reamp-empty (docs/recorder.md)
import { isRecorderDemo, type RecorderDemo } from '../bridge/mock/recorderSim';
import { isInterfaceLevel, type InterfaceLevel } from '../bridge/mock/telemetrySim';
import { isUpdateScenario, type UpdateScenario } from '../bridge/mock/updateSim';
import { isSection, type Overlay, type Section } from '../state/uiStore';

export interface DemoFlags {
  section: Section | null;
  overlay: Overlay;
  host: 'browser' | 'plugin' | 'standalone';
  errors: number[] | undefined;
  preset: string | null;
  overrides: Record<string, number>;
  /** Pedal ids in signal order; null keeps the preset's board. */
  board: string[] | null;
  freeze: boolean;
  demo: boolean;
  grain: boolean;
  fresh: boolean;
  update: UpdateScenario | null;
  input: InterfaceLevel | null;
  recorder: RecorderDemo | null;
  /** Seed the live layer with example bindings and scenes (docs/live.md). */
  midi: boolean;
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
    board: q.has('board')
      ? (q.get('board') ?? '')
          .split(',')
          .map((id) => id.trim())
          .filter(Boolean)
      : null,
    freeze: q.has('freeze'),
    demo: q.has('demo'),
    grain: q.get('grain') !== '0',
    fresh: q.has('fresh'),
    midi: q.get('midi') === 'demo',
    update: isUpdateScenario(q.get('update')) ? (q.get('update') as UpdateScenario) : null,
    input: isInterfaceLevel(q.get('input')) ? (q.get('input') as InterfaceLevel) : null,
    recorder: isRecorderDemo(q.get('recorder')) ? (q.get('recorder') as RecorderDemo) : null,
  };
}
