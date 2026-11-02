// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { describeCapture } from '../../bridge/mock/captureSim';
import { formatValue } from '../../params/format';
import { isGlobalParam } from '../../params/presetScope';
import { bundledSchema } from '../../params/schema';
import { useAppStore } from '../../state/appStore';
import { flush, setupMock, type Harness } from '../../test/harness';
import { AmpSection } from './AmpSection';
import { CAPTURE_SITE_URL } from './CapturePanel';

let h: Harness | null = null;

async function start(): Promise<Harness> {
  h = await setupMock({ initialPresetId: null });
  render(<AmpSection />);
  await act(flush);
  return h;
}

const card = (name: string) => screen.getByRole('radio', { name: new RegExp(`^${name}`) });

async function click(el: HTMLElement) {
  fireEvent.click(el);
  await act(flush);
}

/** A tiny .nam file the way the trainer writes it. */
function namFile(metadata: Record<string, unknown> = {}): string {
  return JSON.stringify({
    version: '0.5.4',
    architecture: 'Linear',
    config: { receptive_field: 2, bias: false },
    weights: [0.5, 0.25],
    sample_rate: 48000,
    metadata,
  });
}

async function drop(name: string, text: string) {
  const panel = screen.getByRole('region', { name: 'Capture' });
  const file = new File([text], name, { type: 'application/octet-stream' });
  const dataTransfer = { types: ['Files'], files: [file], dropEffect: 'none' };
  fireEvent.dragOver(panel, { dataTransfer });
  fireEvent.drop(panel, { dataTransfer });
  await act(async () => {
    await new Promise((r) => setTimeout(r, 0));
    await flush();
  });
}

afterEach(() => {
  h?.dispose();
  h = null;
});

describe('Capture card', () => {
  it('sits after the three amps and switches the amp source', async () => {
    const { mock } = await start();
    const radios = within(screen.getByRole('radiogroup', { name: 'Amp' })).getAllByRole('radio');
    expect(radios.map((r) => r.textContent)).toEqual([
      expect.stringContaining('Lagoon'),
      expect.stringContaining('Reef'),
      expect.stringContaining('Abyss'),
      expect.stringContaining('Capture'),
    ]);
    await click(card('Capture'));
    expect(mock.value('ampSource')).toBe(1);
    expect(card('Capture')).toHaveAttribute('aria-checked', 'true');
    // Back to an amp: the source goes back to the models.
    await click(card('Reef'));
    expect(mock.value('ampSource')).toBe(0);
    expect(mock.value('ampModel')).toBe(1);
  });

  it('moves with the arrow keys like the amps', async () => {
    const { mock } = await start();
    await click(card('Abyss'));
    fireEvent.keyDown(card('Abyss'), { key: 'ArrowRight' });
    await act(flush);
    expect(mock.value('ampSource')).toBe(1);
    expect(card('Capture')).toHaveFocus();
    fireEvent.keyDown(card('Capture'), { key: 'ArrowRight' });
    await act(flush);
    expect(mock.value('ampSource')).toBe(0);
    expect(mock.value('ampModel')).toBe(0);
  });

  it('is hidden against a plug-in without captures', async () => {
    await start();
    act(() => useAppStore.setState({ capturesSupported: false }));
    expect(screen.queryByRole('radio', { name: /^Capture/ })).toBeNull();
  });
});

