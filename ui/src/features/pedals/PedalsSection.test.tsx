// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { getValue } from '../../params/paramStore';
import { setupMock, type Harness } from '../../test/harness';
import { PedalsSection } from './PedalsSection';

describe('Pedals page', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  it('lays the seven pedals out in signal order', () => {
    render(<PedalsSection />);
    const names = screen.getAllByRole('region').map((region) => region.getAttribute('aria-label'));
    expect(names).toEqual([
      'Trench Octave down',
      'Tide Compressor',
      'Urchin Octave-up fuzz',
      'Pearl Overdrive',
      'Kelp Mid overdrive',
      'Coral Distortion',
      'Ripple Modulation',
    ]);
  });

  it('switches each new pedal with its footswitch', () => {
    render(<PedalsSection />);
    for (const [name, param] of [
      ['Trench', 'trenchEnabled'],
      ['Urchin', 'urchinEnabled'],
      ['Kelp', 'kelpEnabled'],
    ] as const) {
      const toggle = screen.getByRole('switch', { name: `${name} on/off` });
      expect(toggle).toHaveAttribute('aria-checked', 'false');
      fireEvent.click(toggle);
      expect(getValue(param)).toBe(1);
      expect(toggle).toHaveAttribute('aria-checked', 'true');
    }
  });

  it('gives every knob of the new pedals a name and a value', () => {
    render(<PedalsSection />);
    const knobs = (pedal: string) =>
      within(screen.getByRole('region', { name: new RegExp(`^${pedal} `) }))
        .getAllByRole('slider')
        .map((s) => s.getAttribute('aria-label'));
    // Knobs are named after their parameter, as the host shows it.
    expect(knobs('Trench')).toEqual([
      'Trench Sub -1',
      'Trench Sub -2',
      'Trench Direct',
      'Trench Tone',
    ]);
    expect(knobs('Urchin')).toEqual([
      'Urchin Fuzz',
      'Urchin Octave',
      'Urchin Tone',
      'Urchin Level',
    ]);
    expect(knobs('Kelp')).toEqual(['Kelp Drive', 'Kelp Tone', 'Kelp Level']);
  });

  it('reads the playing hint with the footswitch it belongs to', () => {
    render(<PedalsSection />);
    expect(screen.getByRole('switch', { name: 'Trench on/off' })).toHaveAccessibleDescription(
      'One note at a time',
    );
    expect(screen.getByRole('switch', { name: 'Urchin on/off' })).toHaveAccessibleDescription(
      'Neck pickup, above the 12th fret',
    );
    expect(screen.getByRole('switch', { name: 'Kelp on/off' })).not.toHaveAttribute(
      'aria-describedby',
    );
  });
});
