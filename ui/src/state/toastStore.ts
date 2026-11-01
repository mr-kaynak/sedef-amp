// SPDX-License-Identifier: AGPL-3.0-or-later
// One non-blocking notice at a time ("Loaded Riptide · Undo"). A new toast replaces the old one;
// app/Toasts.tsx renders it and dismisses it after a while.
import { create } from 'zustand';

export interface Toast {
  id: number;
  message: string;
  /** Optional action, e.g. Undo. Running it dismisses the toast. */
  action?: { label: string; run: () => void };
}

interface ToastStore {
  toast: Toast | null;
}

export const useToastStore = create<ToastStore>()(() => ({ toast: null }));

let nextId = 1;

export function showToast(message: string, action?: Toast['action']): number {
  const id = nextId++;
  useToastStore.setState({ toast: { id, message, ...(action ? { action } : {}) } });
  return id;
}

/** Dismisses the toast `id`, or whichever is showing. */
export function dismissToast(id?: number): void {
  const { toast } = useToastStore.getState();
  if (toast && (id === undefined || toast.id === id)) useToastStore.setState({ toast: null });
}

/** Runs the visible toast's action, if it has one. Returns whether it did. */
export function runToastAction(): boolean {
  const { toast } = useToastStore.getState();
  if (!toast?.action) return false;
  useToastStore.setState({ toast: null });
  toast.action.run();
  return true;
}
