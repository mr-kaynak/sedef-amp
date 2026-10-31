// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { setupMock, type Harness } from '../../test/harness';
import { IntonationWorkbench } from './IntonationWorkbench';
import { useIntonationStore, type StringResult } from './store';

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
    expect(screen.getByText(/bring it to/)).toBeInTheDocument();

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
