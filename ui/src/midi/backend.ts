// SPDX-License-Identifier: AGPL-3.0-or-later
// Sedef MIDI's backends: the WebView bridge inside the plug-in, and a mock that plays a bass line
// so the UI runs in a plain browser and in tests.
import type { Backend, NativeArgs, NativeName, NativeResult } from '../bridge/backend';
import { isInsideJuce } from '../bridge/juceBackend';
import type { HelloResponse, ParamSpec, ParamValues } from '../bridge/protocol';
import generated from '../generated/midi-parameters.json';
import { defaultValues, indexSchema } from '../params/schema';
import {
  MIDI_PROTOCOL_VERSION,
  type MidiEventName,
  type MidiEvents,
  type MidiFunctionName,
  type MidiFunctions,
  type MidiHello,
  type MidiTelemetry,
  type PortsState,
} from './protocol';

type Args<K extends MidiFunctionName> = Parameters<MidiFunctions[K]>;
type Listener = (payload: never) => void;

class MidiEmitter {
  private listeners = new Map<MidiEventName, Set<Listener>>();

  on<E extends MidiEventName>(event: E, fn: (payload: MidiEvents[E]) => void): () => void {
    let set = this.listeners.get(event);
    if (!set) {
      set = new Set();
      this.listeners.set(event, set);
    }
    set.add(fn as Listener);
    return () => set.delete(fn as Listener);
  }

  emit<E extends MidiEventName>(event: E, payload: MidiEvents[E]): void {
    this.listeners.get(event)?.forEach((fn) => (fn as (p: MidiEvents[E]) => void)(payload));
  }
}
type Result<K extends MidiFunctionName> = Awaited<ReturnType<MidiFunctions[K]>>;

export const midiSchema: readonly ParamSpec[] = (generated as { parameters: ParamSpec[] })
  .parameters;

/**
 * What the MIDI UI needs from a backend. It also satisfies the shared Backend interface, so the
 * shared parameter sender (params/sender.ts) talks through it: those calls have the same names.
 */
export interface MidiBackend extends Backend {
  midiHello(): Promise<MidiHello>;
  invokeMidi<K extends MidiFunctionName>(name: K, ...args: Args<K>): Promise<Result<K>>;
  onMidi<E extends MidiEventName>(event: E, fn: (payload: MidiEvents[E]) => void): () => void;
}

// -------------------------------------------------------------------------------------------------

type NativeFn = (...args: unknown[]) => Promise<unknown>;

export class JuceMidiBackend implements MidiBackend {
  readonly kind = 'juce' as const;
  private fns = new Map<string, NativeFn>();

  private constructor(private readonly juce: typeof import('@juce-framework/webview')) {}

  static async create(): Promise<JuceMidiBackend> {
    return new JuceMidiBackend(await import('@juce-framework/webview'));
  }

  private fn(name: string): NativeFn {
    let f = this.fns.get(name);
    if (!f) {
      f = this.juce.getNativeFunction(name);
      this.fns.set(name, f);
    }
    return f;
  }

  midiHello(): Promise<MidiHello> {
    return this.invokeMidi('sedef_hello', MIDI_PROTOCOL_VERSION);
  }

  invokeMidi<K extends MidiFunctionName>(name: K, ...args: Args<K>): Promise<Result<K>> {
    return this.fn(name)(...args) as Promise<Result<K>>;
  }

  onMidi<E extends MidiEventName>(event: E, fn: (payload: MidiEvents[E]) => void): () => void {
    const handle = window.__JUCE__.backend.addEventListener(event, (payload) =>
      fn(payload as MidiEvents[E]),
    );
    return () => window.__JUCE__.backend.removeEventListener(handle);
  }

  // The shared Backend interface: only the parameter calls reach it.
  hello(): Promise<HelloResponse> {
    return Promise.reject(new Error('Sedef MIDI has no Sedef Amp handshake'));
  }
  call<K extends NativeName>(name: K, ...args: NativeArgs<K>): Promise<NativeResult<K>> {
    return this.fn(name)(...args) as Promise<NativeResult<K>>;
  }
  on(): () => void {
    return () => {};
  }
}

