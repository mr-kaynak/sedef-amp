// SPDX-License-Identifier: AGPL-3.0-or-later
// One non-blocking notice at a time ("Loaded Riptide · Undo"). A new toast replaces the old one,
// except that a plain notice never pushes an Undo off the screen; app/Toasts.tsx renders it and
// dismisses it after a while.
import { create } from 'zustand';
import type { SnapshotSlot } from '../bridge/protocol';

export interface Toast {
  id: number;
  message: string;
  /** Optional action, e.g. Undo. Running it dismisses the toast. */
  action?: { label: string; run: () => void };
  /**
   * The snapshot the action puts back. The toast goes as soon as that snapshot stops meaning
   * what it said (a new browser session retakes "browse"; an A/B switch changes the slot).
   */
  restores?: SnapshotSlot;
}

interface ToastStore {
  toast: Toast | null;
}

export const useToastStore = create<ToastStore>()(() => ({ toast: null }));

let nextId = 1;

/** Shows a toast and returns its id, or null when a plain notice yields to a visible action. */
export function showToast(
  message: string,
  action?: Toast['action'],
  restores?: SnapshotSlot,
): number | null {
  if (!action && useToastStore.getState().toast?.action) return null;
  const id = nextId++;
  useToastStore.setState({
    toast: { id, message, ...(action ? { action } : {}), ...(restores ? { restores } : {}) },
  });
  return id;
}

/** Dismisses the toast `id`, or whichever is showing. */
export function dismissToast(id?: number): void {
  const { toast } = useToastStore.getState();
  if (toast && (id === undefined || toast.id === id)) useToastStore.setState({ toast: null });
}

/** Dismisses the toast if its action would put back one of these snapshots. */
export function dismissToastRestoring(...slots: SnapshotSlot[]): void {
  const { toast } = useToastStore.getState();
  if (toast?.restores && slots.includes(toast.restores)) useToastStore.setState({ toast: null });
}

/** Runs the visible toast's action, if it has one. Returns whether it did. */
export function runToastAction(): boolean {
  const { toast } = useToastStore.getState();
  if (!toast?.action) return false;
  useToastStore.setState({ toast: null });
  toast.action.run();
  return true;
}
