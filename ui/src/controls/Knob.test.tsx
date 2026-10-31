// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { getValue } from '../params/paramStore';
import { presetActions } from '../state/presetStore';
import { flush, setupMock, type Harness } from '../test/harness';
import { Knob } from './Knob';

describe('Knob', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => {
    h.dispose();
    vi.useRealTimers();
  });

  const knob = () => screen.getByRole('slider', { name: 'Pearl Gain' });
  const gestureCalls = (call: { mock: { calls: unknown[][] } }) =>
    call.mock.calls
      .filter(([name]) => String(name).includes('Gesture'))
      .map(([name, id]) => `${String(name).replace('sedef_', '')} ${String(id)}`);

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

  it('ends the gesture of a drag cut short by unmounting', () => {
    const call = vi.spyOn(h.mock, 'call');
    const { unmount } = render(<Knob param="pearlGain" />);
    fireEvent.pointerDown(knob(), { button: 0, clientY: 300, pointerId: 1 });
    unmount();
    expect(gestureCalls(call)).toEqual(['beginGesture pearlGain', 'endGesture pearlGain']);
  });

  it('ends the gesture of the parameter a drag began on when the knob is rebound', () => {
    const call = vi.spyOn(h.mock, 'call');
    const { rerender } = render(<Knob param="pearlGain" />);
    fireEvent.pointerDown(knob(), { button: 0, clientY: 300, pointerId: 1 });
    rerender(<Knob param="pearlLevel" />);
    expect(gestureCalls(call)).toEqual(['beginGesture pearlGain', 'endGesture pearlGain']);
    const level = getValue('pearlLevel');
    fireEvent.pointerMove(screen.getByRole('slider', { name: 'Pearl Level' }), {
      clientY: 200,
      pointerId: 1,
    });
    expect(getValue('pearlLevel')).toBe(level);
  });

  it('ends a drag when the pointer capture is lost', () => {
    const call = vi.spyOn(h.mock, 'call');
    render(<Knob param="pearlGain" />);
    fireEvent.pointerDown(knob(), { button: 0, clientY: 300, pointerId: 1 });
    fireEvent.lostPointerCapture(knob(), { pointerId: 1 });
    expect(gestureCalls(call)).toEqual(['beginGesture pearlGain', 'endGesture pearlGain']);
  });

  it('keeps one host gesture open while a wheel burst and a drag overlap', () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'performance'] });
    const call = vi.spyOn(h.mock, 'call');
    render(<Knob param="pearlGain" />);
    fireEvent.wheel(knob(), { deltaY: -100 });
    fireEvent.pointerDown(knob(), { button: 0, clientY: 300, pointerId: 1 });
    act(() => vi.advanceTimersByTime(500)); // the wheel burst ends mid-drag
    fireEvent.keyDown(knob(), { key: 'ArrowUp' });
    expect(gestureCalls(call)).toEqual(['beginGesture pearlGain']);
    fireEvent.pointerUp(knob(), { pointerId: 1 });
    expect(gestureCalls(call)).toEqual(['beginGesture pearlGain', 'endGesture pearlGain']);
  });

  it('turns with the wheel by the distance scrolled', () => {
    render(<Knob param="pearlGain" />);
    fireEvent.wheel(knob(), { deltaY: -50 });
    expect(getValue('pearlGain')).toBeCloseTo(0.51);
    // A trackpad swipe: many small deltas add up instead of 2 % each.
    for (let i = 0; i < 20; i += 1) fireEvent.wheel(knob(), { deltaY: -2 });
    expect(getValue('pearlGain')).toBeCloseTo(0.518);
    // Line mode (a mouse in some browsers): 3 lines is one 100 px notch.
    fireEvent.wheel(knob(), { deltaY: 3, deltaMode: 1 });
    expect(getValue('pearlGain')).toBeCloseTo(0.498);
    fireEvent.wheel(knob(), { deltaY: -100, shiftKey: true });
    expect(getValue('pearlGain')).toBeCloseTo(0.502);
  });

  it('collects small wheel deltas into whole steps on stepped parameters', () => {
    render(<Knob param="driftNote" />);
    const k = screen.getByRole('slider', { name: 'Drift Note' });
    const start = getValue('driftNote');
    // However small, the first event of a burst moves one step.
    fireEvent.wheel(k, { deltaY: -4 });
    expect(getValue('driftNote')).toBe(start + 1);
    for (let i = 0; i < 24; i += 1) fireEvent.wheel(k, { deltaY: -4 });
    expect(getValue('driftNote')).toBe(start + 1);
    fireEvent.wheel(k, { deltaY: -4 });
    fireEvent.wheel(k, { deltaY: -4 });
    expect(getValue('driftNote')).toBe(start + 2);
  });

  it('picks up a preset loaded right after a wheel turn', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'performance'] });
    render(<Knob param="pearlGain" />);
    fireEvent.wheel(knob(), { deltaY: -100 });
    await act(flush);
    await act(() => presetActions.step(1));
    const loaded = h.mock.value('pearlGain');
    expect(loaded).not.toBeCloseTo(getValue('pearlGain'));
    act(() => vi.advanceTimersByTime(700));
    expect(getValue('pearlGain')).toBe(loaded);
    expect(knob()).toHaveAttribute('aria-valuenow', String(Number(loaded.toFixed(3))));
  });
});