// -------------------------------------------------------------------------------------------------

export interface MockOptions {
  isStandalone?: boolean;
  isInstrument?: boolean;
  wrapperType?: string;
  /** Play the simulated bass line (off in tests that drive telemetry themselves). */
  autoPlay?: boolean;
  /** Start with this port selected. */
  destination?: string;
  /** Outputs that fail to open, as an unplugged device does. */
  unavailable?: readonly string[];
}

/** The automatic gate the mock reports: 20 dB over its silence, as the plug-in sets it. */
const MOCK_AUTO_GATE_DB = -68;

/** A bass line in E: MIDI notes and their lengths in beats. */
const LINE: readonly [number, number][] = [
  [40, 1],
  [40, 0.5],
  [43, 0.5],
  [45, 1],
  [47, 0.5],
  [45, 0.5],
  [43, 1],
  [38, 1],
];

export class MockMidiBackend implements MidiBackend {
  readonly kind = 'mock' as const;
  private readonly emitter = new MidiEmitter();
  private readonly specs = indexSchema(midiSchema);
  private values: ParamValues = defaultValues(midiSchema);
  private ports: PortsState;
  private timer: ReturnType<typeof setInterval> | null = null;
  private telemetryState: MidiTelemetry = {
    levelDb: -90,
    gateDb: MOCK_AUTO_GATE_DB,
    note: -1,
    pitch: 0,
    velocity: 0,
    sounding: false,
    notesSent: 0,
    lateNotes: 0,
    sampleRate: 48000,
  };
  readonly calls: { name: string; args: unknown[] }[] = [];

  constructor(private readonly options: MockOptions = {}) {
    const standalone = options.isStandalone ?? false;
    if (standalone || options.isInstrument) this.values.audio = 1;
    this.ports = {
      selected: options.destination ?? (standalone ? 'virtual' : 'off'),
      openName: '',
      status: '',
      virtualSupported: true,
      virtualName: standalone ? 'Sedef MIDI' : 'Sedef MIDI (plug-in)',
      outputs: [
        { id: 'device:iac-1', name: 'IAC Driver Bus 1' },
        { id: 'device:usb-1', name: 'USB MIDI Interface' },
      ],
    };
    this.ports.openName = this.nameOf(this.ports.selected);
    if (options.autoPlay ?? true) this.startPlaying();
  }

  private nameOf(destination: string): string {
    if (destination === 'virtual') return this.ports.virtualName;
    return this.ports.outputs.find((o) => o.id === destination)?.name ?? '';
  }

  midiHello(): Promise<MidiHello> {
    return Promise.resolve({
      protocol: MIDI_PROTOCOL_VERSION,
      appVersion: 'dev',
      wrapperType:
        this.options.wrapperType ?? (this.options.isStandalone ? 'Standalone' : 'Browser'),
      isStandalone: this.options.isStandalone ?? false,
      isInstrument: this.options.isInstrument ?? false,
      sampleRate: 48000,
      steadyLagMs: 38,
      schema: [...midiSchema],
      values: { ...this.values },
      ports: { ...this.ports, outputs: [...this.ports.outputs] },
    });
  }

