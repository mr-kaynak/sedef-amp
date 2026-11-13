// SPDX-License-Identifier: AGPL-3.0-or-later
// The Recorder in the standalone app's header (docs/recorder.md): one button records, the time
// and the levels show while it does, the take's files are named and found again, and re-amp
// plays a take, an opened file or a dropped one, looping, muted or mixed.
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { StatusBar } from '../../app/StatusBar';
import { useShortcuts } from '../../app/useShortcuts';
import type { MockOptions } from '../../bridge/mockBackend';
import { flush, setupMock, type Harness } from '../../test/harness';
import { Header } from '../header/Header';
import { useRecorderStore } from './store';

let h: Harness | null = null;

function Shell() {
  useShortcuts();
  return (
    <>
      <Header />
      <input aria-label="Some field" />
      <StatusBar />
    </>
  );
}

async function start(options: MockOptions = {}): Promise<Harness> {
  h = await setupMock({ initialPresetId: null, isStandalone: true, ...options });
  render(<Shell />);
  await act(flush);
  return h;
}

async function settle() {
  await act(async () => {
    await new Promise((r) => setTimeout(r, 0));
    await flush();
  });
}

/** Time passes for the simulated app: telemetry frames at 30 Hz. */
async function play(seconds: number) {
  await act(async () => {
    for (let t = 0; t < seconds; t += 1 / 30) h!.mock.tickTelemetry(1 / 30);
    await flush();
  });
}

const recorderButton = () => screen.getByRole('button', { name: /^Recorder/ });

async function openPanel() {
  fireEvent.click(recorderButton());
  await act(flush);
  return screen.getByRole('dialog', { name: 'Recorder' });
}

afterEach(() => {
  h?.dispose();
  h = null;
});

describe('Recorder', () => {
  it('sits in the transport with the metronome, in the standalone app only', async () => {
    await start();
    const transport = screen.getByRole('group', { name: 'Transport' });
    expect(within(transport).getByRole('button', { name: 'Metronome' })).toBeInTheDocument();
    expect(within(transport).getByRole('button', { name: 'Recorder' })).toBeInTheDocument();
    h!.dispose();
    h = await setupMock({ initialPresetId: null, isStandalone: false, wrapperType: 'VST3' });
    expect(useRecorderStore.getState().state).toBeNull();
  });

  it('records with one button: the time, the levels, then the saved take', async () => {
    const { mock } = await start();
    const panel = await openPanel();
    const big = within(panel).getByRole('button', { name: 'Start recording' });
    expect(big).toHaveFocus();
    expect(within(panel).getByText('Ready to record')).toBeInTheDocument();
    expect(within(panel).getByText('/Users/you/Music/Sedef Amp/Recordings')).toBeInTheDocument();

    fireEvent.click(big);
    await act(flush);
    await play(2.5);
    expect(within(panel).getByText('Recording')).toBeInTheDocument();
    expect(within(panel).getByRole('button', { name: 'Stop recording' })).toBeInTheDocument();
    expect(within(panel).getByText(/^0:02\.\d$/)).toBeInTheDocument();
    // The header button says it too, and the status bar keeps the time in sight.
    expect(recorderButton()).toHaveAccessibleName('Recorder, recording, 2 seconds');
    expect(screen.getByRole('contentinfo')).toHaveTextContent('Recording 0:02');
    expect(screen.getByRole('status', { name: '' })).toHaveTextContent('Recording');

    fireEvent.click(within(panel).getByRole('button', { name: 'Stop recording' }));
    await act(flush);
    await play(0.2);
    expect(within(panel).getByText('Ready to record')).toBeInTheDocument();
    const take = mock.recorder.state().recorder.take!;
    expect(take.ended).toBe('stopped');
    expect(take.diPath).toBe(`/Users/you/Music/Sedef Amp/Recordings/${take.name} DI.wav`);
    expect(take.outputPath).toBe(`/Users/you/Music/Sedef Amp/Recordings/${take.name} Output.wav`);
    expect(within(panel).getByText(take.name)).toBeInTheDocument();
    expect(within(panel).getByText('DI and Output, 0:02')).toBeInTheDocument();
    expect(recorderButton()).toHaveAccessibleName('Recorder');
    expect(
      screen.getAllByRole('status').some((s) => s.textContent === 'Take saved, 2 seconds'),
    ).toBe(true);
  });

  it('shows the folder and the take in the file manager', async () => {
    const { mock } = await start({ recorder: 'saved' });
    const call = vi.spyOn(mock, 'call');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Show folder' }));
    const take = mock.recorder.state().recorder.take!;
    fireEvent.click(
      within(panel).getByRole('button', { name: `Show the take ${take.name} in its folder` }),
    );
    await act(flush);
    expect(call.mock.calls.filter(([n]) => n === 'sedef_recorder_reveal')).toEqual([
      ['sedef_recorder_reveal', undefined],
      ['sedef_recorder_reveal', take.diPath],
    ]);
  });

  it('R starts and stops a take from anywhere but a text field', async () => {
    const { mock } = await start();
    fireEvent.keyDown(window, { key: 'r' });
    await act(flush);
    await play(0.1);
    expect(mock.recorder.state().recorder.status).toBe('recording');
    // Typing an R into a field is a letter.
    const field = screen.getByRole('textbox', { name: 'Some field' });
    field.focus();
    fireEvent.keyDown(field, { key: 'r' });
    await act(flush);
    expect(mock.recorder.state().recorder.status).toBe('recording');
    fireEvent.keyDown(window, { key: 'R' });
    await act(flush);
    expect(mock.recorder.state().recorder.status).toBe('saving');
  });

  it('says how much a slow disk dropped, and why a take ended', async () => {
    await start({ recorder: 'dropouts' });
    const panel = await openPanel();
    expect(
      within(panel).getByText(/^2 dropouts: 0\.09 s of this take is silence\./),
    ).toBeInTheDocument();
    h!.dispose();
    h = null;
    document.body.innerHTML = '';
    await start({ recorder: 'error' });
    const again = await openPanel();
    expect(within(again).getByRole('alert')).toHaveTextContent(
      /Could not write to .*Free some space/,
    );
  });

  it('says a dropout aloud once while recording, and again with the saved take', async () => {
    const { mock } = await start();
    const said = () => screen.getAllByRole('status').map((s) => s.textContent);
    fireEvent.keyDown(window, { key: 'r' });
    await act(flush);
    await play(1);
    act(() => mock.recorder.dropout());
    await play(0.2);
    expect(said()).toContain('Dropout: the disk could not keep up');
    fireEvent.keyDown(window, { key: 'r' });
    await act(flush);
    await play(0.2);
    expect(said()).toContain('Take saved, 1 second, 1 dropout');
  });

  it('moves between Record and Re-amp with the arrow keys', async () => {
    await start();
    const panel = await openPanel();
    const record = within(panel).getByRole('tab', { name: 'Record' });
    expect(record).toHaveAttribute('aria-selected', 'true');
    record.focus();
    fireEvent.keyDown(record, { key: 'ArrowRight' });
    await act(flush);
    const reamp = within(panel).getByRole('tab', { name: 'Re-amp' });
    expect(reamp).toHaveAttribute('aria-selected', 'true');
    expect(reamp).toHaveFocus();
    expect(within(panel).getByRole('tabpanel')).toHaveAccessibleName('Re-amp');
    // Only the shown panel exists, so only its tab points at it.
    expect(reamp).toHaveAttribute('aria-controls', within(panel).getByRole('tabpanel').id);
    expect(record).not.toHaveAttribute('aria-controls');
  });
});

