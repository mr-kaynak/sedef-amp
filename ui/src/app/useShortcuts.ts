// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect } from 'react';
import {
  compareSwitch,
  presetBrowser,
  saveCurrent,
  stepPreset,
  useBrowserStore,
} from '../features/presets/session';
import { looperPress } from '../features/looper/actions';
import { toggleRecording } from '../features/recorder/actions';
import { useRecorderStore } from '../features/recorder/store';
import { useAppStore } from '../state/appStore';
import { usePresetStore } from '../state/presetStore';
import { runToastAction } from '../state/toastStore';
import { SECTIONS, uiActions, useUiStore } from '../state/uiStore';

function isTyping(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false;
  return target.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName);
}

/** A field where ⌘S and ⌘F mean nothing to the text (the preset search) lets them through. */
const takesCommandKeys = (target: EventTarget | null) =>
  target instanceof HTMLElement && target.hasAttribute('data-command-keys');

/**
 * Global keys (listed for people in shortcuts.ts). ⌘/Ctrl+S saves (Save as for factory presets),
 * ⌘/Ctrl+Shift+S is Save as, / and ⌘/Ctrl+F search presets, ⌘/Ctrl+Z runs a toast's Undo; none
 * of them while a text field has focus (they keep their editing meaning there), except ⌘S and
 * ⌘F in the preset search. Without modifiers and never while a text field has focus: P opens or
 * closes the preset browser, [ and ] step presets, A and B pick the compare slot, T opens or
 * closes the tuner (as its button does: an auditioned preset is kept), 1–5 switch sections (not
 * while the browser covers them), and Escape closes the open popover, then the open overlay.
 * The browser handles its own Escape. A held key repeats only where that means something ([
 * and ]). In the standalone app L is the looper's main button and Shift+L its Stop, and R starts
 * and stops a take, from anywhere, the preset browser included (docs/looper.md,
 * docs/recorder.md).
 */
export function useShortcuts(): void {
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.defaultPrevented || e.altKey) return;
      const typing = isTyping(e.target);
      const key = e.key.toLowerCase();

      if (e.metaKey || e.ctrlKey) {
        const editing = typing && !takesCommandKeys(e.target);
        if (key === 's') {
          // Never the browser's "save page", even where saving a preset is not on offer.
          e.preventDefault();
          if (editing || e.repeat) return;
          if (useBrowserStore.getState().mode !== 'list') return; // a form is open
          if (e.shiftKey) presetBrowser.open('saveAs');
          else void saveCurrent();
        } else if (key === 'f' && !e.shiftKey && !editing) {
          e.preventDefault();
          presetBrowser.open('list', { focusSearch: true });
        } else if (key === 'z' && !e.shiftKey && !typing && runToastAction()) {
          e.preventDefault();
        }
        return;
      }

      if (typing) return;
      const preset = (action: () => void) => {
        e.preventDefault();
        if (!e.repeat) action();
      };
      if (key === 'l' && useAppStore.getState().session.isStandalone) {
        e.preventDefault();
        if (!e.repeat) looperPress(e.shiftKey ? 'stop' : 'main');
        return;
      }
      if (key === 'r' && !e.shiftKey && useRecorderStore.getState().state !== null)
        return preset(() => void toggleRecording());
      switch (e.key) {
        case '/':
          return preset(() => presetBrowser.open('list', { focusSearch: true }));
        case 'p':
        case 'P':
          return preset(() => presetBrowser.toggle());
        case '[':
        case ']':
          e.preventDefault();
          void stepPreset(e.key === ']' ? 1 : -1);
          return;
        case 't':
        case 'T':
          return preset(() => {
            presetBrowser.keep();
            uiActions.toggleOverlay('tuner');
          });
        case 'a':
        case 'A':
        case 'b':
        case 'B': {
          const slot = key.toUpperCase();
          return preset(() => {
            if (usePresetStore.getState().state.compare.active !== slot) void compareSwitch();
          });
        }
      }
      const index = Number(e.key) - 1;
      const section = SECTIONS[index];
      if (section && Number.isInteger(index)) {
        if (useBrowserStore.getState().open) return;
        uiActions.setSection(section);
        uiActions.openOverlay(null);
        e.preventDefault();
        return;
      }
      if (e.key === 'Escape') {
        const { popover, overlay } = useUiStore.getState();
        if (popover) uiActions.setPopover(null);
        else if (overlay) uiActions.openOverlay(null);
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, []);
}
