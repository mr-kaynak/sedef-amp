// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Sedef bridge protocol, version 1. This file is the contract between the web UI and the C++
// plugin shell (plugin/src/PluginEditor.cpp, which registers the native functions and emits the
// events). Any change here must be mirrored there and bump PROTOCOL_VERSION when it is not
// backwards compatible.
//
// Transport: JUCE WebBrowserComponent native functions (UI -> C++, return a Promise) and events
// (C++ -> UI). All parameter values cross the bridge in REAL units (dB, Hz, ms, 0..1 for knobs,
// index for choices, 0/1 for bools), never normalised.

export const PROTOCOL_VERSION = 1;

// ---------------------------------------------------------------------------------------------
// Schema

export type ParamKind = 'float' | 'int' | 'bool' | 'choice';

export interface ParamSpec {
  id: string;
  name: string;
  group: string;
  kind: ParamKind;
  min: number;
  max: number;
  default: number;
  /** JUCE NormalisableRange skew: normalised = ((value - min) / (max - min)) ^ skew */
  skew: number;
  /** "dB" | "Hz" | "ms" | "s" | "st" | "%" | "bpm" | "knob" | "" ("knob" = 0..1 shown as 0-10) */
  unit: string;
  automatable: boolean;
  choices: string[];
}

export type ParamValues = Record<string, number>;

// ---------------------------------------------------------------------------------------------
// Presets

export interface PresetInfo {
  /** Stable id: "factory:<slug>" or "user:<relative path>" */
  id: string;
  name: string;
  category: 'Factory' | 'User';
  /** Folder inside the category, e.g. "Clean", "Lead"; empty for root. */
  folder: string;
  author: string;
  tags: string[];
  favorite: boolean;
}

export interface PresetState {
  items: PresetInfo[];
  /** Currently loaded preset id, or null for an unsaved/initial state. */
  currentId: string | null;
  currentName: string;
  /** True when parameters differ from the loaded preset. */
  dirty: boolean;
}

// ---------------------------------------------------------------------------------------------
// Custom impulse responses (one slot per mic x cab)

export interface IrSlot {
  mic: 0 | 1;
  cab: 0 | 1 | 2;
  /** File name of the loaded IR, or null when empty. */
  name: string | null;
  durationMs: number | null;
}

export interface IrState {
  slots: IrSlot[];
}

// ---------------------------------------------------------------------------------------------
// Hello (initial handshake)

export type WrapperType = 'Standalone' | 'VST3' | 'AudioUnit' | 'CLAP' | 'Browser';

export interface HelloResponse {
  protocol: number;
  appVersion: string;
  wrapperType: WrapperType;
  isStandalone: boolean;
  sampleRate: number;
  latencySamples: number;
  /** Input/output channel layout chosen by the host, e.g. "Mono", "Mono → Stereo", "Stereo". */
  layout: string;
  schema: ParamSpec[];
  values: ParamValues;
  /** Opaque UI state previously stored with ui.setState (null on first run). */
  uiState: UiState | null;
  presets: PresetState;
  ir: IrState;
}

/** UI-owned state persisted inside the plugin state. The plugin treats it as opaque JSON. */
export interface UiState {
  section?: 'pedals' | 'amp' | 'eq' | 'cab' | 'fx';
  scale?: number;
  metronomeSteps?: number[];
  intonation?: unknown;
  [key: string]: unknown;
}

// ---------------------------------------------------------------------------------------------
// Native functions (UI -> C++). Names are registered with WebBrowserComponent::withNativeFunction.

export interface NativeFunctions {
  sedef_hello(clientProtocol: number): HelloResponse;

  sedef_setParam(id: string, value: number): void;
  sedef_beginGesture(id: string): void;
  sedef_endGesture(id: string): void;
  sedef_resetParam(id: string): void;

  sedef_presets_list(): PresetState;
  sedef_presets_load(id: string): PresetState;
  /** Saves the current state as a user preset; overwrites when a user preset with that name exists. */
  sedef_presets_save(name: string, folder: string): PresetState;
  sedef_presets_delete(id: string): PresetState;
  sedef_presets_rename(id: string, newName: string): PresetState;
  sedef_presets_setFavorite(id: string, favorite: boolean): PresetState;
  sedef_presets_step(delta: number): PresetState;
  sedef_presets_init(): PresetState;
  sedef_presets_revealFolder(): void;

  /** Opens a native file chooser; resolves after the IR is loaded (or the chooser is cancelled). */
  sedef_ir_load(mic: number, cab: number): IrState;
  sedef_ir_clear(mic: number, cab: number): IrState;

  /** Starts/stops pitch analysis (tuner and intonation). Events flow only while active. */
  sedef_tuner_setActive(active: boolean): void;

  /**
   * One entry per step, step 0 = downbeat, up to 13 entries (Metronome::kMaxSteps). Values follow
   * Metronome::StepState: 0 off, 1 normal, 2 accent, 3 soft. The UI always sends all 13 so a
   * pattern survives switching to a shorter signature and back.
   */
  sedef_metronome_setPattern(steps: number[]): void;
  sedef_metronome_tap(): number; // returns new BPM

  sedef_ui_setState(state: UiState): void;
  /** Requests a new editor size in CSS pixels at scale 1 multiplied by scale. */
  sedef_ui_setScale(scale: number): void;
  sedef_openUrl(url: string): void;
}

// ---------------------------------------------------------------------------------------------
// Events (C++ -> UI). Event ids are the keys of this interface.

export interface TelemetryEvent {
  /** Peak levels in dBFS since the previous event, per channel (1 or 2 entries). */
  inputDb: number[];
  outputDb: number[];
  inputClip: boolean;
  outputClip: boolean;
  gateOpen: boolean;
  gateReductionDb: number;
  tideReductionDb: number;
  /** Current metronome step or -1. */
  metronomeStep: number;
  /** Effective Drift delay time in ms (after sync). */
  driftTimeMs: number;
  /** Audio thread load estimate 0..1. */
  cpu: number;
}

export interface PitchEvent {
  valid: boolean;
  frequencyHz: number;
  midiNote: number;
  /** Note name without octave, e.g. "C#". */
  note: string;
  octave: number;
  cents: number;
  clarity: number;
  levelDb: number;
  stable: boolean;
  stableHz: number;
  stableStdCents: number;
  stableCount: number;
  onsetId: number;
}

export interface Events {
  /** Changed parameter values (host automation, preset loads, undo). Batched at 60 Hz. */
  'sedef:params': { values: ParamValues };
  /** 30 Hz while the editor is open. */
  'sedef:telemetry': TelemetryEvent;
  /** 60 Hz while pitch analysis is active (sedef_tuner_setActive). */
  'sedef:pitch': PitchEvent;
  'sedef:presets': PresetState;
  'sedef:ir': IrState;
}

export type EventName = keyof Events;
