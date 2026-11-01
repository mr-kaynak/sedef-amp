// SPDX-License-Identifier: AGPL-3.0-or-later
// The preset browser session. Opening the browser asks the plugin for a "browse" snapshot of the
// sound; a click or arrow key auditions a preset (it plays at once, the browser stays open);
// Enter keeps it and Esc restores the snapshot exactly, unsaved edits included. Loads that would
// drop unsaved edits outside the browser keep an "undo" snapshot and offer it in a toast.
import { create } from 'zustand';
import { currentPreset, presetActions, usePresetStore } from '../../state/presetStore';
import { dismissToast, showToast } from '../../state/toastStore';
import { uiActions } from '../../state/uiStore';
import { DEFAULT_FILTER, type BrowseFilter } from './library';

export type BrowserMode = 'list' | 'saveAs' | 'rename';

interface Origin {
  id: string | null;
  name: string;
  dirty: boolean;
}

interface BrowserState {
  open: boolean;
  mode: BrowserMode;
  /** Where the form returns to: the list, or out of the browser when it was opened to save. */
  formReturn: 'list' | 'close';
  /** The sound when the browser opened (Esc goes back to it). */
  origin: Origin | null;
  /** True once something else was loaded during this session. */
  auditioned: boolean;
  /** The highlighted row. Follows the current preset, ahead of it while a load is in flight. */
  cursor: string | null;
  /** Loads requested by the browser that have not answered yet. */
  pending: number;
  filter: BrowseFilter;
  /** Bumped to ask the browser to focus its search field. */
  searchFocus: number;
}

const closedState = {
  open: false,
  mode: 'list' as BrowserMode,
  formReturn: 'list' as const,
  origin: null,
  auditioned: false,
  cursor: null,
  pending: 0,
};

export const useBrowserStore = create<BrowserState>()(() => ({
  ...closedState,
  filter: DEFAULT_FILTER,
  searchFocus: 0,
}));

const get = () => useBrowserStore.getState();
const set = (partial: Partial<BrowserState>) => useBrowserStore.setState(partial);
const presets = () => usePresetStore.getState().state;

function originNow(): Origin {
  const s = presets();
  return { id: s.currentId, name: s.currentName, dirty: s.dirty };
}

/** Focus goes back to the preset bar when the browser closes. */
export const BROWSER_TRIGGER_ID = 'preset-browser-trigger';

/** The browser's latest load; keeping waits for it so Recent and the toast name the right preset. */
let lastLoad: Promise<void> = Promise.resolve();

function close(): void {
  set({ ...closedState });
  document.getElementById(BROWSER_TRIGGER_ID)?.focus({ preventScroll: true });
}

