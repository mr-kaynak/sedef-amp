// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, render } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { pitchEvent } from '../../app/demoHook';
import { pitch } from '../../state/live';
import { resetStores } from '../../test/harness';
import { TunerGauge } from './TunerGauge';

const LOW_E = 40;
const target = { kind: 'target' as const, midi: LOW_E };

describe('TunerGauge in-tune zone', () => {
  beforeEach(() => resetStores());
  afterEach(() => resetStores());

  const stateAt = (cents: number, inTuneCents?: number) => {
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
