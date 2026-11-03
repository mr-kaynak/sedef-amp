// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { getValue } from '../../params/paramStore';
import { flush, setupMock, type Harness } from '../../test/harness';
import { BreakerPedal, SargassoPedal, RemoraPedal } from './DrivePedals';

describe('drive pedals', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  it('Remora renders on its own with its four knobs and the transistor switch', () => {
    render(<RemoraPedal />);
    const pedal = screen.getByRole('region', { name: 'Remora Fuzz' });
    for (const name of ['Remora Fuzz', 'Remora Volume', 'Remora Bias', 'Remora Guitar Volume'])
      expect(within(pedal).getByRole('slider', { name })).toBeInTheDocument();
    // The guitar's own knob reads like one: 10 is full up.
    expect(within(pedal).getByRole('slider', { name: 'Remora Guitar Volume' })).toHaveAttribute(
      'aria-valuetext',
      '10.0',
    );
    expect(within(pedal).getByText('Guitar volume')).toBeInTheDocument();
    const transistor = within(pedal).getByRole('radiogroup', { name: 'Remora Transistor' });
    expect(within(transistor).getByRole('radio', { name: 'Germanium' })).toHaveAttribute(
      'aria-checked',
      'true',
    );
  });

  it('switches Remora to silicon from the keyboard', async () => {
    render(<RemoraPedal />);
    const germanium = screen.getByRole('radio', { name: 'Germanium' });
    fireEvent.keyDown(germanium, { key: 'ArrowRight' });
    expect(getValue('remoraTransistor')).toBe(1);
    expect(screen.getByRole('radio', { name: 'Silicon' })).toHaveAttribute('aria-checked', 'true');
    await flush();
    expect(h.mock.value('remoraTransistor')).toBe(1);
  });

  it('Sargasso renders on its own with sustain, tone and volume', () => {
    render(<SargassoPedal />);
    const pedal = screen.getByRole('region', { name: 'Sargasso Sustain fuzz' });
    for (const name of ['Sargasso Sustain', 'Sargasso Tone', 'Sargasso Volume'])
      expect(within(pedal).getByRole('slider', { name })).toBeInTheDocument();
  });

  it('Breaker renders on its own; its range switch and footswitch work', async () => {
    render(<BreakerPedal />);
    const pedal = screen.getByRole('region', { name: 'Breaker Treble booster' });
    expect(within(pedal).getByRole('slider', { name: 'Breaker Boost' })).toBeInTheDocument();
    fireEvent.click(within(pedal).getByRole('radio', { name: 'Full' }));
    expect(getValue('breakerRange')).toBe(1);
    fireEvent.click(within(pedal).getByRole('switch', { name: 'Breaker on/off' }));
    expect(getValue('breakerEnabled')).toBe(1);
    await flush();
    expect(h.mock.value('breakerEnabled')).toBe(1);
  });
});
