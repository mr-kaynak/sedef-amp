// SPDX-License-Identifier: AGPL-3.0-or-later
// The pedalboard, mirrored from the plugin. Edits apply at once here and go to the plugin as the
// whole board; its answer to the latest edit, and the sedef:board events it sends for changes made
// elsewhere (presets, A/B, host restores), replace ours unless they are older than what we have.
import { create } from 'zustand';
import type { BoardState } from '../bridge/protocol';
import {
  addPedal,
  LEGACY_BOARD,
  movePedal,
  pedalName,
  removePedal,
  sanitiseBoard,
  sameBoard,
} from '../features/pedals/registry';
import { invoke } from './backendRef';
import { dismissToast, showToast } from './toastStore';

interface BoardStore {
  pedals: string[];
  revision?: number;
  /** False against a plug-in that predates the board: its pedals play in a fixed order. */
  supported: boolean;
}

export const useBoardStore = create<BoardStore>()(() => ({
  pedals: [...LEGACY_BOARD],
  supported: false,
}));

/** Edits sent to the plugin; only the answer to the latest one is taken. */
let sent = 0;
/** The Removed notice: its Undo means nothing once the board changes elsewhere. */
let removedToast: number | null = null;

/** Back to the state before any plugin answered (tests). */
export function resetBoardStore(): void {
  sent = 0;
  removedToast = null;
  useBoardStore.setState({ pedals: [...LEGACY_BOARD], supported: false, revision: undefined });
}

/** Takes a board state from the plugin unless it is older than the one we have. */
export function applyBoardState(state: BoardState | undefined): void {
  if (!state) {
    useBoardStore.setState({ pedals: [...LEGACY_BOARD], supported: false, revision: undefined });
    return;
  }
  const have = useBoardStore.getState().revision;
  if (typeof state.revision === 'number' && typeof have === 'number' && state.revision < have)
    return;
  const pedals = sanitiseBoard(state.pedals);
  if (removedToast !== null && !sameBoard(pedals, useBoardStore.getState().pedals)) {
    dismissToast(removedToast);
    removedToast = null;
  }
  useBoardStore.setState({
    pedals,
    supported: true,
    ...(typeof state.revision === 'number' ? { revision: state.revision } : {}),
  });
}

function set(pedals: string[]): Promise<void> {
  if (sameBoard(pedals, useBoardStore.getState().pedals)) return Promise.resolve();
  useBoardStore.setState({ pedals });
  const edit = ++sent;
  return invoke('sedef_board_set', pedals).then((answer) => {
    // An answer to an earlier edit would undo the later ones until their answers came.
    if (answer && edit === sent) applyBoardState(answer);
  });
}

export const boardActions = {
  set,
  /** Adds a pedal at its default position; returns where it went. */
  add(id: string): number {
    const next = addPedal(useBoardStore.getState().pedals, id);
    void set(next);
    return next.indexOf(id);
  },
  move(id: string, index: number): void {
    void set(movePedal(useBoardStore.getState().pedals, id, index));
  },
  /** Takes a pedal off the board, with Undo in a notice instead of a confirmation. */
  remove(id: string): void {
    const before = useBoardStore.getState().pedals;
    const at = before.indexOf(id);
    if (at < 0) return;
    void set(removePedal(before, id));
    removedToast = showToast(`Removed ${pedalName(id)}`, {
      label: 'Undo',
      // Back where it was, on the board as it is by then.
      run: () => {
        removedToast = null;
        const now = useBoardStore.getState().pedals;
        if (!now.includes(id)) void set(movePedal([...now, id], id, at));
      },
    });
  },
};
