// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { presetActions, usePresetStore } from '../../state/presetStore';
import { flush, setupMock, type Harness } from '../../test/harness';
import { PresetBrowser } from './PresetBrowser';

const SUNDAY = 'user:Live/Sunday set.sedefpreset';

describe('PresetBrowser', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock();
  });
  afterEach(() => h.dispose());

  it('keeps focus inside the browser when it switches between the list and a form', () => {
    render(<PresetBrowser onDone={() => undefined} />);
    const saveAs = screen.getByRole('button', { name: 'Save as…' });
    saveAs.focus();
    fireEvent.click(saveAs);
    expect(screen.getByRole('textbox', { name: 'Name' })).toHaveFocus();
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(screen.getByRole('button', { name: 'Save as…' })).toHaveFocus();
  });

  it('refuses a rename onto another preset in the same folder, and says why', async () => {
    await act(async () => {
      await h.mock.call('sedef_presets_save', 'Encore', 'Live');
      await presetActions.load(SUNDAY);
      await flush();
    });
    const rename = vi.spyOn(presetActions, 'rename');
    render(<PresetBrowser onDone={() => undefined} />);
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
    const input = screen.getByRole('textbox', { name: 'New name' });
    expect(input).toHaveFocus();

    fireEvent.change(input, { target: { value: 'encore' } });
    expect(screen.getByRole('alert')).toHaveTextContent('already called “encore”');
    expect(input).toHaveAttribute('aria-invalid', 'true');
    fireEvent.submit(input);
    expect(rename).not.toHaveBeenCalled();
    expect(screen.getByRole('textbox', { name: 'New name' })).toBeInTheDocument();

    fireEvent.change(input, { target: { value: 'Sunday best' } });
    expect(screen.queryByRole('alert')).toBeNull();
    fireEvent.submit(input);
    await act(flush);
    expect(rename).toHaveBeenCalledWith(SUNDAY, 'Sunday best');
    expect(usePresetStore.getState().state.currentName).toBe('Sunday best');
  });
});
