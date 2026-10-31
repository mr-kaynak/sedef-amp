// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { SetupStep } from '../features/intonation/SetupStep';
import { useIntonationStore } from '../features/intonation/store';
import { SettingsPanel } from '../features/settings/SettingsPanel';
import { useUiStore } from '../state/uiStore';
import { setupMock, type Harness } from '../test/harness';

describe('radio groups', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock();
  });
  afterEach(() => h.dispose());

  const tabStops = (group: HTMLElement) =>
    within(group)
      .getAllByRole('radio')
      .filter((r) => r.tabIndex === 0);

  it('makes the window size one tab stop that arrow keys move through', () => {
    render(<SettingsPanel />);
    const group = screen.getByRole('radiogroup', { name: 'Window size' });
    expect(tabStops(group)).toEqual([within(group).getByRole('radio', { name: '100%' })]);

    within(group).getByRole('radio', { name: '100%' }).focus();
    fireEvent.keyDown(document.activeElement!, { key: 'ArrowRight' });
    const next = within(group).getByRole('radio', { name: '125%' });
    expect(next).toHaveFocus();
    expect(next).toHaveAttribute('aria-checked', 'true');
    expect(useUiStore.getState().scale).toBe(1.25);
    fireEvent.keyDown(next, { key: 'Home' });
    expect(useUiStore.getState().scale).toBe(0.75);
  });

  it('moves through the instrument and scale choices with the arrow keys', () => {
    render(<SetupStep />);
    const instruments = screen.getByRole('radiogroup', { name: 'Instrument' });
    expect(tabStops(instruments)).toHaveLength(1);
    tabStops(instruments)[0]!.focus();
    fireEvent.keyDown(document.activeElement!, { key: 'ArrowDown' });
    expect(useIntonationStore.getState().instrumentId).not.toBe('gtr6');
    expect(document.activeElement).toHaveAttribute('aria-checked', 'true');

    const scales = screen.getByRole('radiogroup', { name: 'Scale length' });
    fireEvent.keyDown(tabStops(scales)[0]!, { key: 'End' });
    expect(useIntonationStore.getState().scaleId).toBe('custom');
    expect(screen.getByRole('spinbutton')).toBeInTheDocument();
  });
});
