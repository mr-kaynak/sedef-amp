// SPDX-License-Identifier: AGPL-3.0-or-later
// The header's update slot: a quiet pill when a newer release is out ("Update available"), while
// it downloads and installs ("Updating…") or once it waits for a restart ("Restart to update"),
// and the Updates panel while it is open.
// Renders nothing otherwise, and nothing at all in builds without the checker.
//
// The pill lives in the status bar (UpdatePill): the header row has no width to spare, and the
// section tabs line under it now holds the tabs, Transpose and the scene bar. The panel stays
// with the header (UpdateSlot).
import { restartText } from './text';
import { selectPill, selectSupported, updateActions, useUpdateStore } from './updateStore';
import { UpdatesPanel } from './UpdatesPanel';
import buttons from '../../controls/Button.module.css';
import styles from './UpdateSlot.module.css';

/** The Updates panel, while it is open. */
export function UpdateSlot() {
  const open = useUpdateStore((s) => s.open);
  return open ? <UpdatesPanel /> : null;
}

/** The pill in the status bar; nothing when there is no news. */
export function UpdatePill() {
  const pill = useUpdateStore(selectPill);
  const state = useUpdateStore(selectSupported);
  const open = useUpdateStore((s) => s.open);

  const version = state?.latest?.version ?? '';
  const restart = pill === 'restart' && state ? restartText(state) : null;
  // The visible label, and the longer line for the accessible name and tooltip.
  const [label, detail] = restart
    ? ['Restart to update', `${restart.headline}. ${restart.status}`]
    : pill === 'updating'
      ? ['Updating…', `Updating to Sedef Amp ${version}`]
      : ['Update available', `Sedef Amp ${version} is available`];
  const name =
    pill === 'updating' ? detail : `${label}: ${restart ? detail : `Sedef Amp ${version}`}`;

  if (!pill) return null;
  return (
    <button
      type="button"
      className={`${buttons.press} ${styles.pill}`}
      aria-haspopup="dialog"
      aria-expanded={open}
      aria-label={name}
      title={detail}
      onClick={updateActions.openPanel}
    >
      <span className={styles.dot} aria-hidden="true" />
      {label}
    </button>
  );
}
