// SPDX-License-Identifier: AGPL-3.0-or-later
// The keyboard shortcuts, behind a button in the browser's footer.
import { Fragment } from 'react';
import { Icon } from '../../controls/Icon';
import buttons from '../../controls/Button.module.css';
import { shortcuts } from '../../app/shortcuts';
import { useRecorderStore } from '../recorder/store';
import styles from './ShortcutList.module.css';

export function ShortcutList({
  open,
  onToggle,
}: {
  open: boolean;
  onToggle: (open: boolean) => void;
}) {
  const recorder = useRecorderStore((s) => s.state !== null);
  return (
    <div className={styles.anchor}>
      <button
        type="button"
        className={`${buttons.ghost} ${buttons.small} ${buttons.lead}`}
        aria-expanded={open}
        aria-controls="preset-shortcuts"
        onClick={() => onToggle(!open)}
      >
        {/* The regular 1.5px line: at 2px the keys fill the 14px case in. */}
        <Icon name="keyboard" size={14} />
        Shortcuts
      </button>
      {open && (
        <div id="preset-shortcuts" className={styles.panel} role="group" aria-label="Shortcuts">
          {shortcuts({ recorder }).map((group) => (
            <section key={group.title}>
              <h3 className={styles.heading}>{group.title}</h3>
              <dl className={styles.list}>
                {group.items.map((item) => (
                  <Fragment key={item.label}>
                    <dt>
                      {item.keys.map((k, i) => (
                        <Fragment key={k}>
                          {i > 0 && <span className={styles.or}>/</span>}
                          <kbd>{k}</kbd>
                        </Fragment>
                      ))}
                    </dt>
                    <dd>{item.label}</dd>
                  </Fragment>
                ))}
              </dl>
            </section>
          ))}
        </div>
      )}
    </div>
  );
}