  invokeMidi<K extends MidiFunctionName>(name: K, ...args: Args<K>): Promise<Result<K>> {
    this.calls.push({ name, args });
    const result = (() => {
      switch (name) {
        case 'sedef_hello':
          return this.midiHello();
        case 'sedef_setParam': {
          const [id, value] = args as Args<'sedef_setParam'>;
          if (this.specs[id]) this.values[id] = value;
          return undefined;
        }
        case 'sedef_resetParam': {
          const [id] = args as Args<'sedef_resetParam'>;
          const spec = this.specs[id];
          if (spec) {
            this.values[id] = spec.default;
            this.emitter.emit('sedef:params', { values: { [id]: spec.default } });
          }
          return undefined;
        }
        case 'sedef_midi_panic':
          this.telemetryState = { ...this.telemetryState, note: -1, sounding: false };
          this.emitter.emit('sedef:midi:telemetry', this.telemetryState);
          return undefined;
        case 'sedef_midi_ports':
          return { ...this.ports };
        case 'sedef_midi_setPort': {
          const [destination] = args as Args<'sedef_midi_setPort'>;
          const fails = this.options.unavailable?.includes(destination) ?? false;
          this.ports = {
            ...this.ports,
            selected: destination,
            openName: fails ? '' : this.nameOf(destination),
            status: fails
              ? 'That MIDI output is not available. Connect it, or pick another output.'
              : '',
          };
          this.emitter.emit('sedef:midi:ports', this.ports);
          return { ...this.ports };
        }
        default:
          return undefined;
      }
    })();
    return Promise.resolve(result as Result<K>);
  }

  onMidi<E extends MidiEventName>(event: E, fn: (payload: MidiEvents[E]) => void): () => void {
    return this.emitter.on(event, fn);
  }

  /** Tests: publish telemetry as the plug-in would. */
  publish(t: Partial<MidiTelemetry>): void {
    this.telemetryState = { ...this.telemetryState, ...t };
    this.emitter.emit('sedef:midi:telemetry', this.telemetryState);
  }

  /** Tests and demos: a value changed by the host (automation). */
  hostSets(values: ParamValues): void {
    Object.assign(this.values, values);
    this.emitter.emit('sedef:params', { values });
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  private startPlaying(): void {
    const bpm = 96;
    const start = Date.now();
    const total = LINE.reduce((sum, [, beats]) => sum + beats, 0);
    let lastIndex = -1;
    this.timer = setInterval(() => {
      const beats = (((Date.now() - start) / 60000) * bpm) % total;
      let at = 0;
      let index = 0;
      for (; index < LINE.length; index += 1) {
        const length = LINE[index]?.[1] ?? 1;
        if (beats < at + length) break;
        at += length;
      }
      const [played = 40, length = 1] = LINE[Math.min(index, LINE.length - 1)] ?? [];
      const into = (beats - at) / length;
      const octave = [-2, -1, 0, 1][Math.round(this.values.octave ?? 1)] ?? -1;
      const gate =
        Math.round(this.values.gateMode ?? 0) === 1 ? (this.values.gate ?? -75) : MOCK_AUTO_GATE_DB;
      const levelDb = -16 - 30 * into + 1.5 * Math.sin(Date.now() / 37);
      const sounding = into < 0.85 && levelDb > gate;
      if (index !== lastIndex && sounding) {
        this.telemetryState.notesSent += 1;
        this.telemetryState.velocity = 70 + ((index * 37) % 50);
        lastIndex = index;
      }
      const note = played + 12 * octave;
      const cents = 9 * Math.sin(Date.now() / 160);
      this.telemetryState = {
        ...this.telemetryState,
        levelDb: into < 0.9 ? levelDb : -88,
        gateDb: gate,
        note: sounding ? note : -1,
        pitch: sounding ? note + cents / 100 : 0,
        sounding,
      };
      this.emitter.emit('sedef:midi:telemetry', this.telemetryState);
    }, 33);
  }

  // The shared Backend interface, for the shared parameter sender.
  hello(): Promise<HelloResponse> {
    return Promise.reject(new Error('Sedef MIDI has no Sedef Amp handshake'));
  }
  call<K extends NativeName>(name: K, ...args: NativeArgs<K>): Promise<NativeResult<K>> {
    return this.invokeMidi(
      name as unknown as MidiFunctionName,
      ...(args as unknown as Args<MidiFunctionName>),
    ) as unknown as Promise<NativeResult<K>>;
  }
  on(): () => void {
    return () => {};
  }
}

export async function createMidiBackend(options: MockOptions = {}): Promise<MidiBackend> {
  if (isInsideJuce()) return JuceMidiBackend.create();
  return new MockMidiBackend(options);
}
