// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { getValue } from '../../params/paramStore';
import { flush, setupMock, type Harness } from '../../test/harness';
import { Header } from './Header';

describe('Doubler switch in the header', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  it('switches the Doubler from any page, as one host gesture', async () => {
    const call = vi.spyOn(h.mock, 'call');
    render(<Header />);
    const toggle = screen.getByRole('switch', { name: 'Doubler' });
    expect(toggle).toHaveAttribute('aria-checked', 'false');
    fireEvent.click(toggle);
    expect(toggle).toHaveAttribute('aria-checked', 'true');
    expect(getValue('doublerEnabled')).toBe(1);
    await flush();
    const names = call.mock.calls.map(([name]) => name);
    expect(names).toEqual(['sedef_beginGesture', 'sedef_setParam', 'sedef_endGesture']);
    expect(h.mock.value('doublerEnabled')).toBe(1);
    fireEvent.click(toggle);
    expect(getValue('doublerEnabled')).toBe(0);
  });

  it('says where its settings are', () => {
    render(<Header />);
    expect(screen.getByRole('switch', { name: 'Doubler' })).toHaveAttribute(
      'title',
      expect.stringContaining('FX page'),
    );
  });
});