describe('Re-amp', () => {
  async function reampTab() {
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('tab', { name: 'Re-amp' }));
    await settle();
    return panel;
  }

  it('plays a recorded take, looping, and stops', async () => {
    const { mock } = await start();
    const panel = await reampTab();
    const play = within(panel).getByRole('button', { name: 'Play the file through the rig' });
    expect(play).toHaveAttribute('aria-disabled', 'true');
    expect(within(panel).getByText('No file yet')).toBeInTheDocument();

    const list = within(panel).getByRole('list');
    const takes = within(list).getAllByRole('button');
    expect(takes.map((b) => b.textContent)).toEqual([
      expect.stringContaining('2026-11-13 14-12-33'),
      expect.stringContaining('2026-11-13 14-05-33'),
      expect.stringContaining('Rhythm DI, 44.1 kHz'),
    ]);
    fireEvent.click(takes[1]!);
    await act(flush);
    await play_(0.6);
    expect(within(panel).getByText('Ready')).toBeInTheDocument();
    expect(within(panel).getByText('2026-11-13 14-05-33 DI.wav')).toBeInTheDocument();
    expect(within(panel).getByText('0:41, mono, 48 kHz')).toBeInTheDocument();
    expect(within(panel).getByRole('status')).toHaveTextContent(
      '2026-11-13 14-05-33 DI.wav is ready to play',
    );
    expect(within(list).getAllByRole('button')[1]).toHaveAttribute('aria-current', 'true');

    fireEvent.click(play);
    await act(flush);
    await play_(1.5);
    expect(mock.recorder.state().reamp.playing).toBe(true);
    expect(within(panel).getByText('Re-amping')).toBeInTheDocument();
    expect(within(panel).getByText('0:01 / 0:41')).toBeInTheDocument();
    expect(recorderButton()).toHaveAccessibleName('Recorder, re-amping');
    expect(screen.getByRole('contentinfo')).toHaveTextContent('Re-amping 0:01 / 0:41');
    fireEvent.click(within(panel).getByRole('button', { name: 'Stop re-amping' }));
    await act(flush);
    await play_(0.1);
    expect(mock.recorder.state().reamp.playing).toBe(false);
  });

  it('sets Loop and whether the live guitar is muted or mixed in', async () => {
    const { mock } = await start();
    const panel = await reampTab();
    const loop = within(panel).getByRole('switch', { name: 'Loop' });
    expect(loop).toHaveAttribute('aria-checked', 'true');
    fireEvent.click(loop);
    await act(flush);
    expect(mock.recorder.state().reamp.loop).toBe(false);
    expect(loop).toHaveAttribute('aria-checked', 'false');

    const guitar = within(panel).getByRole('radiogroup', { name: 'Your guitar' });
    const muted = within(guitar).getByRole('radio', { name: 'Muted' });
    expect(muted).toHaveAttribute('aria-checked', 'true');
    fireEvent.keyDown(muted, { key: 'ArrowRight' });
    await act(flush);
    expect(mock.recorder.state().reamp.mix).toBe(true);
    expect(within(guitar).getByRole('radio', { name: 'Mixed in' })).toHaveAttribute(
      'aria-checked',
      'true',
    );
  });

  it('converts a file at another rate, with its progress', async () => {
    await start({ recorder: 'converting' });
    useRecorderStore.setState({ tab: 'reamp' });
    const panel = await openPanel();
    await settle();
    expect(within(panel).getByText('Converting…')).toBeInTheDocument();
    expect(within(panel).getByRole('progressbar', { name: 'Converting' })).toHaveAttribute(
      'aria-valuenow',
      '40',
    );
    expect(within(panel).getByText(/converted for this device/)).toBeInTheDocument();
    // One status region, there before its text changes; the percentages are not read out.
    expect(within(panel).getByRole('status')).toHaveTextContent(
      'Converting the file for this device',
    );
  });

  it('takes a dropped WAV in pieces and plays it; refuses what it cannot play', async () => {
    const { mock } = await start();
    const call = vi.spyOn(mock, 'call');
    const panel = await reampTab();
    // The tab inside the panel takes the drop.
    const tab = within(panel).getByRole('tabpanel').firstElementChild!;
    const drop = async (file: File) => {
      const dataTransfer = { types: ['Files'], files: [file], dropEffect: 'none' };
      fireEvent.dragOver(tab, { dataTransfer });
      fireEvent.drop(tab, { dataTransfer });
      await settle();
      await settle();
    };

    await drop(new File(['not audio'], 'notes.txt'));
    expect(within(panel).getByRole('alert')).toHaveTextContent(
      'Drop a WAV, AIFF or FLAC file to re-amp it.',
    );

    // 2.5 MB: three pieces of at most 1 MB.
    const bytes = new Uint8Array(2.5 * 1024 * 1024).map((_, i) => i & 0xff);
    await drop(new File([bytes], 'My DI.wav'));
    const chunks = call.mock.calls.filter(([n]) => n === 'sedef_reamp_importChunk');
    expect(chunks).toHaveLength(3);
    expect(atob(chunks[0]![2] as string).length).toBe(1024 * 1024);
    expect(call.mock.calls.some(([n]) => n === 'sedef_reamp_importEnd')).toBe(true);
    await play_(0.6);
    expect(mock.recorder.state().reamp.file?.path).toBe(
      '/Users/you/Music/Sedef Amp/Recordings/Imported/My DI.wav',
    );
    expect(within(panel).queryByRole('alert')).toBeNull();
    expect(within(within(panel).getByRole('list')).getAllByRole('button')[0]).toHaveTextContent(
      'My DI',
    );
  });

  it('re-amps the take just recorded from the Record tab', async () => {
    const { mock } = await start({ recorder: 'saved' });
    const panel = await openPanel();
    const take = mock.recorder.state().recorder.take!;
    fireEvent.click(within(panel).getByRole('button', { name: `Re-amp the take ${take.name}` }));
    await settle();
    expect(within(panel).getByRole('tab', { name: 'Re-amp' })).toHaveAttribute(
      'aria-selected',
      'true',
    );
  });

  it('opens any file through the chooser', async () => {
    const { mock } = await start();
    const panel = await reampTab();
    fireEvent.click(within(panel).getByRole('button', { name: 'Open file…' }));
    await settle();
    expect(mock.recorder.state().reamp.file?.name).toBe('Rhythm DI, 44.1 kHz.wav');
  });
});

/** Time passing, under a name that does not clash with the play button. */
async function play_(seconds: number) {
  await play(seconds);
}
