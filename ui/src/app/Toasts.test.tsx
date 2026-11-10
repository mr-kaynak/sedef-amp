// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { dismissToastRestoring, showToast, useToastStore } from '../state/toastStore';
import { Toasts } from './Toasts';

const undo = { label: 'Undo', run: () => undefined };
const text = () => useToastStore.getState().toast?.message ?? null;

describe('toast store', () => {
  afterEach(() => useToastStore.setState({ toast: null }));

  it('never lets a plain notice push an Undo off the screen', () => {
    showToast('Deleted Sunday set', undo);
    expect(showToast('Back to Glassy Chorus')).toBeNull();
    expect(text()).toBe('Deleted Sunday set');
    showToast('Loaded Singing Echo', undo, 'browse');
    expect(text()).toBe('Loaded Singing Echo');
  });

  it('drops only the toasts whose snapshot went away', () => {
    showToast('Deleted Sunday set', undo);
    dismissToastRestoring('browse', 'undo');
    expect(text()).toBe('Deleted Sunday set');
    showToast('Loaded Singing Echo', undo, 'undo');
    dismissToastRestoring('browse');
    expect(text()).toBe('Loaded Singing Echo');
    dismissToastRestoring('undo');
    expect(text()).toBeNull();
  });
});

describe('Toasts', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => {
    cleanup();
    vi.useRealTimers();
    useToastStore.setState({ toast: null });
  });

  it('holds a toast while pointed at, and the next one runs its own timer', () => {
    render(<Toasts />);
    act(() => void showToast('Loaded Singing Echo', undo));
    fireEvent.pointerEnter(screen.getByText('Loaded Singing Echo').parentElement as HTMLElement);
    act(() => vi.advanceTimersByTime(20_000));
    expect(text()).toBe('Loaded Singing Echo');

    // Replaced under the pointer: no pointerleave comes for the old one.
    act(() => void showToast('Deleted Sunday set', undo));
    act(() => vi.advanceTimersByTime(8_000));
    expect(text()).toBeNull();
  });
});
