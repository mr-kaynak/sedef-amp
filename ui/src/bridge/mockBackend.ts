// SPDX-License-Identifier: AGPL-3.0-or-later
// In-memory backend for browser development and tests. It owns parameter values with the real
// schema, a preset library, IR slots and UI state, and simulates telemetry and pitch events.
import type { Backend, NativeArgs, NativeName, NativeResult, SimulatedIntent } from './backend';
import { Emitter } from './emitter';
import {
  PROTOCOL_VERSION,
  type EventName,
  type Events,
  type HelloResponse,
  type IrState,
  type ParamSpec,
  type ParamValues,
  type PresetInfo,
  type PresetState,
  type UiState,
  type WrapperType,
} from './protocol';
import { bundledSchema, defaultValues, indexSchema } from '../params/schema';
import { snapValue } from '../params/normalise';
import { isGlobalParam, sameValue } from '../params/presetScope';
import {
  DEFAULT_PRESET_ID,
  FACTORY_PRESETS,
  USER_PRESETS,
  type MockPreset,
} from './mock/factoryPresets';
import { PitchSimulator } from './mock/pitchSim';
import { TelemetrySimulator } from './mock/telemetrySim';

export interface MockOptions {
  /** Run telemetry/pitch timers (browser). Tests leave this off and call tick* themselves. */
  autoStart?: boolean;
  wrapperType?: WrapperType;
  isStandalone?: boolean;
  /** 12th-fret errors in cents per string, low string first. */
  intonationErrors?: number[];
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
}
const TELEMETRY_HZ = 30;
const PITCH_HZ = 50;
const IR_NAMES = ['Tight room, close.wav', 'Back porch, far.wav', 'Small hall, edge.wav'];

type Handlers = { [K in NativeName]: (...args: NativeArgs<K>) => NativeResult<K> };

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
  };
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
  private uiState: UiState | null;
  private tunerActive = false;
  private taps: number[] = [];
  private timers: ReturnType<typeof setInterval>[] = [];
  private pitchTimer: ReturnType<typeof setInterval> | null = null;
  private readonly pitchSim: PitchSimulator;
  private readonly telemetrySim: TelemetrySimulator;

  constructor(private readonly options: MockOptions = {}) {
    this.uiState = options.uiState ?? this.loadPersistedUiState();
    this.pitchSim = new PitchSimulator({
      getReferenceHz: () => this.value('tunerReference'),
      intonationErrors: options.intonationErrors,
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
    try {
      return Promise.resolve(handler(...args));
    } catch (error) {
      return Promise.reject(error);
    }
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
  }

  // --- Native function implementations ---------------------------------------------------------

  private readonly handlers: Handlers = {
    sedef_hello: (clientProtocol) => {
      if (clientProtocol !== PROTOCOL_VERSION) throw new Error('protocol mismatch');
      return {
        protocol: PROTOCOL_VERSION,
        appVersion: '0.1.0-dev',
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
    sedef_presets_load: (id) => {
      this.loadPreset(id, true);
      return this.snapshotPresets();
    },
    sedef_presets_save: (name, folder) => {
      const trimmed = name.trim() || 'Untitled';
      const existing = this.presets.find(
        (p) => p.info.category === 'User' && p.info.name.toLowerCase() === trimmed.toLowerCase(),
      );
      const values = this.presetValues();
      if (existing) {
        existing.values = values;
        existing.info.folder = folder;
      } else {
        this.presets.push({
          info: {
            id: `user:${folder ? `${folder}/` : ''}${trimmed}.sedefpreset`,
            name: trimmed,
            category: 'User',
            folder,
            author: '',
            tags: [],
            favorite: false,
          },
          values,
        });
      }
      const saved = existing ?? this.presets[this.presets.length - 1];
      // The saved state is the new reference for "dirty".
      this.loaded = { ...this.values };
      this.presetState = {
        ...this.presetState,
        currentId: saved?.info.id ?? null,
        currentName: trimmed,
      };
      return this.publishPresets();
    },
    sedef_presets_delete: (id) => {
      const preset = this.presets.find((p) => p.info.id === id);
      if (!preset || preset.info.category !== 'User') return this.snapshotPresets();
      this.presets = this.presets.filter((p) => p !== preset);
      if (this.presetState.currentId === id) {
        this.presetState = { ...this.presetState, currentId: null };
      }
      return this.publishPresets();
    },
    sedef_presets_rename: (id, newName) => {
      const preset = this.presets.find((p) => p.info.id === id);
      const trimmed = newName.trim();
      if (preset && preset.info.category === 'User' && trimmed) {
        preset.info.name = trimmed;
        if (this.presetState.currentId === id) {
          this.presetState = { ...this.presetState, currentName: trimmed };
        }
      }
      return this.publishPresets();
    },
    sedef_presets_setFavorite: (id, favorite) => {
      const preset = this.presets.find((p) => p.info.id === id);
      if (preset) preset.info.favorite = favorite;
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
      return this.snapshotPresets();
    },
    sedef_presets_init: () => {
      this.applyPresetValues({});
      this.presetState = { ...this.presetState, currentId: null, currentName: 'Init' };
      this.emit('sedef:params', { values: { ...this.values } });
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
    if (notify) {
      this.emit('sedef:params', { values: { ...this.values } });
      this.publishPresets();
    }
  }

  private presetValues(): ParamValues {
    return Object.fromEntries(
      this.schema.filter((s) => !isGlobalParam(s.id)).map((s) => [s.id, this.value(s.id)]),
    );
  }

  private refreshPresetItems(): void {
    this.presetState = {
      ...this.presetState,
      items: sortPresets(this.presets.map((p) => ({ ...p.info }))),
      dirty: this.isDirty(),
    };
  }

  private snapshotPresets(): PresetState {
    return {
      ...this.presetState,
      items: this.presetState.items.map((p) => ({ ...p, tags: [...p.tags] })),
    };
  }

  private publishPresets(): PresetState {
    this.refreshPresetItems();
    const snapshot = this.snapshotPresets();
    this.emit('sedef:presets', snapshot);
    return snapshot;
  }

  private snapshotIr(): IrState {
    return { slots: this.ir.slots.map((s) => ({ ...s })) };
  }

  /** Events from the real bridge arrive asynchronously; mirror that so ordering bugs show up. */
  private emit<E extends EventName>(event: E, payload: Events[E]): void {
    queueMicrotask(() => this.emitter.emit(event, payload));
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
