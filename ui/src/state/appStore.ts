// SPDX-License-Identifier: AGPL-3.0-or-later
// Session facts from the handshake (host, sample rate, layout), the custom IR slots and the
// capture.
import { create } from 'zustand';
import type { CaptureState, HelloResponse, IrState, WrapperType } from '../bridge/protocol';

export interface SessionInfo {
  appVersion: string;
  wrapperType: WrapperType;
  isStandalone: boolean;
  sampleRate: number;
  latencySamples: number;
  layout: string;
  backend: 'juce' | 'mock';
}

interface AppState {
  status: 'connecting' | 'ready' | 'error';
  message: string | null;
  session: SessionInfo;
  ir: IrState;
  capture: CaptureState;
  /** False against a plug-in that predates captures: the UI hides them. */
  capturesSupported: boolean;
}

export const NO_CAPTURE: CaptureState = {
  loaded: null,
  loading: false,
  error: null,
  missing: null,
  recent: [],
};

export const useAppStore = create<AppState>()(() => ({
  status: 'connecting',
  message: null,
  session: {
    appVersion: '',
    wrapperType: 'Browser',
    isStandalone: false,
    sampleRate: 48000,
    latencySamples: 0,
    layout: '',
    backend: 'mock',
  },
  ir: { slots: [] },
  capture: NO_CAPTURE,
  capturesSupported: false,
}));

export function applyHello(hello: HelloResponse, backend: 'juce' | 'mock'): void {
  useAppStore.setState({
    status: 'ready',
    session: {
      appVersion: hello.appVersion,
      wrapperType: hello.wrapperType,
      isStandalone: hello.isStandalone,
      sampleRate: hello.sampleRate,
      latencySamples: hello.latencySamples,
      layout: hello.layout,
      backend,
    },
    ir: hello.ir,
    capture: hello.capture ?? NO_CAPTURE,
    capturesSupported: hello.capture !== undefined,
  });
}
