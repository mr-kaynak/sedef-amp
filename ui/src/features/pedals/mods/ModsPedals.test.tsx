// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { applyRemoteValues, getValue } from '../../../params/paramStore';
import { setupMock, type Harness } from '../../../test/harness';
import { AnemonePedal, BeaconPedal, ConchPedal, GyrePedal, JettyPedal } from '.';
import { lampBrightness, triangle, VibeLamp } from './lamps';

describe('mods pedal panels', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  const cases = [
    {
      Panel: ConchPedal,
      name: 'Conch Wah',
      sliders: ['Conch Position', 'Conch Resonance', 'Conch Auto Sensitivity'],
      groups: ['Conch Mode'],
    },
    {
      Panel: AnemonePedal,
      name: 'Anemone Envelope filter',
      sliders: ['Anemone Sensitivity', 'Anemone Range', 'Anemone Resonance'],
      groups: ['Anemone Mode', 'Anemone Direction'],
    },
    {
      Panel: GyrePedal,
      name: 'Gyre Phaser',
      sliders: ['Gyre Speed', 'Gyre Depth', 'Gyre Feedback'],
      groups: ['Gyre Stages'],
    },
    {
      Panel: JettyPedal,
      name: 'Jetty Flanger',
      sliders: ['Jetty Speed', 'Jetty Depth', 'Jetty Delay', 'Jetty Feedback'],
      groups: [],
    },
    {
      Panel: BeaconPedal,
      name: 'Beacon Photocell vibe',
      sliders: ['Beacon Speed', 'Beacon Depth'],
      groups: ['Beacon Mode'],
    },
  ];

  it.each(cases)('$name renders on its own with every control named', (c) => {
    const { container } = render(<c.Panel />);
    const pedal = within(container).getByRole('region', { name: c.name });
    for (const name of c.sliders) expect(within(pedal).getByRole('slider', { name })).toBeVisible();
    for (const name of c.groups)
      expect(within(pedal).getByRole('radiogroup', { name })).toBeVisible();
    expect(
      within(pedal).getByRole('switch', { name: `${c.name.split(' ')[0]} on/off` }),
    ).toBeVisible();
  });

  it('switches modes and stages from the panels', () => {
    render(
      <>
        <ConchPedal />
        <AnemonePedal />
        <GyrePedal />
        <BeaconPedal />
      </>,
    );
    fireEvent.click(screen.getByRole('radio', { name: 'Auto' }));
    expect(getValue('conchMode')).toBe(1);
    fireEvent.click(screen.getByRole('radio', { name: 'Band' }));
    expect(getValue('anemoneMode')).toBe(1);
    fireEvent.click(screen.getByRole('radio', { name: 'Down' }));
    expect(getValue('anemoneDirection')).toBe(1);
    fireEvent.click(screen.getByRole('radio', { name: '6-stage' }));
    expect(getValue('gyreStages')).toBe(1);
    fireEvent.click(screen.getByRole('radio', { name: 'Vibrato' }));
    expect(getValue('beaconMode')).toBe(1);
  });

  it('shows the flanger feedback with its sign', () => {
    applyRemoteValues({ jettyFeedback: -40 });
    render(<JettyPedal />);
    expect(screen.getByRole('slider', { name: 'Jetty Feedback' })).toHaveAttribute(
      'aria-valuetext',
      '−40%',
    );
  });
});

describe('mods pedal lamps', () => {
  it('sweeps the phaser and flanger lamps as a triangle, never fully dark', () => {
    expect(triangle(0)).toBe(0);
    expect(triangle(0.25)).toBeCloseTo(0.5);
    expect(triangle(0.5)).toBe(1);
    expect(triangle(1.25)).toBeCloseTo(0.5);
    expect(lampBrightness(0)).toBeCloseTo(0.35);
    expect(lampBrightness(1)).toBe(1);
  });

  it("gives the vibe's lamp the engine's lopsided throb", () => {
    // As engine/tests/pedals/BeaconTests.cpp measures: at 1 Hz and full depth the light rises in
    // well under half the cycle.
    const lamp = new VibeLamp();
    const dt = 1 / 1000;
    for (let t = 0; t < 2; t += dt) lamp.advance(dt, 1, 1);
    const light: number[] = [];
    for (let t = 0; t < 1; t += dt) light.push(lamp.advance(dt, 1, 1));
    // Brightening: from the darkest point until the light is within 5 % of its brightest.
    const lo = Math.min(...light);
    const hi = Math.max(...light);
    const min = light.indexOf(lo);
    let lit = min;
    while (light[lit % light.length]! < hi - 0.05 * (hi - lo)) lit++;
    const rise = (lit - min) / light.length;
    expect(rise).toBeLessThan(0.4);
    expect(rise).toBeGreaterThan(0.2);
    expect(hi - lo).toBeGreaterThan(0.8);

    const idle = new VibeLamp();
    for (let t = 0; t < 1; t += dt) idle.advance(dt, 2, 0);
    expect(idle.light).toBeCloseTo(0.25, 3);
  });
});
