// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect, useRef } from 'react';
import type { CSSProperties, ReactNode, RefObject } from 'react';
import styles from './Popover.module.css';

export interface PopoverProps {
  open: boolean;
  onClose: () => void;
  /** The element that toggles the popover; clicks on it are not "outside". */
  anchorRef: RefObject<HTMLElement | null>;
  label: string;
  style?: CSSProperties;
  className?: string;
  children: ReactNode;
}

/** Dialog-like panel anchored in the canvas. Closes on Escape and outside clicks. */
export function Popover({
  open,
  onClose,
  anchorRef,
  label,
  style,
  className,
  children,
}: PopoverProps) {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    const panel = ref.current;
    const anchor = anchorRef.current;
    const first =
      panel?.querySelector<HTMLElement>('[data-autofocus]') ??
      panel?.querySelector<HTMLElement>(
        'input:not(:disabled), button:not(:disabled), [tabindex="0"]',
      );
    first?.focus({ preventScroll: true });

    const onPointerDown = (e: PointerEvent) => {
      const target = e.target as Node;
      if (panel?.contains(target) || anchor?.contains(target)) return;
      onClose();
    };
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return;
      e.stopPropagation();
      onClose();
      anchor?.focus({ preventScroll: true });
    };
    document.addEventListener('pointerdown', onPointerDown, true);
    document.addEventListener('keydown', onKeyDown, true);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown, true);
      document.removeEventListener('keydown', onKeyDown, true);
    };
  }, [open, onClose, anchorRef]);

  if (!open) return null;
  return (
    <div
      ref={ref}
      role="dialog"
      aria-label={label}
      className={[styles.popover, className].filter(Boolean).join(' ')}
      style={style}
    >
      {children}
    </div>
  );
}
