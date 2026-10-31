// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared interaction model for knobs and sliders: vertical drag (shift = fine), wheel,
// double-click reset, and the standard slider keyboard map. Every edit is wrapped in a
// begin/end gesture so hosts record automation correctly.
import { useCallback, useEffect, useRef, useState } from 'react';
import type { KeyboardEvent, PointerEvent } from 'react';
import type { ParamHandle } from '../params/useParam';
import { isStepped, stepCount } from '../params/normalise';

export interface ValueControlOptions {
  /** Screen pixels of vertical travel for the full range at normal sensitivity. */
  pixelsForRange?: number;
  /** Divides sensitivity while shift is held. */
  fineFactor?: number;
  disabled?: boolean;
}

const WHEEL_GESTURE_IDLE_MS = 400;

export function useValueControl(param: ParamHandle, options: ValueControlOptions = {}) {
  const { pixelsForRange = 200, fineFactor = 6, disabled = false } = options;
  const [dragging, setDragging] = useState(false);
  const [wheeling, setWheeling] = useState(false);
  const drag = useRef<{ lastY: number; norm: number; pointerId: number } | null>(null);
  const wheelTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const wheelTarget = useRef<HTMLElement | null>(null);
  const paramRef = useRef(param);
  useEffect(() => {
    paramRef.current = param;
  });

  const stepped = isStepped(param.spec);
  const steps = stepCount(param.spec);

  const onPointerDown = useCallback(
    (e: PointerEvent<HTMLElement>) => {
      if (disabled || e.button !== 0) return;
      e.preventDefault();
      e.currentTarget.focus({ preventScroll: true });
      e.currentTarget.setPointerCapture?.(e.pointerId);
      drag.current = {
        lastY: e.clientY,
        norm: paramRef.current.normalised,
        pointerId: e.pointerId,
      };
      paramRef.current.begin();
      setDragging(true);
    },
    [disabled],
  );

  const onPointerMove = useCallback(
    (e: PointerEvent<HTMLElement>) => {
      const d = drag.current;
      if (!d || d.pointerId !== e.pointerId) return;
      const dy = d.lastY - e.clientY;
      d.lastY = e.clientY;
      const sensitivity = (e.shiftKey ? 1 / fineFactor : 1) / pixelsForRange;
      d.norm = Math.min(1, Math.max(0, d.norm + dy * sensitivity));
      paramRef.current.setNormalised(d.norm);
    },
    [fineFactor, pixelsForRange],
  );

  const finishDrag = useCallback((e: PointerEvent<HTMLElement>) => {
    const d = drag.current;
    if (!d || d.pointerId !== e.pointerId) return;
    drag.current = null;
    e.currentTarget.releasePointerCapture?.(e.pointerId);
    paramRef.current.end();
    setDragging(false);
  }, []);

  const onDoubleClick = useCallback(() => {
    if (!disabled) paramRef.current.reset();
  }, [disabled]);

  const nudge = useCallback(
    (direction: number, coarse: boolean, fine: boolean) => {
      const p = paramRef.current;
      p.begin();
      if (stepped) {
        const by = coarse ? Math.max(1, Math.round(steps / 4)) : 1;
        p.set(p.value + direction * by);
      } else {
        const by = coarse ? 0.1 : fine ? 0.002 : 0.01;
        p.setNormalised(p.normalised + direction * by);
      }
      p.end();
    },
    [stepped, steps],
  );

  const onKeyDown = useCallback(
    (e: KeyboardEvent<HTMLElement>) => {
      if (disabled) return;
      const p = paramRef.current;
      switch (e.key) {
        case 'ArrowUp':
        case 'ArrowRight':
          nudge(1, false, e.shiftKey);
          break;
        case 'ArrowDown':
        case 'ArrowLeft':
          nudge(-1, false, e.shiftKey);
          break;
        case 'PageUp':
          nudge(1, true, false);
          break;
        case 'PageDown':
          nudge(-1, true, false);
          break;
        case 'Home':
          p.begin();
          p.set(p.spec.min);
          p.end();
          break;
        case 'End':
          p.begin();
          p.set(p.spec.max);
          p.end();
          break;
        case 'Delete':
        case 'Backspace':
          p.reset();
          break;
        default:
          return;
      }
      e.preventDefault();
      e.stopPropagation();
    },
    [disabled, nudge],
  );

  // Wheel needs a non-passive listener so the page never scrolls under a knob.
  const wheelRef = useCallback(
    (el: HTMLElement | null) => {
      const previous = wheelTarget.current;
      if (previous) previous.onwheel = null;
      wheelTarget.current = el;
      if (!el) return;
      el.onwheel = (e: WheelEvent) => {
        if (disabled || e.deltaY === 0) return;
        e.preventDefault();
        const p = paramRef.current;
        const direction = e.deltaY < 0 ? 1 : -1;
        if (!wheelTimer.current) {
          p.begin();
          setWheeling(true);
        } else {
          clearTimeout(wheelTimer.current);
        }
        if (stepped) p.set(p.value + direction);
        else p.setNormalised(p.normalised + direction * (e.shiftKey ? 0.004 : 0.02));
        wheelTimer.current = setTimeout(() => {
          wheelTimer.current = null;
          paramRef.current.end();
          setWheeling(false);
        }, WHEEL_GESTURE_IDLE_MS);
      };
    },
    [disabled, stepped],
  );

  useEffect(
    () => () => {
      if (wheelTimer.current) {
        clearTimeout(wheelTimer.current);
        paramRef.current.end();
      }
    },
    [],
  );

  return {
    active: dragging || wheeling,
    dragging,
    wheelRef,
    handlers: {
      onPointerDown,
      onPointerMove,
      onPointerUp: finishDrag,
      onPointerCancel: finishDrag,
      onDoubleClick,
      onKeyDown,
    },
  };
}

/** "Pearl Gain" in group "Pearl" -> "Gain". */
export function shortLabel(name: string, group: string): string {
  return name.startsWith(`${group} `) ? name.slice(group.length + 1) : name;
}
