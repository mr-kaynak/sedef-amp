// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { getValue } from '../params/paramStore';
import { flush, setupMock, type Harness } from '../test/harness';
import { Knob } from './Knob';

describe('Knob', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  const knob = () => screen.getByRole('slider', { name: 'Pearl Gain' });

  it('exposes slider semantics with real values and readable text', () => {
    render(<Knob param="pearlGain" />);
    const k = knob();
    expect(k).toHaveAttribute('aria-valuemin', '0');
    expect(k).toHaveAttribute('aria-valuemax', '1');
    expect(k).toHaveAttribute('aria-valuenow', '0.5');
    expect(k).toHaveAttribute('aria-valuetext', '5.0');
    expect(screen.getByText('Gain')).toBeInTheDocument();
  });

  it('steps with the keyboard inside a host gesture', async () => {
    const call = vi.spyOn(h.mock, 'call');
    render(<Knob param="pearlGain" />);
    fireEvent.keyDown(knob(), { key: 'ArrowUp' });
    expect(getValue('pearlGain')).toBeCloseTo(0.51);
    fireEvent.keyDown(knob(), { key: 'PageDown' });
    expect(getValue('pearlGain')).toBeCloseTo(0.41);
    fireEvent.keyDown(knob(), { key: 'End' });
    expect(getValue('pearlGain')).toBe(1);
    fireEvent.keyDown(knob(), { key: 'Home' });
    expect(getValue('pearlGain')).toBe(0);
    await flush();
    const names = call.mock.calls.map(([name]) => name);
    expect(names.slice(0, 3)).toEqual(['sedef_beginGesture', 'sedef_setParam', 'sedef_endGesture']);
    expect(h.mock.value('pearlGain')).toBe(0);
  });

  it('follows vertical drags, finer with shift', async () => {
    render(<Knob param="pearlGain" />);
    const k = knob();
    fireEvent.pointerDown(k, { button: 0, clientY: 300, pointerId: 1 });
    fireEvent.pointerMove(k, { clientY: 250, pointerId: 1 });
    expect(getValue('pearlGain')).toBeCloseTo(0.75);
    fireEvent.pointerMove(k, { clientY: 190, pointerId: 1, shiftKey: true });
    expect(getValue('pearlGain')).toBeCloseTo(0.8);
    expect(screen.getAllByText('8.0').length).toBeGreaterThan(0);
    fireEvent.pointerUp(k, { pointerId: 1 });
    await flush();
    expect(h.mock.value('pearlGain')).toBeCloseTo(0.8);
  });

  it('resets to the default on double-click', async () => {
    render(<Knob param="pearlGain" />);
    fireEvent.keyDown(knob(), { key: 'End' });
    fireEvent.doubleClick(knob());
    expect(getValue('pearlGain')).toBe(0.5);
    await act(flush);
    expect(h.mock.value('pearlGain')).toBe(0.5);
  });

  it('walks stepped parameters one choice at a time', () => {
    render(<Knob param="driftNote" />);
    const k = screen.getByRole('slider', { name: 'Drift Note' });
    expect(k).toHaveAttribute('aria-valuetext', '1/4');
    fireEvent.keyDown(k, { key: 'ArrowUp' });
    expect(k).toHaveAttribute('aria-valuetext', '1/2T');
  });
});
