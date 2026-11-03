// SPDX-License-Identifier: AGPL-3.0-or-later
// In-memory backend for browser development and tests. It owns parameter values with the real
// schema, a preset library, IR slots and UI state, and simulates telemetry and pitch events.
import type { Backend, NativeArgs, NativeName, NativeResult, SimulatedIntent } from './backend';
import { Emitter } from './emitter';
import {
  PROTOCOL_VERSION,
  type CaptureInfo,
  type CaptureState,
  type EventName,
  type Events,
  type HelloResponse,
  type IrState,
  type ParamSpec,
  type ParamValues,
  type PresetInfo,
  type PresetState,
  type SnapshotSlot,
  type UiState,
  type WrapperType,
} from './protocol';
import { cleanDetails, renameClashes, renamedPresetId, savedPresetId } from '../lib/presetFiles';
import { bundledSchema, defaultValues, indexSchema } from '../params/schema';
import { snapValue } from '../params/normalise';
import { isGlobalParam, sameValue } from '../params/presetScope';
import {
  DEFAULT_PRESET_ID,
  FACTORY_PRESETS,
  presetInfo,
  USER_PRESETS,
  type MockPreset,
} from './mock/factoryPresets';
import { CaptureSim } from './mock/captureSim';
import { PitchSimulator } from './mock/pitchSim';
import { createRandom } from './mock/random';
import { TelemetrySimulator } from './mock/telemetrySim';
import { UpdateSimulator, type UpdateScenario } from './mock/updateSim';

export interface MockOptions {
  /** Run telemetry/pitch timers (browser). Tests leave this off and call tick* themselves. */
  autoStart?: boolean;
  wrapperType?: WrapperType;
  isStandalone?: boolean;
  /** 12th-fret errors in cents per string, low string first. */
  intonationErrors?: number[];
  /** Where the simulated player leaves each open string, in cents from the target. */
  tuningOffsets?: number[];
  uiState?: UiState | null;
  /** localStorage key used to keep UI state across reloads in the browser; null disables. */
  persistKey?: string | null;
  /** Preset loaded at startup; null starts from Init. Defaults to Nacre. */
  initialPresetId?: string | null;
  /**
   * Values applied on top of the initial preset as if they were part of it (no dirty dot).
   * Used by demo URLs and trailer captures.
   */
  overrides?: ParamValues;
  /** What the update checker finds (docs/updates.md). Defaults to "current": nothing newer. */
  update?: UpdateScenario;
  /**
   * Delays each native call's answer and each preset broadcast by a random 0..maxMs (seeded).
   * Calls still run in the order they were made, as in the plugin, but their answers can arrive
   * out of order; tests use it to shake out ordering bugs.
   */
  jitter?: { maxMs: number; seed?: number };
}
// The plugin editor's timer: telemetry every other 60 Hz tick, pitch on every tick.
const TELEMETRY_HZ = 30;
const PITCH_HZ = 60;
const IR_NAMES = ['Tight room, close.wav', 'Back porch, far.wav', 'Small hall, edge.wav'];
const APP_VERSION = '0.1.0-dev';

type Handlers = { [K in NativeName]: (...args: NativeArgs<K>) => NativeResult<K> };

const MAX_RECENT = 12;

/** The preset-relevant state (PresetManager::Snapshot): per-preset values, current preset, reference. */
interface Snapshot {
  values: ParamValues;
  loaded: ParamValues;
  currentId: string | null;
  currentName: string;
  dirty: boolean;
}

const natural = (a: string, b: string) =>
  a.localeCompare(b, 'en', { numeric: true, sensitivity: 'base' });

/** Factory first, then folder, then name (PresetManager::scan). */
function sortPresets(items: PresetInfo[]): PresetInfo[] {
  const cat = (p: PresetInfo) => (p.category === 'Factory' ? 0 : 1);
  return [...items].sort(
    (a, b) => cat(a) - cat(b) || natural(a.folder, b.folder) || natural(a.name, b.name),
  );
}

