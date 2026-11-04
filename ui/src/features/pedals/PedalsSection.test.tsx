// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { getValue } from '../../params/paramStore';
import { setupMock, type Harness } from '../../test/harness';
import { KelpPedal } from './KelpPedal';
import { PedalsSection } from './PedalsSection';
import { TrenchPedal } from './TrenchPedal';
import { UrchinPedal } from './UrchinPedal';

const PANELS = [
  {
    Panel: TrenchPedal,
    region: 'Trench Octave down',
    enable: 'trenchEnabled',
    // Knobs are named after their parameter, as the host shows it.
    knobs: ['Trench Sub -1', 'Trench Sub -2', 'Trench Direct', 'Trench Tone'],
    hint: 'One note at a time',
  },
  {
    Panel: UrchinPedal,
    region: 'Urchin Octave-up fuzz',
    enable: 'urchinEnabled',
    knobs: ['Urchin Fuzz', 'Urchin Octave', 'Urchin Tone', 'Urchin Level'],
    hint: 'Neck pickup, above the 12th fret',
  },
  {
    Panel: KelpPedal,
    region: 'Kelp Mid overdrive',
    enable: 'kelpEnabled',
    knobs: ['Kelp Drive', 'Kelp Tone', 'Kelp Level'],
    hint: undefined,
  },
] as const;

describe('Trench, Urchin and Kelp panels', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  // The pedalboard will host each panel on its own, in any order: a panel needs nothing from
  // the page around it.
  for (const { Panel, region, enable, knobs, hint } of PANELS) {
    it(`${region}: a whole pedal on its own`, () => {
      render(
        <div>
          <Panel />
        </div>,
      );
      const pedal = screen.getByRole('region', { name: region });
      expect(screen.getAllByRole('region')).toHaveLength(1);
      expect(
        within(pedal)
          .getAllByRole('slider')
          .map((s) => s.getAttribute('aria-label')),
      ).toEqual(knobs);

      const name = region.split(' ')[0];
      const toggle = within(pedal).getByRole('switch', { name: `${name} on/off` });
      expect(toggle).toHaveAttribute('aria-checked', 'false');
      fireEvent.click(toggle);
      expect(getValue(enable)).toBe(1);
      expect(toggle).toHaveAttribute('aria-checked', 'true');

      // The playing hint is engraved on the pedal and read with the footswitch.
      if (hint) {
        expect(within(pedal).getByText(hint)).toBeVisible();
        expect(toggle).toHaveAccessibleDescription(hint);
      } else {
        expect(toggle).not.toHaveAttribute('aria-describedby');
      }
    });
  }

  it('sit on the pedals page in signal order', () => {
    render(<PedalsSection />);
    const names = screen.getAllByRole('region').map((region) => region.getAttribute('aria-label'));
    expect(names).toEqual([
      'Trench Octave down',
      'Remora Fuzz',
      'Tide Compressor',
      'Urchin Octave-up fuzz',
      'Sargasso Sustain fuzz',
      'Pearl Overdrive',
      'Kelp Mid overdrive',
      'Coral Distortion',
      'Ripple Modulation',
      'Breaker Treble booster',
    ]);
  });
});
