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

  it('switches amps and shows the matching faceplate', async () => {
    render(<App />);
    fireEvent.click(screen.getByRole('radio', { name: /Abyss/ }));
    expect(screen.getByLabelText('Abyss amp controls')).toBeInTheDocument();
    await act(flush);
    expect(h.mock.value('ampModel')).toBe(2);
  });

  it('loads a preset from the browser', async () => {
    render(<App />);
    fireEvent.click(screen.getByRole('button', { name: /Open preset browser/ }));
    fireEvent.click(screen.getByRole('button', { name: /^Glass Pocket/ }));
    await act(flush);
    expect(screen.getByRole('button', { name: /Preset: Glass Pocket/ })).toBeInTheDocument();
  });
});
