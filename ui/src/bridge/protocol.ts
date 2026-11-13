// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Sedef bridge protocol, version 2. This file is the contract between the web UI and the C++
// plugin shell (plugin/src/PluginEditor.cpp, which registers the native functions and emits the
// events). Any change here must be mirrored there and bump PROTOCOL_VERSION when it is not
// backwards compatible.
//
// Transport: JUCE WebBrowserComponent native functions (UI -> C++, return a Promise) and events
// (C++ -> UI). All parameter values cross the bridge in REAL units (dB, Hz, ms, 0..1 for knobs,
// index for choices, 0/1 for bools), never normalised.

export const PROTOCOL_VERSION = 2;

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
  /** "dB" | "dBu" | "Hz" | "ms" | "s" | "st" | "%" | "bpm" | "knob" | "" ("knob" = 0..1 shown as 0-10) */
  unit: string;
  automatable: boolean;
  choices: string[];
}

export type ParamValues = Record<string, number>;

// ---------------------------------------------------------------------------------------------
// Presets

/** The blocks after the amp and cab, in signal order. */
export type PostBlock = 'eq' | 'doubler' | 'drift' | 'fathom';

/**
 * A block a preset switches on besides the amp and cab: a pedal id from the pedal registry
 * (src/generated/pedals.json), or a post block.
 */
export type PresetBlock = string;

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
  /** One line about the sound (the file's optional "description"); empty when absent. */
  description: string;
  /** ampModel choice index (0 Lagoon, 1 Reef, 2 Abyss), computed with defaults applied. */
  amp: number;
  /** The cab it plays through: the amp's own while ampCabLink is on, otherwise cabModel. */
  cab: number;
  /**
   * Switched-on blocks besides the amp and cab, in signal order: the pedals on its board that it
   * switches on, in the board's order, then the post blocks. The EQ counts only when the amp's
   * bank is on and not flat.
   */
  blocks: PresetBlock[];
}

/** A/B compare. The active slot is the live state; the other one is held by the plugin. */
export interface CompareState {
  active: 'A' | 'B';
  /** The slot not being heard, or null until it has been filled. */
  other: { name: string; dirty: boolean } | null;
}

export interface PresetState {
  items: PresetInfo[];
  /** Currently loaded preset id, or null for an unsaved/initial state. */
  currentId: string | null;
  currentName: string;
  /** True when parameters differ from the loaded preset. */
  dirty: boolean;
  /**
   * Presets picked most recently, newest first (at most 12). Kept with the favourites, so it
   * spans sessions; it may name presets that no longer exist.
   */
  recent: string[];
  compare: CompareState;
  /**
   * Counts up with every state the plugin sends. An action's answer can arrive after a newer
   * broadcast, so the editor drops a state older than the one it has.
   */
  revision?: number;
}

/** Optional metadata stored with a user preset. */
export interface PresetDetails {
  tags: string[];
  /** One line, at most 140 characters. */
  description: string;
}

/**
 * Snapshots of the preset-relevant state (every per-preset parameter, the current preset and
 * its dirty reference) that the plugin keeps for the editor. "browse" is taken when the browser
 * opens (Esc goes back to it), "undo" before a load that would drop unsaved edits. A host state
 * restore invalidates them.
 */
export type SnapshotSlot = 'browse' | 'undo';

// ---------------------------------------------------------------------------------------------
// The pedalboard (docs/architecture.md, "The pedalboard"). Not a parameter: it is saved with the
// session and presets and carried by snapshots and A/B. Pedals off the board keep their
// parameters but don't play.

