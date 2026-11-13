// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { groovesFor } from '../../lib/drums';
import { getValue } from '../../params/paramStore';
import { setParam } from '../../params/useParam';
import { flush, setupMock, type Harness } from '../../test/harness';
import { MetronomePanel } from './MetronomePanel';

describe('Metronome panel: click or drums', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  it('switches from the click to the drums, sharing the play button', async () => {
    render(<MetronomePanel />);
    expect(screen.getByRole('group', { name: 'Beat pattern' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Start metronome' })).toBeInTheDocument();

    const mode = screen.getByRole('radiogroup', { name: 'Click or drums' });
    fireEvent.click(within(mode).getByRole('radio', { name: 'Drums' }));
    expect(getValue('metronomeMode')).toBe(1);
    expect(screen.queryByRole('group', { name: 'Beat pattern' })).toBeNull();

    // One play button: in Drums mode it starts the drums, through the metronome's own switch.
    fireEvent.click(screen.getByRole('button', { name: 'Start drums' }));
    expect(getValue('metronomeEnabled')).toBe(1);
    expect(screen.getByRole('button', { name: 'Stop drums' })).toHaveAttribute(
      'aria-pressed',
      'true',
    );
    await flush();
    expect(h.mock.value('metronomeMode')).toBe(1);
  });

  it('lists the grooves of the meter, each named with its feel', () => {
    setParam('metronomeMode', 1);
    render(<MetronomePanel />);
    const list = screen.getByRole('radiogroup', { name: 'Groove' });
    const radios = within(list).getAllByRole('radio');
    expect(radios.map((r) => r.textContent)).toEqual(
      groovesFor(2).map((g) => `${g.name}${g.feel}`),
    );
    const rock = within(list).getByRole('radio', { name: 'Straight rock' });
    expect(rock).toHaveAttribute('aria-checked', 'true');
    expect(rock).toHaveAccessibleDescription('Eighths on the hat, backbeat on 2 and 4');
    // One tab stop: the groove that plays.
    expect(radios.filter((r) => r.tabIndex === 0)).toEqual([rock]);
  });

  it('picks a groove by click and by arrow keys', () => {
    setParam('metronomeMode', 1);
    render(<MetronomePanel />);
    const list = screen.getByRole('radiogroup', { name: 'Groove' });
    fireEvent.click(within(list).getByRole('radio', { name: 'Shuffle' }));
    expect(getValue('drumsGroove')).toBe(groovesFor(2).find((g) => g.name === 'Shuffle')!.index);

    const shuffle = within(list).getByRole('radio', { name: 'Shuffle' });
    shuffle.focus();
    fireEvent.keyDown(shuffle, { key: 'ArrowDown' });
    expect(getValue('drumsGroove')).toBe(groovesFor(2).find((g) => g.name === 'Swing')!.index);
    expect(within(list).getByRole('radio', { name: 'Swing' })).toHaveFocus();
    fireEvent.keyDown(document.activeElement!, { key: 'Home' });
    expect(within(list).getByRole('radio', { name: 'Straight rock' })).toHaveFocus();
  });

  it('shows another meter’s grooves when the meter changes, its first playing', () => {
    setParam('metronomeMode', 1);
    setParam('drumsGroove', groovesFor(2)[3]!.index);
    render(<MetronomePanel />);
    fireEvent.change(screen.getByRole('combobox', { name: 'Metronome Signature' }), {
      target: { value: '4' },
    });
    const list = screen.getByRole('radiogroup', { name: 'Groove' });
    const radios = within(list).getAllByRole('radio');
    expect(radios).toHaveLength(groovesFor(4).length);
    expect(radios[0]).toHaveAttribute('aria-checked', 'true');
    expect(radios[0]).toHaveAccessibleName(groovesFor(4)[0]!.name);
    // The choice is kept: back in 4/4 the same groove plays again.
    fireEvent.change(screen.getByRole('combobox', { name: 'Metronome Signature' }), {
      target: { value: '2' },
    });
    expect(
      within(screen.getByRole('radiogroup', { name: 'Groove' })).getByRole('radio', {
        name: groovesFor(2)[3]!.name,
      }),
    ).toHaveAttribute('aria-checked', 'true');
  });

  it('sets intensity, fills, level and tone', () => {
    setParam('metronomeMode', 1);
    render(<MetronomePanel />);
    const intensity = screen.getByRole('radiogroup', { name: 'Intensity' });
    expect(intensity).toHaveAccessibleDescription('1 plays the bare groove, 5 the busiest.');
    expect(within(intensity).getByRole('radio', { name: '3' })).toHaveAttribute(
      'aria-checked',
      'true',
    );
    fireEvent.click(within(intensity).getByRole('radio', { name: '5' }));
    expect(getValue('drumsIntensity')).toBe(4);

    const fills = screen.getByRole('radiogroup', { name: 'Fill every' });
    expect(
      within(fills)
        .getAllByRole('radio')
        .map((r) => r.textContent),
    ).toEqual(['Off', '2 bars', '4 bars', '8 bars']);
    fireEvent.click(within(fills).getByRole('radio', { name: 'Off' }));
    expect(getValue('drumsFills')).toBe(0);

    expect(screen.getByRole('slider', { name: 'Drums Level' })).toBeInTheDocument();
    expect(screen.getByRole('slider', { name: 'Drums Tone' })).toBeInTheDocument();
  });
});
