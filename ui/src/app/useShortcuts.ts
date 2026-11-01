// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect } from 'react';
import { presetBrowser, saveCurrent, useBrowserStore } from '../features/presets/session';
import { runToastAction } from '../state/toastStore';
import { SECTIONS, uiActions, useUiStore } from '../state/uiStore';

function isTyping(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false;
  return target.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName);
}

/**
 * Global keys. ⌘/Ctrl+S saves (Save as for factory presets), ⌘/Ctrl+Shift+S is Save as, / and
 * ⌘/Ctrl+F search presets, ⌘/Ctrl+Z runs a toast's Undo. Without modifiers and outside text
 * fields, 1–5 switch sections (not while the preset browser covers them), and Escape closes the
 * open popover, then the open overlay. The browser handles its own Escape.
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
      if (e.key === '/') {
        e.preventDefault();
        presetBrowser.open('list', { focusSearch: true });
        return;
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
