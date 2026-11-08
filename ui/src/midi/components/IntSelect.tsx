// SPDX-License-Identifier: AGPL-3.0-or-later
// A native select over a whole-number parameter's range (the MIDI channel): every value at a
// glance, keyboard and screen readers for free.
import { Icon } from '../../controls/Icon';
import select from '../../controls/ChoiceSelect.module.css';
import { useParam } from '../../params/useParam';

export function IntSelect({
  param,
  ariaLabel,
  className,
  disabled,
  format = String,
}: {
  param: string;
  ariaLabel?: string;
  className?: string;
  disabled?: boolean;
  format?: (value: number) => string;
}) {
  const p = useParam(param);
  const values: number[] = [];
  for (let v = p.spec.min; v <= p.spec.max; v += 1) values.push(v);
  return (
    <span className={[select.wrap, className].filter(Boolean).join(' ')}>
      <select
        className={select.select}
        aria-label={ariaLabel ?? p.spec.name}
        disabled={disabled}
        value={Math.round(p.value)}
        onChange={(e) => {
          p.begin();
          p.set(Number(e.target.value));
          p.end();
        }}
      >
        {values.map((v) => (
          <option key={v} value={v}>
            {format(v)}
          </option>
        ))}
      </select>
      <Icon name="down" size={14} className={select.chevron} />
    </span>
  );
}
