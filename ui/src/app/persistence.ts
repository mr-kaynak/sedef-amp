// SPDX-License-Identifier: AGPL-3.0-or-later
// UI state lives inside the plugin state (sedef_ui_setState) so it travels with host sessions.
// Writes are debounced: dragging through sections or editing a tuning sends one update.
import type { UiState } from '../bridge/protocol';
import { invoke } from '../state/backendRef';
import { isSection, sanitizeSteps, SCALES, useUiStore } from '../state/uiStore';
import {
  hydrateIntonation,
  snapshotIntonation,
  useIntonationStore,
} from '../features/intonation/store';

const SAVE_DELAY_MS = 400;

export function buildUiState(): UiState {
  const ui = useUiStore.getState();
  return {
    section: ui.section,
    scale: ui.scale,
    metronomeSteps: ui.metronomeSteps,
    intonation: snapshotIntonation(),
  };
}

export function hydrateUiState(state: UiState | null): void {
  if (!state) return;
  useUiStore.setState((s) => ({
    section: isSection(state.section) ? state.section : s.section,
    scale:
      typeof state.scale === 'number' && (SCALES as readonly number[]).includes(state.scale)
        ? state.scale
        : s.scale,
    metronomeSteps: state.metronomeSteps ? sanitizeSteps(state.metronomeSteps) : s.metronomeSteps,
  }));
  hydrateIntonation(state.intonation);
}

export function startPersistence(): () => void {
  let timer: ReturnType<typeof setTimeout> | null = null;
  let last = JSON.stringify(buildUiState());
  const schedule = () => {
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => {
      timer = null;
      const state = buildUiState();
      const json = JSON.stringify(state);
      if (json === last) return;
      last = json;
      void invoke('sedef_ui_setState', state);
    }, SAVE_DELAY_MS);
  };
  const offUi = useUiStore.subscribe(schedule);
  const offIntonation = useIntonationStore.subscribe(schedule);
  return () => {
    offUi();
    offIntonation();
    if (timer) clearTimeout(timer);
  };
}
