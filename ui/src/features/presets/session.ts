// SPDX-License-Identifier: AGPL-3.0-or-later
// The preset browser session. Opening the browser asks the plugin for a "browse" snapshot of the
// sound; a click or arrow key auditions a preset (it plays at once, the browser stays open);
// Enter keeps it and Esc restores the snapshot exactly, unsaved edits included. Loads that would
// drop unsaved edits outside the browser keep an "undo" snapshot and offer it in a toast.
import { create } from 'zustand';
import { currentPreset, presetActions, usePresetStore } from '../../state/presetStore';
import { dismissToastRestoring, showToast } from '../../state/toastStore';
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
  /** The sound in the "browse" snapshot, which Esc goes back to. */
  origin: Origin | null;
  /** True once something else was loaded during this session. */
  auditioned: boolean;
  /** The highlighted row. Follows the current preset, ahead of it while a load is on its way. */
  cursor: string | null;
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

// The plugin runs calls in the order they arrive. To keep a snapshot, a restore or an A/B switch
// behind the loads asked for before it, preset commands go out one at a time, in order; arrowing
// through the list faster than the plugin answers sends only the newest of the waiting loads.

let chain: Promise<void> = Promise.resolve();
/** Commands queued or on their way. */
let commands = 0;
/** The load at the end of the chain that has not been sent yet; a newer audition replaces its id. */
let waitingLoad: { id: string } | null = null;
/** Loads queued or on their way; the cursor runs ahead of the current preset meanwhile. */
let loads = 0;
/**
 * Counts browser sessions and A/B switches. Keeping records Recent and offers its Undo only if
 * neither happened while the loads it waits for landed.
 */
let epoch = 0;

function enqueue(task: () => Promise<void>): Promise<void> {
  waitingLoad = null;
  commands += 1;
  const done = chain.then(task).finally(() => {
    commands -= 1;
  });
  chain = done.catch(() => undefined); // one failed command must not stop the ones after it
  return done;
}

function requestLoad(id: string): Promise<void> {
  if (waitingLoad) {
    waitingLoad.id = id;
    return chain;
  }
  const load = { id };
  loads += 1;
  const done = enqueue(async () => {
    if (waitingLoad === load) waitingLoad = null;
    try {
      await presetActions.load(load.id, true);
    } finally {
      loads -= 1;
    }
  });
  waitingLoad = load;
  return done;
}

/** Test helper: forgets the session, the queue and the counters. */
export function resetBrowserSession(): void {
  chain = Promise.resolve();
  commands = 0;
  waitingLoad = null;
  loads = 0;
  epoch = 0;
  useBrowserStore.setState({ ...closedState, filter: DEFAULT_FILTER, searchFocus: 0 });
}

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
    // A new snapshot is about to replace the one an earlier Undo would put back.
    dismissToastRestoring('browse');
    uiActions.setPopover(null);
    const session = ++epoch;
    set({
      open: true,
      mode,
      formReturn: mode === 'list' ? 'list' : 'close',
      origin: originNow(),
      auditioned: false,
      cursor: presets().currentId,
      searchFocus,
    });
    // Taken after any load still on its way, so Esc goes back to what is actually playing.
    void enqueue(async () => {
      await presetActions.snapshot('browse');
      if (session === epoch && get().open) set({ origin: originNow() });
    });
  },

  toggle(): void {
    if (get().open) presetBrowser.keep();
    else presetBrowser.open();
  },

  /** Plays a preset while the browser stays open. Asking for the highlighted row again is a no-op. */
  audition(id: string): Promise<void> {
    const s = get();
    if (id === s.cursor && (loads > 0 || presets().currentId === id)) return chain;
    set({ cursor: id });
    if (loads === 0 && presets().currentId === id) return chain;
    set({ auditioned: true });
    return requestLoad(id);
  },

  /**
   * Closes and keeps what is playing. Recent records it once its load has landed, and when the
   * browser opened on unsaved edits a toast offers them back (not after an A/B switch, which
   * passes `undo: false`: the snapshot belongs to the other slot by then).
   */
  keep(options: { undo?: boolean } = {}): void {
    const s = get();
    if (!s.open) return;
    close();
    if (!s.auditioned) return;
    const session = epoch;
    void chain.then(() => {
      if (session !== epoch) return;
      const now = presets();
      if (now.currentId) void presetActions.markRecent(now.currentId);
      // The edits that were playing when the browser opened are only in the snapshot now.
      if (options.undo !== false && s.origin?.dirty) {
        showToast(
          `Loaded ${now.currentName}`,
          { label: 'Undo', run: () => void enqueue(() => presetActions.restore('browse')) },
          'browse',
        );
      }
    });
  },

  /** Closes and restores the sound from when the browser opened, exactly. */
  cancel(): void {
    const s = get();
    if (!s.open) return;
    if (s.auditioned) {
      void enqueue(() => presetActions.restore('browse'));
      if (s.origin) showToast(`Back to ${s.origin.name}`);
    }
    close();
  },

  /**
   * After a save, rename or delete inside the browser. Before any audition, what plays now
   * becomes the state Esc returns to. After one, Esc still goes back to the sound the browser
   * opened on, so the snapshot stays.
   */
  async rebase(): Promise<void> {
    if (!get().open || get().auditioned) return;
    await enqueue(() => presetActions.snapshot('browse'));
    if (get().open && !get().auditioned) set({ origin: originNow(), cursor: presets().currentId });
  },

  /** A preset was renamed: the snapshot follows it in the plugin, so the origin follows here. */
  renamed(from: string, to: string, name: string): void {
    const origin = get().origin;
    if (origin?.id === from) set({ origin: { ...origin, id: to, name } });
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
    if (get().open && loads === 0) set({ cursor: presets().currentId });
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
    await enqueue(() => presetActions.step(delta));
    presetBrowser.syncCursor();
    return;
  }
  await enqueue(async () => {
    if (!presets().dirty) {
      await presetActions.step(delta);
      return;
    }
    await presetActions.snapshot('undo');
    await presetActions.step(delta);
    showToast(
      `Loaded ${presets().currentName}`,
      { label: 'Undo', run: () => void enqueue(() => presetActions.restore('undo')) },
      'undo',
    );
  });
}

