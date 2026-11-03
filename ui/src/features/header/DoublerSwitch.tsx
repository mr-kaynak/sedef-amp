// SPDX-License-Identifier: AGPL-3.0-or-later
// The Doubler's switch under the output meter, so double tracking is one click away from every
// page. Its settings stay with the unit on the FX page.
import { Led } from '../../controls/Led';
import { useParam } from '../../params/useParam';
import styles from './DoublerSwitch.module.css';

export function DoublerSwitch() {
  const p = useParam('doublerEnabled');
  const on = p.value >= 0.5;
  return (
    <button
      type="button"
      role="switch"
      aria-checked={on}
      className={styles.switch}
      data-on={on || undefined}
      title="Double tracking: a second take of your part on the right. Shape it on the FX page."
      onClick={() => {
        p.begin();
        p.set(on ? 0 : 1);
        p.end();
      }}
    >
      <Led on={on} size={5} />
      Doubler
    </button>
  );
}
