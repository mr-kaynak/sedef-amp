// SPDX-License-Identifier: AGPL-3.0-or-later
// Wires a fresh MockBackend into the stores for a test and resets every store afterwards.
import { MockBackend, type MockOptions } from '../bridge/mockBackend';
import { connect } from '../app/connect';
import { initParams } from '../params/paramStore';
import { bundledSchema, defaultValues } from '../params/schema';
import { resetSenderState } from '../params/sender';
import { useAppStore } from '../state/appStore';
import { setBackend } from '../state/backendRef';
import { pitch, telemetry } from '../state/live';
import { usePresetStore } from '../state/presetStore';
import { useUiStore } from '../state/uiStore';
import { useIntonationStore } from '../features/intonation/store';
import { defaultSteps } from '../lib/metronome';

const initialIntonation = useIntonationStore.getState();

export function resetStores(): void {
  resetSenderState();
  initParams(bundledSchema, defaultValues(bundledSchema));
  usePresetStore.setState({
    state: { items: [], currentId: null, currentName: 'Init', dirty: false },
  });
  useUiStore.setState({
    section: 'amp',
    overlay: null,
    popover: null,
    scale: 1,
    metronomeSteps: defaultSteps(),
  });
  useIntonationStore.setState(initialIntonation, true);
  useAppStore.setState({ status: 'connecting', message: null, ir: { slots: [] } });
  telemetry.clear();
  pitch.clear();
}

export interface Harness {
  mock: MockBackend;
  dispose: () => void;
}

export async function setupMock(options: MockOptions = {}): Promise<Harness> {
  resetStores();
  const mock = new MockBackend({ autoStart: false, persistKey: null, ...options });
  const disconnect = await connect(mock);
  return {
    mock,
    dispose: () => {
      disconnect();
      mock.dispose();
      setBackend(null);
      resetStores();
    },
  };
}

/** Lets queued microtasks (coalesced sends, mock events) run. */
export async function flush(): Promise<void> {
  for (let i = 0; i < 3; i += 1) await Promise.resolve();
}