/** Init: the default sound, as a starting point. */
export async function newPreset(): Promise<void> {
  if (get().open) {
    presetBrowser.noteAudition();
    await enqueue(() => presetActions.init());
    presetBrowser.syncCursor();
    return;
  }
  await enqueue(async () => {
    const dirty = presets().dirty;
    if (dirty) await presetActions.snapshot('undo');
    await presetActions.init();
    if (dirty)
      showToast(
        'Started from Init',
        { label: 'Undo', run: () => void enqueue(() => presetActions.restore('undo')) },
        'undo',
      );
  });
}

/**
 * ⌘S: saves a user preset in place; anything else (factory, Init) goes to Save as. While the
 * browser auditions over unsaved edits it is not clear which sound is meant, so it asks first.
 */
export async function saveCurrent(): Promise<void> {
  if (commands > 0) await chain; // decide on the preset that is about to play
  const browser = get();
  if (browser.open && browser.auditioned && browser.origin?.dirty) {
    showToast(
      `Choose first: Enter keeps ${presets().currentName}, Esc goes back to ` +
        `${browser.origin.name} with your edits. Then save.`,
    );
    return;
  }
  const current = currentPreset(presets());
  if (current?.category !== 'User') {
    presetBrowser.open('saveAs');
    return;
  }
  await enqueue(() => presetActions.save(current.name, current.folder));
  await presetBrowser.rebase();
  showToast(`Saved ${current.name}`);
}

/**
 * A/B switch. The browser keeps what it auditioned first, so Esc can't cross slots, and Undo
 * offers that would put back a snapshot from this slot go.
 */
export function compareSwitch(): Promise<void> {
  presetBrowser.keep({ undo: false });
  return enqueue(async () => {
    epoch += 1;
    dismissToastRestoring('browse', 'undo');
    await presetActions.compare('switch');
  });
}

export function compareCopy(): Promise<void> {
  return enqueue(async () => {
    dismissToastRestoring('browse', 'undo');
    await presetActions.compare('copy');
  });
}

/** Deletes a user preset, with Undo instead of a confirmation. */
export async function deletePreset(id: string): Promise<void> {
  const preset = presets().items.find((p) => p.id === id);
  if (!preset || preset.category !== 'User') return;
  await enqueue(() => presetActions.remove(id));
  await presetBrowser.rebase();
  showToast(`Deleted ${preset.name}`, {
    label: 'Undo',
    run: () => void enqueue(() => presetActions.undoDelete()).then(() => presetBrowser.rebase()),
  });
}

/** Save as and Rename from the browser's forms, in order with the other preset commands. */
export function sendPresetCommand(task: () => Promise<void>): Promise<void> {
  return enqueue(task);
}
