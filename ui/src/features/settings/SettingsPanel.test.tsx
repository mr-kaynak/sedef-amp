// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { StatusBar } from '../../app/StatusBar';
import { useAppStore } from '../../state/appStore';
import { useUiStore } from '../../state/uiStore';
import { setupMock, type Harness } from '../../test/harness';
import { SettingsPanel } from './SettingsPanel';

describe('Settings panel', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock();
  });
  afterEach(() => h.dispose());

  it('offers the window size as a segmented radio group with one tab stop', () => {
    const call = vi.spyOn(h.mock, 'call');
    render(<SettingsPanel />);

    // The same control as the stereo-input choice under it: the shared Segmented classes.
    const group = screen.getByRole('radiogroup', { name: 'Window size' });
    expect(group).toHaveClass('segmented');
    const options = within(group).getAllByRole('radio');
    expect(options.map((o) => o.textContent)).toEqual(['75%', '100%', '125%', '150%', '200%']);
    for (const o of options) expect(o).toHaveClass('option');
    expect(options.map((o) => o.getAttribute('aria-checked'))).toEqual([
      'false',
      'true',
      'false',
      'false',
      'false',
    ]);
    expect(options.map((o) => o.tabIndex)).toEqual([-1, 0, -1, -1, -1]);

    // Arrow keys move the choice and the focus together; the plugin resizes its window.
    options[1]!.focus();
    fireEvent.keyDown(options[1]!, { key: 'ArrowRight' });
    const larger = screen.getByRole('radio', { name: '125%' });
    expect(larger).toHaveAttribute('aria-checked', 'true');
    expect(larger).toHaveFocus();
    expect(larger.tabIndex).toBe(0);
    expect(useUiStore.getState().scale).toBe(1.25);
    expect(call).toHaveBeenCalledWith('sedef_ui_setScale', 1.25);

    fireEvent.keyDown(larger, { key: 'Home' });
    expect(screen.getByRole('radio', { name: '75%' })).toHaveFocus();
    expect(useUiStore.getState().scale).toBe(0.75);

    fireEvent.click(screen.getByRole('radio', { name: '200%' }));
    expect(useUiStore.getState().scale).toBe(2);
    expect(call).toHaveBeenLastCalledWith('sedef_ui_setScale', 2);
  });

  it('switches the latency between low latency and linear phase, and shows the new figure', async () => {
    const call = vi.spyOn(h.mock, 'call');
    render(
      <>
        <SettingsPanel />
        <StatusBar />
      </>,
    );

    // Low latency is the default: the rig's 41 samples at 48 kHz.
    const group = screen.getByRole('radiogroup', { name: 'Latency' });
    const options = within(group).getAllByRole('radio');
    expect(options.map((o) => o.textContent)).toEqual(['Low latency', 'Linear phase']);
    expect(options.map((o) => o.getAttribute('aria-checked'))).toEqual(['true', 'false']);
    expect(screen.getByText(/adds about 5 ms at 48 kHz/)).toBeInTheDocument();
    expect(screen.getByText('0.9 ms latency')).toBeInTheDocument();
    expect(screen.getByText(/41 samples latency/)).toBeInTheDocument();

    // Linear phase: the plug-in prepares again and reports the longer latency.
    fireEvent.click(screen.getByRole('radio', { name: 'Linear phase' }));
    await waitFor(() => expect(useAppStore.getState().session.latencySamples).toBe(298));
    expect(call).toHaveBeenCalledWith('sedef_setParam', 'oversampling', 1);
    expect(screen.getByText('6.2 ms latency')).toBeInTheDocument();
    expect(screen.getByText(/298 samples latency/)).toBeInTheDocument();
    expect(screen.getByRole('radio', { name: 'Linear phase' })).toHaveAttribute(
      'aria-checked',
      'true',
    );

    fireEvent.click(screen.getByRole('radio', { name: 'Low latency' }));
    await waitFor(() => expect(useAppStore.getState().session.latencySamples).toBe(41));
  });
});

describe('Settings, audio', () => {
  let h: Harness;
  afterEach(() => h.dispose());

  it('opens the audio device window from the standalone app only', async () => {
    h = await setupMock({ isStandalone: true, wrapperType: 'Standalone' });
    const call = vi.spyOn(h.mock, 'call');
    const { unmount } = render(<SettingsPanel />);
    fireEvent.click(screen.getByRole('button', { name: 'Audio settings…' }));
    expect(call).toHaveBeenCalledWith('sedef_audioSettings');
    unmount();
    h.dispose();

    h = await setupMock({ isStandalone: false, wrapperType: 'VST3' });
    render(<SettingsPanel />);
    expect(screen.queryByRole('button', { name: 'Audio settings…' })).toBeNull();
  });
});
