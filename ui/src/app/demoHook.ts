// SPDX-License-Identifier: AGPL-3.0-or-later
// Scripted frames for screenshots and trailer captures. Installed only in the browser mock with
// ?demo, as window.__sedefDemo. Combine with ?freeze so nothing else moves.
import type { PitchEvent } from '../bridge/protocol';
import { hzToMidi, midiToHz, noteOf } from '../lib/music';
import { getValue } from '../params/paramStore';
import { setParam } from '../params/useParam';
import { getBackend } from '../state/backendRef';
import { pitch } from '../state/live';
import { MockBackend } from '../bridge/mockBackend';
import { useIntonationStore } from '../features/intonation/store';

export interface PitchFrame {
  /** Settled detections since the pluck (the tracker adds 200 per second). */
  stableCount?: number;
  stable?: boolean;
}

export interface SedefDemo {
  /** Publishes one tracker reading: the given note played `cents` away from equal temperament. */
  pitch(midi: number, cents: number, frame?: PitchFrame): void;
  silence(): void;
  /** Merges state into the intonation workbench store. */
  intonation(state: Partial<ReturnType<typeof useIntonationStore.getState>>): void;
  set(id: string, value: number): void;
  /** Sends one MIDI message to the mock, as a controller plugged into the host would. */
  midi(status: number, data1: number, data2?: number): void;
}

declare global {
  interface Window {
    __sedefDemo?: SedefDemo;
  }
}

export function pitchEvent(midi: number, cents: number, frame: PitchFrame = {}): PitchEvent {
  const reference = getValue('tunerReference');
  const hz = midiToHz(midi, reference) * Math.pow(2, cents / 1200);
  const midiFloat = hzToMidi(hz, reference);
  const nearest = Math.round(midiFloat);
  const { note, octave } = noteOf(nearest);
  const stable = frame.stable ?? true;
  return {
    valid: true,
    frequencyHz: hz,
    midiNote: nearest,
    note,
    octave,
    cents: 100 * (midiFloat - nearest),
    clarity: 0.96,
    levelDb: -20,
    stable,
    stableHz: stable ? hz : 0,
    stableStdCents: stable ? 0.06 : 0,
    stableCount: stable ? (frame.stableCount ?? 20) : 0,
    onsetId: 1,
    displayValid: true,
    displayHeld: false,
    displaySettled: stable,
    displayHz: hz,
    displayNote: nearest,
    displayCents: 100 * (midiFloat - nearest),
  };
}

export function installDemoHook(): void {
  window.__sedefDemo = {
    pitch: (midi, cents, frame) => pitch.publish(pitchEvent(midi, cents, frame)),
    silence: () => pitch.clear(),
    intonation: (state) => useIntonationStore.setState(state),
    set: (id, value) => setParam(id, value),
    midi: (status, data1, data2) => {
      const backend = getBackend();
      if (backend instanceof MockBackend) backend.midi(status, data1, data2);
    },
  };
}