describe('Capture panel', () => {
  it('starts empty with one way to load and a place to find captures', async () => {
    const { mock } = await start();
    await click(card('Capture'));
    expect(screen.getByRole('heading', { name: 'No capture loaded' })).toBeInTheDocument();
    const open = vi.spyOn(mock, 'call');
    await click(screen.getByRole('button', { name: 'Find captures on TONE3000' }));
    expect(open).toHaveBeenCalledWith('sedef_openUrl', CAPTURE_SITE_URL);
    expect(CAPTURE_SITE_URL.startsWith('https://')).toBe(true);
  });

  it('shows what the loaded capture is and follows its cab', async () => {
    const { mock } = await start();
    await click(card('Capture'));
    await click(screen.getByRole('button', { name: 'Load capture…' }));
    expect(screen.getByRole('heading', { name: 'Driftwood Crunch' })).toBeInTheDocument();
    expect(screen.getByText('Driftwood 30 Combo')).toBeInTheDocument();
    expect(screen.getByText('WaveNet, 48 kHz')).toBeInTheDocument();
    expect(screen.getByText('Amp only')).toBeInTheDocument();
    expect(mock.value('captureCab')).toBe(1);
    // The interface level matters for this one: it says what level it was made at.
    expect(screen.getByText(/made at \+12\.2 dBu/)).toBeInTheDocument();

    // A full rig turns Sedef's cab off and needs no interface level.
    await click(screen.getByRole('button', { name: 'Replace…' }));
    expect(screen.getByRole('heading', { name: 'Tidepool Lead, close mic' })).toBeInTheDocument();
    expect(screen.getByText('Full rig, cab and mic included')).toBeInTheDocument();
    expect(mock.value('captureCab')).toBe(0);
    expect(screen.getByRole('switch', { name: 'Capture Through Cab' })).toHaveAttribute(
      'aria-checked',
      'false',
    );
    expect(screen.queryByText(/made at/)).toBeNull();
    // The card names it too.
    expect(card('Capture')).toHaveTextContent('Tidepool Lead, close mic');
  });

  it('keeps a recent list that loads again', async () => {
    const { mock } = await start();
    await click(card('Capture'));
    await click(screen.getByRole('button', { name: 'Load capture…' }));
    await click(screen.getByRole('button', { name: 'Replace…' }));
    const recent = within(screen.getByRole('region', { name: 'Recent captures' }));
    const items = recent.getAllByRole('button');
    expect(items.map((b) => b.textContent)).toEqual([
      'Tidepool Lead, close mic',
      'Driftwood Crunch',
    ]);
    expect(items[0]).toHaveAttribute('aria-current', 'true');
    await click(recent.getByRole('button', { name: 'Driftwood Crunch' }));
    expect(screen.getByRole('heading', { name: 'Driftwood Crunch' })).toBeInTheDocument();
    expect(mock.value('captureCab')).toBe(1);
  });

  it('clears back to the amp models', async () => {
    const { mock } = await start();
    await click(card('Capture'));
    await click(screen.getByRole('button', { name: 'Load capture…' }));
    await click(screen.getByRole('button', { name: 'Clear' }));
    expect(mock.value('ampSource')).toBe(0);
    expect(useAppStore.getState().capture.loaded).toBeNull();
  });

  it('loads a dropped file and refuses what is not a capture', async () => {
    await start();
    await click(card('Capture'));
    await drop('Notes.txt', 'hello');
    expect(screen.getByRole('alert')).toHaveTextContent('Drop a .nam file to load a capture.');
    await drop('Broken.nam', '{ not json');
    expect(screen.getByRole('alert')).toHaveTextContent(
      /This isn’t a NAM capture.*Choose another file to try again\./,
    );
    await drop(
      'Bench.nam',
      namFile({ name: 'Bench Amp', modeled_by: 'Tester', gear_type: 'amp_cab' }),
    );
    expect(screen.getByRole('heading', { name: 'Bench Amp' })).toBeInTheDocument();
    expect(screen.getByText('Tester')).toBeInTheDocument();
    expect(screen.queryByRole('alert')).toBeNull();
  });

  it('says which capture is missing and offers to find it', async () => {
    const { mock } = await start();
    await click(card('Capture'));
    act(() => {
      mock.captures.setMissing('Old Friend', 'Old Friend.nam', '/Volumes/Gone/Old Friend.nam');
      useAppStore.setState({ capture: mock.captures.snapshot() });
    });
    expect(screen.getByRole('heading', { name: 'Old Friend is missing' })).toBeInTheDocument();
    expect(screen.getByText(/at \/Volumes\/Gone\/Old Friend\.nam/)).toBeInTheDocument();
    await click(screen.getByRole('button', { name: 'Locate file…' }));
    expect(screen.queryByRole('heading', { name: /is missing/ })).toBeNull();
  });

  it('announces loading in a live region that is always there', async () => {
    await start();
    await click(card('Capture'));
    const status = screen.getByRole('status');
    expect(status).toBeEmptyDOMElement();
    act(() => useAppStore.setState((s) => ({ capture: { ...s.capture, loading: true } })));
    expect(status).toHaveTextContent('Loading capture…');
  });
});

describe('Capture parameters', () => {
  it('keeps the interface level out of presets and shows it in dBu', () => {
    expect(isGlobalParam('captureInterfaceLevel')).toBe(true);
    expect(isGlobalParam('captureInput')).toBe(false);
    const spec = bundledSchema.find((p) => p.id === 'captureInterfaceLevel');
    expect(spec && formatValue(spec, 12)).toBe('+12.0 dBu');
  });

  it('reads metadata the way the plug-in describes it', () => {
    const info = describeCapture(
      'X.nam',
      namFile({ gear_make: 'Make', gear_model: 'Model', gear_type: 'amp', input_level_dbu: 9.5 }),
    );
    expect(info).toMatchObject({
      name: 'X',
      gear: 'Make Model',
      kind: 'amp',
      architecture: 'Linear',
      inputLevelDbu: 9.5,
      sampleRate: 48000,
    });
    expect(() => describeCapture('Y.nam', '{"weights": []}')).toThrow(/isn’t a NAM capture/);
  });
});
