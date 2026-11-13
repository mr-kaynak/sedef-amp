// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { SectionTabs } from '../../app/SectionTabs';
import { StatusBar } from '../../app/StatusBar';
import { describeTranspose, transposeLatencyMs } from '../../lib/transpose';
import { getValue } from '../../params/paramStore';
import { setParam } from '../../params/useParam';
import { useAppStore } from '../../state/appStore';
import { flush, setupMock, type Harness } from '../../test/harness';

describe('Transpose in the section tab row', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  it('steps a semitone at a time, a host gesture per step, and goes back to 0', async () => {
    const call = vi.spyOn(h.mock, 'call');
    render(<SectionTabs />);
    const group = screen.getByRole('group', { name: 'Transpose' });
    expect(screen.getByRole('status')).toHaveTextContent('off');
    expect(screen.queryByText(/^Adds \d+ ms latency$/)).toBeNull();
    expect(screen.queryByRole('button', { name: 'Back to 0' })).toBeNull();

    const down = screen.getByRole('button', { name: 'Transpose down a semitone' });
    expect(down).toHaveAccessibleDescription(/without retuning/);
    fireEvent.click(down);
    fireEvent.click(down);
    expect(getValue('transpose')).toBe(-2);
    expect(group).toHaveTextContent('−2 st');
    expect(screen.getByRole('status')).toHaveTextContent('2 semitones down');
    expect(group).toHaveTextContent('Adds 3 ms latency');
    await flush();
    expect(h.mock.value('transpose')).toBe(-2);
    const names = call.mock.calls.map(([name]) => name);
    expect(names.slice(0, 3)).toEqual(['sedef_beginGesture', 'sedef_setParam', 'sedef_endGesture']);

    fireEvent.click(screen.getByRole('button', { name: 'Back to 0' }));
    expect(getValue('transpose')).toBe(0);
    expect(screen.queryByRole('button', { name: 'Back to 0' })).toBeNull();
    // The tabs stay tabs: the group is not inside the tab list.
    expect(screen.getByRole('tablist')).not.toContainElement(down);
  });

  it('stops at an octave either way', () => {
    render(<SectionTabs />);
    const up = screen.getByRole('button', { name: 'Transpose up a semitone' });
    for (let i = 0; i < 14; i += 1) fireEvent.click(up);
    expect(getValue('transpose')).toBe(12);
    expect(up).toBeDisabled();
    expect(screen.getByRole('status')).toHaveTextContent('an octave up');
    expect(screen.getByRole('group', { name: 'Transpose' })).toHaveTextContent(
      'Adds 10 ms latency',
    );
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