export interface BoardState {
  /** Pedal ids in signal order, input first. */
  pedals: string[];
  /** Counts up with every board state the plugin sends; drop one older than the one you have. */
  revision?: number;
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
// Captures: Neural Amp Modeler (.nam) files played in place of the amp (docs/dsp/nam.md)

/** What the capture's metadata says it contains. */
export type CaptureKind = 'amp' | 'fullRig' | 'unknown';

export interface CaptureInfo {
  /** The metadata's name, else the file name without its extension. */
  name: string;
  fileName: string;
  /** "<make> <model>" from the metadata, or null. */
  gear: string | null;
  gearType: string | null;
  toneType: string | null;
  /** Who made the capture (metadata "modeled_by"), or null. */
  author: string | null;
  architecture: 'WaveNet' | 'LSTM' | 'ConvNet' | 'Linear';
  /** The model's own rate, or null when the file doesn't say (it then runs at 48 kHz). */
  sampleRate: number | null;
  kind: CaptureKind;
  /** Interface level at 0 dBFS when it was captured; null when the file doesn't say. */
  inputLevelDbu: number | null;
  /** Gain Sedef applies so the capture plays at the built-in amps' level. */
  normalisationDb: number;
  /** Where it was loaded from; null for a session opened on another machine. */
  path: string | null;
}

export interface RecentCapture {
  name: string;
  fileName: string;
  path: string;
  current: boolean;
}

export interface CaptureState {
  loaded: CaptureInfo | null;
  /** A load the player asked for is running (files take up to a second to build). */
  loading: boolean;
  /** Why the last load failed, as a sentence; null after a success. */
  error: string | null;
  /** A capture the session or preset asks for that can't be found. */
  missing: { name: string; fileName: string; path: string } | null;
  /** Last loaded captures, newest first (at most 12); files that are gone are left out. */
  recent: RecentCapture[];
}

// ---------------------------------------------------------------------------------------------
// Updates (docs/updates.md). The settings live in a per-user file, never in the plugin state.

export type UpdatePlatform = 'macOS' | 'Windows' | 'Linux';

export interface ReleaseInfo {
  /** Semver without the "v", e.g. "0.2.0". */
  version: string;
  tag: string;
  name: string;
  /** Markdown from the release. Untrusted: render with features/updates/ReleaseNotes.tsx only. */
  notes: string;
  /** The release page, always under https://github.com/mr-kaynak/sedef-amp/releases. */
  url: string;
  /** ISO 8601; may be empty. */
  publishedAt: string;
  /** This OS's installer, or null when the release has none (then: the releases page). */
  installer: { name: string; size: number } | null;
  hasChecksums: boolean;
}

export type UpdatePhase =
  | 'idle'
  | 'checking'
  | 'downloading'
  | 'verifying'
  /** The OS's installer runs: `install.step` says where it is. */
  | 'installing'
  /** Installed (by this process or another): restart the host to use `installedVersion`. */
  | 'installed'
  /** Windows: Setup waits for the host to exit, then installs `installedVersion`. */
  | 'scheduled'
  /** "Install manually": the verified installer is in Downloads and open. */
  | 'done'
  | 'error';

export type InstallStep =
  /** Checking the file again, mounting or unpacking it. */
  | 'preparing'
  /** The OS asks for a password or permission; the install runs right after. */
  | 'permission'
  /** Installing with nothing to ask (Linux, in your home folder). */
  | 'installing'
  /** Detaching, checking the installed versions. */
  | 'finishing';

/** Why "Update now" can't install here; the panel offers Install manually instead. */
export type ManualReason = 'elsewhere' | 'sandboxed' | 'noElevation' | 'unsupported';

export type UpdateErrorCode =
  | 'network'
  | 'rateLimited'
  | 'badResponse'
  | 'noInstaller'
  | 'noChecksum'
  | 'checksumMismatch'
  | 'incomplete'
  | 'disk'
  | 'openFailed'
  | 'badPackage'
  | 'elevationFailed'
  | 'installFailed'
  | 'unverified';

export type UpdateState =
  /** Built with SEDEF_UPDATE_CHECK=OFF: the UI shows nothing about updates. */
  | { supported: false; currentVersion: string }
  | {
      supported: true;
      currentVersion: string;
      platform: UpdatePlatform;
      autoCheck: boolean;
      /** False when SEDEF_DISABLE_UPDATE_CHECK (or the UI smoke test) turns automatic checks off. */
      autoCheckAllowed: boolean;
      /** '' when none. */
      skippedVersion: string;
      /** Last successful check, ms since the epoch; null when there was none. */
      lastChecked: number | null;
      phase: UpdatePhase;
      /** The latest stable release from the last successful check. */
      latest: ReleaseInfo | null;
      /** latest is newer than currentVersion (whether or not it was skipped). */
      updateAvailable: boolean;
      error: { code: UpdateErrorCode; detail: string } | null;
      /** Bytes, while downloading or verifying. */
      progress: { received: number; total: number } | null;
      /** While installing. */
      install: { step: InstallStep } | null;
      /** The version that is installed (phase installed) or waits for the host to exit (scheduled). */
      installedVersion: string | null;
      /** What to restart: the DAW's name, "your DAW" when it isn't known, "Sedef" standalone. */
      host: string;
      standalone: boolean;
      /** The standalone app can restart itself into installedVersion (sedef_update_restart). */
      canRestart: boolean;
      /** Whether "Update now" can install here, or only Install manually. */
      autoInstall: { available: boolean; reason: ManualReason | null };
      /** "Install manually": the verified installer in Downloads. */
      download: { fileName: string; opened: boolean } | null;
      releasesUrl: string;
    };

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
  /** Absent from plug-ins built before captures. */
  capture?: CaptureState;
  /** Absent from plug-ins built before the pedalboard (their pedals play in a fixed order). */
  board?: BoardState;
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
  /** Loads a preset. An audition (a click in the browser) does not count as a Recent pick. */
  sedef_presets_load(id: string, audition?: boolean): PresetState;
  /** Records a pick for Recent without loading (the browser keeps an auditioned preset). */
  sedef_presets_markRecent(id: string): PresetState;
  /**
   * Saves the current state as a user preset and makes it current; overwrites a user preset with
   * that name in that folder. Without details, an overwritten preset keeps its tags and
   * description.
   */
  sedef_presets_save(name: string, folder: string, details?: PresetDetails): PresetState;
  sedef_presets_delete(id: string): PresetState;
  /** Puts back the user preset deleted last (same file, same id). */
  sedef_presets_undoDelete(): PresetState;
  sedef_presets_rename(id: string, newName: string): PresetState;
  sedef_presets_setFavorite(id: string, favorite: boolean): PresetState;
  sedef_presets_step(delta: number): PresetState;
  sedef_presets_init(): PresetState;
  sedef_presets_revealFolder(): void;
  sedef_presets_snapshot(slot: SnapshotSlot): PresetState;
  /** Restores a snapshot exactly (values, current preset, dirty state); the slot is kept. */
  sedef_presets_restore(slot: SnapshotSlot): PresetState;
  /**
   * A/B compare. "switch" stores the live state in the active slot and brings back the other one
   * (the first switch copies the live state, so B starts as A); "copy" copies the live state into
   * the other slot.
   */
  sedef_presets_compare(action: 'switch' | 'copy'): PresetState;

