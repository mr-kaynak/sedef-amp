// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect } from 'react';
import { SECTIONS, uiActions, useUiStore } from '../state/uiStore';

function isTyping(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false;
  return target.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName);
}

/** 1–5 switch sections; Escape closes the open popover, then the open overlay. */
export function useShortcuts(): void {
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.defaultPrevented || e.metaKey || e.ctrlKey || e.altKey || isTyping(e.target)) return;
      const index = Number(e.key) - 1;
      const section = SECTIONS[index];
      if (section && Number.isInteger(index)) {
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
