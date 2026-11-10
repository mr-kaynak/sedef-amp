// SPDX-License-Identifier: AGPL-3.0-or-later
import { act } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import type { Backend } from '../bridge/backend';
import { LEGACY_BOARD } from '../features/pedals/registry';
import type { BoardState } from '../bridge/protocol';
import { flush, setupMock, type Harness } from '../test/harness';
import { setBackend } from './backendRef';
import { applyBoardState, boardActions, resetBoardStore, useBoardStore } from './boardStore';
import { presetActions, usePresetStore } from './presetStore';
import { useToastStore } from './toastStore';

const LEGACY = [...LEGACY_BOARD];
/** 0.3.0's pedals, a short board to edit. */
const FIVE = ['gate', 'tide', 'pearl', 'coral', 'ripple'];
/** The board Glassy Chorus (the mock's start-up preset) carries: the pedals it switches on. */
const NACRE = ['gate', 'tide', 'ripple'];
const pedals = () => useBoardStore.getState().pedals;
const dirty = () => usePresetStore.getState().state.dirty;
const settle = () =>
  act(async () => {
    await flush();
    await new Promise((resolve) => setTimeout(resolve, 30));
  });

describe('board store', () => {
  let h: Harness;
  afterEach(async () => {
    await settle();
    h.dispose();
  });
  const plugin = async () => (await h.mock.hello()).board?.pedals;

  it('starts from the plugin board, which counts towards the dirty dot', async () => {
    h = await setupMock({ board: ['ripple', 'gate'] });
    expect(pedals()).toEqual(['ripple', 'gate']);
    expect(useBoardStore.getState().supported).toBe(true);
    expect(dirty()).toBe(false);

    boardActions.add('pearl');
    await settle();
    expect(pedals()).toEqual(['ripple', 'gate', 'pearl']);
    expect(await plugin()).toEqual(pedals());
    expect(dirty()).toBe(true);

    boardActions.remove('pearl');
    await settle();
    expect(dirty()).toBe(false);
    expect(useToastStore.getState().toast?.message).toBe('Removed Pearl');
  });

  it('keeps the latest edit when the answers to earlier ones come late', async () => {
    h = await setupMock({ board: FIVE, jitter: { maxMs: 20, seed: 7 } });
    boardActions.move('ripple', 0);
    boardActions.move('pearl', 0);
    boardActions.move('gate', 4);
    boardActions.remove('tide');
    expect(pedals()).toEqual(['pearl', 'ripple', 'coral', 'gate']);
    await settle();
    expect(pedals()).toEqual(['pearl', 'ripple', 'coral', 'gate']);
    expect(await plugin()).toEqual(pedals());
  });

  it('takes only the answer to the latest edit', async () => {
    h = await setupMock();
    // A plugin that answers when told to.
    const answers: ((state: BoardState) => void)[] = [];
    setBackend({
      kind: 'mock',
      call: () => new Promise((resolve) => answers.push(resolve as (s: BoardState) => void)),
    } as unknown as Backend);
    resetBoardStore();
    applyBoardState({ pedals: FIVE, revision: 1 });
    boardActions.move('ripple', 0);
    boardActions.move('pearl', 0);
    expect(pedals()).toEqual(['pearl', 'ripple', 'gate', 'tide', 'coral']);
    // The first answer would put Pearl back until the second came.
    await act(async () => answers[0]!({ pedals: ['ripple', ...FIVE.slice(0, 4)], revision: 2 }));
    expect(pedals()).toEqual(['pearl', 'ripple', 'gate', 'tide', 'coral']);
    await act(async () =>
      answers[1]!({ pedals: ['pearl', 'ripple', 'gate', 'tide', 'coral'], revision: 3 }),
    );
    expect(pedals()).toEqual(['pearl', 'ripple', 'gate', 'tide', 'coral']);
    expect(useBoardStore.getState().revision).toBe(3);
  });

  it('ignores a board state older than the one it has', async () => {
    h = await setupMock();
    applyBoardState({ pedals: ['coral'], revision: 1000 });
    applyBoardState({ pedals: ['pearl'], revision: 999 });
    expect(pedals()).toEqual(['coral']);
  });

  it('takes the board of each preset it loads; one saved without a board gets the legacy board', async () => {
    h = await setupMock();
    boardActions.set(['coral', 'pearl']);
    await settle();
    await act(async () => {
      await presetActions.save('Board order', 'Live');
    });
    await settle();
    const saved = usePresetStore.getState().state.currentId;
    expect(dirty()).toBe(false);

    // Factory presets carry only the pedals they switch on.
    await act(async () => presetActions.load('factory:mariana'));
    await settle();
    expect(pedals()).toEqual(['gate']);

    await act(async () => presetActions.load(saved!));
    await settle();
    expect(pedals()).toEqual(['coral', 'pearl']);
    expect(dirty()).toBe(false);

    // A user preset saved before the board: the whole fixed chain it played.
    await act(async () => presetActions.load('user:Live/Sunday set.sedefpreset'));
    await settle();
    expect(pedals()).toEqual(LEGACY);
    expect(dirty()).toBe(false);

    // Init has no file: the pedals its defaults switch on, the Gate alone.
    await act(async () => presetActions.init());
    await settle();
    expect(pedals()).toEqual(['gate']);
    expect(dirty()).toBe(false);
  });

  it('switches boards with A/B', async () => {
    h = await setupMock();
    await act(async () => presetActions.compare('switch'));
    await settle();
    boardActions.set(['ripple']);
    await settle();
    await act(async () => presetActions.compare('switch'));
    await settle();
    expect(pedals()).toEqual(NACRE);
    await act(async () => presetActions.compare('switch'));
    await settle();
    expect(pedals()).toEqual(['ripple']);
    expect(await plugin()).toEqual(['ripple']);
  });

  it('brings the board back with a snapshot, as Undo after a preset load does', async () => {
    h = await setupMock();
    boardActions.set(['tide', 'coral']);
    await settle();
    await act(async () => presetActions.snapshot('undo'));
    await act(async () => presetActions.load('factory:mariana'));
    await settle();
    expect(pedals()).toEqual(['gate']);
    await act(async () => presetActions.restore('undo'));
    await settle();
    expect(pedals()).toEqual(['tide', 'coral']);
    expect(dirty()).toBe(true);
  });
});
