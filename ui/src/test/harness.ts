// SPDX-License-Identifier: AGPL-3.0-or-later
// Wires a fresh MockBackend into the stores for a test and resets every store afterwards.
import { MockBackend, type MockOptions } from '../bridge/mockBackend';
import { connect } from '../app/connect';
import { initParams } from '../params/paramStore';
import { bundledSchema, defaultValues } from '../params/schema';
import { resetSenderState } from '../params/sender';
import { NO_CAPTURE, useAppStore } from '../state/appStore';
import { setBackend } from '../state/backendRef';
import { resetBoardStore } from '../state/boardStore';
import { pitch, telemetry } from '../state/live';
import { EMPTY_PRESET_STATE, usePresetStore } from '../state/presetStore';
import { useToastStore } from '../state/toastStore';
import { resetBrowserSession } from '../features/presets/session';
import { useUiStore } from '../state/uiStore';
import { useIntonationStore } from '../features/intonation/store';
import { defaultSteps } from '../lib/metronome';
import { useRecorderStore } from '../features/recorder/store';

const initialIntonation = useIntonationStore.getState();

export function resetStores(): void {
  resetSenderState();
  initParams(bundledSchema, defaultValues(bundledSchema));
  usePresetStore.setState({ state: EMPTY_PRESET_STATE });
  resetBrowserSession();
  useToastStore.setState({ toast: null });
  resetBoardStore();
  useUiStore.setState({
    section: 'amp',
    overlay: null,
    popover: null,
    scale: 1,
    metronomeSteps: defaultSteps(),
  });
  useIntonationStore.setState(initialIntonation, true);
  useAppStore.setState({
    status: 'connecting',
    message: null,
    ir: { slots: [] },
    capture: NO_CAPTURE,
    capturesSupported: false,
  });
  useRecorderStore.setState({
    state: null,
    files: [],
    tab: 'record',
    importing: null,
    dropError: null,
  });
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

/** Lets queued microtasks (coalesced sends, mock events, call completions) run. */
export async function flush(): Promise<void> {
  for (let i = 0; i < 6; i += 1) await Promise.resolve();
}
