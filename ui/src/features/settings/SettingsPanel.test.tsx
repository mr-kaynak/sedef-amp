// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { useUiStore } from '../../state/uiStore';
import { setupMock, type Harness } from '../../test/harness';
import { SettingsPanel } from './SettingsPanel';

describe('Settings panel', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock();
  });
  afterEach(() => h.dispose());

  it('offers the window size as a segmented radio group with one tab stop', () => {
    const call = vi.spyOn(h.mock, 'call');
    render(<SettingsPanel />);

    // The same control as the stereo-input choice under it: the shared Segmented classes.
    const group = screen.getByRole('radiogroup', { name: 'Window size' });
    expect(group).toHaveClass('segmented');
    const options = within(group).getAllByRole('radio');
    expect(options.map((o) => o.textContent)).toEqual(['75%', '100%', '125%', '150%', '200%']);
    for (const o of options) expect(o).toHaveClass('option');
    expect(options.map((o) => o.getAttribute('aria-checked'))).toEqual([
      'false',
      'true',
      'false',
      'false',
      'false',
    ]);
    expect(options.map((o) => o.tabIndex)).toEqual([-1, 0, -1, -1, -1]);

    // Arrow keys move the choice and the focus together; the plugin resizes its window.
    options[1]!.focus();
    fireEvent.keyDown(options[1]!, { key: 'ArrowRight' });
    const larger = screen.getByRole('radio', { name: '125%' });
    expect(larger).toHaveAttribute('aria-checked', 'true');
    expect(larger).toHaveFocus();
    expect(larger.tabIndex).toBe(0);
    expect(useUiStore.getState().scale).toBe(1.25);
    expect(call).toHaveBeenCalledWith('sedef_ui_setScale', 1.25);

    fireEvent.keyDown(larger, { key: 'Home' });
    expect(screen.getByRole('radio', { name: '75%' })).toHaveFocus();
    expect(useUiStore.getState().scale).toBe(0.75);

    fireEvent.click(screen.getByRole('radio', { name: '200%' }));
    expect(useUiStore.getState().scale).toBe(2);
    expect(call).toHaveBeenLastCalledWith('sedef_ui_setScale', 2);
  });
});
