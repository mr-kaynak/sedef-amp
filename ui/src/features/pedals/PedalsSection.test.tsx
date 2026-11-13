// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { getValue } from '../../params/paramStore';
import { flush, setupMock, type Harness } from '../../test/harness';
import { KelpPedal } from './KelpPedal';
import { PedalBoard } from './PedalBoard';
import { PEDALS, switchedOnBoardFor } from './registry';
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

  it('Trench switches between Mono and Poly, and its hint follows the mode', async () => {
    render(<TrenchPedal />);
    const pedal = screen.getByRole('region', { name: 'Trench Octave down' });
    const mode = within(pedal).getByRole('radiogroup', { name: 'Trench Mode' });
    const mono = within(mode).getByRole('radio', { name: 'Mono' });
    const toggle = within(pedal).getByRole('switch', { name: 'Trench on/off' });
    // Mono is the default, so sessions and presets from before the switch keep their sound.
    expect(mono).toHaveAttribute('aria-checked', 'true');
    expect(toggle).toHaveAccessibleDescription('One note at a time');

    fireEvent.keyDown(mono, { key: 'ArrowRight' });
    expect(getValue('trenchMode')).toBe(1);
    expect(within(mode).getByRole('radio', { name: 'Poly' })).toHaveAttribute(
      'aria-checked',
      'true',
    );
    expect(within(pedal).getByText('Chords and single notes')).toBeVisible();
    expect(toggle).toHaveAccessibleDescription('Chords and single notes');
    await flush();
    expect(h.mock.value('trenchMode')).toBe(1);

    fireEvent.click(within(mode).getByRole('radio', { name: 'Mono' }));
    expect(getValue('trenchMode')).toBe(0);
    expect(toggle).toHaveAccessibleDescription('One note at a time');
  });

  it('sit in signal order on a board with every pedal', async () => {
    // Init's board holds the Gate alone; switch every pedal on (first copies) to see where each
    // one sits.
    h.dispose();
    const firsts = new Set(PEDALS.map((p) => p.enable));
    h = await setupMock({
      initialPresetId: null,
      board: switchedOnBoardFor((enable) => firsts.has(enable)),
    });
    render(<PedalBoard />);
    const names = screen.getAllByRole('region').map((region) => region.getAttribute('aria-label'));
    expect(names).toEqual([
      'Gate Noise gate',
      'Trench Octave down',
      'Anemone Envelope filter',
      'Conch Wah',
      'Remora Fuzz',
      'Tide Compressor',
      'Urchin Octave-up fuzz',
      'Sargasso Sustain fuzz',
      'Pearl Overdrive',
      'Kelp Mid overdrive',
      'Coral Distortion',
      'Beacon Photocell vibe',
      'Gyre Phaser',
      'Jetty Flanger',
      'Ripple Modulation',
      'Breaker Treble booster',
    ]);
  });
});
