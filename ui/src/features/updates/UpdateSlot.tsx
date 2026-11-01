// SPDX-License-Identifier: AGPL-3.0-or-later
// The header's update slot: a quiet pill when a newer release is out, and the Updates panel while
// it is open. Renders nothing otherwise, and nothing at all in builds without the checker.
//
// The header row has no width to spare, so the pill takes none: it is positioned against the
// header and hangs at the right end of the section tabs line below it, which is empty.
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
          title={`Sedef Amp ${version} is available`}
          onClick={updateActions.openPanel}
        >
          <span className={styles.dot} aria-hidden="true" />
          Update available
        </button>
      )}
      {open && <UpdatesPanel />}
    </>
  );
}
