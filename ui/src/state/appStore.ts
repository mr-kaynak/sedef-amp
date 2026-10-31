// SPDX-License-Identifier: AGPL-3.0-or-later
// Session facts from the handshake (host, sample rate, layout) and the custom IR slots.
import { create } from 'zustand';
import type { HelloResponse, IrState, WrapperType } from '../bridge/protocol';

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
}

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
  });
}
