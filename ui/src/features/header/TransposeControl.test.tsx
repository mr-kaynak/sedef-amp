// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { StatusBar } from '../../app/StatusBar';
import { describeTranspose, transposeLatencyMs } from '../../lib/transpose';
import { getValue } from '../../params/paramStore';
import { setParam } from '../../params/useParam';
import { useAppStore } from '../../state/appStore';
import { flush, setupMock, type Harness } from '../../test/harness';
import { Header } from './Header';

describe('Transpose in the header', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  it('shows the setting on the chip and steps it in its panel, a host gesture per step', async () => {
    const call = vi.spyOn(h.mock, 'call');
    render(<Header />);
    const chip = screen.getByRole('button', { name: 'Transpose, off' });
    expect(chip).toHaveTextContent(/^Transpose$/);
    fireEvent.click(chip);
    const panel = screen.getByRole('dialog', { name: 'Transpose' });
    expect(panel).toHaveTextContent('0 st');
    // Focus moves into the panel, to its first button.
    expect(screen.getByRole('button', { name: 'Transpose down a semitone' })).toHaveFocus();

    fireEvent.click(screen.getByRole('button', { name: 'Transpose down a semitone' }));
    fireEvent.click(screen.getByRole('button', { name: 'Transpose down a semitone' }));
    expect(getValue('transpose')).toBe(-2);
    expect(screen.getByRole('button', { name: 'Transpose, 2 semitones down' })).toHaveTextContent(
      'Transpose−2',
    );
    expect(panel).toHaveTextContent('Adds 3 ms of latency while it shifts down.');
    await flush();
    expect(h.mock.value('transpose')).toBe(-2);
    const names = call.mock.calls.map(([name]) => name);
    expect(names.slice(0, 3)).toEqual(['sedef_beginGesture', 'sedef_setParam', 'sedef_endGesture']);

    fireEvent.click(screen.getByRole('button', { name: 'Back to 0' }));
    expect(getValue('transpose')).toBe(0);
    expect(screen.queryByRole('button', { name: 'Back to 0' })).toBeNull();
    expect(panel).toHaveTextContent('Shifting down adds 3 ms of latency, shifting up 10 ms.');
  });

  it('stops at an octave either way', () => {
    render(<Header />);
    fireEvent.click(screen.getByRole('button', { name: /^Transpose,/ }));
    const up = screen.getByRole('button', { name: 'Transpose up a semitone' });
    for (let i = 0; i < 14; i += 1) fireEvent.click(up);
    expect(getValue('transpose')).toBe(12);
    expect(up).toBeDisabled();
    expect(screen.getByRole('status')).toHaveTextContent('an octave up');
    expect(screen.getByRole('dialog', { name: 'Transpose' })).toHaveTextContent(
      'Adds 10 ms of latency while it shifts up.',
    );
    expect(screen.getByRole('button', { name: 'Transpose, an octave up' })).toBeInTheDocument();
  });

  it('closes its panel with Escape and gives focus back to the chip', () => {
    render(<Header />);
    const chip = screen.getByRole('button', { name: /^Transpose,/ });
    fireEvent.click(chip);
    expect(screen.getByRole('dialog', { name: 'Transpose' })).toBeInTheDocument();
    fireEvent.keyDown(document, { key: 'Escape' });
    expect(screen.queryByRole('dialog', { name: 'Transpose' })).toBeNull();
    expect(chip).toHaveFocus();
  });

  it('names its share of the latency in the status bar while it shifts', async () => {
    render(<StatusBar />);
    expect(screen.getByText(/ms latency$/)).toBeInTheDocument();
    act(() => setParam('transpose', 5));
    await flush();
    expect(useAppStore.getState().session.latencySamples).toBe(41 + 480);
    expect(screen.getByText('10.9 ms latency (Transpose adds 10 ms)')).toBeInTheDocument();
  });
});

describe('Transpose wording', () => {
  it('says the shift and the latency it adds in plain words', () => {
    expect(describeTranspose(0)).toBe('off');
    expect(describeTranspose(-1)).toBe('1 semitone down');
    expect(describeTranspose(5)).toBe('5 semitones up');
    expect(describeTranspose(-12)).toBe('an octave down');
    expect(transposeLatencyMs(0)).toBe(0);
    expect(transposeLatencyMs(-7)).toBe(3);
    expect(transposeLatencyMs(1)).toBe(10);
  });
});
