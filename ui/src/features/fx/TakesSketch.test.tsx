// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { setLocalValue } from '../../params/paramStore';
import { setupMock, type Harness } from '../../test/harness';
import { FxSection } from './FxSection';

describe('Doubler panel', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  it('shows every control and how far the second take trails', () => {
    render(<FxSection />);
    for (const name of ['Doubler Spread', 'Doubler Variation', 'Doubler Width', 'Doubler Mix'])
      expect(screen.getByRole('slider', { name })).toBeInTheDocument();
    expect(screen.getByRole('img', { name: 'Second take, 4–9 ms behind the first' })).toBeVisible();
    act(() => {
      setLocalValue('doublerVariation', 0);
      setLocalValue('doublerWidth', 0);
    });
    expect(screen.getByRole('img', { name: 'Second take, 6 ms behind the first' })).toBeVisible();
    expect(screen.getByText('Both takes in the centre')).toBeInTheDocument();
  });
});
