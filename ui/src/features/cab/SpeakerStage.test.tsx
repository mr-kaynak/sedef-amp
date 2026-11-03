// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { getValue } from '../../params/paramStore';
import { setParam } from '../../params/useParam';
import { flush, setupMock, type Harness } from '../../test/harness';
import { MicStrip } from './MicStrip';
import { clockLabel, CONE_R, markerPoint, placementAt } from './placement';
import { SpeakerStage } from './SpeakerStage';
const set = (id: string, value: number) => act(() => setParam(id, value));

describe('stage geometry', () => {
  it('turns position and azimuth into a point and back', () => {
    for (const [position, azimuth] of [
      [0.5, 60],
      [1, -120],
      [0.25, 179],
      [0.8, 0],
    ] as const) {
      const p = markerPoint(position, azimuth);
      const back = placementAt(p.x, p.y, 0);
      expect(back.position).toBeCloseTo(position, 6);
      expect(back.azimuth).toBeCloseTo(azimuth, 6);
    }
    // 12 o'clock is up, 3 o'clock to the right (SVG y grows downwards).
    expect(markerPoint(1, 0)).toEqual({ x: 200, y: 200 - CONE_R });
    expect(markerPoint(1, 90).x).toBeCloseTo(200 + CONE_R, 9);
  });

  it('clamps to the cone and keeps the azimuth at the dust cap', () => {
    expect(placementAt(200 + 3 * CONE_R, 200, 0)).toEqual({ position: 1, azimuth: 90 });
    expect(placementAt(200, 200, 42)).toEqual({ position: 0, azimuth: 42 });
  });

  it('reads azimuths as clock positions', () => {
    expect(clockLabel(60)).toBe("2 o'clock");
    expect(clockLabel(120)).toBe("4 o'clock");
    expect(clockLabel(0)).toBe("12 o'clock");
    expect(clockLabel(-90)).toBe("9 o'clock");
    expect(clockLabel(-170)).toBe("6 o'clock");
  });
});