  /** Opens a native file chooser; resolves after the IR is loaded (or the chooser is cancelled). */
  sedef_ir_load(mic: number, cab: number): IrState;
  sedef_ir_clear(mic: number, cab: number): IrState;

  /**
   * Opens a native file chooser for a .nam file; resolves once it is loaded (or the chooser is
   * cancelled). A successful load sets the amp source to Capture and, when the metadata says,
   * "Capture Through Cab".
   */
  sedef_capture_load(): CaptureState;
  /** A file dropped on the editor: its name and contents (at most 24 MB). Kept by the plugin. */
  sedef_capture_loadData(fileName: string, text: string): CaptureState;
  /** One of CaptureState.recent, by path. */
  sedef_capture_loadRecent(path: string): CaptureState;
  /** Unloads the capture and goes back to the amp models. */
  sedef_capture_clear(): CaptureState;

  /**
   * Replaces the pedalboard: pedal ids in signal order. Unknown ids and repeats are dropped. The
   * engine takes it at the next block, through bypass fades or a short duck.
   */
  sedef_board_set(pedals: string[]): BoardState;

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
  /** Standalone app only: opens its audio device window (interface, input, rate, buffer). */
  sedef_audioSettings(): void;

  /** The update state; also runs the automatic check when it is on and due (once a day). */
  sedef_update_getState(): UpdateState;
  /** Checks now, ignoring the once-a-day limit. */
  sedef_update_check(): UpdateState;
  sedef_update_setAutoCheck(enabled: boolean): UpdateState;
  /** Stops offering a version; '' offers it again. */
  sedef_update_skip(version: string): UpdateState;
  /**
   * "Update now": downloads this OS's installer into a private cache, verifies its SHA-256 and
   * installs it through the OS's installer. Where that can't run, installs manually. Progress:
   * sedef:update.
   */
  sedef_update_install(): UpdateState;
  /** "Install manually": the verified installer goes to Downloads and opens, as a download would. */
  sedef_update_installManually(): UpdateState;
  /** Stops a download (not the OS's installer: its own prompt is where to cancel). */
  sedef_update_cancel(): UpdateState;
  /** Shows the installer "Install manually" saved in the file manager. */
  sedef_update_reveal(): UpdateState;
  /** The standalone app, when canRestart: quits and opens the new version. */
  sedef_update_restart(): UpdateState;
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
  /**
   * What the tuner needle shows (the engine's TunerDisplay, docs/dsp/tuner.md "Display"): calm on
   * a sustained note, prompt on a peg turn, one note from the pick to the end of the decay, and
   * the last value held when the note dies.
   */
  displayValid: boolean;
  /** The note has ended; the display holds its last value. */
  displayHeld: boolean;
  /** The display comes from the long window and has converged: good to a tenth of a cent. */
  displaySettled: boolean;
  displayHz: number;
  /** The note shown (MIDI), locked with hysteresis. */
  displayNote: number;
  /** displayHz relative to displayNote. */
  displayCents: number;
}

/** The session facts of the handshake that can change while the editor is open. */
export interface SessionEvent {
  sampleRate: number;
  latencySamples: number;
  layout: string;
}

export interface Events {
  /** Changed parameter values (host automation, preset loads, undo). Batched at 60 Hz. */
  'sedef:params': { values: ParamValues };
  /**
   * The sample rate or the latency changed (the host prepared again, or the oversampling
   * setting). Absent from plug-ins up to 0.4.0: they only report these in the hello.
   */
  'sedef:session': SessionEvent;
  /** 30 Hz while the editor is open. */
  'sedef:telemetry': TelemetryEvent;
  /** 60 Hz while pitch analysis is active (sedef_tuner_setActive). */
  'sedef:pitch': PitchEvent;
  'sedef:presets': PresetState;
  'sedef:ir': IrState;
  /** Capture loads, clears, restores and recent-list changes. */
  'sedef:capture': CaptureState;
  /** The board changed elsewhere: a preset load, A/B, a snapshot or a host restore. */
  'sedef:board': BoardState;
  /** Update checks and downloads (state changes and download progress, at most ~10 Hz). */
  'sedef:update': UpdateState;
}

export type EventName = keyof Events;
