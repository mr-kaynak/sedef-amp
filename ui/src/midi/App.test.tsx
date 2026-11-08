// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it } from 'vitest';
import { MidiApp } from './App';
import { MockMidiBackend, type MockOptions } from './backend';
import { noteName, spokenNote } from './notes';
import { connectMidi } from './store';

let disconnect: (() => void) | null = null;
let backend: MockMidiBackend | null = null;

async function setup(options: MockOptions = {}) {
  backend = new MockMidiBackend({ autoPlay: false, ...options });
  disconnect = await connectMidi(backend);
  render(<MidiApp />);
  return backend;
}

afterEach(() => {
  disconnect?.();
  backend?.stop();
  disconnect = null;
});

describe('note names', () => {
  it('name MIDI notes the way players do', () => {
    expect(noteName(40)).toEqual({ name: 'E', octave: 2 });
    expect(noteName(61)).toEqual({ name: 'C♯', octave: 4 });
    expect(spokenNote(42)).toBe('F sharp 2');
  });
});

describe('Sedef MIDI app', () => {
  it('shows the note being sent, and the last one once it ends', async () => {
    const b = await setup();
    const panel = screen.getByRole('region', { name: 'Note being sent' });
    expect(within(panel).getByText('No note yet')).toBeInTheDocument();
    act(() => b.publish({ note: 40, pitch: 40.05, velocity: 96, sounding: true, levelDb: -20 }));
    expect(within(panel).getByText('E 2, velocity 96')).toBeInTheDocument();
    expect(panel).toHaveAttribute('data-sounding');
    act(() => b.publish({ note: -1, pitch: 0, sounding: false }));
    expect(within(panel).getByText('No note. Last: E 2')).toBeInTheDocument();
    expect(panel).not.toHaveAttribute('data-sounding');
  });

  it('sends Panic', async () => {
    const b = await setup();
    await userEvent.click(screen.getByRole('button', { name: 'Panic' }));
    expect(b.calls.some((c) => c.name === 'sedef_midi_panic')).toBe(true);
  });

  it('enables the bend range only with Notes + Bend', async () => {
    await setup();
    const range = screen.getByRole('combobox', { name: 'Bend range' });
    expect(range).toBeDisabled();
    await userEvent.click(screen.getByRole('radio', { name: 'Notes + Bend' }));
    expect(range).toBeEnabled();
  });

  it('explains Steady with its lag', async () => {
    await setup();
    await userEvent.click(screen.getByRole('radio', { name: 'Steady' }));
    expect(screen.getByText(/Set the track delay to −38 ms/)).toBeInTheDocument();
  });

  it('sends the notes to a picked port and says where they go', async () => {
    const b = await setup();
    const picker = screen.getByRole('combobox', { name: 'Also send notes to' });
    expect(screen.getByText('Notes go to the host only.')).toBeInTheDocument();
    await userEvent.selectOptions(picker, 'device:iac-1');
    expect(
      b.calls.some((c) => c.name === 'sedef_midi_setPort' && c.args[0] === 'device:iac-1'),
    ).toBe(true);
    expect(await screen.findByText('Sending to IAC Driver Bus 1.')).toBeInTheDocument();
  });

  it('opens the standalone app on its virtual port, with the audio input button', async () => {
    await setup({ isStandalone: true });
    const picker = screen.getByRole('combobox', { name: 'Send notes to' });
    expect(picker).toHaveValue('virtual');
    expect(screen.getByText(/Other apps list “Sedef MIDI” as a MIDI input/)).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Audio input…' })).toBeInTheDocument();
    // The standalone app plays notes, not the guitar.
    expect(screen.getByRole('radio', { name: 'Mute' })).toHaveAttribute('aria-checked', 'true');
  });

  it('names the instrument variant and its side chain', async () => {
    await setup({ isInstrument: true, wrapperType: 'AudioUnit' });
    expect(screen.getByText('Instrument · guitar on the side chain')).toBeInTheDocument();
    expect(screen.getByText('Side chain')).toBeInTheDocument();
  });

  it('follows the host when it changes a parameter', async () => {
    const b = await setup();
    act(() => b.hostSets({ octave: 0 }));
    expect(screen.getByRole('radio', { name: '−2' })).toHaveAttribute('aria-checked', 'true');
  });

  it('moves the gate from the keyboard', async () => {
    const b = await setup();
    const gate = screen.getByRole('slider', { name: 'Gate' });
    gate.focus();
    await userEvent.keyboard('{ArrowUp}');
    const sent = b.calls.filter((c) => c.name === 'sedef_setParam' && c.args[0] === 'gate');
    expect(sent.length).toBeGreaterThan(0);
    expect(Number(sent.at(-1)?.args[1])).toBeGreaterThan(-54);
  });
});
