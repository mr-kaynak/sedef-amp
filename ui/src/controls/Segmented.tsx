// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import type { KeyboardEvent } from 'react';
import { useParam } from '../params/useParam';
import styles from './Segmented.module.css';

export interface SegmentedProps {
  param: string;
  /** Display labels per choice; defaults to the schema's choice labels. */
  labels?: readonly string[];
  ariaLabel?: string;
  size?: 'sm' | 'md';
  className?: string;
}

/** Radio-group selector for a choice parameter with a handful of options. */
export function Segmented({ param, labels, ariaLabel, size = 'md', className }: SegmentedProps) {
  const p = useParam(param);
  const options = labels ?? p.spec.choices;
  const selected = Math.round(p.value);
  const refs = useRef<(HTMLButtonElement | null)[]>([]);

  const choose = (index: number) => {
    if (index === selected) return;
    p.begin();
    p.set(index);
    p.end();
  };

  const onKeyDown = (e: KeyboardEvent<HTMLDivElement>) => {
    const delta =
      e.key === 'ArrowRight' || e.key === 'ArrowDown'
        ? 1
        : e.key === 'ArrowLeft' || e.key === 'ArrowUp'
          ? -1
          : 0;
    if (!delta) return;
    e.preventDefault();
    const next = (selected + delta + options.length) % options.length;
    choose(next);
    refs.current[next]?.focus();
  };

  return (
    <div
      role="radiogroup"
      aria-label={ariaLabel ?? p.spec.name}
      className={[styles.segmented, styles[size], className].filter(Boolean).join(' ')}
      onKeyDown={onKeyDown}
    >
      {options.map((label, i) => (
        <button
          key={label}
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
          {label}
        </button>
      ))}
    </div>
  );
}
