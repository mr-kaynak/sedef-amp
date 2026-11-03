// SPDX-License-Identifier: AGPL-3.0-or-later
import { useParam } from '../params/useParam';
import styles from './Footswitch.module.css';

/** Stomp switch for a module's enable parameter. */
export function Footswitch({
  param,
  label,
  describedBy,
}: {
  param: string;
  label?: string;
  /** Id of a visible hint that tells how to play the effect. */
  describedBy?: string;
}) {
  const p = useParam(param);
  const on = p.value >= 0.5;
  const toggle = () => {
    p.begin();
    p.set(on ? 0 : 1);
    p.end();
  };
  return (
    <button
      type="button"
      role="switch"
      aria-checked={on}
      aria-label={label ?? p.spec.name}
      aria-describedby={describedBy}
      className={styles.footswitch}
      data-on={on || undefined}
      onClick={toggle}
    >
      <span className={styles.nut} aria-hidden="true">
        <span className={styles.plunger} />
      </span>
    </button>
  );
}
