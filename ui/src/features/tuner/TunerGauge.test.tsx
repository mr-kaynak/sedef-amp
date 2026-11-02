// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { pitchEvent } from '../../app/demoHook';
import { createRandom, gaussian } from '../../bridge/mock/random';
import { pitch } from '../../state/live';
import { resetStores } from '../../test/harness';
import { TunerGauge } from './TunerGauge';

const LOW_E = 40;
const target = { kind: 'target' as const, midi: LOW_E };

describe('TunerGauge in-tune zone', () => {
  beforeEach(() => resetStores());
  afterEach(() => resetStores());

  // A fresh gauge on one settled reading (nothing left in the channel from the last one).
  const stateAt = (cents: number, inTuneCents?: number) => {
    pitch.clear();
    const { container, unmount } = render(
      <TunerGauge target={target} compact inTuneCents={inTuneCents} />,
    );
    act(() => pitch.publish(pitchEvent(LOW_E, cents)));
    const gauge = container.firstElementChild as HTMLElement;
    const state = gauge.dataset.state;
    const zone = gauge.querySelector('path')?.getAttribute('d');
    unmount();
    return { state, zone };
  };

  it('keeps the tuner precise by default', () => {
    expect(stateAt(0.8).state).toBe('in-tune');
    expect(stateAt(3).state).toBe('sharp');
    expect(stateAt(-3).state).toBe('flat');
  });

  it('shows a wider tolerance as in tune, with a wider band', () => {
    expect(stateAt(3, 5).state).toBe('in-tune');
    expect(stateAt(-4.9, 5).state).toBe('in-tune');
    expect(stateAt(5.5, 5).state).toBe('sharp');
    expect(stateAt(-5.5, 5).state).toBe('flat');
    // The band's ends move out with the tolerance.
    const startX = (d: string | null | undefined) => Number(d?.match(/^M([\d.]+)/)?.[1]);
    expect(startX(stateAt(0, 5).zone)).toBeLessThan(startX(stateAt(0).zone) - 10);
  });
});

describe('TunerGauge announcements', () => {
  beforeEach(() => {
    resetStores();
    vi.useFakeTimers({ toFake: ['performance'] });
  });
  afterEach(() => {
    vi.useRealTimers();
    resetStores();
  });

  /** Plays readings at 60 Hz and returns what the live region said, in order. */
  const listen = (seconds: number, at: (t: number) => Parameters<typeof pitch.publish>[0]) => {
    const region = screen.getByRole('status');
    const said: string[] = [];
    for (let i = 0; i < seconds * 60; i += 1) {
      act(() => {
        vi.advanceTimersByTime(1000 / 60);
        pitch.publish(at(i / 60));
      });
      const text = region.textContent ?? '';
      if (text && text !== said[said.length - 1]) said.push(text);
    }
    return said;
  };

  it('says a settled state once, not every wobble at the edge of the zone', () => {
    render(<TunerGauge />);
    const random = createRandom(5);
    const said = listen(10, () => pitchEvent(LOW_E, 0.9 + 0.2 * gaussian(random)));
    expect(said).toEqual(['E 2, in tune']);
  });

  it('stays quiet while the note dies and the reading holds', () => {
    render(<TunerGauge />);
    const said = listen(4, (t) => {
      const e = pitchEvent(LOW_E, -7);
      return t < 1 ? e : { ...e, valid: false, displayHeld: true, displaySettled: false };
    });
    expect(said).toEqual(['E 2, flat, tune up']);
  });

  it('follows a peg turned through the zone with a few words, not a running commentary', () => {
    render(<TunerGauge />);
    // From 15 cents flat to pitch over two seconds, the reading moving (not settled) meanwhile.
    const said = listen(5, (t) => {
      const cents = -15 + 15 * Math.min(1, Math.max(0, (t - 1) / 2));
      return pitchEvent(LOW_E, cents, { stable: t < 1 || t > 3.3 });
    });
    expect(said).toEqual(['E 2, flat, tune up', 'E 2, in tune']);
  });
});
