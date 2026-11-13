// SPDX-License-Identifier: AGPL-3.0-or-later
// Playing live (docs/live.md), mirrored from the plug-in: scenes, MIDI bindings, program
// assignments and learn. Every call answers with the whole state; the answer to the latest call
// and the sedef:live events for changes made elsewhere (a footswitch, another window) replace
// ours unless they are older than what we have.
import { create } from 'zustand';
import type { LiveActivity, LiveBinding, LiveState } from '../bridge/protocol';
import type { NativeArgs, NativeName } from '../bridge/backend';
import { invoke } from './backendRef';

interface LiveStore {
  /** Null against a plug-in from before 0.10.0: no MIDI, no scenes. */
  state: LiveState | null;
  activity: LiveActivity | null;
}

export const useLiveStore = create<LiveStore>()(() => ({ state: null, activity: null }));

let sent = 0;

/** Back to the state before any plug-in answered (tests). */
export function resetLiveStore(): void {
  sent = 0;
  useLiveStore.setState({ state: null, activity: null });
}

/** Takes a live state from the plug-in unless it is older than the one we have. */
export function applyLiveState(state: LiveState | undefined): void {
  if (!state) return;
  const have = useLiveStore.getState().state?.revision;
  if (typeof state.revision === 'number' && typeof have === 'number' && state.revision < have)
    return;
  useLiveStore.setState({ state });
}

export function applyLiveActivity(activity: LiveActivity): void {
  useLiveStore.setState({ activity });
}

type LiveCall = {
  [K in NativeName]: K extends `sedef_live_${string}` ? K : never;
}[NativeName];

function call<K extends LiveCall>(name: K, ...args: NativeArgs<K>): Promise<void> {
  const edit = ++sent;
  return invoke(name, ...args).then((answer) => {
    // An answer to an earlier call would undo later ones until their answers came.
    if (answer && edit === sent) applyLiveState(answer as LiveState);
  });
}

/** Optimistic: the scene bar shows a selection at once. */
function optimistic(change: (s: LiveState) => LiveState): void {
  const s = useLiveStore.getState().state;
  if (s) useLiveStore.setState({ state: change(s) });
}

export const liveActions = {
  selectScene(scene: number): Promise<void> {
    optimistic((s) => ({
      ...s,
      scenes: {
        ...s.scenes,
        active: scene,
        made: s.scenes.made.map((m, i) => m || i === scene),
      },
    }));
    return call('sedef_live_selectScene', scene);
  },
  copyScene: (scene: number) => call('sedef_live_copyScene', scene),
  remember: (id: string, remember: boolean) => call('sedef_live_remember', id, remember),
  learn: (target: string) => call('sedef_live_learn', target),
  cancelLearn: () => call('sedef_live_cancelLearn'),
  forget: (target: string) => call('sedef_live_forget', target),
  updateBinding: (index: number, changes: Partial<Pick<LiveBinding, 'mode' | 'low' | 'high'>>) =>
    call('sedef_live_updateBinding', index, changes),
  removeBinding: (index: number) => call('sedef_live_removeBinding', index),
  clearBindings: () => call('sedef_live_clearBindings'),
  setProgram: (program: number, target: { preset: string } | { scene: number } | null) =>
    call('sedef_live_setProgram', program, target),
  setProgramChannel: (channel: number) => call('sedef_live_setProgramChannel', channel),
  setInput: (id: string, enabled: boolean) => call('sedef_live_setInput', id, enabled),
};

/** The binding of a target, if any. */
export function bindingFor(state: LiveState | null, target: string): LiveBinding | undefined {
  return state?.bindings.find((b) => b.target === target);
}
