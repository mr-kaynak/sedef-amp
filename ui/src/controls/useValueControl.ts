// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared interaction model for knobs and sliders: vertical drag (shift = fine), wheel,
// double-click reset, and the standard slider keyboard map. Every edit is wrapped in a
// begin/end gesture so hosts record automation correctly. A drag or wheel burst remembers the
// parameter it began on and ends that one, also when the control unmounts or is rebound to
// another parameter halfway through.
import { useCallback, useEffect, useRef, useState } from 'react';
import type { KeyboardEvent, PointerEvent } from 'react';
import type { ParamHandle } from '../params/useParam';
import { isStepped, stepCount, toNormalised } from '../params/normalise';
import { getValue } from '../params/paramStore';
import { beginGesture, endGesture } from '../params/sender';

export interface ValueControlOptions {
  /** Pixels of vertical travel for the full range at normal sensitivity. */
  pixelsForRange?: number;
  /**
   * Count pixelsForRange in the control's own layout pixels rather than screen pixels, so it
   * follows the pointer one to one at any window scale (faders).
   */
  layoutPixels?: boolean;
  /** Divides sensitivity while shift is held. */
  fineFactor?: number;
  disabled?: boolean;
}

const WHEEL_GESTURE_IDLE_MS = 400;
/** A 100 px wheel notch moves a continuous control 2 % (0.4 % with shift) or one step. */
const WHEEL_NORMALISED_PER_PX = 0.02 / 100;
const WHEEL_FINE_FACTOR = 5;
const WHEEL_PX_PER_STEP = 100;
/** WheelEvent.deltaMode units in pixels: a mouse notch is 3 lines; a page matches PageUp. */
const WHEEL_LINE_PX = 100 / 3;
const WHEEL_PAGE_PX = 500;

/** Wheel travel in pixels, positive towards higher values (scrolling up). */
export function wheelPixels(
  e: Pick<WheelEvent, 'deltaX' | 'deltaY' | 'deltaMode' | 'shiftKey'>,
): number {
  // Some platforms turn a vertical wheel into a horizontal one while shift is held.
  const delta = e.deltaY !== 0 ? e.deltaY : e.shiftKey ? e.deltaX : 0;
  const unit = e.deltaMode === 1 ? WHEEL_LINE_PX : e.deltaMode === 2 ? WHEEL_PAGE_PX : 1;
  return -delta * unit;
}

interface Drag {
  id: string;
  pointerId: number;
  lastY: number;
  norm: number;
  /** Screen pixels per counted pixel. */
  scale: number;
}

interface Wheel {
  id: string;
  /** Fractional steps not applied yet (stepped parameters). */
  steps: number;
  timer?: ReturnType<typeof setTimeout>;
}