export const presetBrowser = {
  /** Opens the browser (or switches its mode when it is open). */
  open(mode: BrowserMode = 'list', options: { focusSearch?: boolean } = {}): void {
    const s = get();
    const searchFocus = options.focusSearch ? s.searchFocus + 1 : s.searchFocus;
    if (s.open) {
      set({ mode, formReturn: 'list', searchFocus });
      return;
    }
    dismissToast();
    uiActions.setPopover(null);
    set({
      open: true,
      mode,
      formReturn: mode === 'list' ? 'list' : 'close',
      origin: originNow(),
      auditioned: false,
      cursor: presets().currentId,
      pending: 0,
      searchFocus,
    });
    void presetActions.snapshot('browse');
  },

  toggle(): void {
    if (get().open) presetBrowser.keep();
    else presetBrowser.open();
  },

  /** Plays a preset while the browser stays open. */
  async audition(id: string): Promise<void> {
    const s = get();
    set({ cursor: id });
    if (s.pending === 0 && presets().currentId === id) return;
    set({ auditioned: true, pending: s.pending + 1 });
    const load = presetActions.load(id, true);
    lastLoad = load;
    await load;
    set({ pending: Math.max(0, get().pending - 1) });
  },

  /** Closes and keeps what is playing. */
  keep(): void {
    const s = get();
    if (!s.open) return;
    close();
    if (!s.auditioned) return;
    void lastLoad.then(() => {
      const now = presets();
      if (now.currentId) void presetActions.markRecent(now.currentId);
      // The edits that were playing when the browser opened are only in the snapshot now.
      if (s.origin?.dirty) {
        showToast(`Loaded ${now.currentName}`, {
          label: 'Undo',
          run: () => void presetActions.restore('browse'),
        });
      }
    });
  },

  /** Closes and restores the sound from when the browser opened, exactly. */
  cancel(): void {
    const s = get();
    if (!s.open) return;
    if (s.auditioned) {
      void presetActions.restore('browse');
      if (s.origin) showToast(`Back to ${s.origin.name}`);
    }
    close();
  },

  /**
   * After a save, rename or delete inside the browser the library changed under the snapshot:
   * what plays now becomes the state Esc returns to.
   */
  async rebase(): Promise<void> {
    if (!get().open) return;
    await presetActions.snapshot('browse');
    set({ origin: originNow(), auditioned: false, cursor: presets().currentId });
  },

  /** Leaves a form: back to the list, or out of the browser when it was opened to save. */
  endForm(): void {
    if (get().formReturn === 'close') close();
    else set({ mode: 'list' });
  },

  setFilter(partial: Partial<BrowseFilter>): void {
    set({ filter: { ...get().filter, ...partial } });
  },

  /** Keeps the highlighted row on the current preset when it changes from elsewhere. */
  syncCursor(): void {
    const s = get();
    if (s.open && s.pending === 0) set({ cursor: presets().currentId });
  },

  /** Marks the session as changed by a load that did not come from a row (prev/next, New). */
  noteAudition(): void {
    if (get().open) set({ auditioned: true });
  },
};

// Commands that work with or without the browser --------------------------------------------------

/** Previous/next preset. Unsaved edits stay recoverable: in the browser through Esc, else Undo. */
export async function stepPreset(delta: number): Promise<void> {
  if (get().open) {
    presetBrowser.noteAudition();
    await presetActions.step(delta);
    presetBrowser.syncCursor();
    return;
  }
  if (!presets().dirty) {
    await presetActions.step(delta);
    return;
  }
  await presetActions.snapshot('undo');
  await presetActions.step(delta);
  showToast(`Loaded ${presets().currentName}`, {
    label: 'Undo',
    run: () => void presetActions.restore('undo'),
  });
}

/** Init: the default sound, as a starting point. */
export async function newPreset(): Promise<void> {
  if (get().open) {
    presetBrowser.noteAudition();
    await presetActions.init();
    presetBrowser.syncCursor();
    return;
  }
  const dirty = presets().dirty;
  if (dirty) await presetActions.snapshot('undo');
  await presetActions.init();
  if (dirty)
    showToast('Started from Init', {
      label: 'Undo',
      run: () => void presetActions.restore('undo'),
    });
}

/** ⌘S: saves a user preset in place; anything else (factory, Init) goes to Save as. */
export async function saveCurrent(): Promise<void> {
  const state = presets();
  const current = currentPreset(state);
  if (current?.category !== 'User') {
    presetBrowser.open('saveAs');
    return;
  }
  await presetActions.save(current.name, current.folder);
  await presetBrowser.rebase();
  showToast(`Saved ${current.name}`);
}

/** A/B switch. The browser keeps what it auditioned first, so Esc can't cross slots. */
export function compareSwitch(): Promise<void> {
  presetBrowser.keep();
  return presetActions.compare('switch');
}

export function compareCopy(): Promise<void> {
  return presetActions.compare('copy');
}

/** Deletes a user preset, with Undo instead of a confirmation. */
export async function deletePreset(id: string): Promise<void> {
  const preset = presets().items.find((p) => p.id === id);
  if (!preset || preset.category !== 'User') return;
  await presetActions.remove(id);
  await presetBrowser.rebase();
  showToast(`Deleted ${preset.name}`, {
    label: 'Undo',
    run: () => void presetActions.undoDelete().then(() => presetBrowser.rebase()),
  });
}
