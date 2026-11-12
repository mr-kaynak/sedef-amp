// SPDX-License-Identifier: AGPL-3.0-or-later
// The input level chip, its panel and the places that point at it (docs/ui/input-level.md).
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { App } from '../../app/App';
import { flush, setupMock, type Harness } from '../../test/harness';
import { useUiStore } from '../../state/uiStore';
import { useInputLevel, type InputLevelState } from './inputLevel';
import { InputLevelChip } from './InputLevelChip';
import { LevelMeter } from './LevelMeter';
import { telemetry } from '../../state/live';

const CHIP: [InputLevelState, string, string][] = [
  ['none', 'No signal', 'No signal'],
  ['waiting', 'Check level', 'Check your level'],
  ['quiet', 'Too quiet', 'Too quiet'],
  ['good', 'Level good', 'Level good'],
  ['hot', 'Too hot', 'Too hot'],
  ['clip', 'Too hot', 'Your interface is clipping'],
];

describe('Input level chip', () => {
  let h: Harness;
  afterEach(() => h.dispose());

  it.each(CHIP)(
    'says %s in words, and the panel says what to do',
    async (state, chip, headline) => {
      h = await setupMock();
      render(<InputLevelChip />);
      act(() => useInputLevel.setState({ state, loudestDb: state === 'none' ? null : -9 }));
      const button = screen.getByRole('button', { name: `${chip}, input level` });
      expect(button).toHaveAttribute('aria-haspopup', 'dialog');
      expect(button).toHaveAttribute('aria-expanded', 'false');

      fireEvent.click(button);
      const panel = screen.getByRole('dialog', { name: 'Input level' });
      expect(button).toHaveAttribute('aria-expanded', 'true');
      const status = within(panel).getByRole('status');
      expect(status).toHaveTextContent(headline);
      // Focus starts on the panel's Input knob.
      expect(document.activeElement).toBe(
        within(panel).getByRole('slider', { name: /Input Gain/ }),
      );
      if (state === 'none') expect(within(panel).queryByText(/Loudest peak/)).toBeNull();
      else expect(within(panel).getByText(/Loudest peak/)).toHaveTextContent('−9 dB');
    },
  );

  it('points the standalone app at its audio settings, and a plug-in at the track', async () => {
    h = await setupMock({ isStandalone: true });
    const call = vi.spyOn(h.mock, 'call');
    const { unmount } = render(<InputLevelChip />);
    act(() => useInputLevel.setState({ state: 'none', loudestDb: null }));
    fireEvent.click(screen.getByRole('button', { name: 'No signal, input level' }));
    expect(screen.getByRole('status')).toHaveTextContent(/switch off Mute audio input/);
    fireEvent.click(screen.getByRole('button', { name: 'Audio settings…' }));
    expect(call).toHaveBeenCalledWith('sedef_audioSettings');
    unmount();
    h.dispose();

    h = await setupMock({ isStandalone: false, wrapperType: 'VST3' });
    render(<InputLevelChip />);
    act(() => useInputLevel.setState({ state: 'none', loudestDb: null }));
    fireEvent.click(screen.getByRole('button', { name: 'No signal, input level' }));
    expect(screen.getByRole('status')).toHaveTextContent(/this track's input/);
    expect(screen.queryByRole('button', { name: 'Audio settings…' })).toBeNull();
  });

  it('follows the telemetry: the mock strums in the zone', async () => {
    h = await setupMock();
    render(<InputLevelChip />);
    // Two seconds of simulated playing: the strums peak between −11 and −3 dBFS.
    act(() => {
      for (let i = 0; i < 60; i += 1) h.mock.tickTelemetry();
    });
    expect(screen.getByRole('button', { name: 'Level good, input level' })).toBeInTheDocument();
  });

  it('opens from the tuner when nothing is coming in', async () => {
    h = await setupMock();
    render(<App />);
    fireEvent.keyDown(window, { key: 't' });
    expect(useUiStore.getState().overlay).toBe('tuner');
    const tuner = screen.getByRole('region', { name: 'Tuner' });
    expect(within(tuner).queryByRole('button', { name: 'Check input' })).toBeNull();
    act(() => useInputLevel.setState({ state: 'none', loudestDb: null }));
    fireEvent.click(within(tuner).getByRole('button', { name: 'Check input' }));
    expect(screen.getByRole('dialog', { name: 'Input level' })).toBeInTheDocument();
    // T again closes the tuner.
    fireEvent.keyDown(window, { key: 'Escape' });
    fireEvent.keyDown(window, { key: 'T' });
    expect(useUiStore.getState().overlay).toBeNull();
    await act(flush);
  });
});

describe('Clip lamp', () => {
  let h: Harness;
  afterEach(() => h.dispose());

  it('says when it has latched, and goes back when clicked', async () => {
    h = await setupMock();
    vi.useFakeTimers();
    try {
      render(<LevelMeter which="input" label="Input" zones />);
      const lamp = screen.getByRole('button', { name: 'Input clip light' });
      act(() => {
        telemetry.publish({
          inputDb: [0.5],
          outputDb: [-6],
          inputClip: true,
          outputClip: false,
          gateOpen: true,
          gateReductionDb: 0,
          tideReductionDb: 0,
          metronomeStep: -1,
          driftTimeMs: 500,
          cpu: 0.05,
        });
        vi.advanceTimersByTime(50);
      });
      expect(lamp).toHaveAccessibleName('Input clipped. Click to reset');
      fireEvent.click(lamp);
      expect(lamp).toHaveAccessibleName('Input clip light');
    } finally {
      vi.useRealTimers();
    }
  });
});