export class MockBackend implements Backend {
  readonly kind = 'mock' as const;
  readonly schema: readonly ParamSpec[] = bundledSchema;
  private readonly specs = indexSchema(bundledSchema);
  private readonly emitter = new Emitter();
  private values: ParamValues = defaultValues(bundledSchema);
  /** Reference values for the dirty check: the loaded or last saved preset. */
  private loaded: ParamValues = defaultValues(bundledSchema);
  private presets: MockPreset[] = [...FACTORY_PRESETS, ...USER_PRESETS].map((p) => ({
    info: { ...p.info },
    values: { ...p.values },
  }));
  private presetState: PresetState = {
    items: [],
    currentId: null,
    currentName: 'Init',
    dirty: false,
    recent: [],
    compare: { active: 'A', other: null },
  };
  private snapshots: Partial<Record<SnapshotSlot, Snapshot>> = {};
  private compareOther: Snapshot | null = null;
  private removed: { preset: MockPreset; wasCurrent: boolean } | null = null;
  /** PresetState.revision: counts every state handed out. */
  private revision = 0;
  private readonly jitterRandom: (() => number) | null;
  private ir: IrState = {
    slots: [0, 1].flatMap((mic) =>
      [0, 1, 2].map((cab) => ({
        mic: mic as 0 | 1,
        cab: cab as 0 | 1 | 2,
        name: null,
        durationMs: null,
      })),
    ),
  };
  /** Captures (CaptureLibrary): the chooser offers made-up ones; nothing is played. */
  readonly captures = new CaptureSim();
  private uiState: UiState | null;
  private tunerActive = false;
  private taps: number[] = [];
  private timers: ReturnType<typeof setInterval>[] = [];
  private pitchTimer: ReturnType<typeof setInterval> | null = null;
  private readonly pitchSim: PitchSimulator;
  private readonly telemetrySim: TelemetrySimulator;
  /** The update checker; tests advance its checks and downloads with step(). */
  readonly updates: UpdateSimulator;

  constructor(private readonly options: MockOptions = {}) {
    this.jitterRandom = options.jitter ? createRandom(options.jitter.seed) : null;
    this.uiState = options.uiState ?? this.loadPersistedUiState();
    this.pitchSim = new PitchSimulator({
      getReferenceHz: () => this.value('tunerReference'),
      intonationErrors: options.intonationErrors,
      tuningOffsets: options.tuningOffsets,
    });
    this.telemetrySim = new TelemetrySimulator(
      (id) => this.value(id),
      this.specs.driftNote?.choices ?? [],
    );
    this.refreshPresetItems();
    const initial =
      options.initialPresetId === undefined ? DEFAULT_PRESET_ID : options.initialPresetId;
    if (initial) this.loadPreset(initial, false);
    for (const [id, value] of Object.entries(options.overrides ?? {})) {
      const spec = this.specs[id];
      if (!spec) continue;
      const v = snapValue(spec, value);
      this.values[id] = v;
      if (!isGlobalParam(id)) this.loaded[id] = v;
    }
    this.refreshPresetItems();
    this.updates = new UpdateSimulator({
      scenario: options.update,
      currentVersion: APP_VERSION,
      autoStart: options.autoStart,
      storageKey: options.persistKey ? 'sedef-mock-updates' : null,
      emit: (state) => this.emit('sedef:update', state),
    });
    if (options.autoStart) this.start();
  }

  readonly simulator = {
    setIntent: (intent: SimulatedIntent) => this.pitchSim.setIntent(intent),
  };

  // --- Backend --------------------------------------------------------------------------------

  async hello(): Promise<HelloResponse> {
    return this.handlers.sedef_hello(PROTOCOL_VERSION);
  }

  call<K extends NativeName>(name: K, ...args: NativeArgs<K>): Promise<NativeResult<K>> {
    const handler = this.handlers[name] as (...a: NativeArgs<K>) => NativeResult<K>;
    let result: NativeResult<K>;
    try {
      result = handler(...args);
    } catch (error) {
      return Promise.reject(error);
    }
    const delay = this.jitterDelay();
    if (delay === null) return Promise.resolve(result);
    return new Promise((resolve) => setTimeout(() => resolve(result), delay));
  }

  on<E extends EventName>(event: E, fn: (payload: Events[E]) => void): () => void {
    return this.emitter.on(event, fn);
  }

  // --- Test and demo helpers --------------------------------------------------------------------

  value(id: string): number {
    return this.values[id] ?? this.specs[id]?.default ?? 0;
  }

  /** Simulates host automation or a control surface moving a parameter. */
  hostSet(values: ParamValues): void {
    Object.assign(this.values, values);
    this.emit('sedef:params', { values: { ...values } });
  }

