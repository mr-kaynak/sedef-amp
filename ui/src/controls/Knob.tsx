// SPDX-License-Identifier: AGPL-3.0-or-later
import { useId, useMemo } from 'react';
import type { CSSProperties } from 'react';
import { useParam } from '../params/useParam';
import { isBipolar, stepCount, toNormalised } from '../params/normalise';
import { angleFor, arcPath, polar, START_ANGLE, SWEEP } from './arc';
import { shortLabel, useValueControl } from './useValueControl';
import styles from './Knob.module.css';

export type KnobSize = 'sm' | 'md' | 'lg' | 'xl';

const SIZES: Record<KnobSize, number> = { sm: 36, md: 50, lg: 66, xl: 92 };

export interface KnobProps {
  param: string;
  label?: string;
  /** A named size, or the dial's diameter in pixels (an enclosure that scales its knobs). */
  size?: KnobSize | number;
  /** Hide the numeric readout under the label (the drag tooltip still shows it). */
  hideValue?: boolean;
  disabled?: boolean;
  className?: string;
}

/**
 * Rotary control. The nacre cap's sheen is a conic gradient that turns with the knob while the
 * specular highlight stays put, so light appears to travel across the shell as you turn it.
 */
export function Knob({ param, label, size = 'md', hideValue, disabled, className }: KnobProps) {
  const p = useParam(param);
  const { active, handlers, wheelRef } = useValueControl(p, { disabled });
  const labelId = useId();
  const px = typeof size === 'number' ? size : SIZES[size];
  // A dial by the pixel takes the type of the named size nearest below it.
  const named: KnobSize =
    typeof size !== 'number' ? size : size < 44 ? 'sm' : size < 60 ? 'md' : size < 84 ? 'lg' : 'xl';
  const text = label ?? shortLabel(p.spec.name, p.spec.group);

  const angle = angleFor(p.normalised);
  const origin = isBipolar(p.spec) ? angleFor(toNormalised(p.spec, 0)) : START_ANGLE;

  const ticks = useMemo(() => {
    const n = stepCount(p.spec);
    const count = n > 0 && n <= 12 ? n + 1 : 11;
    return Array.from({ length: count }, (_, i) => {
      const a = START_ANGLE + (SWEEP * i) / (count - 1);
      const [x0, y0] = polar(50, 50, 44, a);
      const [x1, y1] = polar(50, 50, i === 0 || i === count - 1 ? 49.5 : 48, a);
      return <line key={i} x1={x0} y1={y0} x2={x1} y2={y1} />;
    });
  }, [p.spec]);

  const style = { '--knob': `${px}px`, '--angle': `${angle}deg` } as CSSProperties;

  return (
    <div
      className={[styles.knob, styles[named], className].filter(Boolean).join(' ')}
      style={style}
    >
      <div
        ref={wheelRef}
        className={styles.dial}
        data-param={p.spec.id}
        role="slider"
        tabIndex={disabled ? -1 : 0}
        aria-label={p.spec.name}
        aria-describedby={labelId}
        aria-valuemin={p.spec.min}
        aria-valuemax={p.spec.max}
        aria-valuenow={Number(p.value.toFixed(4))}
        aria-valuetext={p.display}
        aria-disabled={disabled || undefined}
        data-active={active || undefined}
        {...handlers}
      >
        <svg className={styles.scale} viewBox="0 0 100 100" aria-hidden="true">
          <g className={styles.ticks}>{ticks}</g>
          <path
            className={styles.track}
            d={arcPath(50, 50, 46, START_ANGLE, START_ANGLE + SWEEP)}
          />
          <path className={styles.fill} d={arcPath(50, 50, 46, origin, angle)} />
        </svg>
        <div className={styles.skirt} aria-hidden="true" />
        <div className={styles.cap} aria-hidden="true">
          <div className={styles.shell}>
            <div className={styles.pointer} />
          </div>
        </div>
        {active && (
          <div className={styles.bubble} aria-hidden="true">
            {p.display}
          </div>
        )}
      </div>
      <div className={styles.label} id={labelId}>
        {text}
      </div>
      {!hideValue && <div className={styles.value}>{p.display}</div>}
    </div>
  );
}
