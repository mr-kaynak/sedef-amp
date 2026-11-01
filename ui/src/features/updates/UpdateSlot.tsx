// SPDX-License-Identifier: AGPL-3.0-or-later
// The header's update slot: a quiet pill when a newer release is out, and the Updates panel while
// it is open. Renders nothing otherwise, and nothing at all in builds without the checker. The
// header has about 46 px to spare (standalone, with the metronome), so the pill shows a nacre dot
// and an arrow; its name and tooltip say "Update available".
import { selectShowPill, selectSupported, updateActions, useUpdateStore } from './updateStore';
import { UpdatesPanel } from './UpdatesPanel';
import styles from './UpdateSlot.module.css';

export function UpdateSlot() {
  const show = useUpdateStore(selectShowPill);
  const version = useUpdateStore((s) => selectSupported(s)?.latest?.version ?? '');
  const open = useUpdateStore((s) => s.open);

  return (
    <>
      {show && (
        <button
          type="button"
          className={styles.pill}
          aria-haspopup="dialog"
          aria-expanded={open}
          aria-label={`Update available: Sedef Amp ${version}`}
          title={`Update available: Sedef Amp ${version}`}
          onClick={updateActions.openPanel}
        >
          <span className={styles.dot} aria-hidden="true" />
          <svg
            width={12}
            height={12}
            viewBox="0 0 16 16"
            fill="none"
            stroke="currentColor"
            strokeWidth={1.6}
            strokeLinecap="round"
            strokeLinejoin="round"
            aria-hidden="true"
            focusable="false"
          >
            <path d="M8 2v8.5M4.5 7.5 8 11l3.5-3.5M3 14h10" />
          </svg>
        </button>
      )}
      {open && <UpdatesPanel />}
    </>
  );
}
