// SPDX-License-Identifier: AGPL-3.0-or-later
// The toast area: one polite live region above the status bar. A toast stays for a while, longer
// while the pointer or focus is on it, and never takes focus itself.
import { useEffect, useState } from 'react';
import { Icon } from '../controls/Icon';
import { dismissToast, runToastAction, useToastStore } from '../state/toastStore';
import buttons from '../controls/Button.module.css';
import { modKey } from './platform';
import styles from './Toasts.module.css';

const SHOW_MS = 4000;
const SHOW_WITH_ACTION_MS = 8000;

export function Toasts() {
  const toast = useToastStore((s) => s.toast);
  // Which toast the pointer or focus is on, by id: a toast that replaces a held one starts its
  // own timer even though no pointerleave or blur arrives for the one that went.
  const [hoverId, setHoverId] = useState<number | null>(null);
  const [focusId, setFocusId] = useState<number | null>(null);
  const held = toast !== null && (hoverId === toast.id || focusId === toast.id);

  useEffect(() => {
    if (!toast || held) return;
    const timer = setTimeout(
      () => dismissToast(toast.id),
      toast.action ? SHOW_WITH_ACTION_MS : SHOW_MS,
    );
    return () => clearTimeout(timer);
  }, [toast, held]);

  return (
    <div className={styles.region} role="status" aria-live="polite">
      {toast && (
        <div
          key={toast.id}
          className={styles.toast}
          onPointerEnter={() => setHoverId(toast.id)}
          onPointerLeave={() => setHoverId(null)}
          onFocus={() => setFocusId(toast.id)}
          onBlur={() => setFocusId(null)}
        >
          <span className={styles.message}>{toast.message}</span>
          {toast.action && (
            <button
              type="button"
              className={`${buttons.ghost} ${styles.action}`}
              aria-keyshortcuts={`${modKey().aria}+Z`}
              onClick={() => runToastAction()}
            >
              {toast.action.label}
              <kbd className={styles.kbd} aria-hidden="true">
                {modKey().symbol}Z
              </kbd>
            </button>
          )}
          <button
            type="button"
            className={`${buttons.ghost} ${buttons.icon} ${styles.close}`}
            aria-label="Dismiss"
            onClick={() => dismissToast(toast.id)}
          >
            <Icon name="close" size={12} />
          </button>
        </div>
      )}
    </div>
  );
}