  tickTelemetry(dt = 1 / TELEMETRY_HZ): void {
    this.emitter.emit('sedef:telemetry', this.telemetrySim.step(dt));
  }

  tickPitch(dt = 1 / PITCH_HZ): void {
    if (this.tunerActive) this.emitter.emit('sedef:pitch', this.pitchSim.step(dt));
  }

  /** Publishes one fixed telemetry frame (static meters for screenshots). */
  publishStill(): void {
    const t = this.telemetrySim.step(0);
    this.emitter.emit('sedef:telemetry', {
      ...t,
      inputDb: [-17],
      outputDb: [-13, -14],
      inputClip: false,
      outputClip: false,
      gateOpen: true,
      gateReductionDb: 0,
      tideReductionDb: this.value('tideEnabled') >= 0.5 ? -3.5 : 0,
      cpu: 0.064,
    });
  }

  isTunerActive(): boolean {
    return this.tunerActive;
  }

  start(): void {
    if (this.timers.length) return;
    this.timers.push(setInterval(() => this.tickTelemetry(), 1000 / TELEMETRY_HZ));
  }

  dispose(): void {
    this.timers.forEach(clearInterval);
    this.timers = [];
    if (this.pitchTimer) clearInterval(this.pitchTimer);
    this.pitchTimer = null;
    this.updates.dispose();
  }

  // --- Native function implementations ---------------------------------------------------------

