// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { MockBackend } from '../../bridge/mockBackend';
import type { UpdateScenario } from '../../bridge/mock/updateSim';
import { flush, setupMock, type Harness } from '../../test/harness';
import { useUiStore } from '../../state/uiStore';
import { PresetBrowser } from '../presets/PresetBrowser';
import { presetBrowser, useBrowserStore } from '../presets/session';
import { Header } from '../header/Header';
import { formatBytes, formatSince } from './text';
import { useUpdateStore } from './updateStore';

let h: Harness | null = null;

async function start(update: UpdateScenario): Promise<Harness> {
  h = await setupMock({ update });
  render(<Header />);
  await act(flush);
  return h;
}

/** Advances the mock's check or download by n steps and lets the events land. */
async function step(n = 1): Promise<void> {
  await act(async () => {
    for (let i = 0; i < n; i += 1) h?.mock.updates.step();
    await flush();
  });
}

const pill = () => screen.queryByRole('button', { name: /^Update available/ });
const dialog = () => screen.getByRole('dialog', { name: 'Updates' });

async function openPanel(): Promise<HTMLElement> {
  const button = pill();
  if (!button) throw new Error('no pill');
  fireEvent.click(button);
  await act(flush);
  return dialog();
}

afterEach(() => {
  h?.dispose();
  h = null;
  window.localStorage.clear();
});

describe('Update pill', () => {
  it('stays away when Sedef is up to date', async () => {
    await start('current');
    expect(pill()).toBeNull();
  });

  it('appears when a newer release is out', async () => {
    await start('available');
    expect(pill()).toHaveAccessibleName('Update available: Sedef Amp 0.2.0');
  });

  it('shows nothing about updates when the build has no checker', async () => {
    await start('unsupported');
    expect(pill()).toBeNull();
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    expect(screen.getByRole('dialog', { name: 'Settings' })).toBeInTheDocument();
    expect(screen.queryByText('Updates')).toBeNull();
  });

  it('asks the backend once on startup, which runs the automatic check', async () => {
    const spy = vi.spyOn(MockBackend.prototype, 'call');
    await start('current');
    expect(spy.mock.calls.filter(([name]) => name === 'sedef_update_getState')).toHaveLength(1);
    spy.mockRestore();
  });

  it('goes away for good when the version is skipped', async () => {
    const harness = await start('available');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Skip this version' }));
    await act(flush);
    expect(screen.queryByRole('dialog', { name: 'Updates' })).toBeNull();
    expect(pill()).toBeNull();
    const state = await harness.mock.call('sedef_update_getState');
    expect(state.supported && state.skippedVersion).toBe('0.2.0');
  });

  it('stays hidden for a skipped version, which the panel can offer again', async () => {
    await start('skipped');
    expect(pill()).toBeNull();
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    fireEvent.click(screen.getByRole('button', { name: 'Updates…' }));
    await act(flush);
    expect(within(dialog()).getByText('Skipped')).toBeInTheDocument();
    fireEvent.click(within(dialog()).getByRole('button', { name: 'Offer this version again' }));
    await act(flush);
    expect(pill()).toBeInTheDocument();
  });

  it('steps aside while the preset browser covers the tabs line', async () => {
    await start('available');
    act(() => useBrowserStore.setState({ open: true }));
    expect(pill()).toBeNull();
    act(() => useBrowserStore.setState({ open: false }));
    expect(pill()).toBeInTheDocument();
  });

  it('"Later" hides it until the next session', async () => {
    await start('available');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Later' }));
    expect(pill()).toBeNull();
    expect(useUpdateStore.getState().snoozed).toBe(true);
  });
});

