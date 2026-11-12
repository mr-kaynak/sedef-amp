// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { flush, setupMock, type Harness } from '../test/harness';
import { useUiStore } from '../state/uiStore';
import { App } from './App';

describe('App', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock();
  });
  afterEach(() => h.dispose());

  it('renders every section from the keyboard without errors', () => {
    const errors = vi.spyOn(console, 'error');
    render(<App />);
    const expectations: [string, RegExp][] = [
      ['1', /Tide Compressor/],
      ['2', /Lagoon amp controls/],
      ['3', /EQ frequency response/],
      ['4', /Mic A/],
      ['5', /Drift Tape echo/],
    ];
    for (const [key, label] of expectations) {
      fireEvent.keyDown(window, { key });
      expect(screen.getAllByLabelText(label).length).toBeGreaterThan(0);
    }
    expect(errors).not.toHaveBeenCalled();
  });

  it('opens the tuner, starts analysis, and closes with Escape', async () => {
    render(<App />);
    fireEvent.click(screen.getByRole('button', { name: 'Tuner' }));
    expect(screen.getByRole('region', { name: 'Tuner' })).toBeInTheDocument();
    await act(flush);
    expect(h.mock.isTunerActive()).toBe(true);
    fireEvent.keyDown(window, { key: 'Escape' });
    expect(useUiStore.getState().overlay).toBeNull();
    await act(flush);
    expect(h.mock.isTunerActive()).toBe(false);
  });

  it('opens and closes the tuner with T, but not while typing', async () => {
    render(<App />);
    expect(screen.getByRole('button', { name: 'Tuner' })).toHaveAttribute('aria-keyshortcuts', 'T');
    fireEvent.keyDown(window, { key: 't' });
    expect(useUiStore.getState().overlay).toBe('tuner');
    fireEvent.keyDown(window, { key: 'T' });
    expect(useUiStore.getState().overlay).toBeNull();
    // In the preset search, T is a letter.
    fireEvent.keyDown(window, { key: '/' });
    await act(flush);
    const search = screen.getByRole('combobox', { name: 'Search presets' });
    fireEvent.keyDown(search, { key: 't' });
    expect(useUiStore.getState().overlay).toBeNull();
  });

  it('switches amps and shows the matching faceplate', async () => {
    render(<App />);
    fireEvent.click(screen.getByRole('radio', { name: /Abyss/ }));
    expect(screen.getByLabelText('Abyss amp controls')).toBeInTheDocument();
    await act(flush);
    expect(h.mock.value('ampModel')).toBe(2);
  });

  it('auditions a preset in the browser over the rig and keeps it', async () => {
    render(<App />);
    fireEvent.click(screen.getByRole('button', { name: /Open preset browser/ }));
    await act(flush);
    // The rig behind the browser is out of reach until it closes.
    expect(screen.getByRole('tablist', { name: 'Rig sections' }).closest('[inert]')).not.toBeNull();
    fireEvent.click(screen.getByRole('option', { name: /^Funk Compressor/ }));
    await act(flush);
    expect(screen.getByRole('button', { name: /Preset: Funk Compressor/ })).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: /^Use & close/ }));
    await act(flush);
    expect(screen.queryByRole('dialog', { name: 'Preset browser' })).toBeNull();
    expect(screen.getByRole('tablist', { name: 'Rig sections' }).closest('[inert]')).toBeNull();
    expect(h.mock.value('tideEnabled')).toBe(1);
  });
});
