// SPDX-License-Identifier: AGPL-3.0-or-later
import { useParam } from '../params/useParam';
import { Icon } from './Icon';
import styles from './ChoiceSelect.module.css';

/** Native select for long choice lists (mic types): accessible and familiar in every host. */
export function ChoiceSelect({
  param,
  ariaLabel,
  className,
}: {
  param: string;
  ariaLabel?: string;
  className?: string;
}) {
  const p = useParam(param);
  return (
    <span className={[styles.wrap, className].filter(Boolean).join(' ')}>
      <select
        className={styles.select}
        aria-label={ariaLabel ?? p.spec.name}
        data-param={p.spec.id}
        value={Math.round(p.value)}
        onChange={(e) => {
          p.begin();
          p.set(Number(e.target.value));
          p.end();
        }}
      >
        {p.spec.choices.map((label, i) => (
          <option key={label} value={i}>
            {label}
          </option>
        ))}
      </select>
      <Icon name="down" size={14} className={styles.chevron} />
    </span>
  );
}