  private readonly handlers: Handlers = {
    sedef_hello: (clientProtocol) => {
      if (clientProtocol !== PROTOCOL_VERSION) throw new Error('protocol mismatch');
      return {
        protocol: PROTOCOL_VERSION,
        appVersion: APP_VERSION,
        wrapperType: this.options.wrapperType ?? 'Browser',
        isStandalone: this.options.isStandalone ?? true,
        sampleRate: 48000,
        latencySamples: 64,
        layout: 'Mono → Stereo',
        schema: [...this.schema],
        values: { ...this.values },
        uiState: this.uiState,
        presets: this.snapshotPresets(),
        ir: this.snapshotIr(),
        capture: this.captures.snapshot(),
      };
    },

    sedef_setParam: (id, value) => this.setValue(id, value),
    sedef_beginGesture: () => undefined,
    sedef_endGesture: () => undefined,
    sedef_resetParam: (id) => {
      const spec = this.specs[id];
      if (!spec) return;
      this.setValue(id, spec.default);
      this.emit('sedef:params', { values: { [id]: spec.default } });
    },

    sedef_presets_list: () => this.snapshotPresets(),
    sedef_presets_load: (id, audition) => {
      const found = this.presets.some((p) => p.info.id === id);
      this.loadPreset(id, true);
      if (found && !audition) this.markRecent(id);
      return this.publishPresets();
    },
    sedef_presets_markRecent: (id) => {
      this.markRecent(id);
      return this.publishPresets();
    },
    sedef_presets_save: (name, folder, details) => {
      const trimmed = name.trim() || 'Untitled';
      const dir = folder.trim();
      const id = savedPresetId(trimmed, dir);
      // Like PresetManager::save: same file (compared without case) means overwrite.
      const existing = this.presets.find(
        (p) => p.info.category === 'User' && p.info.id.toLowerCase() === id.toLowerCase(),
      );
      const values = this.presetValues();
      const clean = details ? cleanDetails(details) : null;
      const base = {
        id: existing?.info.id ?? id,
        name: trimmed,
        category: 'User' as const,
        folder: dir,
        author: existing?.info.author || 'You',
        tags: clean ? clean.tags : (existing?.info.tags ?? []),
        favorite: existing?.info.favorite ?? false,
        description: clean ? clean.description : (existing?.info.description ?? ''),
      };
      const saved: MockPreset = { info: presetInfo(base, values), values };
      this.presets = existing
        ? this.presets.map((p) => (p === existing ? saved : p))
        : [...this.presets, saved];
      // The saved state is the new reference for "dirty".
      this.loaded = { ...this.values };
      this.presetState = { ...this.presetState, currentId: saved.info.id, currentName: trimmed };
      this.markRecent(saved.info.id);
      return this.publishPresets();
    },
    sedef_presets_delete: (id) => {
      const preset = this.presets.find((p) => p.info.id === id);
      if (!preset || preset.info.category !== 'User') return this.snapshotPresets();
      this.presets = this.presets.filter((p) => p !== preset);
      const wasCurrent = this.presetState.currentId === id;
      this.removed = { preset, wasCurrent };
      if (wasCurrent) this.presetState = { ...this.presetState, currentId: null };
      return this.publishPresets();
    },
    sedef_presets_undoDelete: () => {
      const removed = this.removed;
      if (!removed || this.presets.some((p) => p.info.id === removed.preset.info.id))
        return this.snapshotPresets();
      this.removed = null;
      this.presets = [...this.presets, removed.preset];
      // Current again only when nothing else was loaded since (PresetManager::undoRemove).
      if (removed.wasCurrent && this.presetState.currentId === null) {
        this.presetState = {
          ...this.presetState,
          currentId: removed.preset.info.id,
          currentName: removed.preset.info.name,
        };
      }
      return this.publishPresets();
    },
    sedef_presets_rename: (id, newName) => {
      const preset = this.presets.find((p) => p.info.id === id);
      const trimmed = newName.trim();
      const items = this.presets.map((p) => p.info);
      // Like PresetManager::rename: the file is renamed too, never over another preset's file.
      if (
        preset &&
        preset.info.category === 'User' &&
        trimmed &&
        !renameClashes(items, id, trimmed)
      ) {
        const nextId = renamedPresetId(id, trimmed);
        preset.info = { ...preset.info, name: trimmed, id: nextId };
        const recent = this.presetState.recent.map((r) => (r === id ? nextId : r));
        this.presetState = {
          ...this.presetState,
          recent: recent.filter((r, i) => recent.indexOf(r) === i),
          ...(this.presetState.currentId === id ? { currentId: nextId, currentName: trimmed } : {}),
        };
        // Snapshots and the other A/B slot follow the preset to its new name.
        for (const snapshot of [...Object.values(this.snapshots), this.compareOther]) {
          if (snapshot?.currentId === id) {
            snapshot.currentId = nextId;
            snapshot.currentName = trimmed;
          }
        }
      }
      return this.publishPresets();
    },
    sedef_presets_setFavorite: (id, favorite) => {
      const preset = this.presets.find((p) => p.info.id === id);
      if (preset) preset.info = { ...preset.info, favorite };
      return this.publishPresets();
    },
    sedef_presets_step: (delta) => {
      const items = this.presetState.items;
      if (!items.length) return this.snapshotPresets();
      const index = items.findIndex((p) => p.id === this.presetState.currentId);
      const next =
        index < 0
          ? delta > 0
            ? 0
            : items.length - 1
          : (index + delta + items.length) % items.length;
      const target = items[next];
      if (target) this.loadPreset(target.id, true);
      return this.publishPresets();
    },
    sedef_presets_init: () => {
      this.applyPresetValues({});
      this.presetState = { ...this.presetState, currentId: null, currentName: 'Init' };
      this.emit('sedef:params', { values: { ...this.values } });
      return this.publishPresets();
    },
    sedef_presets_snapshot: (slot) => {
      this.snapshots[slot] = this.capture();
      return this.snapshotPresets();
    },
    sedef_presets_restore: (slot) => {
      const snapshot = this.snapshots[slot];
      if (snapshot) this.restore(snapshot);
      return this.publishPresets();
    },
    sedef_presets_compare: (action) => {
      const live = this.capture();
      if (action === 'copy') {
        this.compareOther = live;
      } else {
        // The first switch copies the live state, so B starts as A (PresetManager::compareSwitch).
        this.restore(this.compareOther ?? live);
        this.compareOther = live;
        const active = this.presetState.compare.active === 'A' ? 'B' : 'A';
        this.presetState = { ...this.presetState, compare: { active, other: null } };
      }
      return this.publishPresets();
    },
    sedef_presets_revealFolder: () => undefined,

    sedef_ir_load: (mic, cab) => {
      const name = IR_NAMES[(mic * 3 + cab) % IR_NAMES.length] ?? 'Impulse.wav';
      this.ir = {
        slots: this.ir.slots.map((s) =>
          s.mic === mic && s.cab === cab ? { ...s, name, durationMs: 180 + 60 * cab } : s,
        ),
      };
      this.emit('sedef:ir', this.snapshotIr());
      return this.snapshotIr();
    },
    sedef_ir_clear: (mic, cab) => {
      this.ir = {
        slots: this.ir.slots.map((s) =>
          s.mic === mic && s.cab === cab ? { ...s, name: null, durationMs: null } : s,
        ),
      };
      this.emit('sedef:ir', this.snapshotIr());
      return this.snapshotIr();
    },

    sedef_capture_load: () => this.captureLoaded(this.captures.choose()),
    sedef_capture_loadData: (fileName, text) =>
      this.captureLoaded(this.captures.drop(fileName, text)),
    sedef_capture_loadRecent: (path) => this.captureLoaded(this.captures.recent(path)),
    sedef_capture_clear: () => {
      this.captures.clear();
      this.setValue('ampSource', 0);
      this.emit('sedef:params', { values: { ampSource: 0 } });
      return this.publishCapture();
    },

    sedef_tuner_setActive: (active) => {
      this.tunerActive = active;
      if (active && this.options.autoStart && !this.pitchTimer) {
        this.pitchTimer = setInterval(() => this.tickPitch(), 1000 / PITCH_HZ);
      } else if (!active && this.pitchTimer) {
        clearInterval(this.pitchTimer);
        this.pitchTimer = null;
      }
    },

    sedef_metronome_setPattern: () => undefined,
    sedef_metronome_tap: () => {
      const now = Date.now();
      this.taps = [...this.taps.filter((t) => now - t < 2500), now].slice(-5);
      if (this.taps.length < 2) return this.value('tempo');
      const intervals = this.taps.slice(1).map((t, i) => t - (this.taps[i] ?? t));
      const mean = intervals.reduce((a, b) => a + b, 0) / intervals.length;
      const bpm = Math.min(240, Math.max(40, Math.round((60000 / mean) * 10) / 10));
      this.values.tempo = bpm;
      this.emit('sedef:params', { values: { tempo: bpm } });
      return bpm;
    },

    sedef_ui_setState: (state) => {
      this.uiState = state;
      this.persistUiState(state);
    },
    sedef_ui_setScale: () => undefined,
    sedef_openUrl: (url) => {
      if (typeof window !== 'undefined' && this.options.autoStart)
        window.open(url, '_blank', 'noopener');
    },

    sedef_update_getState: () => this.updates.getState(),
    sedef_update_check: () => this.updates.check(),
    sedef_update_setAutoCheck: (enabled) => this.updates.setAutoCheck(enabled),
    sedef_update_skip: (version) => this.updates.skip(version),
    sedef_update_install: () => this.updates.install(),
    sedef_update_installManually: () => this.updates.installManually(),
    sedef_update_cancel: () => this.updates.cancel(),
    sedef_update_reveal: () => this.updates.reveal(),
    sedef_update_restart: () => this.updates.restart(),
  };

