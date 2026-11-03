// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { MockBackend } from '../../bridge/mockBackend';
import type { UpdateScenario } from '../../bridge/mock/updateSim';
import type { UpdateState } from '../../bridge/protocol';
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
const restartPill = () => screen.queryByRole('button', { name: /^Restart to update/ });
const dialog = () => screen.getByRole('dialog', { name: 'Updates' });

async function openPanel(which: () => HTMLElement | null = pill): Promise<HTMLElement> {
  const button = which();
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
      within(panel).getByText(
        /installs it over the formats you have\. macOS asks for your password/,
      ),
    ).toBeInTheDocument();
    expect(within(panel).getByRole('button', { name: 'Update now' })).toBeEnabled();
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
    const update = within(panel).getByRole('button', { name: 'Update now' });
    update.focus();
    fireEvent.keyDown(update, { key: 'Tab' });
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

const steps = (state: UpdateState) =>
  state.supported && state.phase === 'installing'
    ? state.install?.step
    : state.supported && state.phase;

describe('Update now', () => {
  it('downloads, verifies, installs and asks for a restart', async () => {
    const harness = await start('available');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Update now' }));
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

    await step(); // verified: the OS's installer takes over
    expect(
      within(panel).getByRole('progressbar', { name: 'Preparing Sedef Amp 0.2.0…' }),
    ).toBeInTheDocument();
    expect(within(panel).queryByRole('button', { name: 'Cancel' })).toBeNull();
    await step();
    expect(
      within(panel).getByRole('progressbar', { name: 'Waiting for your password…' }),
    ).not.toHaveAttribute('aria-valuenow');
    expect(within(panel).getByText(/Enter your password when macOS asks/)).toHaveTextContent(
      'Keep this window open until it finishes.',
    );
    expect(within(panel).getByRole('status')).toHaveTextContent('Waiting for your password');
    await step();
    expect(
      within(panel).getByRole('progressbar', { name: 'Checking the installed files…' }),
    ).toBeInTheDocument();
    await step();

    expect(within(panel).queryByRole('progressbar')).toBeNull();
    expect(within(panel).getByText('Sedef Amp 0.2.0 is installed')).toBeInTheDocument();
    expect(within(panel).getByText('Restart your DAW to use it.')).toBeInTheDocument();
    expect(within(panel).queryByRole('button', { name: 'Update now' })).toBeNull();
    expect(within(panel).queryByRole('button', { name: 'Restart Sedef' })).toBeNull(); // a plug-in
    expect(within(panel).getByRole('button', { name: 'Done' })).toBeInTheDocument();
    expect(within(panel).getByRole('status')).toHaveTextContent(
      'Sedef Amp 0.2.0 is installed. Restart your DAW to use it.',
    );
    expect(pill()).toBeNull();
    expect(restartPill()).toHaveAccessibleName(
      'Restart to update: Sedef Amp 0.2.0 is installed. Restart your DAW to use it.',
    );
    const state = await harness.mock.call('sedef_update_getState');
    expect(state.supported && state.installedVersion).toBe('0.2.0');
  });

  it('keeps focus in the panel when the button it was on goes away', async () => {
    await start('available');
    const panel = await openPanel();
    const update = within(panel).getByRole('button', { name: 'Update now' });
    update.focus();
    fireEvent.click(update);
    await act(flush);
    expect(within(panel).getByRole('heading', { name: 'Updates' })).toHaveFocus();
  });

  it('can be cancelled while it downloads', async () => {
    await start('available');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Update now' }));
    await step(2);
    fireEvent.click(within(panel).getByRole('button', { name: 'Cancel' }));
    await act(flush);
    expect(within(panel).queryByRole('progressbar')).toBeNull();
    expect(within(panel).getByRole('button', { name: 'Update now' })).toBeInTheDocument();
    expect(within(panel).queryByRole('alert')).toBeNull();
  });

  it('goes back to ready when the password dialog is cancelled', async () => {
    const harness = await start('declined');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Update now' }));
    await step(9); // downloaded and verified
    await step(2); // preparing, then the password dialog
    expect(steps(await harness.mock.call('sedef_update_getState'))).toBe('idle');
    expect(within(panel).queryByRole('alert')).toBeNull();
    expect(within(panel).getByRole('button', { name: 'Update now' })).toBeEnabled();
    expect(pill()).toBeInTheDocument();
  });

  it('restarts the standalone app into the new version', async () => {
    await start('standalone');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Update now' }));
    await step(9 + 3);
    expect(within(panel).getByText('Restart Sedef to use it.')).toBeInTheDocument();
    expect(
      within(panel).getByText('Your DAWs load the new version the next time they start.'),
    ).toBeInTheDocument();
    expect(within(panel).getByRole('button', { name: 'Later' })).toBeInTheDocument();
    fireEvent.click(within(panel).getByRole('button', { name: 'Restart Sedef' }));
    await act(flush);
    expect(
      within(panel).getByText('Sedef Amp is up to date.', { exact: false }),
    ).toBeInTheDocument();
    expect(within(panel).getAllByText('0.2.0', { selector: 'dd' })).toHaveLength(2); // running it
  });

  it('installs on Linux without a password step', async () => {
    const harness = await start('linux');
    const panel = await openPanel();
    expect(
      within(panel).getByText(
        /For a copy installed for every user, your system asks for your password/,
      ),
    ).toBeInTheDocument();
    fireEvent.click(within(panel).getByRole('button', { name: 'Update now' }));
    await step(9);
    const seen: unknown[] = [];
    for (let i = 0; i < 3; i += 1) {
      seen.push(steps(await harness.mock.call('sedef_update_getState')));
      await step();
    }
    expect(seen).toEqual(['preparing', 'installing', 'finishing']);
    expect(within(panel).getByText('Sedef Amp 0.2.0 is installed')).toBeInTheDocument();
  });
});