export function useValueControl(param: ParamHandle, options: ValueControlOptions = {}) {
  const { pixelsForRange = 200, layoutPixels = false, fineFactor = 6, disabled = false } = options;
  const [dragging, setDragging] = useState(false);
  const [wheeling, setWheeling] = useState(false);
  const drag = useRef<Drag | null>(null);
  const wheel = useRef<Wheel | null>(null);
  const wheelTarget = useRef<HTMLElement | null>(null);
  const paramRef = useRef(param);
  useEffect(() => {
    paramRef.current = param;
  });

  const stepped = isStepped(param.spec);
  const steps = stepCount(param.spec);

  const endDrag = useCallback(() => {
    const d = drag.current;
    if (!d) return;
    drag.current = null;
    endGesture(d.id);
    setDragging(false);
  }, []);

  const endWheel = useCallback(() => {
    const w = wheel.current;
    if (!w) return;
    clearTimeout(w.timer);
    wheel.current = null;
    endGesture(w.id);
    setWheeling(false);
  }, []);

  // Unmounting, or switching to another parameter, mid-edit ends what the old one began.
  useEffect(
    () => () => {
      endDrag();
      endWheel();
    },
    [param.id, endDrag, endWheel],
  );

  const onPointerDown = useCallback(
    (e: PointerEvent<HTMLElement>) => {
      if (disabled || e.button !== 0 || drag.current) return;
      e.preventDefault();
      const el = e.currentTarget;
      el.focus({ preventScroll: true });
      el.setPointerCapture?.(e.pointerId);
      const p = paramRef.current;
      const height = el.getBoundingClientRect().height;
      drag.current = {
        id: p.id,
        pointerId: e.pointerId,
        lastY: e.clientY,
        norm: p.normalised,
        // The canvas is scaled with a transform: layout height vs on-screen height.
        scale: layoutPixels && el.offsetHeight && height ? height / el.offsetHeight : 1,
      };
      beginGesture(p.id);
      setDragging(true);
    },
    [disabled, layoutPixels],
  );

  const onPointerMove = useCallback(
    (e: PointerEvent<HTMLElement>) => {
      const d = drag.current;
      const p = paramRef.current;
      if (!d || d.pointerId !== e.pointerId || d.id !== p.id) return;
      const dy = (d.lastY - e.clientY) / d.scale;
      d.lastY = e.clientY;
      const sensitivity = (e.shiftKey ? 1 / fineFactor : 1) / pixelsForRange;
      d.norm = Math.min(1, Math.max(0, d.norm + dy * sensitivity));
      p.setNormalised(d.norm);
    },
    [fineFactor, pixelsForRange],
  );

  const onPointerUp = useCallback(
    (e: PointerEvent<HTMLElement>) => {
      if (drag.current?.pointerId !== e.pointerId) return;
      endDrag();
      e.currentTarget.releasePointerCapture?.(e.pointerId);
    },
    [endDrag],
  );

  // The browser took the pointer away (element removed, system gesture): the drag is over.
  const onLostPointerCapture = useCallback(
    (e: PointerEvent<HTMLElement>) => {
      if (drag.current?.pointerId === e.pointerId) endDrag();
    },
    [endDrag],
  );

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
        if (disabled) return;
        const px = wheelPixels(e);
        if (px === 0) return;
        e.preventDefault();
        const p = paramRef.current;
        if (wheel.current && wheel.current.id !== p.id) endWheel();
        let w = wheel.current;
        const fresh = !w;
        if (!w) {
          w = { id: p.id, steps: 0 };
          wheel.current = w;
          beginGesture(p.id);
          setWheeling(true);
        }
        clearTimeout(w.timer);
        // Read the store, not the last render: trackpads send several events per frame.
        const current = getValue(p.id);
        if (stepped) {
          if (Math.sign(w.steps) !== Math.sign(px)) w.steps = 0;
          w.steps += px / WHEEL_PX_PER_STEP;
          // Some wheels send only a few pixels per notch: a burst always moves at least one step.
          const whole = Math.trunc(w.steps) || (fresh ? Math.sign(px) : 0);
          if (whole !== 0) {
            w.steps = fresh ? 0 : w.steps - whole;
            p.set(current + whole);
          }
        } else {
          const fine = e.shiftKey ? 1 / WHEEL_FINE_FACTOR : 1;
          p.setNormalised(toNormalised(p.spec, current) + px * WHEEL_NORMALISED_PER_PX * fine);
        }
        w.timer = setTimeout(endWheel, WHEEL_GESTURE_IDLE_MS);
      };
    },
    [disabled, stepped, endWheel],
  );

  return {
    active: dragging || wheeling,
    dragging,
    wheelRef,
    handlers: {
      onPointerDown,
      onPointerMove,
      onPointerUp,
      onPointerCancel: onPointerUp,
      onLostPointerCapture,
      onDoubleClick,
      onKeyDown,
    },
  };
}

/** "Pearl Gain" in group "Pearl" -> "Gain". */
export function shortLabel(name: string, group: string): string {
  return name.startsWith(`${group} `) ? name.slice(group.length + 1) : name;
}
