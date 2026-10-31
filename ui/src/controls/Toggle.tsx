// SPDX-License-Identifier: AGPL-3.0-or-later
import { useParam } from '../params/useParam';
import { shortLabel } from './useValueControl';
import { Led } from './Led';
import styles from './Toggle.module.css';

export interface ToggleProps {
  param: string;
  label?: string;
  /** "lever": hardware bat switch; "chip": compact LED button for panels. */
  variant?: 'lever' | 'chip';
  title?: string;
}

/** On/off control for a bool parameter. */
export function Toggle({ param, label, variant = 'chip', title }: ToggleProps) {
  const p = useParam(param);
  const on = p.value >= 0.5;
  const text = label ?? shortLabel(p.spec.name, p.spec.group);
  const toggle = () => {
    p.begin();
    p.set(on ? 0 : 1);
    p.end();
  };

  if (variant === 'lever') {
    return (
      <button
        type="button"
        role="switch"
        aria-checked={on}
        aria-label={p.spec.name}
        title={title}
        className={styles.lever}
        data-on={on || undefined}
        onClick={toggle}
      >
        <span className={styles.slot} aria-hidden="true">
          <span className={styles.bat} />
        </span>
        <span className={styles.leverLabel}>
          <Led on={on} size={5} />
          {text}
        </span>
      </button>
    );
  }

  return (
    <button
      type="button"
      role="switch"
      aria-checked={on}
      aria-label={p.spec.name}
      title={title}
      className={styles.chip}
      data-on={on || undefined}
      onClick={toggle}
    >
      <Led on={on} size={6} />
      <span>{text}</span>
    </button>
  );
}
