// SPDX-License-Identifier: AGPL-3.0-or-later
// The recorder and re-amp's state from the plug-in (docs/recorder.md): null in a plug-in. Live
// values (elapsed time, levels, the play position) come with telemetry instead.
import { create } from 'zustand';
import type { ReampListItem, RecorderBridgeState } from '../../bridge/protocol';

export type RecorderTab = 'record' | 'reamp';

interface RecorderStore {
  state: RecorderBridgeState | null;
  /** Recorded DIs and imported files, newest first (fetched when needed). */
  files: ReampListItem[];
  tab: RecorderTab;
  /** A dropped file on its way into the plug-in. */
  importing: { name: string; progress: number } | null;
  /** Why a drop was refused before it reached the plug-in. */
  dropError: string | null;
}

export const useRecorderStore = create<RecorderStore>()(() => ({
  state: null,
  files: [],
  tab: 'record',
  importing: null,
  dropError: null,
}));

const listeners = new Set<(previous: RecorderBridgeState | null) => void>();

/** Called when a take finishes saving, so the file list can follow. */
export function onRecorderChange(fn: (previous: RecorderBridgeState | null) => void): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function applyRecorderState(state: RecorderBridgeState | null | undefined): void {
  const previous = useRecorderStore.getState().state;
  useRecorderStore.setState({ state: state ?? null });
  listeners.forEach((fn) => fn(previous));
}

export const recorderUi = {
  setTab: (tab: RecorderTab) => useRecorderStore.setState({ tab }),
};