  // --- Internals ------------------------------------------------------------------------------

  private setValue(id: string, value: number): void {
    const spec = this.specs[id];
    if (!spec) return;
    const next = snapValue(spec, value);
    if (this.values[id] === next) return;
    this.values[id] = next;
    if (isGlobalParam(id)) return;
    const dirty = this.isDirty();
    if (dirty !== this.presetState.dirty) {
      this.presetState = { ...this.presetState, dirty };
      this.publishPresets();
    }
  }

  /** Mirrors PresetManager::isDirty: any preset parameter away from the loaded reference. */
  private isDirty(): boolean {
    return this.schema.some(
      (spec) =>
        !isGlobalParam(spec.id) &&
        !sameValue(this.value(spec.id), this.loaded[spec.id] ?? spec.default, spec.min, spec.max),
    );
  }

  /** Loads preset values: stored ones snapped to range, missing ones default; globals untouched. */
  private applyPresetValues(stored: ParamValues): void {
    for (const spec of this.schema) {
      if (isGlobalParam(spec.id)) continue;
      const raw = stored[spec.id];
      const value = raw === undefined ? spec.default : snapValue(spec, raw);
      this.values[spec.id] = value;
      this.loaded[spec.id] = value;
    }
  }

  private loadPreset(id: string, notify: boolean): void {
    const preset = this.presets.find((p) => p.info.id === id);
    if (!preset) return;
    this.applyPresetValues(preset.values);
    this.presetState = { ...this.presetState, currentId: id, currentName: preset.info.name };
    if (notify) this.emit('sedef:params', { values: { ...this.values } });
  }

