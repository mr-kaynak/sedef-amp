// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect } from 'react';
import {
  compareSwitch,
  presetBrowser,
  saveCurrent,
  stepPreset,
  useBrowserStore,
} from '../features/presets/session';
import { usePresetStore } from '../state/presetStore';
import { runToastAction } from '../state/toastStore';
import { SECTIONS, uiActions, useUiStore } from '../state/uiStore';

function isTyping(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false;
  return target.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName);
}

/**
 * Global keys (listed for people in shortcuts.ts). ⌘/Ctrl+S saves (Save as for factory presets),
 * ⌘/Ctrl+Shift+S is Save as, / and ⌘/Ctrl+F search presets, ⌘/Ctrl+Z runs a toast's Undo.
 * Without modifiers and never while a text field has focus: P opens or closes the preset browser,
 * [ and ] step presets, A and B pick the compare slot, 1–5 switch sections (not while the browser
 * covers them), and Escape closes the open popover, then the open overlay. The browser handles
 * its own Escape.
 */
export function useShortcuts(): void {
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.defaultPrevented || e.altKey) return;
      const typing = isTyping(e.target);
      const key = e.key.toLowerCase();

      if (e.metaKey || e.ctrlKey) {
        if (key === 's') {
          e.preventDefault();
          if (useBrowserStore.getState().mode !== 'list') return; // a form is open
          if (e.shiftKey) presetBrowser.open('saveAs');
          else void saveCurrent();
        } else if (key === 'f' && !e.shiftKey) {
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
        action();
      };
      switch (e.key) {
        case '/':
          return preset(() => presetBrowser.open('list', { focusSearch: true }));
        case 'p':
        case 'P':
          return preset(() => presetBrowser.toggle());
        case '[':
          return preset(() => void stepPreset(-1));
        case ']':
          return preset(() => void stepPreset(1));
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
