// SPDX-License-Identifier: AGPL-3.0-or-later
// The Updates row in Settings: where things stand, and the way into the Updates panel.
import buttons from '../../controls/Button.module.css';
import { uiActions } from '../../state/uiStore';
import { selectSupported, updateActions, useUpdateStore } from './updateStore';
import styles from './UpdatesSettings.module.css';

export function UpdatesSettings({ className }: { className?: string }) {
  const state = useUpdateStore(selectSupported);
  if (!state) return null;

  const status =
    state.phase === 'checking'
      ? 'Checking for updates…'
      : state.updateAvailable && state.latest
        ? `Sedef Amp ${state.latest.version} is available`
        : state.latest
          ? 'Sedef Amp is up to date'
          : 'Not checked yet';

  return (
    <section className={className}>
      <h3 className={styles.heading}>Updates</h3>
      <div className={styles.row}>
        <p className={styles.status}>{status}</p>
        <button
          type="button"
          className={`${buttons.solid} ${buttons.small}`}
          aria-haspopup="dialog"
          onClick={() => {
            uiActions.setPopover(null);
            updateActions.openPanel();
          }}
        >
          Updates…
        </button>
      </div>
      <p className={styles.note}>
        {state.autoCheck && state.autoCheckAllowed
          ? 'Checks once a day when you open Sedef.'
          : 'Automatic checks are off.'}
      </p>
    </section>
  );
}
