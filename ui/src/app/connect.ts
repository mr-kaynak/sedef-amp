// SPDX-License-Identifier: AGPL-3.0-or-later
// Startup: pick a backend, perform the handshake, seed the stores and wire bridge events.
import type { Backend } from '../bridge/backend';
import { isInsideJuce, JuceBackend } from '../bridge/juceBackend';
import { MockBackend } from '../bridge/mockBackend';
import { PROTOCOL_VERSION } from '../bridge/protocol';
import { applyRemoteValues, initParams } from '../params/paramStore';
import { applyHello, applySession, useAppStore } from '../state/appStore';
import { invoke, setBackend } from '../state/backendRef';
import { pitch, telemetry } from '../state/live';
import { applyPresetState } from '../state/presetStore';
import { applyBoardState } from '../state/boardStore';
import { applyRecorderState } from '../features/recorder/store';
import { applyLiveActivity, applyLiveState } from '../state/liveStore';
import { connectUpdates } from '../features/updates/updateStore';
import { startInputLevel } from '../features/header/inputLevel';
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
    ...(flags.board ? { board: flags.board } : {}),
    ...(flags.input ? { input: flags.input } : {}),
    ...(flags.recorder ? { recorder: flags.recorder } : {}),
    liveDemo: flags.midi,
    ...(flags.looper ? { looper: flags.looper } : {}),
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
    backend.on('sedef:capture', (capture) => useAppStore.setState({ capture })),
    backend.on('sedef:board', (board) => applyBoardState(board)),
    backend.on('sedef:session', (e) => applySession(e)),
    backend.on('sedef:recorder', (state) => applyRecorderState(state)),
    backend.on('sedef:live', (s) => applyLiveState(s)),
    backend.on('sedef:liveActivity', (a) => applyLiveActivity(a)),
  ];

  try {
    const hello = await backend.hello();
    initParams(hello.schema, hello.values);
    applyPresetState(hello.presets);
    applyBoardState(hello.board);
    applyLiveState(hello.live);
    if (hello.liveActivity) applyLiveActivity(hello.liveActivity);
    applyHello(hello, backend.kind);
    applyRecorderState(hello.recorder ?? null);
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
      message: `Sedef couldn't reach its audio engine (${error instanceof Error ? error.message : String(error)}). Close this window and open it again; if it stays like this, reinstall Sedef Amp.`,
    });
  }

  offs.push(startInputLevel());
  offs.push(startPersistence());
  offs.push(connectUpdates(backend));
  return () => offs.forEach((off) => off());
}
