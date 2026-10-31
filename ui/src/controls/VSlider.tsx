// SPDX-License-Identifier: AGPL-3.0-or-later
import type { CSSProperties } from 'react';
import { useParam } from '../params/useParam';
import { isBipolar, toNormalised } from '../params/normalise';
import { shortLabel, useValueControl } from './useValueControl';
import styles from './VSlider.module.css';

export interface VSliderProps {
  param: string;
  label?: string;
  /** Track height in CSS px; the cap follows the pointer one to one. */
  height?: number;
  disabled?: boolean;
}

/** Vertical fader (EQ bands). Bipolar ranges fill from the centre line. */
export function VSlider({ param, label, height = 180, disabled }: VSliderProps) {
  const p = useParam(param);
  const { active, handlers, wheelRef } = useValueControl(p, {
    pixelsForRange: height,
    layoutPixels: true,
    fineFactor: 8,
    disabled,
  });
  const origin = isBipolar(p.spec) ? toNormalised(p.spec, 0) : 0;
  const lo = Math.min(origin, p.normalised);
  const hi = Math.max(origin, p.normalised);
  const style = {
    '--h': `${height}px`,
    '--pos': p.normalised,
    '--lo': lo,
    '--hi': hi,
  } as CSSProperties;

  return (
    <div className={styles.vslider} style={style} data-disabled={disabled || undefined}>
      <div className={styles.readout} data-active={active || undefined}>
        {p.display.replace(' dB', '')}
      </div>
      <div
        ref={wheelRef}
        className={styles.track}
        role="slider"
        aria-orientation="vertical"
        tabIndex={disabled ? -1 : 0}
        aria-label={p.spec.name}
        aria-valuemin={p.spec.min}
        aria-valuemax={p.spec.max}
        aria-valuenow={Number(p.value.toFixed(3))}
        aria-valuetext={p.display}
        aria-disabled={disabled || undefined}
        data-active={active || undefined}
        {...handlers}
      >
        <div className={styles.groove} aria-hidden="true" />
        <div className={styles.fill} aria-hidden="true" />
        <div className={styles.cap} aria-hidden="true" />
      </div>
      <div className={styles.label}>{label ?? shortLabel(p.spec.name, p.spec.group)}</div>
    </div>
  );
}
