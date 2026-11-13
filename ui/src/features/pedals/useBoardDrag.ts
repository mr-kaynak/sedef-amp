// SPDX-License-Identifier: AGPL-3.0-or-later
// Dragging a pedal, by its handle or by its enclosure. The pedal follows the pointer, the others
// stay put, and a bar marks the gap it would land in. Positions come from the layout in canvas pixels (offsetLeft and
// friends), pointer movement from the screen, divided by the Stage's scale. Escape, a lost
// pointer capture or a cancelled pointer put the pedal back.
import { useCallback, useEffect, useRef, useState } from 'react';
import type { PointerEvent as ReactPointerEvent, RefObject } from 'react';

export interface DragView {
  id: string;
  /** How far the pedal has travelled, in canvas pixels. */
  dx: number;
  dy: number;
  /** Where it would land on the board without it (movePedal's index). */
  index: number;
  /** The pointer is past where this pedal can go (after the amp), so it lands short of it. */
  clamped: boolean;
  /** The drop bar: its centre line and height, in the list's canvas pixels. */
  bar: { x: number; y: number; height: number } | null;
}

interface Session {
  id: string;
  pointerId: number;
  startX: number;
  startY: number;
  startScroll: number;
  x: number;
  y: number;
  started: boolean;
}

/** Movement before a press on the handle becomes a drag, in screen pixels. */
const SLOP = 4;
/** Autoscroll: how close to the scroller's edge, in screen pixels, and how fast. */
const EDGE = 48;
const SPEED = 14;
/** What a press on the enclosure belongs to instead of a drag. */
const CONTROLS = 'button, a, input, select, [role="slider"], [role="radiogroup"], [role="switch"]';

export function useBoardDrag(options: {
  listRef: RefObject<HTMLElement | null>;
  scrollerRef: RefObject<HTMLElement | null>;
  /** Space between pedals in the list, in canvas pixels (the bar sits in the middle). */
  gap: number;
  /**
   * The row a pedal's element stands in, in the list's canvas pixels: pedals differ in height, so
   * the bar spans the row. Without it the bar takes the neighbouring pedal's own box.
   */
  rowOf?: (el: HTMLElement) => { top: number; height: number } | undefined;
  /** The nearest index the pedal may land at, for one that can't go everywhere. */
  clamp?: (id: string, index: number) => number;
  onDrop: (id: string, index: number) => void;
}) {
  const { listRef, scrollerRef, gap, rowOf, clamp, onDrop } = options;
  const [drag, setDrag] = useState<DragView | null>(null);
  const session = useRef<Session | null>(null);

  const others = useCallback(
    (id: string) =>
      [...(listRef.current?.querySelectorAll<HTMLElement>('[data-pedal]') ?? [])].filter(
        (el) => el.dataset.pedal !== id,
      ),
    [listRef],
  );

  /** The gap under the pointer, in reading order: before the first pedal it comes before. */
  const measure = useCallback(
    (s: Session): DragView => {
      const list = listRef.current;
      const scale =
        list && list.offsetWidth > 0 ? list.getBoundingClientRect().width / list.offsetWidth : 1;
      const rest = others(s.id);
      // A short pedal stands at the bottom of its row: the row's top and bottom decide which row
      // the pointer is in, not the pedal's own.
      const rects = rest.map((el) => el.getBoundingClientRect());
      const bands = new Map<string, { top: number; bottom: number }>();
      rest.forEach((el, i) => {
        const key = el.dataset.row ?? '';
        const band = bands.get(key);
        const r = rects[i]!;
        bands.set(key, {
          top: Math.min(band?.top ?? r.top, r.top),
          bottom: Math.max(band?.bottom ?? r.bottom, r.bottom),
        });
      });
      let pointed = rest.length;
      for (let i = 0; i < rest.length; i += 1) {
        const r = rects[i]!;
        const band = bands.get(rest[i]!.dataset.row ?? '')!;
        if (s.y < band.top || (s.y <= band.bottom && s.x < r.left + r.width / 2)) {
          pointed = i;
          break;
        }
      }
      const index = clamp ? clamp(s.id, pointed) : pointed;
      const before = rest[index];
      const after = rest[index - 1];
      const beside = before ?? after;
      const row = beside
        ? (rowOf?.(beside) ?? { top: beside.offsetTop, height: beside.offsetHeight })
        : null;
      const bar =
        beside && row
          ? {
              x: before
                ? before.offsetLeft - gap / 2
                : beside.offsetLeft + beside.offsetWidth + gap / 2,
              y: row.top,
              height: row.height,
            }
          : null;
      const scroll = (scrollerRef.current?.scrollTop ?? 0) - s.startScroll;
      return {
        id: s.id,
        dx: (s.x - s.startX) / scale,
        dy: (s.y - s.startY) / scale + scroll,
        index,
        clamped: index !== pointed,
        bar,
      };
    },
    [clamp, gap, listRef, others, rowOf, scrollerRef],
  );

  const stop = useCallback(() => {
    session.current = null;
    setDrag(null);
  }, []);

  // Near the top or bottom edge the board scrolls, so a pedal can travel to any row.
  const dragging = drag !== null;
  useEffect(() => {
    if (!dragging) return;
    let frame = 0;
    const tick = () => {
      const s = session.current;
      const scroller = scrollerRef.current;
      if (s?.started && scroller) {
        const r = scroller.getBoundingClientRect();
        const step = s.y < r.top + EDGE ? -SPEED : s.y > r.bottom - EDGE ? SPEED : 0;
        const before = scroller.scrollTop;
        if (step !== 0) scroller.scrollTop = before + step;
        if (scroller.scrollTop !== before) setDrag(measure(s));
      }
      frame = requestAnimationFrame(tick);
    };
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, [dragging, measure, scrollerRef]);

  useEffect(() => {
    if (!drag) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return;
      e.preventDefault();
      e.stopPropagation();
      stop();
    };
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [drag, stop]);

  const dragProps = (id: string, fromBody: boolean) => ({
    onPointerDown: (e: ReactPointerEvent<HTMLElement>) => {
      if (e.button !== 0 || session.current) return;
      // On the enclosure, a press on a knob, a switch or a button is that control's.
      if (fromBody && (e.target as Element).closest?.(CONTROLS)) return;
      e.currentTarget.setPointerCapture?.(e.pointerId);
      session.current = {
        id,
        pointerId: e.pointerId,
        startX: e.clientX,
        startY: e.clientY,
        startScroll: scrollerRef.current?.scrollTop ?? 0,
        x: e.clientX,
        y: e.clientY,
        started: false,
      };
    },
    onPointerMove: (e: ReactPointerEvent<HTMLElement>) => {
      const s = session.current;
      if (!s || s.pointerId !== e.pointerId) return;
      s.x = e.clientX;
      s.y = e.clientY;
      if (!s.started) {
        if (Math.hypot(s.x - s.startX, s.y - s.startY) < SLOP) return;
        s.started = true;
      }
      setDrag(measure(s));
    },
    onPointerUp: (e: ReactPointerEvent<HTMLElement>) => {
      const s = session.current;
      if (!s || s.pointerId !== e.pointerId) return;
      const view = s.started ? measure(s) : null;
      stop();
      if (view) onDrop(view.id, view.index);
    },
    onPointerCancel: () => stop(),
    onLostPointerCapture: () => {
      if (session.current) stop();
    },
  });

  return {
    drag,
    /** For the pedal's handle. */
    handleProps: (id: string) => dragProps(id, false),
    /** For the pedal's slot: its enclosure, wherever no control is. */
    bodyProps: (id: string) => dragProps(id, true),
  };
}
