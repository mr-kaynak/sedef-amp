// SPDX-License-Identifier: AGPL-3.0-or-later
// Sedef MIDI's bridge (plugin/midi/MidiEditor.cpp). Parameters use the same calls and event as
// Sedef Amp's (sedef_setParam, sedef_beginGesture, sedef_endGesture, sedef_resetParam,
// sedef:params), so the shared controls work unchanged.
import type { ParamSpec, ParamValues } from '../bridge/protocol';

export const MIDI_PROTOCOL_VERSION = 1;

/** Where the notes go besides the host: "off", "virtual", or "device:" + an identifier. */
export type Destination = string;

export interface PortOption {
  id: Destination;
  name: string;
}

export interface PortsState {
  selected: Destination;
  /** The open port's name; empty when off or when opening failed. */
  openName: string;
  /** Why the selected port is not open; empty when it is (or when off). */
  status: string;
  virtualSupported: boolean;
  /** The virtual port's name as other apps list it. */
  virtualName: string;
  outputs: PortOption[];
}

/** The audio side of a note's delay, and the Lowest Note range in effect. */
export interface AudioState {
  sampleRate: number;
  /** Samples per audio buffer; 0 before the first one. */
  blockSize: number;
  /** What the buffer adds to a note sent through the MIDI port (ms): at most, and on average. */
  bufferWorstMs: number;
  bufferMeanMs: number;
  /** The audio device's own input latency (ms); -1 when unknown (a host does not say). */
  inputLatencyMs: number;
  /** The Lowest Note range in effect: 1 E2, 2 B1, 3 F♯1 (what Auto has heard, when on Auto). */
  lowNote: number;
}

export interface MidiHello {
  protocol: number;
  appVersion: string;
  wrapperType: string;
  isStandalone: boolean;
  isInstrument: boolean;
  sampleRate: number;
  steadyLagMs: number;
  schema: ParamSpec[];
  values: ParamValues;
  ports: PortsState;
  audio: AudioState;
}

export interface MidiTelemetry {
  /** The input's level in the tracker's band, the scale the gate uses (dBFS). */
  levelDb: number;
  /** The gate in effect on the same scale: the automatic one, or the Gate parameter in Manual. */
  gateDb: number;
  /** The note being played (after the octave), -1 when none. */
  note: number;
  /** The tracked pitch as a fractional MIDI note, 0 when none. */
  pitch: number;
  velocity: number;
  sounding: boolean;
  notesSent: number;
  lateNotes: number;
  sampleRate: number;
}

export interface MidiFunctions {
  sedef_hello(protocol: number): Promise<MidiHello>;
  sedef_setParam(id: string, value: number): Promise<void>;
  sedef_beginGesture(id: string): Promise<void>;
  sedef_endGesture(id: string): Promise<void>;
  sedef_resetParam(id: string): Promise<void>;
  sedef_midi_panic(): Promise<void>;
  sedef_midi_ports(): Promise<PortsState>;
  sedef_midi_setPort(destination: Destination): Promise<PortsState>;
  /** Standalone app only: JUCE's audio device dialog (input, sample rate, buffer). */
  sedef_midi_audioSettings(): Promise<void>;
  /** The automatic Lowest Note range starts again as a six-string's. */
  sedef_midi_forgetLowNote(): Promise<void>;
  sedef_openUrl(url: string): Promise<void>;
}

export interface MidiEvents {
  'sedef:params': { values: ParamValues };
  'sedef:midi:telemetry': MidiTelemetry;
  'sedef:midi:ports': PortsState;
  'sedef:midi:audio': AudioState;
}

export type MidiFunctionName = keyof MidiFunctions;
export type MidiEventName = keyof MidiEvents;

export const SETUP_GUIDE_URL = 'https://github.com/mr-kaynak/sedef-amp/blob/main/docs/midi.md';
