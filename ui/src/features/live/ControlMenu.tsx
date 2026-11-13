// SPDX-License-Identifier: AGPL-3.0-or-later
// The menu a control opens on right-click, the context-menu key or Shift+F10 (docs/live.md §3):
// learn or forget its MIDI controller, keep it per scene, copy the scene in use into a scene.
// Controls take part by carrying data-param="<id>" (a parameter) or data-learn="<target>" (a
// scene button, a preset arrow); nothing else is touched, so text fields keep their own menu.
import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import type { KeyboardEvent as ReactKeyboardEvent } from 'react';
import { CANVAS_HEIGHT, CANVAS_WIDTH } from '../../app/Stage';
import { useParamStore } from '../../params/paramStore';
import { useLiveStore } from '../../state/liveStore';
import { menuItems, targetOf } from './menu';
import { parseTarget, targetLabel } from './model';
import styles from './ControlMenu.module.css';

const SELECTOR = '[data-param], [data-learn]';
/** Keeps the menu this far inside the canvas edges. */
const EDGE = 8;

interface Open {
  target: string;
  x: number;
  y: number;
  invoker: HTMLElement;
}

/** Canvas coordinates of a client point: the stage scales the canvas to the window. */
function toCanvas(el: HTMLElement, clientX: number, clientY: number): { x: number; y: number } {
  const canvas = el.closest<HTMLElement>('[data-canvas]');
  if (!canvas) return { x: clientX, y: clientY };
  const r = canvas.getBoundingClientRect();
  const scale = r.width > 0 ? r.width / CANVAS_WIDTH : 1;
  return { x: (clientX - r.left) / scale, y: (clientY - r.top) / scale };
}

export function ControlMenu() {
  const [open, setOpen] = useState<Open | null>(null);
  const state = useLiveStore((s) => s.state);
  const specs = useParamStore((s) => s.specs);
  const ref = useRef<HTMLDivElement>(null);
  const [place, setPlace] = useState<{ left: number; top: number } | null>(null);

  const invoker = useRef<HTMLElement | null>(null);
  useEffect(() => {
    invoker.current = open?.invoker ?? null;
  }, [open]);

  const close = useCallback((refocus: boolean) => {
    setOpen(null);
    setPlace(null);
    if (refocus) invoker.current?.focus({ preventScroll: true });
  }, []);

  // Opening: a right-click on a control, or the keyboard on the focused one.
  const supported = state !== null;
  useEffect(() => {
    if (!supported) return;
    const show = (el: HTMLElement, clientX: number, clientY: number) => {
      const target = targetOf(el);
      if (!target || !parseTarget(target)) return false;
      const { x, y } = toCanvas(el, clientX, clientY);
      setPlace(null);
      setOpen({ target, x, y, invoker: el });
      return true;
    };
    const fromKeyboard = (el: HTMLElement) => {
      const r = el.getBoundingClientRect();
      return show(el, r.left, r.bottom + 4);
    };
    const onContextMenu = (e: MouseEvent) => {
      const el = (e.target as Element | null)?.closest<HTMLElement>(SELECTOR);
      if (!el || el.closest('[inert]')) return;
      // A keyboard-made event (the context-menu key) has no pointer: open under the control.
      const shown = e.button === 2 ? show(el, e.clientX, e.clientY) : fromKeyboard(el);
      if (shown) e.preventDefault();
    };
    const onKeyDown = (e: KeyboardEvent) => {
      if (!(e.key === 'F10' && e.shiftKey)) return;
      const el = (document.activeElement as Element | null)?.closest<HTMLElement>(SELECTOR);
      if (el && fromKeyboard(el)) e.preventDefault();
    };
    document.addEventListener('contextmenu', onContextMenu);
    document.addEventListener('keydown', onKeyDown);
    return () => {
      document.removeEventListener('contextmenu', onContextMenu);
      document.removeEventListener('keydown', onKeyDown);
    };
  }, [supported]);

  // Placing: inside the canvas, flipped above or to the left near the edges; then focus.
  useLayoutEffect(() => {
    const menu = ref.current;
    if (!open || !menu) return;
    const w = menu.offsetWidth;
    const h = menu.offsetHeight;
    const left = open.x + w + EDGE > CANVAS_WIDTH ? Math.max(EDGE, open.x - w) : open.x;
    const top = open.y + h + EDGE > CANVAS_HEIGHT ? Math.max(EDGE, open.y - h) : open.y;
    setPlace({ left, top });
    menu.querySelector<HTMLElement>('[role="menuitem"]:not([aria-disabled="true"])')?.focus({
      preventScroll: true,
    });
  }, [open]);

  // Closing: a click elsewhere, the window losing focus or resizing.
  useEffect(() => {
    if (!open) return;
    const onPointerDown = (e: PointerEvent) => {
      if (!ref.current?.contains(e.target as Node)) close(false);
    };
    const onBlur = () => close(false);
    document.addEventListener('pointerdown', onPointerDown, true);
    window.addEventListener('blur', onBlur);
    window.addEventListener('resize', onBlur);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown, true);
      window.removeEventListener('blur', onBlur);
      window.removeEventListener('resize', onBlur);
    };
  }, [open, close]);

  if (!open || !state) return null;
  const items = menuItems(open.target, state, specs);
  const label = targetLabel(open.target, specs);

  const onKeyDown = (e: ReactKeyboardEvent<HTMLDivElement>) => {
    const all = [...(ref.current?.querySelectorAll<HTMLElement>('[role="menuitem"]') ?? [])];
    const enabled = all.filter((el) => el.getAttribute('aria-disabled') !== 'true');
    const at = enabled.indexOf(document.activeElement as HTMLElement);
    let next: number;
    switch (e.key) {
      case 'ArrowDown':
        next = (at + 1) % enabled.length;
        break;
      case 'ArrowUp':
        next = (at - 1 + enabled.length) % enabled.length;
        break;
      case 'Home':
        next = 0;
        break;
      case 'End':
        next = enabled.length - 1;
        break;
      case 'Escape':
        e.preventDefault();
        e.stopPropagation();
        close(true);
        return;
      case 'Tab':
        e.preventDefault();
        close(true);
        return;
      default:
        return;
    }
    e.preventDefault();
    enabled[next]?.focus();
  };

  return (
    <div
      ref={ref}
      role="menu"
      aria-label={`${label}: MIDI and scenes`}
      className={styles.menu}
      style={{
        left: place?.left ?? open.x,
        // Placed by the layout effect before the first paint: no hidden pass (a hidden menu
        // could not take focus).
        top: place?.top ?? open.y,
      }}
      onKeyDown={onKeyDown}
    >
      <p className={styles.title} aria-hidden="true">
        {label}
      </p>
      {items.map((item) => (
        <button
          key={item.label}
          type="button"
          role="menuitem"
          tabIndex={-1}
          aria-disabled={item.disabled || undefined}
          className={styles.item}
          onPointerEnter={(e) => {
            if (!item.disabled) e.currentTarget.focus({ preventScroll: true });
          }}
          onClick={() => {
            if (item.disabled) return;
            close(true);
            item.run();
          }}
        >
          {item.label}
        </button>
      ))}
    </div>
  );
}
