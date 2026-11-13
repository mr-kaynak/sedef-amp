// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import type { CSSProperties, KeyboardEvent } from 'react';
import { useParam } from '../params/useParam';
import styles from './ChoiceLever.module.css';

/** Multi-position lever switch (Reef's voice switch). The lever points at the active label. */
export function ChoiceLever({ param, label }: { param: string; label?: string }) {
  const p = useParam(param);
  const options = p.spec.choices;
  const selected = Math.round(p.value);
  const refs = useRef<(HTMLButtonElement | null)[]>([]);

  const choose = (i: number) => {
    if (i === selected) return;
    p.begin();
    p.set(i);
    p.end();
  };

  const onKeyDown = (e: KeyboardEvent) => {
    const delta =
      e.key === 'ArrowDown' || e.key === 'ArrowRight'
        ? 1
        : e.key === 'ArrowUp' || e.key === 'ArrowLeft'
          ? -1
          : 0;
    if (!delta) return;
    e.preventDefault();
    const next = Math.min(options.length - 1, Math.max(0, selected + delta));
    choose(next);
    refs.current[next]?.focus();
  };

  const tilt = options.length > 1 ? -35 + (70 * selected) / (options.length - 1) : 0;

  return (
    <div className={styles.wrap}>
      <div
        role="radiogroup"
        aria-label={p.spec.name}
        data-param={p.spec.id}
        className={styles.lever}
        onKeyDown={onKeyDown}
        style={{ '--tilt': `${tilt}deg` } as CSSProperties}
      >
        <span className={styles.base} aria-hidden="true">
          <span className={styles.bat} />
        </span>
        <span className={styles.options}>
          {options.map((o, i) => (
            <button
              key={o}
              ref={(el) => {
                refs.current[i] = el;
              }}
              type="button"
              role="radio"
              aria-checked={i === selected}
              tabIndex={i === selected ? 0 : -1}
              className={styles.option}
              onClick={() => choose(i)}
            >
              {o}
            </button>
          ))}
        </span>
      </div>
      {label && <span className={styles.caption}>{label}</span>}
    </div>
  );
}
