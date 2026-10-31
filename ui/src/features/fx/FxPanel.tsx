// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ReactNode } from 'react';
import { Toggle } from '../../controls/Toggle';
import { useParamValue } from '../../params/useParam';
import styles from './FxPanel.module.css';

/** Rack-style panel shared by the time effects: name, what it is, its switch, its controls. */
export function FxPanel({
  name,
  kind,
  enableParam,
  extra,
  className,
  children,
}: {
  name: string;
  kind: string;
  enableParam: string;
  extra?: ReactNode;
  className?: string;
  children: ReactNode;
}) {
  const on = useParamValue(enableParam) >= 0.5;
  return (
    <section
      className={[styles.panel, className].filter(Boolean).join(' ')}
      data-on={on || undefined}
      aria-label={`${name} ${kind}`}
    >
      <header className={styles.head}>
        <h2 className={styles.name}>{name}</h2>
        <span className={styles.kind}>{kind}</span>
        <span className={styles.spacer} />
        {extra}
        <Toggle param={enableParam} label={on ? 'On' : 'Off'} />
      </header>
      <div className={styles.body}>{children}</div>
    </section>
  );
}
