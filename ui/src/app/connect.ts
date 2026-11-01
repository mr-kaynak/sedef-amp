// SPDX-License-Identifier: AGPL-3.0-or-later
// Startup: pick a backend, perform the handshake, seed the stores and wire bridge events.
import type { Backend } from '../bridge/backend';
import { isInsideJuce, JuceBackend } from '../bridge/juceBackend';
import { MockBackend } from '../bridge/mockBackend';
import { PROTOCOL_VERSION } from '../bridge/protocol';
import { applyRemoteValues, initParams } from '../params/paramStore';
import { applyHello, useAppStore } from '../state/appStore';
import { invoke, setBackend } from '../state/backendRef';
import { pitch, telemetry } from '../state/live';
import { applyPresetState } from '../state/presetStore';
import { connectUpdates } from '../features/updates/updateStore';
import { useUiStore } from '../state/uiStore';
import type { DemoFlags } from './demo';
import { hydrateUiState, startPersistence } from './persistence';

export async function createBackend(flags: DemoFlags): Promise<Backend> {
  if (isInsideJuce()) return JuceBackend.create();
  return new MockBackend({
    autoStart: !flags.freeze,
    wrapperType:
      flags.host === 'plugin' ? 'VST3' : flags.host === 'standalone' ? 'Standalone' : 'Browser',
    isStandalone: flags.host !== 'plugin',
    intonationErrors: flags.errors,
    persistKey: flags.fresh || flags.freeze ? null : 'sedef-mock-ui-state',
    ...(flags.preset ? { initialPresetId: `factory:${flags.preset}` } : {}),
    overrides: flags.overrides,
    ...(flags.update ? { update: flags.update } : {}),
  });
}

/** Connects the stores to a backend. Returns a function that detaches all listeners. */
export async function connect(backend: Backend): Promise<() => void> {
  setBackend(backend);
  const offs = [
    backend.on('sedef:params', (e) => applyRemoteValues(e.values)),
    backend.on('sedef:telemetry', (e) => telemetry.publish(e)),
    backend.on('sedef:pitch', (e) => pitch.publish(e)),
    backend.on('sedef:presets', (s) => applyPresetState(s)),
    backend.on('sedef:ir', (ir) => useAppStore.setState({ ir })),
  ];

  try {
    const hello = await backend.hello();
    initParams(hello.schema, hello.values);
    applyPresetState(hello.presets);
    applyHello(hello, backend.kind);
    hydrateUiState(hello.uiState);
    // The engine keeps no copy of the beat pattern outside the UI state; restore it.
    if (hello.isStandalone)
      void invoke('sedef_metronome_setPattern', useUiStore.getState().metronomeSteps);
    if (hello.protocol !== PROTOCOL_VERSION) {
      useAppStore.setState({
        message: `This interface speaks bridge protocol ${PROTOCOL_VERSION}, the plugin speaks ${hello.protocol}. Reinstall Sedef Amp so both match.`,
      });
    }
  } catch (error) {
    useAppStore.setState({
      status: 'error',
      message: `Could not reach the audio engine: ${error instanceof Error ? error.message : String(error)}`,
    });
  }

  offs.push(startPersistence());
  offs.push(connectUpdates(backend));
  return () => offs.forEach((off) => off());
}
