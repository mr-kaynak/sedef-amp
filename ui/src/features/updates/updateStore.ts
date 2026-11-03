// SPDX-License-Identifier: AGPL-3.0-or-later
// Update checker state (docs/updates.md). The plugin owns it; this store mirrors sedef:update and
// adds what only this window cares about: whether the panel is open and whether "Later" was
// chosen.
import { create } from 'zustand';
import type { Backend } from '../../bridge/backend';
import type { UpdateState } from '../../bridge/protocol';
import { invoke } from '../../state/backendRef';

export type SupportedUpdateState = Extract<UpdateState, { supported: true }>;

interface UpdateStore {
  /** null until the backend answers. */
  state: UpdateState | null;
  open: boolean;
  /** "Later": no pill until the editor is opened again. */
  snoozed: boolean;
}

const initial: UpdateStore = { state: null, open: false, snoozed: false };

export const useUpdateStore = create<UpdateStore>()(() => initial);

// Events are the source of truth. A call's answer is applied only when no event arrived while it
// was in flight; otherwise it may be older than what the event already showed.
let events = 0;

function onEvent(state: UpdateState): void {
  events += 1;
  useUpdateStore.setState({ state });
}

function request(call: () => Promise<UpdateState | undefined>): Promise<void> {
  const before = events;
  return call().then((state) => {
    if (state && events === before) useUpdateStore.setState({ state });
  });
}

/** The state when this build has an update checker, else null. */
export function selectSupported(s: UpdateStore): SupportedUpdateState | null {
  return s.state?.supported ? s.state : null;
}

/** An update is installed, or waits for the host to exit: a restart is what's left. */
export function needsRestart(state: SupportedUpdateState | null): boolean {
  return state?.phase === 'installed' || state?.phase === 'scheduled';
}

/**
 * The header pill: "Restart to update" once an update is installed, else "Update available" for a
 * newer release that was neither skipped nor put off with "Later".
 */
export function selectPill(s: UpdateStore): 'restart' | 'available' | null {
  const state = selectSupported(s);
  if (needsRestart(state)) return 'restart';
  return !!state?.latest &&
    state.updateAvailable &&
    state.latest.version !== state.skippedVersion &&
    !s.snoozed
    ? 'available'
    : null;
}

export const updateActions = {
  openPanel: () => useUpdateStore.setState({ open: true }),
  closePanel: () => useUpdateStore.setState({ open: false }),
  later: () => useUpdateStore.setState({ open: false, snoozed: true }),
  checkNow: () => request(() => invoke('sedef_update_check')),
  setAutoCheck: (enabled: boolean) => {
    const state = selectSupported(useUpdateStore.getState());
    if (state) useUpdateStore.setState({ state: { ...state, autoCheck: enabled } });
    return request(() => invoke('sedef_update_setAutoCheck', enabled));
  },
  skip: (version: string) => {
    useUpdateStore.setState({ open: false });
    return request(() => invoke('sedef_update_skip', version));
  },
  unskip: () => request(() => invoke('sedef_update_skip', '')),
  install: () => request(() => invoke('sedef_update_install')),
  installManually: () => request(() => invoke('sedef_update_installManually')),
  cancel: () => request(() => invoke('sedef_update_cancel')),
  reveal: () => request(() => invoke('sedef_update_reveal')),
  restart: () => request(() => invoke('sedef_update_restart')),
  /** Opens a page in the system browser (the plugin refuses anything but https). */
  openPage: (url: string) => invoke('sedef_openUrl', url),
};

/**
 * Listens to sedef:update and asks for the first state, which also lets the plugin run its
 * automatic check (only now that a window is open). Returns a function that undoes both.
 */
export function connectUpdates(backend: Backend): () => void {
  const off = backend.on('sedef:update', onEvent);
  void request(() => invoke('sedef_update_getState'));
  return () => {
    off();
    useUpdateStore.setState(initial, true);
  };
}