describe('Windows', () => {
  it('asks for permission, then installs when the DAW closes', async () => {
    await start('windows');
    const panel = await openPanel();
    expect(
      within(panel).getByText(
        /Once Windows gives permission, it installs when you close your DAW\./,
      ),
    ).toBeInTheDocument();
    fireEvent.click(within(panel).getByRole('button', { name: 'Update now' }));
    await step(9 + 1);
    expect(
      within(panel).getByRole('progressbar', { name: 'Waiting for permission…' }),
    ).toBeInTheDocument();
    expect(within(panel).getByText(/Choose Yes when Windows asks/)).toBeInTheDocument();
    await step();
    expect(within(panel).getByText('Sedef Amp 0.2.0 is ready to install')).toBeInTheDocument();
    expect(within(panel).getByText('It installs when you close your DAW.')).toBeInTheDocument();
    expect(
      within(panel).getByText(/Windows finishes the update when you restart your computer/),
    ).toBeInTheDocument();
    expect(restartPill()).toBeInTheDocument();
  });

  it('stays scheduled across a check', async () => {
    await start('scheduled');
    expect(restartPill()).toHaveAccessibleName(
      'Restart to update: Sedef Amp 0.2.0 is ready to install. It installs when you close your DAW.',
    );
    const panel = await openPanel(restartPill);
    fireEvent.click(within(panel).getByRole('button', { name: 'Check now' }));
    await step();
    expect(within(panel).getByText('Sedef Amp 0.2.0 is ready to install')).toBeInTheDocument();
  });
});

describe('Install manually', () => {
  it('is the way forward after a failed install', async () => {
    await start('installfail');
    const panel = await openPanel();
    fireEvent.click(within(panel).getByRole('button', { name: 'Update now' }));
    await step(9 + 2);
    expect(within(panel).getByRole('alert')).toHaveTextContent(
      'The installer reported a problem and stopped. Try again, or use Install manually.',
    );
    expect(within(panel).getByRole('button', { name: 'Try again' })).toBeInTheDocument();
    expect(within(panel).getByRole('button', { name: 'Open releases page' })).toBeInTheDocument();
    expect(within(panel).queryByRole('button', { name: 'Skip this version' })).toBeNull();
    fireEvent.click(within(panel).getByRole('button', { name: 'Install manually' }));
    await step(9);
    expect(within(panel).getByText('Sedef-Amp-macOS.dmg')).toBeInTheDocument();
    expect(
      within(panel).getByText('Downloaded to your Downloads folder and verified.'),
    ).toBeInTheDocument();
    expect(within(panel).getByText(/The disk image is open\. Quit your DAW/)).toHaveTextContent(
      'Privacy & Security and click Open Anyway',
    );
    expect(within(panel).getByRole('button', { name: 'Show in folder' })).toBeInTheDocument();
    expect(within(panel).getByRole('status')).toHaveTextContent('Download verified');
  });

  it("is the only way where the automatic install can't run", async () => {
    const harness = await start('elsewhere');
    const panel = await openPanel();
    expect(within(panel).queryByRole('button', { name: 'Update now' })).toBeNull();
    expect(
      within(panel).getByText(/runs from a folder the installer doesn't update/),
    ).toHaveTextContent(
      'Install manually saves the disk image to your Downloads folder and opens it.',
    );
    fireEvent.click(within(panel).getByRole('button', { name: 'Install manually' }));
    await step(9);
    const state = await harness.mock.call('sedef_update_getState');
    expect(state.supported && state.phase).toBe('done');
  });
});

describe('Installed elsewhere', () => {
  it('another window or DAW installed it: restart, no second download', async () => {
    const harness = await start('installed');
    expect(pill()).toBeNull();
    const panel = await openPanel(restartPill);
    expect(within(panel).getByText('Sedef Amp 0.2.0 is installed')).toBeInTheDocument();
    expect(
      within(panel).getByText(
        'Any other DAW with Sedef open keeps the old version until you restart it too.',
      ),
    ).toBeInTheDocument();
    expect(within(panel).queryByRole('button', { name: 'Update now' })).toBeNull();
    await harness.mock.call('sedef_update_install');
    const state = await harness.mock.call('sedef_update_getState');
    expect(state.supported && state.phase).toBe('installed');
    fireEvent.click(within(panel).getByRole('button', { name: 'Done' }));
    expect(screen.queryByRole('dialog', { name: 'Updates' })).toBeNull();
    expect(restartPill()).toBeInTheDocument(); // the reminder stays
  });

  it('says so in Settings', async () => {
    await start('installed');
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    expect(screen.getByText('Sedef Amp 0.2.0 is installed')).toBeInTheDocument();
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