describe('SpeakerStage', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
    // jsdom has no layout: give the stage its design size, so screen pixels are SVG units.
    vi.spyOn(Element.prototype, 'getBoundingClientRect').mockReturnValue({
      x: 0,
      y: 0,
      left: 0,
      top: 0,
      right: 400,
      bottom: 400,
      width: 400,
      height: 400,
      toJSON: () => ({}),
    });
  });
  afterEach(() => {
    vi.restoreAllMocks();
    vi.useRealTimers();
    h.dispose();
  });

  const marker = (mic: 'A' | 'B') => screen.getByRole('slider', { name: `Mic ${mic} placement` });
  const svg = () => (marker('A') as unknown as SVGGElement).ownerSVGElement as SVGSVGElement;
  const pad = () => svg().querySelector('circle[data-wheel]') as SVGCircleElement;
  const gestures = (call: { mock: { calls: unknown[][] } }) =>
    call.mock.calls
      .filter(([name]) => String(name).includes('Gesture'))
      .map(([name, id]) => `${String(name).replace('sedef_', '')} ${String(id)}`);
  const grab = (mic: 'A' | 'B', pointerId: number, extra: object = {}) => {
    const at = markerPoint(getValue(`mic${mic}Position`), getValue(`mic${mic}Azimuth`));
    fireEvent.pointerDown(marker(mic), {
      button: 0,
      clientX: at.x,
      clientY: at.y,
      pointerId,
      ...extra,
    });
    return at;
  };

  it('starts where v0.2.0 drew the markers and says where they are', () => {
    render(<SpeakerStage cab={2} cabLabel="4x12" />);
    expect(getValue('micAAzimuth')).toBe(60);
    expect(getValue('micBAzimuth')).toBe(120);
    expect(marker('A')).toHaveAttribute(
      'aria-valuetext',
      "2 o'clock, position 5.0, distance 0.0, on axis",
    );
    expect(marker('B')).toHaveAttribute(
      'aria-valuetext',
      "4 o'clock, position 5.0, distance 0.0, on axis",
    );
    expect(marker('A')).toHaveAccessibleDescription(/Arrow keys move the mic over the cone/);
  });

  it('drags a mic anywhere over the cone in one gesture per parameter', async () => {
    const call = vi.spyOn(h.mock, 'call');
    render(<SpeakerStage cab={2} cabLabel="4x12" />);
    grab('A', 1);
    // To 8 o'clock, three quarters of the way out: across the cone, not along a line.
    const target = markerPoint(0.75, -120);
    fireEvent.pointerMove(svg(), { clientX: 250, clientY: 300, pointerId: 1 });
    fireEvent.pointerMove(svg(), { clientX: target.x, clientY: target.y, pointerId: 1 });
    expect(getValue('micAPosition')).toBeCloseTo(0.75, 5);
    expect(getValue('micAAzimuth')).toBeCloseTo(-120, 4);
    expect(getValue('micADistance')).toBe(0);
    fireEvent.pointerUp(svg(), { pointerId: 1 });
    await flush();
    expect(gestures(call)).toEqual([
      'beginGesture micAPosition',
      'beginGesture micAAzimuth',
      'endGesture micAPosition',
      'endGesture micAAzimuth',
    ]);
    expect(h.mock.value('micAAzimuth')).toBeCloseTo(-120, 4);
  });

  it('keeps a drag past the edge on the rim, and moves finely with shift', () => {
    render(<SpeakerStage cab={0} cabLabel="1x12" />);
    const start = grab('A', 1);
    fireEvent.pointerMove(svg(), { clientX: start.x + 400, clientY: start.y, pointerId: 1 });
    expect(getValue('micAPosition')).toBe(1);
    // Coming back follows the pointer again: the grab point is kept.
    fireEvent.pointerMove(svg(), { clientX: start.x, clientY: start.y, pointerId: 1 });
    expect(getValue('micAPosition')).toBeCloseTo(0.5, 5);
    fireEvent.pointerMove(svg(), {
      clientX: start.x - 40,
      clientY: start.y,
      pointerId: 1,
      shiftKey: true,
    });
    const fine = markerPoint(getValue('micAPosition'), getValue('micAAzimuth'));
    expect(fine.x).toBeCloseTo(start.x - 10, 4);
  });

  it('changes distance with an Alt-drag and with the wheel', () => {
    vi.useFakeTimers();
    const call = vi.spyOn(h.mock, 'call');
    render(<SpeakerStage cab={2} cabLabel="4x12" />);
    const at = grab('B', 2, { altKey: true });
    fireEvent.pointerMove(svg(), { clientX: at.x, clientY: at.y - 70, pointerId: 2 });
    expect(getValue('micBDistance')).toBeCloseTo(0.5, 5);
    expect(getValue('micBPosition')).toBe(0.5);
    fireEvent.pointerUp(svg(), { pointerId: 2 });

    // One notch up over mic B moves it 2 % further away; mic A stays.
    fireEvent.wheel(marker('B'), { deltaY: -100 });
    expect(getValue('micBDistance')).toBeCloseTo(0.52, 5);
    expect(getValue('micADistance')).toBe(0);
    act(() => vi.advanceTimersByTime(500));
    expect(gestures(call)).toEqual([
      'beginGesture micBDistance',
      'endGesture micBDistance',
      'beginGesture micBDistance',
      'endGesture micBDistance',
    ]);
  });

  it('hands a wheel turn over to a drag of the same mic as one host gesture', () => {
    vi.useFakeTimers();
    const call = vi.spyOn(h.mock, 'call');
    render(<SpeakerStage cab={2} cabLabel="4x12" />);
    fireEvent.wheel(marker('A'), { deltaY: -100 });
    expect(getValue('micADistance')).toBeCloseTo(0.02, 5);
    // Within the wheel's idle time an Alt-drag takes the mic over; the wheel's timer running out
    // mid-drag must not end the host gesture under it (Touch and Latch automation would let go).
    const at = grab('A', 5, { altKey: true });
    act(() => vi.advanceTimersByTime(500));
    fireEvent.pointerMove(svg(), { clientX: at.x, clientY: at.y - 70, pointerId: 5 });
    expect(gestures(call)).toEqual(['beginGesture micADistance']);
    // While the drag owns the mic, the wheel and the keys leave it alone.
    fireEvent.wheel(marker('A'), { deltaY: -100 });
    fireEvent.keyDown(marker('A'), { key: 'PageUp' });
    expect(getValue('micADistance')).toBeCloseTo(0.52, 5);
    fireEvent.pointerUp(svg(), { pointerId: 5 });
    act(() => vi.advanceTimersByTime(500));
    expect(gestures(call)).toEqual(['beginGesture micADistance', 'endGesture micADistance']);
  });

  it('scrolls the other mic over the cone when the selected one plays an IR', () => {
    render(<SpeakerStage cab={2} cabLabel="4x12" />);
    set('micACab3Type', 10);
    fireEvent.wheel(pad(), { deltaY: -100 });
    expect(getValue('micBDistance')).toBeCloseTo(0.02, 5);
    expect(getValue('micADistance')).toBe(0);
  });

  it('clicking the cone brings the selected mic there', () => {
    render(<SpeakerStage cab={1} cabLabel="2x12" />);
    fireEvent.focus(marker('B'));
    const at = markerPoint(0.6, -90);
    fireEvent.pointerDown(pad(), { button: 0, clientX: at.x, clientY: at.y, pointerId: 3 });
    fireEvent.pointerUp(svg(), { pointerId: 3 });
    expect(getValue('micBPosition')).toBeCloseTo(0.6, 5);
    expect(getValue('micBAzimuth')).toBeCloseTo(-90, 4);
    expect(getValue('micAPosition')).toBe(0.5);
    expect(marker('B')).toHaveFocus();
  });

  it('moves with the arrow keys, Page Up and Down for distance', () => {
    render(<SpeakerStage cab={2} cabLabel="4x12" />);
    const m = marker('A');
    const before = markerPoint(0.5, 60);
    fireEvent.keyDown(m, { key: 'ArrowUp' });
    let p = markerPoint(getValue('micAPosition'), getValue('micAAzimuth'));
    expect(p.x).toBeCloseTo(before.x, 4);
    expect(p.y).toBeCloseTo(before.y - 0.05 * CONE_R, 4);
    fireEvent.keyDown(m, { key: 'ArrowLeft', shiftKey: true });
    p = markerPoint(getValue('micAPosition'), getValue('micAAzimuth'));
    expect(p.x).toBeCloseTo(before.x - 0.01 * CONE_R, 4);

    fireEvent.keyDown(m, { key: 'PageUp' });
    fireEvent.keyDown(m, { key: 'PageUp' });
    expect(getValue('micADistance')).toBeCloseTo(0.1, 5);
    fireEvent.keyDown(m, { key: 'PageDown', shiftKey: true });
    expect(getValue('micADistance')).toBeCloseTo(0.09, 5);
    fireEvent.keyDown(m, { key: 'Home' });
    expect(getValue('micAPosition')).toBe(0);
    fireEvent.keyDown(m, { key: 'End' });
    expect(getValue('micAPosition')).toBe(1);

    set('micAOffAxis', 30);
    fireEvent.keyDown(m, { key: 'Delete' });
    expect(getValue('micAPosition')).toBe(0.5);
    expect(getValue('micAAzimuth')).toBe(60);
    expect(getValue('micADistance')).toBe(0);
    expect(getValue('micAOffAxis')).toBe(0);
  });

  it('shows the off-axis angle on the marker', () => {
    render(<SpeakerStage cab={0} cabLabel="1x12" />);
    const stem = () => svg().querySelector('[data-mic="A"] line');
    expect(stem()).toBeNull();
    set('micAOffAxis', 30);
    expect(screen.getByText('30°')).toBeInTheDocument();
    expect(stem()).not.toBeNull();
    expect(marker('A').getAttribute('aria-valuetext')).toMatch(/, 30° off axis$/);
  });

  it('leaves a custom IR mic in place and says why', () => {
    render(<SpeakerStage cab={2} cabLabel="4x12" />);
    set('micBCab3Type', 10);
    expect(marker('B')).toHaveAttribute('aria-disabled', 'true');
    expect(marker('B')).toHaveAttribute('tabindex', '-1');
    expect(
      screen.getByText(/Mic B plays an impulse response, which holds its own placement/),
    ).toBeInTheDocument();
    // With B out of reach, a click on the cone moves A even if B was selected before.
    fireEvent.focus(marker('B'));
    const at = markerPoint(0.3, 0);
    fireEvent.pointerDown(pad(), { button: 0, clientX: at.x, clientY: at.y, pointerId: 4 });
    fireEvent.pointerUp(svg(), { pointerId: 4 });
    expect(getValue('micAPosition')).toBeCloseTo(0.3, 5);
    expect(getValue('micBPosition')).toBe(0.5);
  });

  it('shows the other speakers of the 2x12 and 4x12, not of the 1x12', () => {
    const { rerender, container } = render(<SpeakerStage cab={0} cabLabel="1x12" />);
    const speakers = () =>
      container.querySelectorAll('g[aria-hidden="true"] circle:not([data-mic])');
    expect(speakers()).toHaveLength(0);
    rerender(<SpeakerStage cab={1} cabLabel="2x12" />);
    expect(speakers()).toHaveLength(2);
    rerender(<SpeakerStage cab={2} cabLabel="4x12" />);
    expect(speakers()).toHaveLength(4);
  });

  it('closes the host gestures of a mic drag cut short by a section switch', () => {
    const call = vi.spyOn(h.mock, 'call');
    const { unmount } = render(<SpeakerStage cab={2} cabLabel="4x12" />);
    fireEvent.pointerDown(marker('A'), { button: 0, clientX: 100, clientY: 100, pointerId: 1 });
    unmount();
    expect(gestures(call)).toEqual([
      'beginGesture micAPosition',
      'beginGesture micAAzimuth',
      'endGesture micAPosition',
      'endGesture micAAzimuth',
    ]);
  });
});

describe('MicStrip', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  it('has an off-axis knob, and hands position and distance to a custom IR', () => {
    render(<MicStrip mic="A" cab={0} />);
    const offAxis = screen.getByRole('slider', { name: 'Mic A Off-Axis' });
    expect(offAxis).toHaveAttribute('aria-valuetext', '0°');
    const position = () => screen.getByRole('slider', { name: 'Mic A Position' });
    expect(position()).not.toHaveAttribute('aria-disabled');
    set('micACab1Type', 10);
    expect(position()).toHaveAttribute('aria-disabled', 'true');
    expect(screen.getByRole('slider', { name: 'Mic A Distance' })).toHaveAttribute(
      'aria-disabled',
      'true',
    );
    expect(offAxis).not.toHaveAttribute('aria-disabled');
    expect(screen.getByText(/holds its own position and distance/)).toBeInTheDocument();
  });
});
