// SPDX-License-Identifier: AGPL-3.0-or-later
import { useParam } from '../params/useParam';
import { useRadioGroup } from './useRadioGroup';
import nacre from './NacreFill.module.css';
import styles from './Segmented.module.css';

export interface SegmentedProps {
  param: string;
  /** Display labels per choice; defaults to the schema's choice labels. */
  labels?: readonly string[];
  ariaLabel?: string;
  /** Id of a visible line that says what the choices do. */
  ariaDescribedBy?: string;
  size?: 'sm' | 'md';
  className?: string;
}

/** Radio-group selector for a choice parameter with a handful of options. */
export function Segmented({
  param,
  labels,
  ariaLabel,
  ariaDescribedBy,
  size = 'md',
  className,
}: SegmentedProps) {
  const p = useParam(param);
  const options = labels ?? p.spec.choices;
  const { onKeyDown, radio } = useRadioGroup(options.length, Math.round(p.value), (index) => {
    p.begin();
    p.set(index);
    p.end();
  });

  return (
    <div
      role="radiogroup"
      aria-label={ariaLabel ?? p.spec.name}
      aria-describedby={ariaDescribedBy}
      data-param={p.spec.id}
      className={[styles.segmented, styles[size], className].filter(Boolean).join(' ')}
      onKeyDown={onKeyDown}
    >
      {options.map((label, i) => (
        <button
          key={label}
          type="button"
          className={`${nacre.fill} ${styles.option}`}
          {...radio(i)}
        >
          {label}
        </button>
      ))}
    </div>
  );
}