  private markRecent(id: string): void {
    this.presetState = {
      ...this.presetState,
      recent: [id, ...this.presetState.recent.filter((r) => r !== id)].slice(0, MAX_RECENT),
    };
  }

  private capture(): Snapshot {
    return {
      values: this.presetValues(),
      loaded: { ...this.loaded },
      currentId: this.presetState.currentId,
      currentName: this.presetState.currentName,
      dirty: this.isDirty(),
    };
  }

  /** Puts a snapshot back exactly; only the values that differ are sent as changes. */
  private restore(snapshot: Snapshot): void {
    const changed: ParamValues = {};
    for (const [id, value] of Object.entries(snapshot.values)) {
      if (this.values[id] !== value) changed[id] = value;
      this.values[id] = value;
    }
    this.loaded = { ...snapshot.loaded };
    // A preset deleted since the snapshot leaves its sound behind, no longer a preset.
    const exists = this.presets.some((p) => p.info.id === snapshot.currentId);
    this.presetState = {
      ...this.presetState,
      currentId: exists ? snapshot.currentId : null,
      currentName: snapshot.currentName,
    };
    if (Object.keys(changed).length) this.emit('sedef:params', { values: changed });
  }

  private presetValues(): ParamValues {
    return Object.fromEntries(
      this.schema.filter((s) => !isGlobalParam(s.id)).map((s) => [s.id, this.value(s.id)]),
    );
  }

  private refreshPresetItems(): void {
    const other = this.compareOther;
    this.presetState = {
      ...this.presetState,
      items: sortPresets(this.presets.map((p) => ({ ...p.info }))),
      dirty: this.isDirty(),
      compare: {
        active: this.presetState.compare.active,
        other: other ? { name: other.currentName, dirty: other.dirty } : null,
      },
    };
  }

  private snapshotPresets(): PresetState {
    this.refreshPresetItems();
    const s = this.presetState;
    return {
      ...s,
      items: s.items.map((p) => ({ ...p, tags: [...p.tags], blocks: [...p.blocks] })),
      recent: [...s.recent],
      compare: { active: s.compare.active, other: s.compare.other && { ...s.compare.other } },
      revision: ++this.revision,
    };
  }

  private publishPresets(): PresetState {
    this.refreshPresetItems();
    const snapshot = this.snapshotPresets();
    this.emit('sedef:presets', snapshot);
    return snapshot;
  }

  /** SedefProcessor::captureLoaded: plays it, through the cab or not as the metadata says. */
  private captureLoaded(info: CaptureInfo | null): CaptureState {
    if (info) {
      const values: ParamValues = { ampSource: 1 };
      if (info.kind !== 'unknown') values.captureCab = info.kind === 'amp' ? 1 : 0;
      for (const [id, v] of Object.entries(values)) this.setValue(id, v);
      this.emit('sedef:params', { values });
    }
    return this.publishCapture();
  }

  private publishCapture(): CaptureState {
    const snapshot = this.captures.snapshot();
    this.emit('sedef:capture', snapshot);
    return snapshot;
  }

  private snapshotIr(): IrState {
    return { slots: this.ir.slots.map((s) => ({ ...s })) };
  }

  /** Events from the real bridge arrive asynchronously; mirror that so ordering bugs show up. */
  private emit<E extends EventName>(event: E, payload: Events[E]): void {
    const delay = event === 'sedef:presets' ? this.jitterDelay() : null;
    if (delay === null) queueMicrotask(() => this.emitter.emit(event, payload));
    else setTimeout(() => this.emitter.emit(event, payload), delay);
  }

  private jitterDelay(): number | null {
    const max = this.options.jitter?.maxMs;
    return this.jitterRandom && max ? Math.floor(this.jitterRandom() * max) : null;
  }

  private loadPersistedUiState(): UiState | null {
    const key = this.options.persistKey;
    if (!key) return null;
    try {
      const raw = window.localStorage.getItem(key);
      return raw ? (JSON.parse(raw) as UiState) : null;
    } catch {
      return null;
    }
  }

  private persistUiState(state: UiState): void {
    const key = this.options.persistKey;
    if (!key) return;
    try {
      window.localStorage.setItem(key, JSON.stringify(state));
    } catch {
      // Storage can be unavailable (private mode); UI state is a convenience in the mock.
    }
  }
}