describe('Updates panel', () => {
  it('shows versions, release notes and the install advice', async () => {
    await start('available');
    const panel = await openPanel();
    expect(panel).toHaveAttribute('aria-modal', 'true');
    expect(within(panel).getByText('0.1.0-dev')).toBeInTheDocument();
    expect(within(panel).getByText('0.2.0')).toBeInTheDocument();
    expect(within(panel).getByText('3 hours ago')).toBeInTheDocument();
    const notes = within(panel).getByRole('region', { name: 'Release notes' });
    expect(within(notes).getByRole('heading', { name: 'Added' })).toBeInTheDocument();
    expect(
      within(panel).getByText(/Quit your DAW before you run the installer/),
    ).toBeInTheDocument();
    expect(within(panel).getByRole('button', { name: 'Download & install' })).toBeEnabled();
  });

  it('opens release links in the system browser', async () => {
    const harness = await start('available');
    const panel = await openPanel();
    const spy = vi.spyOn(harness.mock, 'call');
    fireEvent.click(within(panel).getByRole('link', { name: 'how it works' }));
    expect(spy).toHaveBeenCalledWith(
      'sedef_openUrl',
      'https://github.com/mr-kaynak/sedef-amp/blob/main/docs/updates.md',
    );
  });

  it('moves focus in, keeps shortcuts out, and closes with Escape', async () => {
    await start('available');
    const button = pill();
    button?.focus();
    const panel = await openPanel();
    expect(within(panel).getByRole('heading', { name: 'Updates' })).toHaveFocus();
    fireEvent.keyDown(within(panel).getByRole('button', { name: 'Later' }), { key: '1' });
    expect(useUiStore.getState().section).toBe('amp');
    fireEvent.keyDown(document.activeElement ?? document.body, { key: 'Escape' });
    expect(screen.queryByRole('dialog', { name: 'Updates' })).toBeNull();
    expect(pill()).toHaveFocus();
  });

  it('takes Escape before the preset browser underneath', async () => {
    await start('available');
    render(<PresetBrowser />);
    act(() => presetBrowser.open());
    await act(flush);
    act(() => useUpdateStore.setState({ open: true }));
    await act(flush);
    fireEvent.keyDown(document.activeElement ?? document.body, { key: 'Escape' });
    expect(screen.queryByRole('dialog', { name: 'Updates' })).toBeNull();
    expect(useBrowserStore.getState().open).toBe(true);
    act(() => presetBrowser.cancel());
  });

  it('keeps Tab inside the panel', async () => {
    await start('available');
    const panel = await openPanel();
    const download = within(panel).getByRole('button', { name: 'Download & install' });
    download.focus();
    fireEvent.keyDown(download, { key: 'Tab' });
    expect(within(panel).getByRole('button', { name: 'Close' })).toHaveFocus();
  });

  it('remembers the automatic-check switch', async () => {
    const harness = await start('current');
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    fireEvent.click(screen.getByRole('button', { name: 'Updates…' }));
    const toggle = within(dialog()).getByRole('switch', {
      name: 'Check for updates automatically',
    });
    expect(toggle).toHaveAttribute('aria-checked', 'true');
    fireEvent.click(toggle);
    expect(toggle).toHaveAttribute('aria-checked', 'false');
    await act(flush);
    const state = await harness.mock.call('sedef_update_getState');
    expect(state.supported && state.autoCheck).toBe(false);

    // A backend that keeps settings (the browser preview) hands it to the next session.
    const first = new MockBackend({ persistKey: 'test', update: 'current' });
    first.updates.setAutoCheck(false);
    const second = new MockBackend({ persistKey: 'test', update: 'current' });
    const next = second.updates.getState();
    expect(next.supported && next.autoCheck).toBe(false);
  });

  it('checks now and reports the time', async () => {
    await start('current');
    useUpdateStore.setState({ open: true });
    await act(flush);
    const panel = dialog();
    expect(
      within(panel).getByText('Sedef Amp is up to date.', { exact: false }),
    ).toBeInTheDocument();
    fireEvent.click(within(panel).getByRole('button', { name: 'Check now' }));
    await act(flush);
    expect(within(panel).getByRole('button', { name: 'Check now' })).toBeDisabled();
    expect(within(panel).getByText('Checking…')).toBeInTheDocument();
    await step();
    expect(within(panel).getByText('just now')).toBeInTheDocument();
  });
});

