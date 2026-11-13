// SPDX-License-Identifier: AGPL-3.0-or-later
// The looper's footswitches from the UI (docs/live.md §4.5): learning them from the looper's own
// buttons and from the MIDI panel, the double-tap option, and the looper they play, against the
// mock.
import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { flush, setupMock, type Harness } from '../../test/harness';
import { useLiveStore } from '../../state/liveStore';
import { Header } from '../header/Header';
import { ControlMenu } from './ControlMenu';
import { LearnBanner } from './LearnBanner';
import { LiveBar } from './LiveBar';

const CC = 0xb0;

function Rig() {
  return (
    <>
      <Header />
      <LiveBar />
      <ControlMenu />
      <LearnBanner />
    </>
  );
}

describe('the looper’s footswitches', () => {
  let h: Harness | undefined;
  afterEach(() => {
    cleanup();
    h?.dispose();
    h = undefined;
  });

  const midi = async (data1: number, data2: number, time: number) => {
    await act(async () => {
      h!.mock.midi(CC, data1, data2, time);
      await flush();
    });
  };
  const click = async (el: HTMLElement) => {
    await act(async () => {
      fireEvent.click(el);
      await flush();
    });
  };
  /** Telemetry frames over `seconds`: the mock looper's clock moves on. */
  const run = (seconds: number) => act(() => h!.mock.tickTelemetry(seconds));
  const looper = () => h!.mock.looper.telemetry();
  const bindings = () => useLiveStore.getState().state?.bindings ?? [];

  /** Learns a momentary footswitch on `controller` for what is waiting. */
  const press = async (controller: number, at: number) => {
    await midi(controller, 127, at);
    await midi(controller, 0, at + 0.1);
  };

  it('learn from the looper’s big button, which the footswitch then presses', async () => {
    h = await setupMock({ initialPresetId: null });
    render(<Rig />);
    await click(screen.getByRole('button', { name: 'Looper' }));
    const dialog = screen.getByRole('dialog', { name: 'Looper' });
    run(0);
    const big = within(dialog).getByRole('button', { name: 'Record' });
    fireEvent.contextMenu(big, { button: 2, clientX: 40, clientY: 40 });
    expect(screen.getAllByRole('menuitem').map((i) => i.textContent)).toEqual(['Learn MIDI']);
    await click(screen.getByRole('menuitem', { name: 'Learn MIDI' }));
    expect(
      screen.getByText(/Move a pedal or press a footswitch for/).closest('[role="status"]'),
    ).toHaveTextContent('Looper: record, play, overdub');

    await press(60, 1);
    expect(bindings()).toEqual([
      expect.objectContaining({ target: 'looper:main', controller: 60, mode: 'toggle' }),
    ]);

    // Now it plays: the press records, as the big button would.
    await midi(60, 127, 5);
    run(0);
    expect(looper().state).toBe('recording');
    expect(within(dialog).getByText('Recording', { selector: 'p' })).toBeInTheDocument();
  });

  it('are offered in the MIDI panel of the app, not in a plug-in', async () => {
    h = await setupMock();
    render(<Rig />);
    await click(screen.getByRole('button', { name: 'MIDI' }));
    let panel = screen.getByRole('dialog', { name: 'MIDI' });
    const group = within(panel).getByRole('group', { name: 'Learn a footswitch for the looper' });
    expect(
      within(group)
        .getAllByRole('button')
        .map((b) => b.firstElementChild?.textContent),
    ).toEqual(['Record, play', 'Stop', 'Undo, redo', 'Clear']);
    cleanup();
    h.dispose();

    h = await setupMock({ isStandalone: false, wrapperType: 'VST3' });
    render(<Rig />);
    await click(screen.getByRole('button', { name: 'MIDI' }));
    panel = screen.getByRole('dialog', { name: 'MIDI' });
    expect(
      within(panel).queryByRole('group', { name: 'Learn a footswitch for the looper' }),
    ).toBeNull();
    expect(
      within(panel).getByRole('group', { name: 'Learn a footswitch for' }),
    ).toBeInTheDocument();
  });

  it('take a double tap that stops, when the player switches it on', async () => {
    h = await setupMock();
    render(<Rig />);
    await click(screen.getByRole('button', { name: 'MIDI' }));
    await click(screen.getByRole('button', { name: /^Record, play/ }));
    expect(screen.queryByRole('dialog', { name: 'MIDI' })).toBeNull(); // the banner takes over
    await press(60, 1);

    await click(screen.getByRole('button', { name: 'MIDI' }));
    const panel = screen.getByRole('dialog', { name: 'MIDI' });
    const list = within(panel).getByRole('list', { name: 'Learned controls' });
    const row = within(list).getByText('Looper: record, play, overdub').closest('li')!;
    const option = within(row).getByRole('checkbox', { name: 'Double tap stops' });
    expect(option).not.toBeChecked();
    expect(option).toHaveAccessibleDescription(
      'Two presses within 0.4 s stop the looper and undo the first',
    );

    // Off: two quick presses are two presses (an overdub begins and ends).
    await midi(60, 127, 5); // record
    run(1);
    await midi(60, 127, 6.1); // close: it plays
    run(1);
    await midi(60, 127, 7.2);
    await midi(60, 127, 7.4);
    expect(looper().state).toBe('playing');

    await click(option);
    expect(bindings()[0]?.doubleTap).toBe(true);
    expect(option).toBeChecked();

    // On: the second press 0.2 s after the first stops and takes the new layer back.
    run(1);
    await midi(60, 127, 9);
    expect(looper().state).toBe('overdubbing');
    await midi(60, 127, 9.2);
    expect(looper()).toMatchObject({ state: 'stopped', canRedo: true, canUndo: false });
    // Further apart: two presses again (play, then overdub).
    await midi(60, 127, 10);
    await midi(60, 127, 10.5);
    expect(looper().state).toBe('overdubbing');
  });
});
