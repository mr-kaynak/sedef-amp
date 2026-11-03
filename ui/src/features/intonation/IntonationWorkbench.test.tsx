// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { pitchEvent } from '../../app/demoHook';
import { pitch } from '../../state/live';
import { setupMock, type Harness } from '../../test/harness';
import { IntonationWorkbench } from './IntonationWorkbench';
import { useIntonationStore, type StringResult } from './store';
import workbenchCss from './Workbench.module.css?raw';

describe('Intonation workbench', () => {
  let h: Harness;
  beforeEach(async () => {
    vi.useFakeTimers({ toFake: ['performance'] });
    h = await setupMock({ intonationErrors: [4, -3, 0.4, 1, 1, 1] });
  });
  afterEach(() => {
    h.dispose();
    vi.useRealTimers();
  });

  /** Plays the simulated guitar at the plugin's 60 Hz until the condition holds (or 20 s pass). */
  const playUntil = (condition: () => boolean) => {
    for (let i = 0; i < 1200 && !condition(); i += 1) {
      act(() => {
        vi.advanceTimersByTime(1000 / 60);
        h.mock.tickPitch(1 / 60);
      });
    }
  };
  const result = (cents: number): StringResult => ({
    openHz: 82.41,
    frettedHz: 2 * 82.41 * Math.pow(2, cents / 1200),
    cents,
    stdCents: 0.1,
    measuredAt: 0,
  });

  it('walks from instrument choice to a saddle recommendation', () => {
    render(<IntonationWorkbench />);

    fireEvent.click(screen.getByRole('radio', { name: /6-string, standard/ }));
    fireEvent.click(screen.getByRole('radio', { name: /25\.5 in/ }));
    fireEvent.click(screen.getByRole('button', { name: 'Start measuring' }));

    expect(screen.getByText('String 6')).toBeInTheDocument();
    expect(screen.getByText(/tune it to/)).toBeInTheDocument();

    playUntil(() => useIntonationStore.getState().step === 'fret');
    expect(
      screen.getByRole('progressbar', { name: 'Capturing the 12th-fret note' }),
    ).toBeInTheDocument();

    playUntil(() => useIntonationStore.getState().step === 'result');
    expect(screen.getByText('Move the saddle back, away from the neck')).toBeInTheDocument();
    // +4 cents on 648 mm is about 1.50 mm.
    expect(screen.getByText(/^1\.(4[5-9]|5[0-5]) mm$/, { selector: 'strong' })).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Next string' }));
    expect(screen.getByText('String 5')).toBeInTheDocument();
    playUntil(() => useIntonationStore.getState().step === 'result');
    expect(screen.getByText('Move the saddle forward, toward the neck')).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Next string' }));
    playUntil(() => useIntonationStore.getState().step === 'result');
    expect(screen.getByText('In tune, leave the saddle where it is')).toBeInTheDocument();
  }, 15_000); // a whole string through the simulated player; slow on a loaded machine

  describe('tuning the open string', () => {
    const startMeasuring = async (options: {
      tuningOffsets: number[];
      intonationErrors?: number[];
    }) => {
      h.dispose();
      h = await setupMock({ intonationErrors: [1.5], ...options });
      render(<IntonationWorkbench />);
      fireEvent.click(screen.getByRole('radio', { name: /6-string, standard/ }));
      fireEvent.click(screen.getByRole('button', { name: 'Start measuring' }));
    };

    it('moves on from a string that is close, though it never sits still', async () => {
      await startMeasuring({ tuningOffsets: [2.5] });
      playUntil(() => screen.queryByText('Good enough, measuring…') !== null);
      expect(screen.getByText('Good enough, measuring…')).toBeInTheDocument();
      // The gauge agrees: 2.5 cents sharp is inside its good-enough zone, not "sharp".
      act(() => pitch.publish(pitchEvent(40, 2.5)));
      expect(document.querySelector('[data-state="in-tune"]')).not.toBeNull();
      expect(
        screen.getByRole('progressbar', { name: 'Measuring the open string' }),
      ).toBeInTheDocument();
      playUntil(() => useIntonationStore.getState().step === 'fret');
      expect(
        screen.getByRole('progressbar', { name: 'Capturing the 12th-fret note' }),
      ).toBeInTheDocument();
    });

    it('offers a string 8 cents flat as it is, and measures against it', async () => {
      await startMeasuring({ tuningOffsets: [-8] });
      playUntil(() => screen.queryByRole('button', { name: 'Use this tuning' }) !== null);
      expect(screen.getByText(/^[89]\.\d cents flat, tune up a little\.$/)).toBeInTheDocument();
      const offered = useIntonationStore.getState().tuneOffer?.hz;
      fireEvent.click(screen.getByRole('button', { name: 'Use this tuning' }));
      expect(useIntonationStore.getState().step).toBe('fret');
      expect(useIntonationStore.getState().openHz).toBe(offered);
      expect(
        screen.getByText(new RegExp(`open string read ${offered?.toFixed(2)} Hz`)),
      ).toBeInTheDocument();

      playUntil(() => useIntonationStore.getState().step === 'result');
      // The 1.5-cent error, measured against the string as it is rather than against E2.
      expect(screen.getByLabelText('+1.5¢ cents')).toBeInTheDocument();
    });

    it('says what it hears when the 12th-fret step hears another note', async () => {
      await startMeasuring({ tuningOffsets: [2] });
      playUntil(() => useIntonationStore.getState().step === 'fret');
      act(() => pitch.publish(pitchEvent(59, 0)));
      expect(
        screen.getByText("That's B3; play the 12th fret of the E2 string."),
      ).toBeInTheDocument();
      // The octave itself, even half a semitone from the grid, is not called a wrong note.
      const openHz = useIntonationStore.getState().openHz ?? 0;
      act(() =>
        pitch.publish({ ...pitchEvent(52, 0), frequencyHz: 2 * openHz * Math.pow(2, 30 / 1200) }),
      );
      expect(screen.queryByText(/play the 12th fret/)).toBeNull();
    });

    it('keeps guiding towards the note when the string is on the wrong one', async () => {
      await startMeasuring({ tuningOffsets: [70] });
      playUntil(() => false);
      expect(useIntonationStore.getState().step).toBe('tune');
      expect(screen.queryByRole('button', { name: 'Use this tuning' })).toBeNull();
      expect(screen.getByText(/That's F2\. Tune down to E2\./)).toBeInTheDocument();
    });

    it('reports the octave against the open string as measured', async () => {
      await startMeasuring({ tuningOffsets: [3], intonationErrors: [-2] });
      playUntil(() => useIntonationStore.getState().step === 'result');
      const r = useIntonationStore.getState().results[0];
      expect(r?.openHz).toBeGreaterThan(82.45);
      expect(
        screen.getByText(new RegExp(`against an open ${r?.openHz.toFixed(2)} Hz`)),
      ).toBeInTheDocument();
      expect(screen.getByLabelText('\u22122.0¢ cents')).toBeInTheDocument();
    });
  });

  it('announces each step and what the pluck is doing in one live region', () => {
    render(<IntonationWorkbench />);
    fireEvent.click(screen.getByRole('radio', { name: /6-string, standard/ }));
    fireEvent.click(screen.getByRole('button', { name: 'Start measuring' }));
    const region = screen.getByText('String 6, tune open string.');
    expect(region).toHaveAttribute('role', 'status');

    playUntil(() => useIntonationStore.getState().step === 'fret');
    // The same element: it outlives the step that moved on by itself.
    expect(region).toHaveTextContent('String 6, play 12th fret.');
    playUntil(() => useIntonationStore.getState().pluck === 'measuring');
    expect(region).toHaveTextContent('Hold it, measuring.');
    playUntil(() => useIntonationStore.getState().step === 'result');
    expect(region).toHaveTextContent(
      /^String 6, adjust saddle\. 4\.\d cents sharp\. Move the saddle back, away from the neck\.$/,
    );
  });

  it('summarises measured strings and offers re-measurement', () => {
    useIntonationStore.setState({
      phase: 'summary',
      results: [
        { openHz: 82.41, frettedHz: 165.2, cents: 3.6, stdCents: 0.1, measuredAt: 0 },
        null,
        null,
        null,
        null,
        null,
      ],
    });
    render(<IntonationWorkbench />);
    expect(screen.getByText(/0 of 6 strings in tune|1 of 6 strings in tune/)).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Copy as text' })).toBeInTheDocument();
    fireEvent.click(screen.getAllByRole('button', { name: 'Re-measure' })[0]!);
    expect(useIntonationStore.getState()).toMatchObject({
      phase: 'measure',
      current: 0,
      step: 'tune',
    });
  });

  it('leaves the summary table selectable when the clipboard is unavailable', async () => {
    useIntonationStore.setState({
      phase: 'summary',
      results: [3.6, -2, 1, 0.5, 4, -1].map(result),
    });
    render(<IntonationWorkbench />);
    // jsdom has neither the async clipboard nor execCommand, as in a locked-down WebView.
    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: 'Copy as text' }));
    });
    expect(screen.getByText(/Select the table and copy it instead/)).toHaveAttribute(
      'role',
      'status',
    );

    // The body is unselectable (global.css), so the table must opt back in, or the fallback the
    // message offers is a dead end. jsdom does not apply CSS modules: read the rule instead.
    expect(screen.getByRole('table')).toHaveClass('table');
    const rule = /(?:^|\n)\.table \{([^}]*)\}/.exec(workbenchCss)?.[1] ?? '';
    expect(rule).toMatch(/(?:^|\s)user-select: text;/);
    expect(rule).toMatch(/-webkit-user-select: text;/);
  });

  it('returns to the summary after re-measuring a string', () => {
    useIntonationStore.setState({
      phase: 'summary',
      results: [3.6, -2, 1, 0.5, 4, -1].map(result),
    });
    render(<IntonationWorkbench />);
    fireEvent.click(screen.getAllByRole('button', { name: 'Re-measure' })[0]!);
    fireEvent.click(screen.getByRole('button', { name: 'Back to summary' }));
    expect(useIntonationStore.getState().phase).toBe('summary');

    fireEvent.click(screen.getAllByRole('button', { name: 'Re-measure' })[0]!);
    playUntil(() => useIntonationStore.getState().step === 'result');
    expect(screen.queryByRole('button', { name: 'Next string' })).toBeNull();
    fireEvent.click(screen.getByRole('button', { name: 'See summary' }));
    expect(useIntonationStore.getState().phase).toBe('summary');
    expect(useIntonationStore.getState().results[0]?.cents).toBeCloseTo(4, 0);
  });

  it('after a string picked from the summary, goes on to the next unmeasured one', () => {
    useIntonationStore.setState({
      phase: 'summary',
      results: [result(3.6), null, result(1), null, result(4), result(-1)],
    });
    render(<IntonationWorkbench />);
    fireEvent.click(screen.getAllByRole('button', { name: 'Measure' })[1]!);
    expect(useIntonationStore.getState().current).toBe(3);
    playUntil(() => useIntonationStore.getState().step === 'result');
    fireEvent.click(screen.getByRole('button', { name: 'Next string' }));
    expect(useIntonationStore.getState()).toMatchObject({ phase: 'measure', current: 1 });
  });
});
