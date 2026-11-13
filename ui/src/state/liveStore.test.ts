// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, describe, expect, it } from 'vitest';
import type { Backend } from '../bridge/backend';
import type { LiveState } from '../bridge/protocol';
import { setBackend } from './backendRef';
import { applyLiveState, liveActions, resetLiveStore, useLiveStore } from './liveStore';

function live(revision: number, active = 0): LiveState {
  return {
    scenes: {
      active,
      count: 8,
      made: Array.from({ length: 8 }, (_, i) => i === active),
      values: [],
      maxValues: 16,
      switches: [],
    },
    bindings: [],
    maxBindings: 64,
    programs: { channel: 0, assigned: [] },
    learn: null,
    midiIn: 'full',
    inputs: null,
    revision,
  };
}

describe('The live store', () => {
  afterEach(() => {
    setBackend(null);
    resetLiveStore();
  });

  it('keeps the newest state: an older revision arriving late is dropped', () => {
    applyLiveState(live(5, 2));
    applyLiveState(live(4, 1));
    expect(useLiveStore.getState().state?.scenes.active).toBe(2);
    applyLiveState(live(6, 3));
    expect(useLiveStore.getState().state?.scenes.active).toBe(3);
    applyLiveState(undefined);
    expect(useLiveStore.getState().state?.scenes.active).toBe(3);
  });

  it('shows a selected scene at once and takes only the answer to the latest call', async () => {
    applyLiveState(live(1));
    const answers: ((s: LiveState) => void)[] = [];
    setBackend({
      call: () => new Promise<LiveState>((resolve) => answers.push(resolve)),
    } as unknown as Backend);

    const first = liveActions.selectScene(1);
    expect(useLiveStore.getState().state?.scenes).toMatchObject({ active: 1 });
    expect(useLiveStore.getState().state?.scenes.made.slice(0, 2)).toEqual([true, true]);
    const second = liveActions.selectScene(2);
    expect(useLiveStore.getState().state?.scenes.active).toBe(2);

    // The second answer comes first; the first, late, would undo it.
    answers[1]!({ ...live(3, 2), revision: undefined });
    await second;
    answers[0]!({ ...live(2, 1), revision: undefined });
    await first;
    expect(useLiveStore.getState().state?.scenes.active).toBe(2);
  });
});