describe('Download & install', () => {
  it('shows progress, then the installer and what to do next', async () => {
    await start('available');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Download & install' }));
    await act(flush);
    const bar = within(panel).getByRole('progressbar', { name: /Downloading Sedef-Amp-macOS.dmg/ });
    expect(bar).toHaveAttribute('aria-valuenow', '0');
    expect(bar).toHaveAttribute('aria-valuemax', '48600000');
    await step(4);
    expect(bar).toHaveAttribute('aria-valuenow', '24300000');
    expect(bar).toHaveAttribute('aria-valuetext', `${formatBytes(24300000)} of 48.6 MB`);
    await step(4);
    expect(within(panel).getByRole('progressbar', { name: /Checking/ })).not.toHaveAttribute(
      'aria-valuenow',
    );
    await step();
    expect(within(panel).queryByRole('progressbar')).toBeNull();
    expect(within(panel).getByText('Sedef-Amp-macOS.dmg')).toBeInTheDocument();
    expect(within(panel).getByText(/The disk image is open\. Quit your DAW/)).toBeInTheDocument();
    expect(within(panel).getByRole('button', { name: 'Show in folder' })).toBeInTheDocument();
    expect(within(panel).getByRole('status')).toHaveTextContent('Download verified');
  });

  it('can be cancelled', async () => {
    await start('available');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Download & install' }));
    await step(2);
    fireEvent.click(within(panel).getByRole('button', { name: 'Cancel' }));
    await act(flush);
    expect(within(panel).queryByRole('progressbar')).toBeNull();
    expect(within(panel).getByRole('button', { name: 'Download & install' })).toBeInTheDocument();
    expect(within(panel).queryByRole('alert')).toBeNull();
  });
});

describe('Errors', () => {
  it('explains a failed check and offers the releases page', async () => {
    const harness = await start('error');
    expect(pill()).toBeNull();
    useUpdateStore.setState({ open: true });
    await act(flush);
    const panel = dialog();
    expect(within(panel).getByRole('alert')).toHaveTextContent("Sedef couldn't reach GitHub.");
    const spy = vi.spyOn(harness.mock, 'call');
    fireEvent.click(within(panel).getByRole('button', { name: 'Open releases page' }));
    expect(spy).toHaveBeenCalledWith(
      'sedef_openUrl',
      'https://github.com/mr-kaynak/sedef-amp/releases',
    );
  });

  it('deletes a download that fails verification and says so', async () => {
    await start('mismatch');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Download & install' }));
    await step(9);
    expect(within(panel).getByRole('alert')).toHaveTextContent(
      "didn't match its published checksum, so Sedef deleted it",
    );
    expect(within(panel).getByRole('button', { name: 'Open releases page' })).toBeInTheDocument();
    expect(within(panel).getByRole('button', { name: 'Download & install' })).toBeInTheDocument();
  });

  it('falls back to the releases page when there is no installer for this OS', async () => {
    const harness = await start('noinstaller');
    const panel = await openPanel();
    expect(within(panel).queryByRole('button', { name: 'Download & install' })).toBeNull();
    expect(within(panel).getByText(/no installer for macOS/)).toBeInTheDocument();
    const spy = vi.spyOn(harness.mock, 'call');
    fireEvent.click(within(panel).getByRole('button', { name: 'Open releases page' }));
    expect(spy).toHaveBeenCalledWith(
      'sedef_openUrl',
      'https://github.com/mr-kaynak/sedef-amp/releases/tag/v0.2.0',
    );
  });
});

describe('text', () => {
  it('formats times since the last check', () => {
    const now = Date.parse('2026-11-20T12:00:00Z');
    expect(formatSince(null, now)).toBe('never');
    expect(formatSince(now - 20_000, now)).toBe('just now');
    expect(formatSince(now - 60_000, now)).toBe('1 minute ago');
    expect(formatSince(now - 5 * 60_000, now)).toBe('5 minutes ago');
    expect(formatSince(now - 3 * 3_600_000, now)).toBe('3 hours ago');
    expect(formatSince(now - 30 * 3_600_000, now)).toBe('yesterday');
    expect(formatSince(Date.parse('2026-11-01T09:00:00Z'), now)).toBe('1 Nov 2026');
    expect(formatBytes(48_600_000)).toBe('48.6 MB');
    expect(formatBytes(290)).toBe('290 B');
  });
});
