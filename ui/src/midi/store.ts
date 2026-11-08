// SPDX-License-Identifier: AGPL-3.0-or-later
// Sedef MIDI's state: the handshake, the port list, and telemetry (a live channel, outside React
// state, for the meter and the note display).
import { create } from 'zustand';
import { applyRemoteValues, initParams } from '../params/paramStore';
import { setBackend } from '../state/backendRef';
import { LiveChannel } from '../state/live';
import type { MidiBackend } from './backend';
import {
  MIDI_PROTOCOL_VERSION,
  type MidiFunctionName,
  type MidiFunctions,
  type MidiHello,
  type MidiTelemetry,
  type PortsState,
} from './protocol';

export type Session = Omit<MidiHello, 'schema' | 'values' | 'ports'>;

interface MidiState {
  status: 'connecting' | 'ready' | 'error';
  /** Shown as an alert above everything else (a failed handshake, a protocol mismatch). */
  message: string | null;
  session: Session | null;
  ports: PortsState | null;
}

export const useMidiStore = create<MidiState>()(() => ({
  status: 'connecting',
  message: null,
  session: null,
  ports: null,
}));

/** Telemetry, plus the last note sent (the display keeps it, unlit, after the note ends). */
export type MidiLive = MidiTelemetry & { lastNote: number };

export const midiTelemetry = new LiveChannel<MidiLive>();
let lastNote = -1;

let current: MidiBackend | null = null;

/** Calls the plug-in; failures resolve to undefined (the next event resynchronises the UI). */
export function midiInvoke<K extends MidiFunctionName>(
  name: K,
  ...args: Parameters<MidiFunctions[K]>
): Promise<Awaited<ReturnType<MidiFunctions[K]>> | undefined> {
  if (!current) return Promise.resolve(undefined);
  return current.invokeMidi(name, ...args).catch((error: unknown) => {
    console.warn(`Sedef MIDI bridge: ${name} failed`, error);
    return undefined;
  });
}

export async function setDestination(destination: string): Promise<void> {
  // Optimistic: the picker shows the choice at once; the plug-in's answer settles the rest.
  useMidiStore.setState((s) =>
    s.ports ? { ports: { ...s.ports, selected: destination, status: '' } } : s,
  );
  const ports = await midiInvoke('sedef_midi_setPort', destination);
  if (ports) useMidiStore.setState({ ports });
}

/** Connects the stores to a backend. Returns a function that detaches every listener. */
export async function connectMidi(backend: MidiBackend): Promise<() => void> {
  current = backend;
  setBackend(backend); // the shared parameter sender calls through it
  const offs = [
    backend.onMidi('sedef:params', (e) => applyRemoteValues(e.values)),
    backend.onMidi('sedef:midi:telemetry', (t) => {
      if (t.note >= 0) lastNote = t.note;
      midiTelemetry.publish({ ...t, lastNote });
    }),
    backend.onMidi('sedef:midi:ports', (ports) => useMidiStore.setState({ ports })),
  ];
  try {
    const hello = await backend.midiHello();
    initParams(hello.schema, hello.values);
    const session: Session = {
      protocol: hello.protocol,
      appVersion: hello.appVersion,
      wrapperType: hello.wrapperType,
      isStandalone: hello.isStandalone,
      isInstrument: hello.isInstrument,
      sampleRate: hello.sampleRate,
      steadyLagMs: hello.steadyLagMs,
    };
    lastNote = -1;
    useMidiStore.setState({
      status: 'ready',
      session,
      ports: hello.ports,
      message:
        hello.protocol === MIDI_PROTOCOL_VERSION
          ? null
          : `This interface speaks bridge protocol ${MIDI_PROTOCOL_VERSION}, the plug-in speaks ${hello.protocol}. Reinstall Sedef MIDI so both match.`,
    });
  } catch (error) {
    useMidiStore.setState({
      status: 'error',
      message: `Could not reach Sedef MIDI's engine: ${error instanceof Error ? error.message : String(error)}`,
    });
  }
  return () => {
    offs.forEach((off) => off());
    current = null;
    setBackend(null);
  };
}
