// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, cleanup, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { switchedOnBoardFor } from '../features/pedals/registry';
import { useBoardStore } from '../state/boardStore';
import { setupMock, type Harness } from '../test/harness';
import { SectionTabs } from './SectionTabs';

describe('the Pedals tab', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => {
    cleanup();
    h.dispose();
  });

  // The lamps are pictures (aria-hidden): count them by their size.
  const lamps = () =>
    [
      ...screen
        .getByRole('tab', { name: /^Pedals/ })
        .querySelectorAll<HTMLElement>('span[aria-hidden="true"]'),
    ].filter((el) => el.style.width === '5px').length;

  it('shows a lamp per pedal up to eight, and past eight five lamps and a count', () => {
    render(<SectionTabs />);
    const board = (pedals: string[]) => act(() => useBoardStore.setState({ pedals }));
    board(['gate', 'tide', 'pearl', 'coral', 'ripple', 'kelp', 'gyre', 'jetty']);
    expect(lamps()).toBe(8);
    expect(screen.queryByText(/^\+\d+$/)).toBeNull();
    // The largest board: never wider than eight lamps, the row being full.
    const largest = switchedOnBoardFor(() => true);
    board([...largest.slice(0, 20), 'amp', ...largest.slice(20)]);
    expect(lamps()).toBe(5);
    expect(screen.getByText('+20')).toBeInTheDocument();
  });
});
