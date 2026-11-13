// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { LooperDemo } from '../../bridge/mock/looperSim';
import { App } from '../../app/App';
import { flush, setupMock, type Harness } from '../../test/harness';
import { useToastStore } from '../../state/toastStore';
import { Header } from '../header/Header';

let h: Harness;
afterEach(() => h.dispose());

async function openPanel(options: Parameters<typeof setupMock>[0] = {}) {
  h = await setupMock({ initialPresetId: null, ...options });
  const call = vi.spyOn(h.mock, 'call');
  render(<Header />);
  fireEvent.click(screen.getByRole('button', { name: 'Looper' }));
  await flush();
  const dialog = screen.getByRole('dialog', { name: 'Looper' });
  // The panel inside the popover: it carries the state's tone and the Space key.
  const panel = dialog.querySelector<HTMLElement>('[data-tone]');
  if (!panel) throw new Error('no looper panel');
  return { call, panel };
}

/** One telemetry frame from the mock, as the plug-in's 30 Hz timer would send it. */
function tick(dt = 1 / 30) {
  act(() => h.mock.tickTelemetry(dt));
}

function demo(state: LooperDemo) {
  h.mock.looper.demo(state);
  tick(0);
}

/** The looper presses the mock was sent, in order. */
const commands = (call: { mock: { calls: unknown[][] } }) =>
  call.mock.calls.filter((args) => args[0] === 'sedef_looper_command').map((args) => args[1]);

describe('the looper panel', () => {
  it('reserves the looper when it opens, and records with the big button', async () => {
    const { call, panel } = await openPanel();
    expect(call).toHaveBeenCalledWith('sedef_looper_open');
    tick();
    const main = within(panel).getByRole('button', { name: 'Record' });
    expect(main).toHaveFocus(); // Space presses it at once
    expect(within(panel).getByText('Ready to record')).toBeInTheDocument();
    fireEvent.click(main);
    await flush();
    expect(commands(call)).toEqual(['main']);
    tick();
    expect(within(panel).getByText('Recording')).toBeInTheDocument();
    expect(within(panel).getByRole('button', { name: 'Close the loop and play' })).toBeEnabled();
    expect(within(panel).getByRole('button', { name: 'Stop' })).toBeEnabled();
    expect(within(panel).getByRole('button', { name: 'Undo the last overdub' })).toBeDisabled();
  });

  it('shows each state with what the big button does next', async () => {
    const { panel } = await openPanel({ overrides: { metronomeEnabled: 1 } });
    const cases: [LooperDemo, string, string][] = [
      ['empty', 'Ready to record', 'Record'],
      ['armed', 'Records on the 1', 'Cancel recording'],
      ['recording', 'Recording', 'Close the loop and play'],
      ['closing', 'Closing at the bar line', 'Closing at the bar line'],
      ['playing', 'Playing', 'Overdub'],
      ['overdubbing', 'Overdubbing', 'Stop overdubbing and play'],
      ['stopped', 'Stopped', 'Play'],
    ];
    for (const [state, title, button] of cases) {
      demo(state);
      expect(within(panel).getByText(title, { selector: 'p' })).toBeInTheDocument();
      expect(within(panel).getByRole('button', { name: button })).toBeInTheDocument();
    }
    demo('playing');
    expect(within(panel).getByText(/^Bar 2 of 4 · 8\.0 s$/)).toBeInTheDocument();
    expect(panel).toHaveAttribute('data-tone', 'play');
    demo('recording');
    expect(panel).toHaveAttribute('data-tone', 'record');
    demo('overdubbing');
    expect(panel).toHaveAttribute('data-tone', 'dub');
  });

  it('says the count-in is a bar of the groove when the metronome plays drums', async () => {
    const { panel } = await openPanel({ overrides: { metronomeEnabled: 1, metronomeMode: 1 } });
    tick();
    expect(within(panel).getByText('One bar of the groove first')).toBeInTheDocument();
    expect(
      within(panel).getByText('The drums are on: the loop snaps to whole bars.'),
    ).toBeInTheDocument();
  });

  it('presses the big button with Space anywhere in the panel but on a button', async () => {
    const { call, panel } = await openPanel();
    tick();
    fireEvent.keyDown(panel, { key: ' ' });
    fireEvent.keyDown(within(panel).getByRole('slider', { name: /Level/ }), { key: ' ' });
    // On another button Space is that button's own.
    fireEvent.keyDown(within(panel).getByRole('switch', { name: 'Looper Count-in' }), { key: ' ' });
    await flush();
    expect(commands(call)).toEqual(['main', 'main']);
  });

  it('offers Undo after Clear, and Undo brings the loop back', async () => {
    const { call, panel } = await openPanel();
    demo('playing');
    fireEvent.click(within(panel).getByRole('button', { name: 'Clear' }));
    await flush();
    tick();
    expect(useToastStore.getState().toast?.message).toBe('Loop cleared');
    expect(within(panel).getByRole('button', { name: 'Undo clear' })).toBeEnabled();
    act(() => useToastStore.getState().toast?.action?.run());
    await flush();
    tick();
    expect(commands(call)).toEqual(['clear', 'undo']);
    expect(within(panel).getByText('Stopped', { selector: 'p' })).toBeInTheDocument();
  });

  it('names the state on the header button while the panel is closed', async () => {
    h = await setupMock({ initialPresetId: null });
    render(<Header />);
    h.mock.looper.demo('recording');
    tick(0);
    const button = screen.getByRole('button', { name: 'Looper, recording' });
    expect(screen.getByRole('status')).toHaveTextContent('Recording');
    // A shape as well as the colour: the badge in the corner follows the state.
    expect(button.querySelector('[data-state="recording"]')).not.toBeNull();
    h.mock.looper.demo('playing');
    tick(0);
    expect(button.querySelector('[data-state="playing"]')).not.toBeNull();
  });
});

describe('the looper keys', () => {
  it('L is the big button and Shift+L is Stop, from anywhere but a text field', async () => {
    h = await setupMock({ initialPresetId: null });
    const call = vi.spyOn(h.mock, 'call');
    render(<App />);
    fireEvent.keyDown(window, { key: 'l' });
    fireEvent.keyDown(window, { key: 'L', shiftKey: true });
    fireEvent.keyDown(window, { key: 'l', repeat: true }); // a held key presses once
    const field = document.createElement('input');
    document.body.append(field);
    fireEvent.keyDown(field, { key: 'l' });
    field.remove();
    await flush();
    expect(commands(call)).toEqual(['main', 'stop']);
  });

  it('a plug-in has no looper: no button, and L does nothing', async () => {
    h = await setupMock({ initialPresetId: null, isStandalone: false, wrapperType: 'VST3' });
    const call = vi.spyOn(h.mock, 'call');
    render(<App />);
    expect(screen.queryByRole('button', { name: /^Looper/ })).toBeNull();
    fireEvent.keyDown(window, { key: 'l' });
    await flush();
    expect(commands(call)).toEqual([]);
  });
});
