// SPDX-License-Identifier: AGPL-3.0-or-later
// The mock backend's captures (plugin/src/CaptureLibrary.cpp): a file chooser that offers two
// made-up captures in turn, dropped files read for their metadata, and the recent list. Nothing
// is played; the plugin validates and builds the real thing.
import type { CaptureInfo, CaptureKind, CaptureState } from '../protocol';

export const MAX_CAPTURE_BYTES = 24 * 1024 * 1024;
const MAX_RECENT = 12;
const ARCHITECTURES = ['WaveNet', 'LSTM', 'ConvNet', 'Linear'] as const;

/** What the simulated file chooser picks, in turn. Invented gear, no real products. */
export const DEMO_CAPTURES: CaptureInfo[] = [
  {
    name: 'Driftwood Crunch',
    fileName: 'Driftwood Crunch.nam',
    gear: 'Driftwood 30 Combo',
    gearType: 'amp',
    toneType: 'crunch',
    author: 'Sedef demo',
    architecture: 'WaveNet',
    sampleRate: 48000,
    kind: 'amp',
    inputLevelDbu: 12.2,
    normalisationDb: -3.1,
    path: '/Users/demo/Captures/Driftwood Crunch.nam',
  },
  {
    name: 'Tidepool Lead, close mic',
    fileName: 'Tidepool Lead.nam',
    gear: 'Tidepool 100',
    gearType: 'amp_cab',
    toneType: 'hi_gain',
    author: 'Sedef demo',
    architecture: 'LSTM',
    sampleRate: 44100,
    kind: 'fullRig',
    inputLevelDbu: null,
    normalisationDb: 2.4,
    path: '/Users/demo/Captures/Tidepool Lead.nam',
  },
];

function kindOf(gearType: string | null): CaptureKind {
  if (gearType === 'amp_cab' || gearType === 'amp_pedal_cab') return 'fullRig';
  if (gearType === 'amp' || gearType === 'pedal_amp' || gearType === 'preamp') return 'amp';
  return 'unknown';
}

const text = (v: unknown): string | null =>
  typeof v === 'string' && v.trim() ? v.trim().slice(0, 120) : null;
const num = (v: unknown): number | null => (typeof v === 'number' && Number.isFinite(v) ? v : null);

/** Reads a dropped file the way the plugin would describe it; throws the plugin's kind of message. */
export function describeCapture(fileName: string, source: string): CaptureInfo {
  if (source.length > MAX_CAPTURE_BYTES)
    throw new Error('This file is larger than 24 MB, so it isn’t a capture Sedef can play.');
  let json: unknown;
  try {
    json = JSON.parse(source);
  } catch {
    throw new Error('This isn’t a NAM capture: the file isn’t JSON.');
  }
  const root = (typeof json === 'object' && json !== null ? json : {}) as Record<string, unknown>;
  const architecture = ARCHITECTURES.find((a) => a === root.architecture);
  if (!architecture || !Array.isArray(root.weights) || typeof root.config !== 'object')
    throw new Error('This isn’t a NAM capture: it has no architecture, config or weights.');
  const meta = (
    typeof root.metadata === 'object' && root.metadata !== null ? root.metadata : {}
  ) as Record<string, unknown>;
  const gear = [text(meta.gear_make), text(meta.gear_model)].filter(Boolean).join(' ');
  const gearType = text(meta.gear_type);
  const rate = num(root.sample_rate);
  return {
    name: text(meta.name) ?? fileName.replace(/\.nam$/i, ''),
    fileName,
    gear: gear || null,
    gearType,
    toneType: text(meta.tone_type),
    author: text(meta.modeled_by),
    architecture,
    sampleRate: rate,
    kind: kindOf(gearType),
    inputLevelDbu: num(meta.input_level_dbu),
    normalisationDb: 0,
    path: `/Users/demo/Library/Application Support/Sedef/Captures/${fileName}`,
  };
}

export class CaptureSim {
  private state: CaptureState = {
    loaded: null,
    loading: false,
    error: null,
    missing: null,
    recent: [],
  };
  private known = new Map<string, CaptureInfo>(DEMO_CAPTURES.map((c) => [c.path ?? '', c]));
  private nextDemo = 0;

  snapshot(): CaptureState {
    const current = this.state.loaded?.path;
    return {
      ...this.state,
      loaded: this.state.loaded && { ...this.state.loaded },
      missing: this.state.missing && { ...this.state.missing },
      recent: this.state.recent.map((r) => ({ ...r, current: r.path === current })),
    };
  }

  /** The simulated chooser: the next demo capture. */
  choose(): CaptureInfo {
    const pick = DEMO_CAPTURES[this.nextDemo % DEMO_CAPTURES.length] ?? DEMO_CAPTURES[0]!;
    this.nextDemo += 1;
    return this.loaded(pick);
  }

  /** A dropped file. Throws nothing: a failure shows up as the state's error. */
  drop(fileName: string, source: string): CaptureInfo | null {
    try {
      const info = describeCapture(fileName, source);
      this.known.set(info.path ?? '', info);
      return this.loaded(info);
    } catch (error) {
      this.state = { ...this.state, error: error instanceof Error ? error.message : String(error) };
      return null;
    }
  }

  recent(path: string): CaptureInfo | null {
    const info = this.known.get(path);
    return info && this.state.recent.some((r) => r.path === path) ? this.loaded(info) : null;
  }

  clear(): void {
    this.state = { ...this.state, loaded: null, missing: null, error: null };
  }

  /** As a session whose capture can't be found (demo and tests). */
  setMissing(name: string, fileName: string, path: string): void {
    this.state = { ...this.state, loaded: null, missing: { name, fileName, path } };
  }

  private loaded(info: CaptureInfo): CaptureInfo {
    const path = info.path ?? '';
    const recent = [
      { name: info.name, fileName: info.fileName, path, current: true },
      ...this.state.recent.filter((r) => r.path !== path),
    ].slice(0, MAX_RECENT);
    this.state = { loaded: { ...info }, loading: false, error: null, missing: null, recent };
    return info;
  }
}
