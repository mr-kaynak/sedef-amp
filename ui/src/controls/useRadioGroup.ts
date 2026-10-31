// SPDX-License-Identifier: AGPL-3.0-or-later
// Keyboard model of a WAI-ARIA radio group of buttons: one tab stop (the checked option, or the
// first when none is), arrow keys move the selection and the focus together, Home and End jump to
// the ends.
import { useRef } from 'react';
import type { KeyboardEvent } from 'react';

export function useRadioGroup(count: number, selected: number, choose: (index: number) => void) {
  const refs = useRef<(HTMLElement | null)[]>([]);
  const tabStop = selected >= 0 && selected < count ? selected : 0;

  const select = (index: number) => {
    if (index !== selected) choose(index);
  };

  const onKeyDown = (e: KeyboardEvent<HTMLElement>) => {
    const focused = refs.current.findIndex((el) => el !== null && el === document.activeElement);
    const from = focused >= 0 ? focused : tabStop;
    let next: number;
    switch (e.key) {
      case 'ArrowRight':
      case 'ArrowDown':
        next = (from + 1) % count;
        break;
      case 'ArrowLeft':
      case 'ArrowUp':
        next = (from - 1 + count) % count;
        break;
      case 'Home':
        next = 0;
        break;
      case 'End':
        next = count - 1;
        break;
      default:
        return;
    }
    e.preventDefault();
    select(next);
    refs.current[next]?.focus();
  };

  /** Props for the option at index; spread them on its button. */
  const radio = (index: number) => ({
    ref: (el: HTMLElement | null) => {
      refs.current[index] = el;
    },
    role: 'radio' as const,
    'aria-checked': index === selected,
    tabIndex: index === tabStop ? 0 : -1,
    onClick: () => select(index),
  });

  return { onKeyDown, radio };
}
